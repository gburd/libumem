/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * In-process introspection control channel — the server side of umemctl.
 *
 * A per-process Unix domain socket, served by a background thread spawned
 * lazily ONLY when UMEM_OPTIONS=introspect=1. It walks the live cache list
 * (rooted at umem_null_cache), maps addresses to buffers, decodes audit
 * records, streams a log-like event feed, records it, and drives a
 * break-before-return engine (condvar spin) useful under gdb.
 *
 * ZERO-COST WHEN DISABLED: when introspect=0 (default) this file's thread is
 * never started and umem_introspect_break_armed stays 0, so the single
 * hot-path check in _umem_alloc predicts not-taken and never calls in here.
 * When the whole feature is compiled out (no UMEM_INTROSPECT), this file is
 * empty and the hook is a no-op inline (see umem_introspect.h).
 */

#include "config.h"

#ifdef UMEM_INTROSPECT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

#include "umem_base.h"
#include "umem_impl.h"
#include "misc.h"
#include "umem_introspect.h"

/* RSS in KiB from /proc/self/statm (resident pages * pagesize). */
static long
self_rss_kb(void)
{
	FILE *f = fopen("/proc/self/statm", "r");
	unsigned long total = 0, resident = 0;
	if (f == NULL)
		return (0);
	if (fscanf(f, "%lu %lu", &total, &resident) != 2)
		resident = 0;
	fclose(f);
	return ((long)(resident * (sysconf(_SC_PAGESIZE) >> 10)));
}

/* ---- option flag, set from envvar.c ---- */
int umem_introspect_enabled = 0;

/* ---- break engine state (only ever armed via the control channel) ---- */
volatile int umem_introspect_break_armed = 0;

/* Break predicates. One active predicate at a time. ponytail: single
 * predicate -- chain them only if a real workflow needs AND/OR. */
enum { BRK_NONE = 0, BRK_SIZE, BRK_CACHE, BRK_SEQ, BRK_LEAKED };
static int brk_kind = BRK_NONE;
static size_t brk_size;			/* BRK_SIZE */
static char brk_cache[UMEM_CACHE_NAMELEN + 1];	/* BRK_CACHE */
static uint64_t brk_seq;		/* BRK_SEQ target */
static uint64_t brk_seq_counter;	/* global alloc counter for BRK_SEQ */

/* Leak set: signatures (size + first stack PC) that a prior --learn-leaks
 * run found never freed. Loaded by "sig ..." lines then "break leaked". */
struct leaksig { size_t size; uintptr_t pc; };
static struct leaksig *brk_leakset;
static size_t brk_leakset_n;

/* Condvar the broken thread spins on until "continue". */
static pthread_mutex_t brk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t brk_cv = PTHREAD_COND_INITIALIZER;
static volatile int brk_continue;	/* bumped by "continue" */
static volatile int brk_stopped;	/* a thread is currently stopped */

/* ================= introspection walks (C mirror of umem_inspect.py) ===== */

static umem_cache_t *
find_cache_by_name(const char *name)
{
	umem_cache_t *cp;
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		if (strcmp(cp->cache_name, name) == 0)
			return (cp);
	}
	return (NULL);
}

/* Find the slab owning addr in cp; returns slab or NULL, sets *base. */
static umem_slab_t *
slab_of(umem_cache_t *cp, uintptr_t addr, uintptr_t *base_out)
{
	umem_slab_t *sp;
	for (sp = cp->cache_nullslab.slab_next; sp != &cp->cache_nullslab;
	    sp = sp->slab_next) {
		uintptr_t base = (uintptr_t)sp->slab_base;
		if (addr - base < cp->cache_slabsize) {
			*base_out = base;
			return (sp);
		}
	}
	return (NULL);
}

/* Is buf_addr currently handed out? Prefer buftag state, else free-list. */
static int
is_allocated(umem_cache_t *cp, umem_slab_t *sp, uintptr_t buf_addr)
{
	if (cp->cache_flags & UMF_BUFTAG) {
		if (cp->cache_buftag) {
			umem_buftag_t *btp =
			    (umem_buftag_t *)(buf_addr + cp->cache_buftag);
			uintptr_t bc = (uintptr_t)btp->bt_bufctl;
			uintptr_t bx = (uintptr_t)btp->bt_bxstat;
			if (bx == (bc ^ UMEM_BUFTAG_ALLOC))
				return (1);
			if (bx == (bc ^ UMEM_BUFTAG_FREE))
				return (0);
		}
	}
	/* Free-list fallback. */
	{
		umem_bufctl_t *bcp;
		for (bcp = sp->slab_head; bcp != NULL; bcp = bcp->bc_next) {
			uintptr_t a = (cp->cache_flags & UMF_HASH)
			    ? (uintptr_t)bcp->bc_addr
			    : ((uintptr_t)bcp - cp->cache_bufctl);
			if (a == buf_addr)
				return (0);
		}
	}
	return (1);
}

/* Locate the audit bufctl for buf_addr (audit caches are UMF_HASH). */
static umem_bufctl_audit_t *
audit_bufctl_for(umem_cache_t *cp, uintptr_t buf_addr)
{
	if (cp->cache_flags & UMF_HASH) {
		umem_bufctl_t **table = cp->cache_hash_table;
		umem_bufctl_t *bcp;
		if (table == NULL)
			return (NULL);
		bcp = *UMEM_HASH(cp, buf_addr);
		for (; bcp != NULL; bcp = bcp->bc_next) {
			if ((uintptr_t)bcp->bc_addr == buf_addr)
				return ((umem_bufctl_audit_t *)bcp);
		}
		return (NULL);
	}
	if (cp->cache_buftag) {
		umem_buftag_t *btp =
		    (umem_buftag_t *)(buf_addr + cp->cache_buftag);
		return ((umem_bufctl_audit_t *)btp->bt_bufctl);
	}
	return (NULL);
}

/* ============================ command handlers =========================== */

static void
cmd_stats(FILE *out)
{
	umem_cache_t *cp;
	uint64_t inuse = 0, total = 0, slab_create = 0, slab_destroy = 0;
	uint64_t depot_contention = 0, mag_reloads = 0;
	int ncaches = 0;

	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		inuse += cp->cache_slab_alloc - cp->cache_slab_free;
		total += cp->cache_buftotal;
		slab_create += cp->cache_slab_create;
		slab_destroy += cp->cache_slab_destroy;
		depot_contention += cp->cache_depot_contention;
		mag_reloads += cp->cache_mag_reloads;
		ncaches++;
	}
	fprintf(out, "pid %ld\n", (long)getpid());
	fprintf(out, "caches %d\n", ncaches);
	fprintf(out, "bufs_inuse %llu\n", (unsigned long long)inuse);
	fprintf(out, "bufs_total %llu\n", (unsigned long long)total);
	fprintf(out, "slab_create %llu\n", (unsigned long long)slab_create);
	fprintf(out, "slab_destroy %llu\n", (unsigned long long)slab_destroy);
	fprintf(out, "depot_contention %llu\n",
	    (unsigned long long)depot_contention);
	fprintf(out, "mag_reloads %llu\n", (unsigned long long)mag_reloads);
	fprintf(out, "rss_kb %ld\n", self_rss_kb());
	fprintf(out, ".\n");
}

static void
cmd_caches(FILE *out)
{
	umem_cache_t *cp;
	fprintf(out, "%-32s %8s %10s %10s %8s\n",
	    "name", "bufsize", "inuse", "total", "flags");
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		uint64_t inuse = cp->cache_slab_alloc - cp->cache_slab_free;
		fprintf(out, "%-32s %8zu %10llu %10llu 0x%x\n",
		    cp->cache_name, cp->cache_bufsize,
		    (unsigned long long)inuse,
		    (unsigned long long)cp->cache_buftotal,
		    cp->cache_flags);
	}
	fprintf(out, ".\n");
}

static void
cmd_cache(FILE *out, const char *name)
{
	umem_cache_t *cp = find_cache_by_name(name);
	if (cp == NULL) {
		fprintf(out, "no such cache: %s\n.\n", name);
		return;
	}
	fprintf(out, "name %s\n", cp->cache_name);
	fprintf(out, "bufsize %zu\n", cp->cache_bufsize);
	fprintf(out, "align %zu\n", cp->cache_align);
	fprintf(out, "chunksize %zu\n", cp->cache_chunksize);
	fprintf(out, "slabsize %zu\n", cp->cache_slabsize);
	fprintf(out, "flags 0x%x\n", cp->cache_flags);
	fprintf(out, "slab_alloc %llu\n",
	    (unsigned long long)cp->cache_slab_alloc);
	fprintf(out, "slab_free %llu\n",
	    (unsigned long long)cp->cache_slab_free);
	fprintf(out, "inuse %llu\n",
	    (unsigned long long)(cp->cache_slab_alloc - cp->cache_slab_free));
	fprintf(out, "buftotal %llu\n",
	    (unsigned long long)cp->cache_buftotal);
	fprintf(out, "slab_create %llu\n",
	    (unsigned long long)cp->cache_slab_create);
	fprintf(out, "slab_destroy %llu\n",
	    (unsigned long long)cp->cache_slab_destroy);
	fprintf(out, "depot_contention %llu\n",
	    (unsigned long long)cp->cache_depot_contention);
	fprintf(out, "mag_reloads %llu\n",
	    (unsigned long long)cp->cache_mag_reloads);
	fprintf(out, ".\n");
}

static void
cmd_whatis(FILE *out, uintptr_t addr)
{
	umem_cache_t *cp, *best = NULL;
	size_t best_slabsize = 0;
	uintptr_t best_base = 0;
	umem_slab_t *best_slab = NULL;

	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		uintptr_t base;
		umem_slab_t *sp = slab_of(cp, addr, &base);
		if (sp == NULL)
			continue;
		if (best != NULL && cp->cache_slabsize >= best_slabsize)
			continue;
		best = cp;
		best_slabsize = cp->cache_slabsize;
		best_base = base;
		best_slab = sp;
	}
	if (best == NULL) {
		fprintf(out, "0x%lx: not a umem buffer\n.\n",
		    (unsigned long)addr);
		return;
	}
	{
		uintptr_t buf = addr -
		    ((addr - best_base) % best->cache_chunksize);
		int alloc = is_allocated(best, best_slab, buf);
		fprintf(out, "addr 0x%lx\n", (unsigned long)addr);
		fprintf(out, "cache %s\n", best->cache_name);
		fprintf(out, "bufsize %zu\n", best->cache_bufsize);
		fprintf(out, "buffer 0x%lx\n", (unsigned long)buf);
		fprintf(out, "slab 0x%lx\n", (unsigned long)best_slab);
		fprintf(out, "state %s\n", alloc ? "allocated" : "free");
		fprintf(out, ".\n");
	}
}

/* Emit every currently-allocated buffer in audit caches, with its stack.
 * cb lets G3's learn-leaks reuse the same walk. */
static void
walk_leaks(void (*cb)(umem_bufctl_audit_t *bcap, umem_cache_t *cp, void *arg),
    void *arg)
{
	umem_cache_t *cp;
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		umem_slab_t *sp;
		if (!(cp->cache_flags & UMF_AUDIT))
			continue;
		for (sp = cp->cache_nullslab.slab_next;
		    sp != &cp->cache_nullslab; sp = sp->slab_next) {
			uintptr_t base = (uintptr_t)sp->slab_base;
			long i;
			for (i = 0; i < sp->slab_chunks; i++) {
				uintptr_t buf = base + i * cp->cache_chunksize;
				umem_bufctl_audit_t *bcap;
				if (!is_allocated(cp, sp, buf))
					continue;
				bcap = audit_bufctl_for(cp, buf);
				if (bcap != NULL)
					cb(bcap, cp, arg);
			}
		}
	}
}

static void
leaks_printer(umem_bufctl_audit_t *bcap, umem_cache_t *cp, void *arg)
{
	FILE *out = arg;
	int d, depth = bcap->bc_depth;
	if (depth < 0)
		depth = 0;
	if (depth > (int)umem_stack_depth)
		depth = (int)umem_stack_depth;
	fprintf(out, "leak addr=0x%lx cache=%s size=%zu thread=%lu depth=%d\n",
	    (unsigned long)(uintptr_t)bcap->bc_addr, cp->cache_name,
	    cp->cache_bufsize, (unsigned long)bcap->bc_thread, depth);
	for (d = 0; d < depth; d++)
		fprintf(out, "  0x%lx\n", (unsigned long)bcap->bc_stack[d]);
}

static void
cmd_leaks(FILE *out)
{
	if (!(umem_flags & UMF_AUDIT)) {
		fprintf(out, "leaks: requires UMEM_DEBUG=audit\n.\n");
		return;
	}
	walk_leaks(leaks_printer, out);
	fprintf(out, ".\n");
}

/* ---- learn-leaks: write signatures (size + top PC) to the client ---- */
static void
learn_printer(umem_bufctl_audit_t *bcap, umem_cache_t *cp, void *arg)
{
	FILE *out = arg;
	uintptr_t pc = (bcap->bc_depth > 0) ? bcap->bc_stack[0] : 0;
	fprintf(out, "sig size=%zu pc=0x%lx\n", cp->cache_bufsize,
	    (unsigned long)pc);
}

static void
cmd_learn_leaks(FILE *out)
{
	if (!(umem_flags & UMF_AUDIT)) {
		fprintf(out, "learn: requires UMEM_DEBUG=audit\n.\n");
		return;
	}
	walk_leaks(learn_printer, out);
	fprintf(out, ".\n");
}

/* =============================== break engine ============================ */

static void
break_disarm(void)
{
	brk_kind = BRK_NONE;
	umem_introspect_break_armed = 0;
	free(brk_leakset);
	brk_leakset = NULL;
	brk_leakset_n = 0;
}

/* Parse "break <predicate>" argument. For "leaked" the leak set is streamed
 * in first via "sig ..." lines (cmd_sig_add). */
static void
cmd_break(FILE *out, char *arg)
{
	int keep_leakset = (strcmp(arg, "leaked") == 0);
	if (!keep_leakset)
		break_disarm();
	else
		brk_kind = BRK_NONE;	/* keep leak set already loaded */

	if (strncmp(arg, "size=", 5) == 0) {
		brk_kind = BRK_SIZE;
		brk_size = (size_t)strtoull(arg + 5, NULL, 0);
	} else if (strncmp(arg, "cache=", 6) == 0) {
		brk_kind = BRK_CACHE;
		strncpy(brk_cache, arg + 6, sizeof (brk_cache) - 1);
		brk_cache[sizeof (brk_cache) - 1] = '\0';
	} else if (strncmp(arg, "seq=", 4) == 0) {
		brk_kind = BRK_SEQ;
		brk_seq = strtoull(arg + 4, NULL, 0);
		brk_seq_counter = 0;
	} else if (strncmp(arg, "token=", 6) == 0) {
		/* Token break: stop the next allocation (one-shot), used by
		 * the recording-token flow (a 'BREAK' marker in a stream). */
		brk_kind = BRK_SEQ;
		brk_seq = 1;
		brk_seq_counter = 0;
	} else if (strcmp(arg, "leaked") == 0) {
		brk_kind = BRK_LEAKED;
		/* leak set already loaded via 'sig' lines before this. */
	} else {
		fprintf(out, "bad predicate: %s\n.\n", arg);
		return;
	}
	umem_introspect_break_armed = 1;
	fprintf(out, "ok armed\n.\n");
}

/* Client pushes leak signatures ahead of "break leaked". */
static void
cmd_sig_add(FILE *out, char *arg)
{
	size_t sz = 0;
	uintptr_t pc = 0;
	char *p;
	struct leaksig *ns;
	if ((p = strstr(arg, "size=")) != NULL)
		sz = (size_t)strtoull(p + 5, NULL, 0);
	if ((p = strstr(arg, "pc=")) != NULL)
		pc = (uintptr_t)strtoull(p + 3, NULL, 0);
	ns = realloc(brk_leakset, (brk_leakset_n + 1) * sizeof (*brk_leakset));
	if (ns == NULL) {
		fprintf(out, "sig oom\n.\n");
		return;
	}
	brk_leakset = ns;
	brk_leakset[brk_leakset_n].size = sz;
	brk_leakset[brk_leakset_n].pc = pc;
	brk_leakset_n++;
	fprintf(out, "ok sig %zu\n.\n", brk_leakset_n);
}

static void
cmd_continue(FILE *out)
{
	(void) pthread_mutex_lock(&brk_lock);
	brk_continue++;
	(void) pthread_cond_broadcast(&brk_cv);
	(void) pthread_mutex_unlock(&brk_lock);
	/* Also disarm so we don't immediately re-trip. */
	break_disarm();
	fprintf(out, "ok continue\n.\n");
}

/*
 * Hot-path hook, called ONLY when umem_introspect_break_armed != 0. Decides
 * whether the current allocation matches an armed predicate and, if so,
 * blocks the allocating thread on the condvar until "continue".
 */
void
umem_introspect_break_check(void *buf, size_t size, umem_cache_t *cp)
{
	int match = 0;

	switch (brk_kind) {
	case BRK_SIZE:
		match = (size == brk_size);
		break;
	case BRK_CACHE:
		match = (cp != NULL &&
		    strcmp(cp->cache_name, brk_cache) == 0);
		break;
	case BRK_SEQ:
		match = (__sync_add_and_fetch(&brk_seq_counter, 1) == brk_seq);
		break;
	case BRK_LEAKED: {
		/* Match on size; the learned set already narrowed to leaked
		 * sizes. ponytail: size-only live match; add a getpcstack
		 * compare if false positives matter under audit. */
		size_t i;
		for (i = 0; i < brk_leakset_n; i++) {
			if (brk_leakset[i].size == size) {
				match = 1;
				break;
			}
		}
		break;
	}
	default:
		break;
	}
	if (!match)
		return;

	/* Stop this (allocating) thread until a client sends "continue". */
	(void) pthread_mutex_lock(&brk_lock);
	{
		int start = brk_continue;
		brk_stopped = 1;
		log_message("umem: BREAK: thread stopped before returning "
		    "buf=%p size=%zu cache=%s (umemctl continue to resume)\n",
		    buf, size, cp ? cp->cache_name : "?");
		while (brk_continue == start)
			(void) pthread_cond_wait(&brk_cv, &brk_lock);
		brk_stopped = 0;
	}
	(void) pthread_mutex_unlock(&brk_lock);
}

/* =============================== server ================================== */

/* Poll the aggregate counters; used by logtail to emit deltas. */
struct logsnap {
	uint64_t slab_create, slab_destroy;
	uint64_t inuse;
};

static void
snapshot(struct logsnap *s)
{
	umem_cache_t *cp;
	memset(s, 0, sizeof (*s));
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		s->slab_create += cp->cache_slab_create;
		s->slab_destroy += cp->cache_slab_destroy;
		s->inuse += cp->cache_slab_alloc - cp->cache_slab_free;
	}
}

/* logtail / record: stream deltas until the client disconnects. The stream
 * is derived by polling counters the allocator already maintains, so it adds
 * no hot-path cost (no alloc/free hook). ponytail: 5 Hz poll -- fine for a
 * log tail; hook slab_create/destroy directly only if sub-poll latency
 * matters. */
static void
cmd_logtail(FILE *out)
{
	struct logsnap prev, cur;
	snapshot(&prev);
	fprintf(out, "logtail start pid=%ld\n", (long)getpid());
	fflush(out);
	for (;;) {
		usleep(200 * 1000);	/* ~5 Hz poll */
		snapshot(&cur);
		if (cur.slab_create != prev.slab_create)
			fprintf(out, "slab_create total=%llu (+%llu)\n",
			    (unsigned long long)cur.slab_create,
			    (unsigned long long)(cur.slab_create -
			    prev.slab_create));
		if (cur.slab_destroy != prev.slab_destroy)
			fprintf(out, "slab_destroy total=%llu (+%llu)\n",
			    (unsigned long long)cur.slab_destroy,
			    (unsigned long long)(cur.slab_destroy -
			    prev.slab_destroy));
		if (cur.inuse != prev.inuse)
			fprintf(out, "inuse %llu (%+lld)\n",
			    (unsigned long long)cur.inuse,
			    (long long)cur.inuse - (long long)prev.inuse);
		if (fflush(out) != 0)
			break;		/* client gone */
		prev = cur;
	}
}

static void
handle_line(FILE *out, char *line)
{
	char *arg;
	line[strcspn(line, "\r\n")] = '\0';
	if (line[0] == '\0')
		return;
	arg = strchr(line, ' ');
	if (arg != NULL)
		*arg++ = '\0';

	if (strcmp(line, "stats") == 0)
		cmd_stats(out);
	else if (strcmp(line, "caches") == 0)
		cmd_caches(out);
	else if (strcmp(line, "cache") == 0 && arg)
		cmd_cache(out, arg);
	else if (strcmp(line, "whatis") == 0 && arg)
		cmd_whatis(out, (uintptr_t)strtoull(arg, NULL, 0));
	else if (strcmp(line, "leaks") == 0)
		cmd_leaks(out);
	else if (strcmp(line, "learn") == 0)
		cmd_learn_leaks(out);
	else if (strcmp(line, "sig") == 0 && arg)
		cmd_sig_add(out, arg);
	else if (strcmp(line, "break") == 0 && arg)
		cmd_break(out, arg);
	else if (strcmp(line, "continue") == 0)
		cmd_continue(out);
	else if (strcmp(line, "logtail") == 0)
		cmd_logtail(out);
	else if (strcmp(line, "record") == 0)
		cmd_logtail(out);
	else
		fprintf(out, "unknown command: %s\n.\n", line);
	fflush(out);
}

static const char *
sock_path(char *buf, size_t n)
{
	const char *env = getenv("UMEM_INTROSPECT_SOCK");
	if (env != NULL && env[0] != '\0') {
		strncpy(buf, env, n - 1);
		buf[n - 1] = '\0';
	} else {
		snprintf(buf, n, "/tmp/umem.%ld.sock", (long)getpid());
	}
	return (buf);
}

static void *
introspect_thread(void *unused)
{
	char path[108];
	struct sockaddr_un addr;
	int lfd;

	(void) unused;
	sock_path(path, sizeof (path));
	(void) unlink(path);

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0)
		return (NULL);
	memset(&addr, 0, sizeof (addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof (addr.sun_path) - 1);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof (addr)) < 0 ||
	    listen(lfd, 4) < 0) {
		close(lfd);
		return (NULL);
	}
	log_message("umem: introspect socket at %s\n", path);

	/* ponytail: single client served at a time; each connection is a
	 * short request/response or a long stream. Fork a per-client thread
	 * only if concurrent umemctl sessions become a real need. */
	for (;;) {
		int cfd = accept(lfd, NULL, NULL);
		FILE *out;
		char line[256];
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		out = fdopen(cfd, "r+");
		if (out == NULL) {
			close(cfd);
			continue;
		}
		while (fgets(line, sizeof (line), out) != NULL)
			handle_line(out, line);
		fclose(out);
	}
	close(lfd);
	(void) unlink(path);
	return (NULL);
}

static pthread_once_t introspect_once = PTHREAD_ONCE_INIT;

static void
introspect_launch(void)
{
	pthread_t tid;
	pthread_attr_t attr;
	(void) pthread_attr_init(&attr);
	(void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	(void) pthread_create(&tid, &attr, introspect_thread, NULL);
	(void) pthread_attr_destroy(&attr);
}

void
umem_introspect_start(void)
{
	if (!umem_introspect_enabled)
		return;
	(void) pthread_once(&introspect_once, introspect_launch);
}

#endif /* UMEM_INTROSPECT */

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
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/*
 * ---- break engine ----
 *
 * CONTRACT (Phase 3 item 5 of the production-readiness plan):
 *
 * B1. ALL predicate state (kind, size, cache name, sequence, leak set) is
 *     read and written ONLY under brk_lock.  It used to be `volatile`, which
 *     orders nothing: an allocating thread could read brk_kind while the
 *     server was still filling in brk_cache, or scan brk_leakset while
 *     break_disarm() freed it, or while cmd_sig_add() realloc'd it.
 *
 * B2. umem_introspect_break_armed is the only field the hot path touches
 *     without the lock.  It is set LAST when arming and cleared FIRST when
 *     disarming, so a thread that observes it set will then take brk_lock and
 *     re-check a fully published predicate -- and a thread that observes it
 *     clear simply skips the check.  It is therefore a hint, never the
 *     decision.
 *
 * B3. A stopped thread waits for brk_generation to CHANGE from the value it
 *     sampled under the lock, not for a specific value.  A thread that
 *     matches after a "continue" has already been processed sees the newer
 *     generation and returns immediately instead of waiting for an event that
 *     already happened (the pre-fix code compared against a snapshot taken
 *     before it had the lock and could strand a thread forever).
 *
 * B4. The SERVER THREAD never stops on a predicate.  A server-side allocation
 *     matching the armed predicate would park the only thread that can
 *     resume anything -- an unrecoverable self-deadlock.  brk_server_thread
 *     is checked on the hot path.
 *
 * B5. FORK: the child has no server thread, so an armed predicate would stop
 *     the child with nothing able to resume it.  umem_introspect_fork_child()
 *     disarms unconditionally and resets the once-control so the child can
 *     start its own server if it wants one.  Called from umem_fork.c's child
 *     handler via the same weak-hook pattern as the interposer.
 */
volatile int umem_introspect_break_armed = 0;

/* Break predicates. One active predicate at a time. ponytail: single
 * predicate -- chain them only if a real workflow needs AND/OR. */
enum { BRK_NONE = 0, BRK_SIZE, BRK_CACHE, BRK_SEQ, BRK_LEAKED };

/* Leak set: signatures (size + first stack PC) that a prior --learn-leaks
 * run found never freed. Loaded by "sig ..." lines then "break leaked". */
struct leaksig { size_t size; uintptr_t pc; };

/*
 * Everything below is protected by brk_lock (B1).  brk_lock is a LEAF lock:
 * nothing is acquired while it is held, and it is taken from the allocation
 * path, so it must never be taken while holding an allocator lock.
 */
static pthread_mutex_t brk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t brk_cv = PTHREAD_COND_INITIALIZER;

static int brk_kind = BRK_NONE;
static size_t brk_size;			/* BRK_SIZE */
static char brk_cache[UMEM_CACHE_NAMELEN + 1];	/* BRK_CACHE */
static uint64_t brk_seq;		/* BRK_SEQ target */
static uint64_t brk_seq_counter;	/* alloc counter for BRK_SEQ */
static struct leaksig *brk_leakset;
static size_t brk_leakset_n;
static uint64_t brk_generation;		/* bumped by every "continue" (B3) */
static int brk_stopped;			/* threads currently stopped */

/* The server thread never stops on its own predicate (B4). */
static volatile int brk_server_valid;
static pthread_t brk_server_thread;

/* Set armed only after the predicate is fully published (B2). */
static void
break_publish_locked(int armed)
{
	if (armed) {
		umem_introspect_break_armed = 1;
	} else {
		umem_introspect_break_armed = 0;
	}
}

/* ================= introspection walks (C mirror of umem_inspect.py) =====
 *
 * CACHE LIFETIME (Phase 3 item 1).  Every walk below holds umem_cache_lock
 * for its whole duration.  umem_cache_destroy() unlinks under that lock and
 * only then destroys the cache's locks and frees the descriptor, so without
 * it a handler could follow a freed cache_next or read a freed descriptor.
 * These handlers previously took NO allocator lock at all while walking
 * caches, slabs, freelists, hash chains and audit records.
 *
 * NOTE ON OUTPUT.  fprintf() allocates, so it must not run under an allocator
 * lock (it would re-enter the allocator under malloc interposition).  Each
 * handler therefore either collects under the lock and prints after, or --
 * where the output is per-cache and unbounded -- holds only umem_cache_lock,
 * which the ALLOCATION path never takes.  umem_cache_lock is taken by
 * umem_cache_create/destroy and the update thread, so printing under it can
 * stall cache creation but cannot deadlock against the allocation that stdio
 * itself performs.
 *
 * ponytail: reuse umem_inspect.c's two-phase collector here instead of two
 * different rules, once the socket protocol is worth a refactor of its own.
 */

/* Caller holds umem_cache_lock. */
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
	/*
	 * Free-list fallback.
	 *
	 * bc_next in a slab freelist is MANGLED (P5.4:
	 * ptr ^ umem_link_cookie ^ (&slot >> 12)), so it must be decoded
	 * before it is followed -- otherwise this walk dereferences a mangled
	 * value, which is a wild read.  Only the SLAB freelist is mangled; the
	 * UMF_HASH allocated-address chain in audit_bufctl_for() below stores
	 * plain pointers and is correctly left alone.  See umem_impl.h's
	 * UMEM_LINK_MANGLE comment, which lists this function as a reader.
	 *
	 * The bounded iteration count is not defensive decoration: if the
	 * chain is corrupt (which is the scenario mangling exists to detect)
	 * the decoded links are garbage, and an inspector must not spin
	 * forever on a cycle while holding cache_lock.
	 */
	{
		umem_bufctl_t *bcp;
		unsigned safety = 0;

		for (bcp = sp->slab_head;
		    bcp != NULL && safety++ < (1u << 20);
		    bcp = UMEM_LINK_DEMANGLE(&bcp->bc_next, bcp->bc_next)) {
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
	uint64_t depot_contention = 0;
	int ncaches = 0;

	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		(void) mutex_lock(&cp->cache_lock);
		inuse += cp->cache_slab_alloc - cp->cache_slab_free;
		total += cp->cache_buftotal;
		slab_create += cp->cache_slab_create;
		slab_destroy += cp->cache_slab_destroy;
		depot_contention += cp->cache_depot_contention;
		(void) mutex_unlock(&cp->cache_lock);
		ncaches++;
	}
	(void) mutex_unlock(&umem_cache_lock);

	/* Printed with no allocator lock held. */
	fprintf(out, "pid %ld\n", (long)getpid());
	fprintf(out, "caches %d\n", ncaches);
	fprintf(out, "bufs_held %llu\n", (unsigned long long)inuse);
	fprintf(out, "bufs_total %llu\n", (unsigned long long)total);
	fprintf(out, "slab_create %llu\n", (unsigned long long)slab_create);
	fprintf(out, "slab_destroy %llu\n", (unsigned long long)slab_destroy);
	fprintf(out, "depot_contention %llu\n",
	    (unsigned long long)depot_contention);
	fprintf(out, "rss_kb %ld\n", self_rss_kb());
	fprintf(out, ".\n");
}

static void
cmd_caches(FILE *out)
{
	umem_cache_t *cp;
	fprintf(out, "%-32s %8s %10s %10s %8s\n",
	    "name", "bufsize", "held", "total", "flags");
	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		uint64_t inuse = cp->cache_slab_alloc - cp->cache_slab_free;
		fprintf(out, "%-32s %8zu %10llu %10llu 0x%x\n",
		    cp->cache_name, cp->cache_bufsize,
		    (unsigned long long)inuse,
		    (unsigned long long)cp->cache_buftotal,
		    cp->cache_flags);
	}
	(void) mutex_unlock(&umem_cache_lock);
	fprintf(out, ".\n");
}

static void
cmd_cache(FILE *out, const char *name)
{
	umem_cache_t *cp;
	/* Copy every field out under the lock; the cache may be destroyed the
	 * moment we release it. */
	struct {
		char name[UMEM_CACHE_NAMELEN + 1];
		size_t bufsize, align, chunksize, slabsize;
		int flags;
		uint64_t slab_alloc, slab_free, buftotal;
		uint64_t slab_create, slab_destroy;
		uint64_t depot_contention;
	} c;

	(void) mutex_lock(&umem_cache_lock);
	cp = find_cache_by_name(name);
	if (cp == NULL) {
		(void) mutex_unlock(&umem_cache_lock);
		fprintf(out, "no such cache: %s\n.\n", name);
		return;
	}
	(void) mutex_lock(&cp->cache_lock);
	(void) strncpy(c.name, cp->cache_name, sizeof (c.name) - 1);
	c.name[sizeof (c.name) - 1] = '\0';
	c.bufsize = cp->cache_bufsize;
	c.align = cp->cache_align;
	c.chunksize = cp->cache_chunksize;
	c.slabsize = cp->cache_slabsize;
	c.flags = cp->cache_flags;
	c.slab_alloc = cp->cache_slab_alloc;
	c.slab_free = cp->cache_slab_free;
	c.buftotal = cp->cache_buftotal;
	c.slab_create = cp->cache_slab_create;
	c.slab_destroy = cp->cache_slab_destroy;
	c.depot_contention = cp->cache_depot_contention;
	(void) mutex_unlock(&cp->cache_lock);
	(void) mutex_unlock(&umem_cache_lock);

	fprintf(out, "name %s\n", c.name);
	fprintf(out, "bufsize %zu\n", c.bufsize);
	fprintf(out, "align %zu\n", c.align);
	fprintf(out, "chunksize %zu\n", c.chunksize);
	fprintf(out, "slabsize %zu\n", c.slabsize);
	fprintf(out, "flags 0x%x\n", c.flags);
	fprintf(out, "slab_alloc %llu\n", (unsigned long long)c.slab_alloc);
	fprintf(out, "slab_free %llu\n", (unsigned long long)c.slab_free);
	fprintf(out, "held %llu\n",
	    (unsigned long long)(c.slab_alloc - c.slab_free));
	fprintf(out, "buftotal %llu\n", (unsigned long long)c.buftotal);
	fprintf(out, "slab_create %llu\n", (unsigned long long)c.slab_create);
	fprintf(out, "slab_destroy %llu\n",
	    (unsigned long long)c.slab_destroy);
	fprintf(out, "depot_contention %llu\n",
	    (unsigned long long)c.depot_contention);
	fprintf(out, ".\n");
}

static void
cmd_whatis(FILE *out, uintptr_t addr)
{
	umem_cache_t *cp, *best = NULL;
	size_t best_slabsize = 0;
	uintptr_t best_base = 0;
	umem_slab_t *best_slab = NULL;
	/* Resolved under the lock, printed after it. */
	char best_name[UMEM_CACHE_NAMELEN + 1];
	size_t best_bufsize = 0, best_chunksize = 0;
	uintptr_t buf = 0;
	int alloc = 0;

	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		uintptr_t base;
		umem_slab_t *sp;

		(void) mutex_lock(&cp->cache_lock);
		sp = slab_of(cp, addr, &base);
		if (sp == NULL) {
			(void) mutex_unlock(&cp->cache_lock);
			continue;
		}
		if (best != NULL && cp->cache_slabsize >= best_slabsize) {
			(void) mutex_unlock(&cp->cache_lock);
			continue;
		}
		best = cp;
		best_slabsize = cp->cache_slabsize;
		best_base = base;
		best_slab = sp;
		best_bufsize = cp->cache_bufsize;
		best_chunksize = cp->cache_chunksize;
		(void) strncpy(best_name, cp->cache_name,
		    sizeof (best_name) - 1);
		best_name[sizeof (best_name) - 1] = '\0';
		buf = addr - ((addr - best_base) % best_chunksize);
		alloc = is_allocated(cp, sp, buf);
		(void) mutex_unlock(&cp->cache_lock);
	}
	(void) mutex_unlock(&umem_cache_lock);

	if (best == NULL) {
		fprintf(out, "0x%lx: not a umem buffer\n.\n",
		    (unsigned long)addr);
		return;
	}
	fprintf(out, "addr 0x%lx\n", (unsigned long)addr);
	fprintf(out, "cache %s\n", best_name);
	fprintf(out, "bufsize %zu\n", best_bufsize);
	fprintf(out, "buffer 0x%lx\n", (unsigned long)buf);
	fprintf(out, "slab 0x%lx\n", (unsigned long)best_slab);
	/*
	 * "held", not "allocated": this reflects the slab/buftag view, so a
	 * buffer the application has already freed into a magazine, an rseq
	 * magazine or a PTC bin still reads held.  Same caveat as HELD in
	 * umem_status_dump().
	 */
	fprintf(out, "state %s\n", alloc ? "held" : "free");
	fprintf(out, ".\n");
}

/* Emit every currently-held buffer in audit caches, with its stack.
 * cb lets G3's learn-leaks reuse the same walk.
 *
 * Holds umem_cache_lock for the whole walk (cache lifetime) and each cache's
 * cache_lock while reading its slabs, freelists and hash chains.  The
 * callback prints, and stdio allocates: that allocation can take cc_lock and
 * ml_lock, which are BELOW cache_lock in THE ONE TRUE LOCK ORDER, so the
 * cache_lock is dropped around the callback. */
static void
walk_leaks(void (*cb)(umem_bufctl_audit_t *bcap, umem_cache_t *cp, void *arg),
    void *arg)
{
	umem_cache_t *cp;

	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		umem_slab_t *sp;
		if (!(cp->cache_flags & UMF_AUDIT))
			continue;
		(void) mutex_lock(&cp->cache_lock);
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
				if (bcap == NULL)
					continue;
				/* Print without cache_lock (stdio allocates);
				 * umem_cache_lock still pins the cache, and
				 * the slab cannot be destroyed while it holds
				 * this held buffer. */
				(void) mutex_unlock(&cp->cache_lock);
				cb(bcap, cp, arg);
				(void) mutex_lock(&cp->cache_lock);
			}
		}
		(void) mutex_unlock(&cp->cache_lock);
	}
	(void) mutex_unlock(&umem_cache_lock);
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

/* Defined with the server, below; declared here for the fork-child reset. */
static void introspect_once_reset(void);

/* Caller holds brk_lock.  Frees the leak set, so no thread may be scanning
 * it -- guaranteed because scanning also happens under brk_lock (B1). */
static void
break_disarm_locked(void)
{
	break_publish_locked(0);
	brk_kind = BRK_NONE;
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
	int kind = BRK_NONE;
	size_t size = 0;
	uint64_t seq = 0;
	char cname[UMEM_CACHE_NAMELEN + 1];

	cname[0] = '\0';

	/* Parse BEFORE touching shared state, so a bad predicate cannot
	 * leave a half-armed engine behind. */
	if (strncmp(arg, "size=", 5) == 0) {
		kind = BRK_SIZE;
		size = (size_t)strtoull(arg + 5, NULL, 0);
	} else if (strncmp(arg, "cache=", 6) == 0) {
		kind = BRK_CACHE;
		strncpy(cname, arg + 6, sizeof (cname) - 1);
		cname[sizeof (cname) - 1] = '\0';
	} else if (strncmp(arg, "seq=", 4) == 0) {
		kind = BRK_SEQ;
		seq = strtoull(arg + 4, NULL, 0);
	} else if (strncmp(arg, "token=", 6) == 0) {
		/* Token break: stop the next allocation (one-shot), used by
		 * the recording-token flow (a 'BREAK' marker in a stream). */
		kind = BRK_SEQ;
		seq = 1;
	} else if (strcmp(arg, "leaked") == 0) {
		kind = BRK_LEAKED;
	} else {
		fprintf(out, "bad predicate: %s\n.\n", arg);
		return;
	}

	(void) pthread_mutex_lock(&brk_lock);
	if (keep_leakset) {
		/* Keep the already-loaded leak set; just disarm the hot path
		 * while we swap the predicate in. */
		break_publish_locked(0);
		brk_kind = BRK_NONE;
	} else {
		break_disarm_locked();
	}

	brk_size = size;
	brk_seq = seq;
	brk_seq_counter = 0;
	if (kind == BRK_CACHE)
		memcpy(brk_cache, cname, sizeof (brk_cache));
	brk_kind = kind;
	/* Published last (B2). */
	break_publish_locked(1);
	(void) pthread_mutex_unlock(&brk_lock);

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
	size_t n;

	if ((p = strstr(arg, "size=")) != NULL)
		sz = (size_t)strtoull(p + 5, NULL, 0);
	if ((p = strstr(arg, "pc=")) != NULL)
		pc = (uintptr_t)strtoull(p + 3, NULL, 0);

	(void) pthread_mutex_lock(&brk_lock);
	/*
	 * The realloc happens under brk_lock, so it cannot move the array
	 * out from under a thread scanning it in break_check (B1).  Pre-fix
	 * this could realloc while a predicate was live.
	 */
	ns = realloc(brk_leakset, (brk_leakset_n + 1) * sizeof (*brk_leakset));
	if (ns == NULL) {
		(void) pthread_mutex_unlock(&brk_lock);
		fprintf(out, "sig oom\n.\n");
		return;
	}
	brk_leakset = ns;
	brk_leakset[brk_leakset_n].size = sz;
	brk_leakset[brk_leakset_n].pc = pc;
	n = ++brk_leakset_n;
	(void) pthread_mutex_unlock(&brk_lock);

	fprintf(out, "ok sig %zu\n.\n", n);
}

static void
cmd_continue(FILE *out)
{
	(void) pthread_mutex_lock(&brk_lock);
	brk_generation++;
	/* Disarm under the SAME lock acquisition as the generation bump, so a
	 * thread cannot match a predicate that "continue" has already
	 * retired. */
	break_disarm_locked();
	(void) pthread_cond_broadcast(&brk_cv);
	(void) pthread_mutex_unlock(&brk_lock);
	fprintf(out, "ok continue\n.\n");
}

/*
 * Hot-path hook, called ONLY when umem_introspect_break_armed != 0. Decides
 * whether the current allocation matches an armed predicate and, if so,
 * blocks the allocating thread on the condvar until "continue".
 *
 * brk_lock is a leaf lock taken from the allocation path; see B1.
 */
void
umem_introspect_break_check(void *buf, size_t size, umem_cache_t *cp)
{
	int match = 0;
	uint64_t my_gen;

	/*
	 * B4: never stop the server thread.  It is the only thread that can
	 * process "continue", so parking it is unrecoverable.
	 */
	if (brk_server_valid && pthread_equal(pthread_self(), brk_server_thread))
		return;

	(void) pthread_mutex_lock(&brk_lock);

	/* Re-check under the lock: armed is only a hint (B2). */
	if (!umem_introspect_break_armed) {
		(void) pthread_mutex_unlock(&brk_lock);
		return;
	}

	switch (brk_kind) {
	case BRK_SIZE:
		match = (size == brk_size);
		break;
	case BRK_CACHE:
		match = (cp != NULL &&
		    strcmp(cp->cache_name, brk_cache) == 0);
		break;
	case BRK_SEQ:
		match = (++brk_seq_counter == brk_seq);
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

	if (!match) {
		(void) pthread_mutex_unlock(&brk_lock);
		return;
	}

	/*
	 * Stop until the generation advances (B3).  Sampling the generation
	 * under the same lock acquisition that decided the match is what
	 * makes this race-free: a "continue" processed before we got here has
	 * already disarmed the predicate, so we could not have matched.
	 */
	my_gen = brk_generation;
	brk_stopped++;
	log_message("umem: BREAK: thread stopped before returning "
	    "buf=%p size=%zu cache=%s (umemctl continue to resume)\n",
	    buf, size, cp ? cp->cache_name : "?");
	while (brk_generation == my_gen)
		(void) pthread_cond_wait(&brk_cv, &brk_lock);
	brk_stopped--;

	(void) pthread_mutex_unlock(&brk_lock);
}

/*
 * Fork child reset (B5).  Called from umem_fork.c's child handler through a
 * weak hook, the same mechanism the malloc interposer uses.
 *
 * The child inherits an armed predicate and a completed pthread_once, but NOT
 * the server thread.  Without this, an armed child stops on its next matching
 * allocation with nothing alive to resume it, and could not even start a new
 * server because the once-control was already satisfied.
 *
 * Runs single-threaded in the child, after umem_fork.c has released the
 * allocator locks.
 */
void
umem_introspect_fork_child(void)
{
	(void) pthread_mutex_init(&brk_lock, NULL);
	(void) pthread_cond_init(&brk_cv, NULL);

	break_disarm_locked();	/* single-threaded here; no lock needed */
	brk_generation = 0;
	brk_seq_counter = 0;
	brk_stopped = 0;
	brk_server_valid = 0;

	/* Let the child start its own server thread if it asks to. */
	introspect_once_reset();
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
	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		s->slab_create += cp->cache_slab_create;
		s->slab_destroy += cp->cache_slab_destroy;
		s->inuse += cp->cache_slab_alloc - cp->cache_slab_free;
	}
	(void) mutex_unlock(&umem_cache_lock);
}

/* logtail / record: stream deltas until the client disconnects. The stream
 * is derived by polling counters the allocator already maintains, so it adds
 * no hot-path cost (no alloc/free hook). ponytail: 5 Hz poll -- fine for a
 * log tail; hook slab_create/destroy directly only if sub-poll latency
 * matters.
 *
 * DISCONNECT DETECTION.  An idle logtail writes nothing, so a vanished client
 * used to go unnoticed indefinitely -- and because one client owns the server
 * until it disconnects, that wedged the whole channel.  Each poll now also
 * checks for EOF on the socket, which is readable-with-zero-bytes once the
 * peer is gone, so an idle logtail notices a dead client within one tick. */
static void
cmd_logtail(FILE *out)
{
	struct logsnap prev, cur;
	int fd = fileno(out);

	snapshot(&prev);
	fprintf(out, "logtail start pid=%ld\n", (long)getpid());
	fflush(out);
	for (;;) {
		usleep(200 * 1000);	/* ~5 Hz poll */

		/* Client gone?  POLLHUP/POLLERR, or readable-at-EOF. */
		if (fd >= 0) {
			struct pollfd pfd;
			pfd.fd = fd;
			pfd.events = POLLIN;
			pfd.revents = 0;
			if (poll(&pfd, 1, 0) > 0) {
				if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
					break;
				if (pfd.revents & POLLIN) {
					char c;
					ssize_t r = recv(fd, &c, 1,
					    MSG_PEEK | MSG_DONTWAIT);
					if (r == 0)
						break;	/* orderly EOF */
				}
			}
		}

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
			fprintf(out, "held %llu (%+lld)\n",
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

/*
 * ---- socket location (P5.6) ----
 *
 * The path used to be /tmp/umem.<pid>.sock: a PREDICTABLE name in a
 * world-writable sticky directory.  The pid is guessable and the target has
 * not bound yet at the moment it matters, so another user on the box can
 * create that path first.  Two things follow, both demonstrated in
 * test/security/test_introspect_sock_path.sh:
 *
 *   - the target then finds the address in use.  If the squatter is
 *     listening, the target refuses to bind (correctly -- A3) and has no
 *     channel at all, while `umemctl <pid> ...` resolves the SAME predictable
 *     path and talks to the squatter: the operator's commands go to the
 *     attacker and the attacker's answers come back as if they were the
 *     allocator's.
 *   - if the squatter is not listening, the target treated the path as its
 *     own stale socket and removed that directory entry -- an entry it did
 *     not create.
 *
 * FIX: put the socket in a directory only this euid can write to, so the
 * path cannot be pre-created by anyone else:
 *
 *   1. $XDG_RUNTIME_DIR, if it is a real directory (not a symlink), owned by
 *      geteuid(), with no group/other permissions.  This is what the variable
 *      is for and systemd already guarantees those properties.
 *   2. otherwise /tmp/umem-<euid>, which the library creates itself with
 *      mkdir(0700).  mkdir() is atomic and fails if ANYTHING is already
 *      there, including a symlink, so a squatter cannot win by pre-creating
 *      it; if it already exists it is accepted only after the same
 *      ownership/permission check as (1).
 *
 * The name inside that directory stays umem.<pid>.sock -- predictable is
 * fine once nobody else can create entries in the directory.
 *
 * UMEM_INTROSPECT_SOCK still overrides, because a developer pointing the
 * channel at a scratch path is the reason it exists -- but NOT in secure
 * mode, where the environment is chosen by a less privileged party.  (In
 * secure mode envvar.c refuses introspect=1 outright, so this is the second
 * of two gates, not the only one.)
 */

/*
 * Is dfd a directory we can trust to hold the control socket?  Owned by this
 * euid, and not writable (or even readable) by group or other.
 */
static int
dir_is_private(int dfd)
{
	struct stat sb;

	if (fstat(dfd, &sb) != 0)
		return (0);
	if (!S_ISDIR(sb.st_mode))
		return (0);
	if (sb.st_uid != geteuid())
		return (0);
	return ((sb.st_mode & (S_IRWXG | S_IRWXO)) == 0);
}

/* Open dir for the checks above without ever following a symlink. */
static int
open_private_dir(const char *dir)
{
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW
#ifdef O_CLOEXEC
	    | O_CLOEXEC
#endif
	    );

	if (dfd < 0)
		return (-1);
	if (!dir_is_private(dfd)) {
		(void) close(dfd);
		return (-1);
	}
	return (dfd);
}

/*
 * Pick the directory for the socket.  Returns 0 and fills dir[] on success.
 * Never trusts a path it has not just validated.
 */
static int
sock_dir(char *dir, size_t n)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	int dfd;

	/*
	 * XDG_RUNTIME_DIR comes from the environment, so it is only used
	 * after the ownership/permission check, and never in secure mode.
	 */
	if (!umem_secure_mode() && xdg != NULL && xdg[0] == '/') {
		if ((size_t)snprintf(dir, n, "%s", xdg) < n) {
			if ((dfd = open_private_dir(dir)) >= 0) {
				(void) close(dfd);
				return (0);
			}
		}
	}

	if ((size_t)snprintf(dir, n, "/tmp/umem-%ld", (long)geteuid()) >= n)
		return (-1);

	/*
	 * mkdir(0700) is the whole defence here: it is atomic, and it fails
	 * with EEXIST for a pre-created directory OR a pre-created symlink,
	 * neither of which we then accept without checking.  umask cannot
	 * loosen the mode below because we verify it afterwards.
	 */
	if (mkdir(dir, S_IRWXU) != 0 && errno != EEXIST)
		return (-1);
	if ((dfd = open_private_dir(dir)) < 0) {
		log_message("umem: introspect: %s is not a private directory "
		    "owned by uid %ld; not opening a control socket\n",
		    dir, (long)geteuid());
		return (-1);
	}
	(void) close(dfd);
	return (0);
}

/*
 * Fill buf with the control socket path.  Returns buf, or NULL if no safe
 * path is available (the caller then does not start a server).
 */
static const char *
sock_path(char *buf, size_t n)
{
	const char *env = getenv("UMEM_INTROSPECT_SOCK");
	char dir[96];

	if (env != NULL && env[0] != '\0' && !umem_secure_mode()) {
		/* Developer override: their path, their directory, their call. */
		if ((size_t)snprintf(buf, n, "%s", env) >= n)
			return (NULL);	/* would be silently truncated */
		return (buf);
	}

	if (sock_dir(dir, sizeof (dir)) != 0)
		return (NULL);
	if ((size_t)snprintf(buf, n, "%s/umem.%ld.sock", dir,
	    (long)getpid()) >= n)
		return (NULL);
	return (buf);
}

/*
 * AUTHORIZATION (Phase 3 item 4).
 *
 * A1. The socket is created with mode 0600 explicitly, via a umask that is
 *     set around bind() rather than inherited.  The control channel can arm
 *     break predicates, i.e. STOP THE TARGET PROCESS, so an inherited 0022
 *     umask leaving it group/other-readable is a real exposure.
 *
 * A2. Every connection's peer credentials are checked with SO_PEERCRED: only
 *     the EFFECTIVE uid, or root, is served.  File permissions alone are not
 *     enough -- the path may live on a filesystem that ignores them, and the
 *     socket may be inherited.
 *
 * A3. The stale path is never unlink()ed.  The sequence used to be
 *     stat -> probe connect -> unlink -> bind, which acts on a path after
 *     looking at it, and looked at it with stat(2) -- which FOLLOWS symlinks,
 *     so a symlink pointing at somebody else's socket read back as "my own
 *     stale socket" and its directory entry was removed.  See
 *     umem_introspect_peer_authorized() and bind_control_socket() below.
 */

/*
 * P5.7: the peer-authorization decision, factored out so it can be tested
 * without a setuid binary (see test/security/test_introspect_peer_uid.c).
 *
 * It used to accept `peer == getuid() || peer == geteuid() || peer == 0`.
 * The REAL uid has no business here.  In a setuid target the real uid is the
 * unprivileged invoker, so accepting it handed that invoker the whole control
 * channel: whatis/bufctl read process memory at addresses the client chooses,
 * and `break` parks allocating threads until a `continue` that need never
 * come -- a DoS of the privileged process from an unprivileged account.
 *
 * euid is the identity the process actually acts with, so euid is the identity
 * that may drive it.  root is kept: root can ptrace the process anyway, so
 * refusing it buys nothing.
 */
int
umem_introspect_peer_authorized(uid_t peer, uid_t euid)
{
	return (peer == euid || peer == 0);
}

static int
peer_is_authorized(int cfd)
{
#ifdef SO_PEERCRED
	struct ucred cred;
	socklen_t len = sizeof (cred);

	if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
		return (0);	/* cannot verify -> refuse */
	if (umem_introspect_peer_authorized(cred.uid, geteuid()))
		return (1);
	log_message("umem: introspect: refused connection from uid %ld\n",
	    (long)cred.uid);
	return (0);
#else
	/*
	 * No peer-credential primitive on this platform.  The 0600 socket mode
	 * is then the only control; say so rather than pretending otherwise.
	 */
	(void) cfd;
	return (1);
#endif
}

/*
 * Reclaim a stale socket path WITHOUT unlinking it (A3, P5.6).
 *
 * Called only after bind() returned EADDRINUSE and the path was found to be
 * a socket (by lstat, so a symlink is not mistaken for one) that nothing is
 * listening on.  Rather than unlink() the caller's path and bind again -- a
 * TOCTOU, because between the look and the unlink the entry can be replaced
 * -- bind a unique name in the SAME directory and rename() it over the path.
 *
 * rename(2) is atomic and never follows a symlink at either end, so the worst
 * an attacker who swaps the path in the window achieves is having their own
 * entry replaced.  No file outside our own directory entry is ever removed,
 * whatever the path turns into mid-sequence.
 *
 * Returns 0 on success.  On failure the temporary entry is cleaned up; that
 * unlink is safe because the name is ours and was just created by us.
 */
static int
rebind_over_stale(int lfd, const char *path)
{
	struct sockaddr_un tmpaddr;
	char tmp[sizeof (tmpaddr.sun_path)];
	const char *slash = strrchr(path, '/');
	size_t dirlen = (slash != NULL) ? (size_t)(slash - path) + 1 : 0;

	/* Same directory, so rename() cannot fail with EXDEV. */
	if ((size_t)snprintf(tmp, sizeof (tmp), "%.*sumem.%ld.tmp",
	    (int)dirlen, path, (long)getpid()) >= sizeof (tmp))
		return (-1);

	memset(&tmpaddr, 0, sizeof (tmpaddr));
	tmpaddr.sun_family = AF_UNIX;
	(void) snprintf(tmpaddr.sun_path, sizeof (tmpaddr.sun_path), "%s", tmp);

	/* A leftover of ours from a previous run; our name, safe to remove. */
	(void) unlink(tmp);
	if (bind(lfd, (struct sockaddr *)&tmpaddr, sizeof (tmpaddr)) != 0)
		return (-1);
	if (rename(tmp, path) != 0) {
		(void) unlink(tmp);
		return (-1);
	}
	return (0);
}

/* Bind, reclaiming only a socket path that nothing is listening on (A3). */
static int
bind_control_socket(int lfd, const char *path)
{
	struct sockaddr_un addr;
	mode_t old;
	int rc;

	memset(&addr, 0, sizeof (addr));
	addr.sun_family = AF_UNIX;
	/* Truncation would bind a DIFFERENT path than the caller asked for. */
	if ((size_t)snprintf(addr.sun_path, sizeof (addr.sun_path), "%s",
	    path) >= sizeof (addr.sun_path)) {
		errno = ENAMETOOLONG;
		return (-1);
	}

	/* 0600 regardless of the inherited umask (A1). */
	old = umask(077);
	rc = bind(lfd, (struct sockaddr *)&addr, sizeof (addr));
	if (rc < 0 && errno == EADDRINUSE) {
		/*
		 * Something is at the path.  Only reclaim it if it is a socket
		 * with no listener -- i.e. a leftover from a dead process.
		 *
		 * lstat, NOT stat: stat follows symlinks, so a symlink aimed at
		 * somebody else's socket used to read back as "my own stale
		 * socket" and get its directory entry removed.  A symlink is
		 * never a socket this process left behind.
		 */
		struct stat sb;
		int probe;
		if (lstat(path, &sb) == 0 && S_ISSOCK(sb.st_mode) &&
		    (probe = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0) {
			int live = (connect(probe,
			    (struct sockaddr *)&addr, sizeof (addr)) == 0);
			(void) close(probe);
			if (!live)
				rc = rebind_over_stale(lfd, path);
			else
				log_message("umem: introspect: %s already "
				    "has a live server; not replacing it\n",
				    path);
		} else {
			log_message("umem: introspect: %s exists and is not a "
			    "stale socket of ours; not replacing it\n", path);
		}
	}
	(void) umask(old);

	/* Belt and braces: bind() honours the umask, chmod states the intent
	 * and fixes up any platform that does not. */
	if (rc == 0)
		(void) chmod(path, S_IRUSR | S_IWUSR);
	return (rc);
}

static void *
introspect_thread(void *unused)
{
	char path[108];
	int lfd;

	(void) unused;

	/*
	 * SIGPIPE (Phase 3 item 4).  Responses go out through stdio on a
	 * socket; a client that disconnects mid-response makes the write raise
	 * SIGPIPE, whose default action TERMINATES THE TARGET PROCESS.  A
	 * debugging channel must never be able to kill the program it is
	 * inspecting.
	 *
	 * Blocked per-thread rather than SIG_IGN'd process-wide: the
	 * disposition is shared state that belongs to the application, and
	 * libumem must not change it behind the application's back.  Blocking
	 * is thread-local, so writes here fail with EPIPE while the
	 * application's own SIGPIPE handling is untouched.
	 */
	{
		sigset_t pipeset;
		(void) sigemptyset(&pipeset);
		(void) sigaddset(&pipeset, SIGPIPE);
		(void) pthread_sigmask(SIG_BLOCK, &pipeset, NULL);
	}

	/* Record identity so break_check never parks this thread (B4). */
	brk_server_thread = pthread_self();
	brk_server_valid = 1;

	if (sock_path(path, sizeof (path)) == NULL) {
		log_message("umem: introspect: no safe socket path; "
		    "control channel not started\n");
		brk_server_valid = 0;
		return (NULL);
	}

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0)
		return (NULL);
	if (bind_control_socket(lfd, path) < 0 || listen(lfd, 4) < 0) {
		close(lfd);
		return (NULL);
	}
	log_message("umem: introspect socket at %s (mode 0600, same-euid "
	    "peers only)\n", path);

	/*
	 * CONCURRENCY LIMIT, stated honestly: ONE CLIENT AT A TIME.  A
	 * connection is served to completion before the next is accepted, so a
	 * long-lived stream (logtail/record) blocks every other command
	 * INCLUDING "continue".  Do not hold a logtail open on the same socket
	 * you intend to resume a break with -- use a second, sequential
	 * connection, or stop the logtail first.
	 *
	 * ponytail: serial accept loop.  A per-client thread is the upgrade
	 * path, and needs the break engine's single-predicate model revisited
	 * first (two clients arming different predicates is undefined today).
	 */
	for (;;) {
		int cfd = accept(lfd, NULL, NULL);
		FILE *out;
		char line[256];
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (!peer_is_authorized(cfd)) {
			close(cfd);
			continue;
		}
		out = fdopen(cfd, "r+");
		if (out == NULL) {
			close(cfd);
			continue;
		}
		while (fgets(line, sizeof (line), out) != NULL) {
			handle_line(out, line);
			/* Client vanished mid-conversation: with SIGPIPE
			 * blocked the write failed with EPIPE instead of
			 * killing us.  Drop the connection. */
			if (ferror(out))
				break;
		}
		fclose(out);
	}
	close(lfd);
	(void) unlink(path);
	brk_server_valid = 0;
	return (NULL);
}

static pthread_once_t introspect_once = PTHREAD_ONCE_INIT;

/* Re-arm the once-control in a fork child (B5): the child inherited a
 * SATISFIED once with no server thread to show for it. */
static void
introspect_once_reset(void)
{
	static const pthread_once_t fresh = PTHREAD_ONCE_INIT;
	introspect_once = fresh;
}

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

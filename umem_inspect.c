/*
 * umem_inspect.c -- runtime introspection for libumem.
 *
 * Implements the dcmds documented in umem_inspect.h.  Everything here
 * walks data structures that the allocator already maintains when
 * UMEM_DEBUG is set; nothing new is recorded on the hot path.
 *
 * Algorithmic structure mirrors mdb's ::findleaks dcmd:
 *
 *   1. Walk the circular cache list rooted at umem_null_cache.
 *   2. For each cache that has UMF_HASH, walk cache_hash_table and
 *      enumerate live (allocated) bufctls.  For non-hashed caches walk
 *      every slab and derive the allocated set by subtracting bufctls
 *      on slab_head from the slab's buffer range.
 *   3. If the cache has UMF_AUDIT, each bufctl is actually a
 *      umem_bufctl_audit_t with a PC stack; fingerprint by hashing the
 *      first N frames and bucket the allocation into a leak class.
 *   4. Sort classes by total bytes and emit a report.
 *
 * Transaction log walk:
 *
 *   umem_transaction_log is a umem_log_header_t with per-CPU chunks
 *   packed with fixed-size audit records.  We sweep every chunk in
 *   UMEM_BUFCTL_AUDIT_SIZE strides, filter by plausibility (addr
 *   non-null, cache pointer matches a known cache), and sort by
 *   bc_timestamp to present a chronological timeline.
 */

#include "config.h"
#include "umem_impl.h"
#include "umem_inspect.h"
#include "umem_stacktrace.h"
#include "misc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#ifndef UMC_QCACHE
#define UMC_QCACHE	0x00100000
#endif

/* ------------------------------------------------------------------------
 * Access to allocator globals we need.
 * ------------------------------------------------------------------------ */

extern umem_cache_t umem_null_cache;
extern umem_log_header_t *umem_transaction_log;
extern uint32_t umem_max_ncpus;
extern uint32_t umem_stack_depth;

/* ------------------------------------------------------------------------
 * Event hooks.  These are intentionally tiny; the whole point is to be
 * a stable symbol a debugger can set a breakpoint on.  Keep the
 * function bodies trivial so the linker doesn't constant-fold them and
 * so `call` from a debugger always resolves.
 * ------------------------------------------------------------------------ */

static atomic_int umem_inspect_events_enabled;
static _Atomic(umem_event_cb_t) umem_inspect_event_cb;
static void *umem_inspect_event_cb_arg;
static atomic_uint umem_inspect_event_mask = 0xffffffffu;

__attribute__((noinline, used))
void
umem_event_alloc(void *buf, size_t size, void *cache)
{
	(void) buf; (void) size; (void) cache;
	__asm__ volatile ("" ::: "memory");
}

__attribute__((noinline, used))
void
umem_event_free(void *buf, size_t size, void *cache)
{
	(void) buf; (void) size; (void) cache;
	__asm__ volatile ("" ::: "memory");
}

__attribute__((noinline, used))
void
umem_event_error(int code, void *buf, void *cache)
{
	(void) code; (void) buf; (void) cache;
	__asm__ volatile ("" ::: "memory");
}

void
umem_inspect_enable_events(int on)
{
	atomic_store(&umem_inspect_events_enabled, on ? 1 : 0);
}

void
umem_inspect_set_event_cb(umem_event_cb_t cb, void *arg, unsigned event_mask)
{
	umem_inspect_event_cb_arg = arg;
	atomic_store(&umem_inspect_event_mask, event_mask);
	atomic_store(&umem_inspect_event_cb, cb);
}

/* Internal fan-out called from umem.c hot paths (one cache-miss callout).
 * Kept out of the header so external code can't depend on it. */
void
umem_inspect_notify(umem_event_t ev, const umem_buffer_info_t *info)
{
	if (!atomic_load(&umem_inspect_events_enabled))
		return;

	switch (ev) {
	case UMEM_EV_ALLOC:
		umem_event_alloc(info->addr, info->size, info->cache);
		break;
	case UMEM_EV_FREE:
		umem_event_free(info->addr, info->size, info->cache);
		break;
	case UMEM_EV_CORRUPT:
	case UMEM_EV_DOUBLE_FREE:
		umem_event_error(ev, info->addr, info->cache);
		break;
	default:
		break;
	}

	unsigned mask = atomic_load(&umem_inspect_event_mask);
	if ((mask & (1u << ev)) == 0)
		return;

	umem_event_cb_t cb = atomic_load(&umem_inspect_event_cb);
	if (cb != NULL)
		cb(ev, info, umem_inspect_event_cb_arg);
}

/* ------------------------------------------------------------------------
 * Helpers: cache list walking.
 * ------------------------------------------------------------------------ */

typedef void (*cache_visitor_t)(umem_cache_t *cp, void *arg);

static void
for_each_cache(cache_visitor_t v, void *arg)
{
	umem_cache_t *cp = umem_null_cache.cache_next;
	unsigned safety = 0;

	while (cp != &umem_null_cache && safety++ < 65536) {
		v(cp, arg);
		cp = cp->cache_next;
	}
}

/* Size of a persisted audit record (fixed once umem_stack_depth is
 * frozen; that happens during umem_init()). */
static size_t
audit_record_size(void)
{
	return ((size_t)(&((umem_bufctl_audit_t *)0)->bc_stack[umem_stack_depth]));
}

static int
cache_has_audit(const umem_cache_t *cp)
{
	/*
	 * Every UMF_AUDIT cache allocates umem_bufctl_audit_t (not plain
	 * umem_bufctl_t) and fills the audit fields on every alloc/free.
	 * UMF_BUFTAG changes which code path writes the fields and how
	 * the bufctl is reached, but the shape of the struct is the same.
	 */
	return ((cp->cache_flags & UMF_AUDIT) != 0);
}

/* True if buf is on the slab's freelist. */
static int
slab_buf_is_free(umem_slab_t *sp, void *buf, umem_cache_t *cp)
{
	umem_bufctl_t *bcp = sp->slab_head;
	unsigned safety = 0;
	size_t chunksize = cp->cache_chunksize;

	while (bcp != NULL && safety++ < 1u << 20) {
		void *b;
		if (cp->cache_flags & UMF_HASH)
			b = bcp->bc_addr;
		else
			b = (char *)bcp - cp->cache_bufctl;
		if (b == buf)
			return (1);
		bcp = bcp->bc_next;
		(void)chunksize;
	}
	return (0);
}

/* Fill an info record from a bufctl that the cache believes to be live.
 * Returns 1 if the entry looks coherent, 0 otherwise. */
static int
info_from_bufctl(umem_cache_t *cp, umem_bufctl_t *bcp,
    umem_buffer_info_t *info, int state)
{
	memset(info, 0, sizeof (*info));
	info->cache = cp;
	info->cache_name = cp->cache_name;
	info->size = cp->cache_bufsize;
	info->state = state;
	info->bufctl = bcp;
	info->slab = bcp->bc_slab;

	if (cp->cache_flags & UMF_HASH)
		info->addr = bcp->bc_addr;
	else
		info->addr = (char *)bcp - cp->cache_bufctl;

	if (cache_has_audit(cp)) {
		umem_bufctl_audit_t *bcap = (umem_bufctl_audit_t *)bcp;
		info->timestamp = (uint64_t)bcap->bc_timestamp;
		info->thread = (uint64_t)bcap->bc_thread;
		info->depth = bcap->bc_depth;
		if (info->depth < 0)
			info->depth = 0;
		if (info->depth > UMEM_INSPECT_MAX_STACK)
			info->depth = UMEM_INSPECT_MAX_STACK;
		if (info->depth > (int)umem_stack_depth)
			info->depth = (int)umem_stack_depth;
		for (int i = 0; i < info->depth; i++)
			info->stack[i] = bcap->bc_stack[i];
	}
	return (1);
}

/* ------------------------------------------------------------------------
 * Public walkers.
 * ------------------------------------------------------------------------ */

struct walk_ctx {
	umem_buffer_cb_t cb;
	void *arg;
	size_t count;
	int stop;
};

static void
walk_allocated_cache(umem_cache_t *cp, void *arg)
{
	struct walk_ctx *ctx = arg;
	umem_buffer_info_t info;

	if (ctx->stop)
		return;

	(void) mutex_lock(&cp->cache_lock);

	if (cp->cache_flags & UMF_HASH) {
		size_t buckets = cp->cache_hash_mask + 1;
		if (cp->cache_hash_table == NULL || buckets == 0 ||
		    buckets > (1ull << 28)) {
			(void) mutex_unlock(&cp->cache_lock);
			return;
		}
		for (size_t i = 0; i < buckets; i++) {
			umem_bufctl_t *bcp = cp->cache_hash_table[i];
			unsigned safety = 0;
			while (bcp != NULL && safety++ < (1u << 24)) {
				if (info_from_bufctl(cp, bcp, &info,
				    UMEM_BUF_ALLOCATED)) {
					ctx->count++;
					if (ctx->cb(&info, ctx->arg) != 0) {
						ctx->stop = 1;
						goto done;
					}
				}
				bcp = bcp->bc_next;
			}
		}
	} else {
		/* Non-hashed: walk slabs, enumerate buffers, skip freelist. */
		umem_slab_t *sp;
		for (sp = cp->cache_nullslab.slab_next;
		    sp != &cp->cache_nullslab;
		    sp = sp->slab_next) {
			char *base = sp->slab_base;
			size_t stride = cp->cache_chunksize;
			long chunks = sp->slab_chunks;
			for (long j = 0; j < chunks; j++) {
				void *buf = base + j * stride;
				if (slab_buf_is_free(sp, buf, cp))
					continue;
				memset(&info, 0, sizeof (info));
				info.addr = buf;
				info.size = cp->cache_bufsize;
				info.cache = cp;
				info.cache_name = cp->cache_name;
				info.slab = sp;
				info.state = UMEM_BUF_ALLOCATED;
				ctx->count++;
				if (ctx->cb(&info, ctx->arg) != 0) {
					ctx->stop = 1;
					goto done;
				}
			}
		}
	}

done:
	(void) mutex_unlock(&cp->cache_lock);
}

size_t
umem_walk_allocated(umem_buffer_cb_t cb, void *arg)
{
	struct walk_ctx ctx = { cb, arg, 0, 0 };
	for_each_cache(walk_allocated_cache, &ctx);
	return (ctx.count);
}

static void
walk_freed_cache(umem_cache_t *cp, void *arg)
{
	struct walk_ctx *ctx = arg;
	umem_buffer_info_t info;
	umem_slab_t *sp;

	if (ctx->stop)
		return;

	(void) mutex_lock(&cp->cache_lock);
	for (sp = cp->cache_nullslab.slab_next;
	    sp != &cp->cache_nullslab;
	    sp = sp->slab_next) {
		umem_bufctl_t *bcp = sp->slab_head;
		unsigned safety = 0;
		while (bcp != NULL && safety++ < (1u << 20)) {
			info_from_bufctl(cp, bcp, &info, UMEM_BUF_FREE);
			ctx->count++;
			if (ctx->cb(&info, ctx->arg) != 0) {
				ctx->stop = 1;
				goto done;
			}
			bcp = bcp->bc_next;
		}
	}
done:
	(void) mutex_unlock(&cp->cache_lock);
}

size_t
umem_walk_freed(umem_buffer_cb_t cb, void *arg)
{
	struct walk_ctx ctx = { cb, arg, 0, 0 };
	for_each_cache(walk_freed_cache, &ctx);
	return (ctx.count);
}

/* ------------------------------------------------------------------------
 * Transaction log walk.
 *
 * The log is a set of chunks; each chunk holds a dense sequence of
 * audit records.  We don't have a write head per-chunk exposed so we
 * scan the whole chunk in stride and filter by "looks like a valid
 * audit record" (non-NULL addr and cache pointer that matches a known
 * cache).  This is what mdb's ::umem_logs does.
 * ------------------------------------------------------------------------ */

static int
is_known_cache(umem_cache_t *candidate)
{
	umem_cache_t *cp = umem_null_cache.cache_next;
	unsigned safety = 0;
	while (cp != &umem_null_cache && safety++ < 65536) {
		if (cp == candidate)
			return (1);
		cp = cp->cache_next;
	}
	return (0);
}

size_t
umem_walk_log(umem_buffer_cb_t cb, void *arg)
{
	umem_log_header_t *lhp = umem_transaction_log;
	size_t rec_sz = audit_record_size();
	size_t visited = 0;

	if (lhp == NULL || rec_sz == 0)
		return (0);

	(void) mutex_lock(&lhp->lh_lock);

	size_t chunksize = lhp->lh_chunksize;
	char *base = lhp->lh_base;
	int nchunks = lhp->lh_nchunks;

	if (base == NULL || chunksize == 0 || nchunks <= 0) {
		(void) mutex_unlock(&lhp->lh_lock);
		return (0);
	}

	for (int c = 0; c < nchunks; c++) {
		char *chunk = base + (size_t)c * chunksize;
		for (size_t off = 0; off + rec_sz <= chunksize; off += rec_sz) {
			umem_bufctl_audit_t *rec =
			    (umem_bufctl_audit_t *)(chunk + off);
			if (rec->bc_addr == NULL)
				continue;
			if (rec->bc_cache == NULL)
				continue;
			if (!is_known_cache(rec->bc_cache))
				continue;
			if (rec->bc_timestamp == 0)
				continue;

			umem_buffer_info_t info;
			memset(&info, 0, sizeof (info));
			info.addr = rec->bc_addr;
			info.cache = rec->bc_cache;
			info.cache_name = rec->bc_cache->cache_name;
			info.size = rec->bc_cache->cache_bufsize;
			info.slab = rec->bc_slab;
			info.bufctl = rec;
			info.state = UMEM_BUF_UNKNOWN;
			info.timestamp = (uint64_t)rec->bc_timestamp;
			info.thread = (uint64_t)rec->bc_thread;
			info.depth = rec->bc_depth;
			if (info.depth < 0)
				info.depth = 0;
			if (info.depth > UMEM_INSPECT_MAX_STACK)
				info.depth = UMEM_INSPECT_MAX_STACK;
			if (info.depth > (int)umem_stack_depth)
				info.depth = (int)umem_stack_depth;
			for (int i = 0; i < info.depth; i++)
				info.stack[i] = rec->bc_stack[i];

			visited++;
			if (cb(&info, arg) != 0) {
				(void) mutex_unlock(&lhp->lh_lock);
				return (visited);
			}
		}
	}

	(void) mutex_unlock(&lhp->lh_lock);
	return (visited);
}

/* ------------------------------------------------------------------------
 * Output helpers.
 * ------------------------------------------------------------------------ */

static void
print_stack_text(FILE *out, const umem_buffer_info_t *info, const char *indent)
{
	char buf[512];
	for (int i = 0; i < info->depth; i++) {
		umem_stacktrace_format(info->stack[i], i, buf, sizeof (buf));
		(void) fprintf(out, "%s%s\n", indent, buf);
	}
}

static void
print_stack_json(FILE *out, const umem_buffer_info_t *info)
{
	(void) fputs("[", out);
	for (int i = 0; i < info->depth; i++) {
		(void) fprintf(out, "%s\"0x%" PRIxPTR "\"",
		    i == 0 ? "" : ",", info->stack[i]);
	}
	(void) fputs("]", out);
}

static void
json_escape(FILE *out, const char *s)
{
	(void) fputc('"', out);
	for (; s != NULL && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			(void) fprintf(out, "\\%c", c);
		else if (c < 0x20)
			(void) fprintf(out, "\\u%04x", c);
		else
			(void) fputc(c, out);
	}
	(void) fputc('"', out);
}

/* ------------------------------------------------------------------------
 * Cached-buffer set: every buffer currently sitting in a magazine
 * (loaded, previous, or depot) is logically free from the user's POV
 * but still appears in the cache's hash table because slab_free has
 * not run on it.  findleaks excludes these from its leak count.
 * ------------------------------------------------------------------------ */

#define CACHED_BUCKETS	8192

struct cached_set {
	struct cached_node {
		void *addr;
		struct cached_node *next;
	} *buckets[CACHED_BUCKETS];
	size_t count;
};

static void
cached_set_add(struct cached_set *cs, void *addr)
{
	if (addr == NULL)
		return;
	size_t b = ((uintptr_t)addr * 11400714819323198485ULL) >> 51;
	b &= (CACHED_BUCKETS - 1);
	for (struct cached_node *n = cs->buckets[b]; n != NULL; n = n->next)
		if (n->addr == addr)
			return;
	struct cached_node *n = calloc(1, sizeof (*n));
	if (n == NULL)
		return;
	n->addr = addr;
	n->next = cs->buckets[b];
	cs->buckets[b] = n;
	cs->count++;
}

static int
cached_set_contains(const struct cached_set *cs, void *addr)
{
	if (addr == NULL)
		return (0);
	size_t b = ((uintptr_t)addr * 11400714819323198485ULL) >> 51;
	b &= (CACHED_BUCKETS - 1);
	for (struct cached_node *n = cs->buckets[b]; n != NULL; n = n->next)
		if (n->addr == addr)
			return (1);
	return (0);
}

static void
cached_set_destroy(struct cached_set *cs)
{
	for (size_t i = 0; i < CACHED_BUCKETS; i++) {
		struct cached_node *n = cs->buckets[i];
		while (n != NULL) {
			struct cached_node *next = n->next;
			free(n);
			n = next;
		}
		cs->buckets[i] = NULL;
	}
	cs->count = 0;
}

/*
 * Walk a magazine list and add up to `cap` rounds per magazine.  The
 * mag_round[] array is indexed [0..rounds-1] for the rounds in use.
 */
static void
cached_set_add_maglist(struct cached_set *cs, umem_magazine_t *mp, int cap,
    unsigned safety_max)
{
	unsigned safety = 0;
	while (mp != NULL && safety++ < safety_max) {
		for (int r = 0; r < cap; r++)
			cached_set_add(cs, mp->mag_round[r]);
		mp = (umem_magazine_t *)mp->mag_next;
	}
}

/*
 * Build the cached set for a single cache: depot full mags + per-CPU
 * loaded/previous + per-CPU depot full mags.
 */
static void
cached_set_build_cache(struct cached_set *cs, umem_cache_t *cp)
{
	if (cp->cache_magtype == NULL)
		return;
	int magsize = cp->cache_magtype->mt_magsize;
	if (magsize <= 0)
		return;

	(void) mutex_lock(&cp->cache_lock);

	/* Central depot: full magazines hold magsize rounds each. */
	cached_set_add_maglist(cs, cp->cache_full.ml_list, magsize, 1u << 20);

	/* Per-CPU depot arrays. */
	if (cp->cache_depot_full != NULL && cp->cache_depot_ncpus > 0) {
		for (int i = 0; i < cp->cache_depot_ncpus; i++) {
			cached_set_add_maglist(cs,
			    cp->cache_depot_full[i].ml_list, magsize,
			    1u << 20);
		}
	}

	/* Per-CPU loaded/previous magazines. */
	for (uint32_t cpu = 0; cpu <= cp->cache_cpu_mask; cpu++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[cpu];
		if (ccp->cc_loaded != NULL && ccp->cc_rounds > 0) {
			int r = ccp->cc_rounds;
			if (r > magsize) r = magsize;
			for (int i = 0; i < r; i++)
				cached_set_add(cs,
				    ccp->cc_loaded->mag_round[i]);
		}
		if (ccp->cc_ploaded != NULL && ccp->cc_prounds > 0) {
			int r = ccp->cc_prounds;
			if (r > magsize) r = magsize;
			for (int i = 0; i < r; i++)
				cached_set_add(cs,
				    ccp->cc_ploaded->mag_round[i]);
		}
	}

	(void) mutex_unlock(&cp->cache_lock);
}

static void
cached_set_build_visit(umem_cache_t *cp, void *arg)
{
	cached_set_build_cache((struct cached_set *)arg, cp);
}

static void
cached_set_build_all(struct cached_set *cs)
{
	for_each_cache(cached_set_build_visit, cs);
}

/* ------------------------------------------------------------------------
 * findleaks: group by stack fingerprint, rank by bytes.
 * ------------------------------------------------------------------------ */

struct leak_class {
	uint64_t fingerprint;
	size_t count;
	size_t bytes;
	const char *cache_name;
	size_t bufsize;
	int depth;
	uintptr_t stack[UMEM_INSPECT_MAX_STACK];
	void *sample_addr;
	struct leak_class *next;
};

#define	LEAK_BUCKETS	1024

struct leak_state {
	struct leak_class *buckets[LEAK_BUCKETS];
	size_t nclasses;
	size_t total_count;
	size_t total_bytes;
};

static uint64_t
fnv1a(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < len; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return (h);
}

static int
leak_classify(const umem_buffer_info_t *info, void *arg)
{
	struct leak_state *st = arg;

	uint64_t fp;
	if (info->depth > 0) {
		fp = fnv1a(info->stack,
		    sizeof (info->stack[0]) * (size_t)info->depth);
	} else {
		fp = fnv1a(info->cache_name,
		    info->cache_name ? strlen(info->cache_name) : 0);
	}

	size_t bucket = fp % LEAK_BUCKETS;
	struct leak_class *lc = st->buckets[bucket];
	while (lc != NULL) {
		if (lc->fingerprint == fp &&
		    lc->cache_name == info->cache_name &&
		    lc->depth == info->depth &&
		    (lc->depth == 0 ||
		     memcmp(lc->stack, info->stack,
		         sizeof (lc->stack[0]) * (size_t)lc->depth) == 0)) {
			lc->count++;
			lc->bytes += info->size;
			st->total_count++;
			st->total_bytes += info->size;
			return (0);
		}
		lc = lc->next;
	}

	lc = calloc(1, sizeof (*lc));
	if (lc == NULL)
		return (0);	/* best-effort; skip this entry */
	lc->fingerprint = fp;
	lc->count = 1;
	lc->bytes = info->size;
	lc->cache_name = info->cache_name;
	lc->bufsize = info->size;
	lc->depth = info->depth;
	lc->sample_addr = info->addr;
	memcpy(lc->stack, info->stack,
	    sizeof (lc->stack[0]) * (size_t)info->depth);
	lc->next = st->buckets[bucket];
	st->buckets[bucket] = lc;
	st->nclasses++;
	st->total_count++;
	st->total_bytes += info->size;
	return (0);
}

static int
leak_compare(const void *a, const void *b)
{
	const struct leak_class *la = *(const struct leak_class *const *)a;
	const struct leak_class *lb = *(const struct leak_class *const *)b;
	if (la->bytes != lb->bytes)
		return (la->bytes < lb->bytes ? 1 : -1);
	if (la->count != lb->count)
		return (la->count < lb->count ? 1 : -1);
	return (0);
}

/*
 * Wrapper used by umem_findleaks(): classify only if the buffer is not
 * in the magazine cached set.  arg points to a findleaks_filter_arg.
 */
struct findleaks_filter_arg {
	struct leak_state *st;
	struct cached_set *cs;
	size_t *skipped;
};

static int
leak_classify_filtered(const umem_buffer_info_t *info, void *arg)
{
	struct findleaks_filter_arg *ctx = arg;
	if (cached_set_contains(ctx->cs, info->addr)) {
		(*ctx->skipped)++;
		return (0);
	}
	return (leak_classify(info, ctx->st));
}

/*
 * If we found cached buffers (skipped from the leak count), tell the
 * user how many -- a useful diagnostic when the count is unexpectedly
 * low or high.
 */
static void
findleaks_emit_cache_note(FILE *out, umem_inspect_format_t fmt,
    size_t cached_skipped)
{
	if (cached_skipped == 0 || fmt != UMEM_FMT_TEXT)
		return;
	(void) fprintf(out,
	    "(skipped %zu buffer%s currently sitting in magazines / per-CPU "
	    "caches)\n\n",
	    cached_skipped, cached_skipped == 1 ? "" : "s");
}

size_t
umem_findleaks(FILE *out, umem_inspect_format_t fmt, unsigned max_classes)
{
	struct leak_state st;
	struct cached_set cs;

	if (out == NULL)
		out = stderr;
	if (max_classes == 0)
		max_classes = 50;

	(void) umem_stacktrace_init();
	memset(&st, 0, sizeof (st));
	memset(&cs, 0, sizeof (cs));

	/* Build the magazine cached-buffer set so we can subtract
	 * still-cached buffers from the leak count.  Without this,
	 * any free() that landed in a magazine would look like a
	 * leak. */
	cached_set_build_all(&cs);

	/* Walk user-visible caches only.  Three kinds get skipped:
	 *   - UMC_NOHASH: pure allocator bookkeeping (umem_slab_cache,
	 *     umem_bufctl_*_cache, umem_magazine_*).
	 *   - UMC_QCACHE: quantum caches backing a vmem arena.  Their
	 *     buffers double-count with user allocations (the qcache
	 *     buffer IS a user cache's slab).
	 * Caller can get the full walk by calling umem_walk_allocated()
	 * directly. */
	size_t visited = 0;
	size_t cached_skipped = 0;
	{
		umem_cache_t *cp = umem_null_cache.cache_next;
		unsigned safety = 0;
		while (cp != &umem_null_cache && safety++ < 65536) {
			if ((cp->cache_cflags & (UMC_NOHASH | UMC_QCACHE)) == 0) {
				struct findleaks_filter_arg ctx_arg = {
				    &st, &cs, &cached_skipped
				};
				struct walk_ctx ctx = {
				    leak_classify_filtered,
				    &ctx_arg, 0, 0,
				};
				walk_allocated_cache(cp, &ctx);
				visited += ctx.count;
			}
			cp = cp->cache_next;
		}
	}

	/* Flatten into an array and sort. */
	struct leak_class **arr = calloc(st.nclasses, sizeof (*arr));
	if (arr == NULL && st.nclasses > 0) {
		(void) fprintf(out,
		    "umem_findleaks: out of memory sorting %zu classes\n",
		    st.nclasses);
		goto cleanup;
	}

	size_t idx = 0;
	for (size_t i = 0; i < LEAK_BUCKETS; i++) {
		for (struct leak_class *lc = st.buckets[i];
		    lc != NULL; lc = lc->next) {
			arr[idx++] = lc;
		}
	}
	if (idx > 0)
		qsort(arr, idx, sizeof (*arr), leak_compare);

	size_t shown = idx < max_classes ? idx : max_classes;

	if (fmt == UMEM_FMT_JSON) {
		(void) fprintf(out,
		    "{\"version\":%d,\"total_buffers\":%zu,"
		    "\"total_bytes\":%zu,\"cached_skipped\":%zu,"
		    "\"classes\":[",
		    UMEM_INSPECT_VERSION, st.total_count, st.total_bytes,
		    cached_skipped);
		for (size_t i = 0; i < shown; i++) {
			struct leak_class *lc = arr[i];
			(void) fprintf(out, "%s{\"count\":%zu,\"bytes\":%zu,"
			    "\"bufsize\":%zu,\"cache\":",
			    i == 0 ? "" : ",", lc->count, lc->bytes,
			    lc->bufsize);
			json_escape(out, lc->cache_name ? lc->cache_name : "");
			(void) fputs(",\"stack\":", out);
			umem_buffer_info_t tmp = {0};
			tmp.depth = lc->depth;
			memcpy(tmp.stack, lc->stack,
			    sizeof (tmp.stack[0]) * (size_t)lc->depth);
			print_stack_json(out, &tmp);
			(void) fprintf(out, ",\"sample\":\"%p\"}",
			    lc->sample_addr);
		}
		(void) fputs("]}\n", out);
	} else {
		(void) fprintf(out,
		    "findleaks: %zu allocated buffer%s (%zu bytes) in %zu "
		    "distinct leak class%s\n",
		    st.total_count, st.total_count == 1 ? "" : "s",
		    st.total_bytes, st.nclasses,
		    st.nclasses == 1 ? "" : "es");
		findleaks_emit_cache_note(out, fmt, cached_skipped);

		if (st.total_count == 0)
			goto cleanup;

		(void) fprintf(out,
		    "%8s %12s %12s %-24s %s\n",
		    "COUNT", "BYTES", "SIZE", "CACHE", "STACK (top frame)");
		(void) fprintf(out,
		    "-------- ------------ ------------ "
		    "------------------------ ------------------------------\n");

		for (size_t i = 0; i < shown; i++) {
			struct leak_class *lc = arr[i];
			char frame[256] = "";
			if (lc->depth > 0) {
				umem_stacktrace_format(lc->stack[0], 0,
				    frame, sizeof (frame));
			} else {
				(void) snprintf(frame, sizeof (frame),
				    "<no stack; enable UMEM_DEBUG=audit>");
			}
			(void) fprintf(out, "%8zu %12zu %12zu %-24.24s %s\n",
			    lc->count, lc->bytes, lc->bufsize,
			    lc->cache_name ? lc->cache_name : "?", frame);
		}

		(void) fputc('\n', out);
		for (size_t i = 0; i < shown; i++) {
			struct leak_class *lc = arr[i];
			(void) fprintf(out,
			    "== class %zu: %zu alloc%s, %zu bytes, cache=%s "
			    "(sample buffer %p) ==\n",
			    i, lc->count, lc->count == 1 ? "" : "s",
			    lc->bytes,
			    lc->cache_name ? lc->cache_name : "?",
			    lc->sample_addr);
			if (lc->depth == 0) {
				(void) fprintf(out,
				    "  (no stack; run with "
				    "UMEM_DEBUG=audit)\n\n");
				continue;
			}
			umem_buffer_info_t tmp = {0};
			tmp.depth = lc->depth;
			memcpy(tmp.stack, lc->stack,
			    sizeof (tmp.stack[0]) * (size_t)lc->depth);
			print_stack_text(out, &tmp, "  ");
			(void) fputc('\n', out);
		}
	}

	if (shown < idx && fmt == UMEM_FMT_TEXT) {
		(void) fprintf(out,
		    "... %zu more classes not shown (use max_classes to see more)\n",
		    idx - shown);
	}

cleanup:
	for (size_t i = 0; i < LEAK_BUCKETS; i++) {
		struct leak_class *lc = st.buckets[i];
		while (lc != NULL) {
			struct leak_class *n = lc->next;
			free(lc);
			lc = n;
		}
	}
	free(arr);
	cached_set_destroy(&cs);
	return (visited);
}

/* ------------------------------------------------------------------------
 * log_dump: collect, sort by timestamp, emit.
 * ------------------------------------------------------------------------ */

struct log_rec {
	umem_buffer_info_t info;
};

struct log_state {
	struct log_rec *recs;
	size_t cap;
	size_t n;
	int oom;
};

static int
log_collect(const umem_buffer_info_t *info, void *arg)
{
	struct log_state *st = arg;
	if (st->n == st->cap) {
		size_t ncap = st->cap == 0 ? 1024 : st->cap * 2;
		if (ncap > (1u << 26)) {
			st->oom = 1;
			return (1);
		}
		struct log_rec *n = realloc(st->recs, ncap * sizeof (*n));
		if (n == NULL) {
			st->oom = 1;
			return (1);
		}
		st->recs = n;
		st->cap = ncap;
	}
	st->recs[st->n++].info = *info;
	return (0);
}

static int
log_compare(const void *a, const void *b)
{
	const struct log_rec *la = a, *lb = b;
	if (la->info.timestamp < lb->info.timestamp) return (-1);
	if (la->info.timestamp > lb->info.timestamp) return (1);
	return (0);
}

size_t
umem_log_dump(FILE *out, umem_inspect_format_t fmt, unsigned max_records)
{
	struct log_state st = { 0 };
	if (out == NULL)
		out = stderr;
	(void) umem_stacktrace_init();

	if (umem_transaction_log == NULL) {
		if (fmt == UMEM_FMT_JSON)
			(void) fputs(
			    "{\"error\":\"transaction log disabled; "
			    "set UMEM_LOGGING=transaction=1m\"}\n", out);
		else
			(void) fputs(
			    "transaction log disabled "
			    "(UMEM_LOGGING=transaction=1m to enable)\n", out);
		return (0);
	}

	(void) umem_walk_log(log_collect, &st);

	if (st.n > 1)
		qsort(st.recs, st.n, sizeof (*st.recs), log_compare);

	size_t shown = max_records && st.n > max_records ? max_records : st.n;
	size_t first = st.n > shown ? st.n - shown : 0;

	if (fmt == UMEM_FMT_JSON) {
		(void) fprintf(out, "{\"records\":[");
		for (size_t i = first; i < st.n; i++) {
			const umem_buffer_info_t *info = &st.recs[i].info;
			(void) fprintf(out,
			    "%s{\"time\":%" PRIu64 ",\"thread\":%" PRIu64
			    ",\"addr\":\"%p\",\"size\":%zu,\"cache\":",
			    i == first ? "" : ",",
			    info->timestamp, info->thread,
			    info->addr, info->size);
			json_escape(out, info->cache_name ? info->cache_name : "");
			(void) fputs(",\"stack\":", out);
			print_stack_json(out, info);
			(void) fputc('}', out);
		}
		(void) fputs("]}\n", out);
	} else {
		(void) fprintf(out,
		    "transaction log: %zu record%s (showing %zu)\n\n",
		    st.n, st.n == 1 ? "" : "s", shown);
		for (size_t i = first; i < st.n; i++) {
			const umem_buffer_info_t *info = &st.recs[i].info;
			(void) fprintf(out,
			    "t=%" PRIu64 " thr=0x%" PRIx64 " %p size=%zu "
			    "cache=%s\n",
			    info->timestamp, info->thread, info->addr,
			    info->size,
			    info->cache_name ? info->cache_name : "?");
			print_stack_text(out, info, "    ");
		}
	}

	if (st.oom) {
		(void) fprintf(out,
		    "warning: log_dump ran out of memory; "
		    "output may be truncated\n");
	}

	free(st.recs);
	return (st.n);
}

/* ------------------------------------------------------------------------
 * status_dump: ::umastat.
 * ------------------------------------------------------------------------ */

struct status_ctx {
	FILE *out;
	umem_inspect_format_t fmt;
	int first;
};

static void
status_visit(umem_cache_t *cp, void *arg)
{
	struct status_ctx *ctx = arg;
	FILE *out = ctx->out;

	(void) mutex_lock(&cp->cache_lock);
	uint64_t inuse = cp->cache_slab_alloc - cp->cache_slab_free;
	uint64_t total = cp->cache_buftotal;
	uint64_t mem = (cp->cache_slab_create - cp->cache_slab_destroy)
	    * cp->cache_slabsize;
	(void) mutex_unlock(&cp->cache_lock);

	if (ctx->fmt == UMEM_FMT_JSON) {
		(void) fprintf(out,
		    "%s{\"name\":", ctx->first ? "" : ",");
		json_escape(out, cp->cache_name);
		(void) fprintf(out,
		    ",\"bufsize\":%zu,\"inuse\":%" PRIu64
		    ",\"total\":%" PRIu64 ",\"memory\":%" PRIu64
		    ",\"alloc_ops\":%" PRIu64 ",\"alloc_fail\":%" PRIu64
		    ",\"depot_contention\":%" PRIu64 ",\"flags\":%d}",
		    cp->cache_bufsize, inuse, total, mem,
		    cp->cache_alloc_ops, cp->cache_alloc_fail,
		    cp->cache_depot_contention, cp->cache_flags);
		ctx->first = 0;
	} else {
		(void) fprintf(out,
		    "%-24.24s %8zu %8" PRIu64 " %8" PRIu64 " %12" PRIu64
		    " %10" PRIu64 " %6" PRIu64 "\n",
		    cp->cache_name, cp->cache_bufsize, inuse, total, mem,
		    cp->cache_alloc_ops, cp->cache_alloc_fail);
	}
}

void
umem_status_dump(FILE *out, umem_inspect_format_t fmt)
{
	if (out == NULL)
		out = stderr;
	struct status_ctx ctx = { out, fmt, 1 };

	if (fmt == UMEM_FMT_JSON) {
		(void) fputs("{\"caches\":[", out);
		for_each_cache(status_visit, &ctx);
		(void) fputs("]}\n", out);
	} else {
		(void) fprintf(out,
		    "%-24s %8s %8s %8s %12s %10s %6s\n",
		    "CACHE", "BUFSIZE", "INUSE", "TOTAL", "MEMORY",
		    "ALLOCS", "FAIL");
		(void) fprintf(out,
		    "------------------------ -------- -------- -------- "
		    "------------ ---------- ------\n");
		for_each_cache(status_visit, &ctx);
	}
}

/*
 * If we found cached buffers (skipped from the leak count), tell the
 * user how many -- a useful diagnostic when the count is unexpectedly
 * low or high.
 */

/* ------------------------------------------------------------------------
 * whatis: resolve an address to a cache/slab/state.
 * ------------------------------------------------------------------------ */

struct whatis_ctx {
	const void *target;
	umem_buffer_info_t *result;
	umem_buffer_info_t fallback;
	int found;
	int have_fallback;
};

static void
whatis_visit(umem_cache_t *cp, void *arg)
{
	struct whatis_ctx *ctx = arg;
	umem_slab_t *sp;
	if (ctx->found)
		return;

	(void) mutex_lock(&cp->cache_lock);
	for (sp = cp->cache_nullslab.slab_next;
	    sp != &cp->cache_nullslab;
	    sp = sp->slab_next) {
		char *base = sp->slab_base;
		if ((const char *)ctx->target < base)
			continue;
		if ((const char *)ctx->target >=
		    base + cp->cache_slabsize)
			continue;

		/*
		 * Found the slab.  Compute the buffer address and state.
		 */
		size_t off = (size_t)((const char *)ctx->target - base);
		size_t idx = off / cp->cache_chunksize;
		void *buf = base + idx * cp->cache_chunksize;
		int state = slab_buf_is_free(sp, buf, cp)
		    ? UMEM_BUF_FREE : UMEM_BUF_ALLOCATED;

		umem_buffer_info_t info;
		memset(&info, 0, sizeof (info));
		info.addr = buf;
		info.size = cp->cache_bufsize;
		info.cache = cp;
		info.cache_name = cp->cache_name;
		info.slab = sp;
		info.state = state;

		if (cp->cache_flags & UMF_HASH) {
			umem_bufctl_t *bcp = *UMEM_HASH(cp, buf);
			unsigned safety = 0;
			while (bcp != NULL && safety++ < (1u << 20)) {
				if (bcp->bc_addr == buf) {
					info_from_bufctl(cp, bcp, &info,
					    state);
					break;
				}
				bcp = bcp->bc_next;
			}
		} else {
			umem_bufctl_t *bcp = (umem_bufctl_t *)
			    ((char *)buf + cp->cache_bufctl);
			if (cache_has_audit(cp))
				info_from_bufctl(cp, bcp, &info, state);
			else
				info.bufctl = bcp;
		}

		/*
		 * Prefer user-visible caches over the qcaches that back
		 * them.  A umem_va_* qcache claims ownership of the same
		 * memory as the umem_alloc_* cache it backs; we want the
		 * latter.  Stash an internal/qcache hit as fallback and
		 * keep searching.
		 */
		if (cp->cache_cflags & (UMC_NOHASH | UMC_QCACHE)) {
			if (!ctx->have_fallback) {
				ctx->fallback = info;
				ctx->have_fallback = 1;
			}
			(void) mutex_unlock(&cp->cache_lock);
			return;
		}

		*ctx->result = info;
		ctx->found = 1;
		(void) mutex_unlock(&cp->cache_lock);
		return;
	}
	(void) mutex_unlock(&cp->cache_lock);
}

int
umem_whatis(const void *addr, umem_buffer_info_t *out)
{
	if (addr == NULL || out == NULL) {
		errno = EINVAL;
		return (-1);
	}
	struct whatis_ctx ctx = { addr, out, {0}, 0, 0 };
	for_each_cache(whatis_visit, &ctx);
	if (ctx.found)
		return (0);
	if (ctx.have_fallback) {
		*out = ctx.fallback;
		return (0);
	}
	errno = ENOENT;
	return (-1);
}

int
umem_bufctl_audit_dump(FILE *out, const void *addr)
{
	umem_buffer_info_t info;
	if (out == NULL)
		out = stderr;
	if (umem_whatis(addr, &info) != 0) {
		(void) fprintf(out, "%p: not in any umem cache\n", addr);
		return (-1);
	}
	(void) umem_stacktrace_init();
	(void) fprintf(out, "%p: %s (%zu byte%s, %s)\n",
	    info.addr, info.cache_name, info.size,
	    info.size == 1 ? "" : "s",
	    info.state == UMEM_BUF_ALLOCATED ? "ALLOCATED" :
	    info.state == UMEM_BUF_FREE ? "FREE" : "UNKNOWN");
	(void) fprintf(out, "  cache=%p slab=%p bufctl=%p\n",
	    info.cache, info.slab, info.bufctl);
	if (info.depth > 0) {
		(void) fprintf(out,
		    "  thread=0x%" PRIx64 " timestamp=%" PRIu64 "\n",
		    info.thread, info.timestamp);
		(void) fprintf(out, "  stack (%d frame%s):\n",
		    info.depth, info.depth == 1 ? "" : "s");
		print_stack_text(out, &info, "    ");
	} else {
		(void) fprintf(out,
		    "  no audit record (enable UMEM_DEBUG=audit)\n");
	}
	return (0);
}

/* ------------------------------------------------------------------------
 * Snapshot (minimal v1: text + JSON findleaks + status).
 * ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------
 * walk_dump: stream every allocated/freed/log entry.
 * ------------------------------------------------------------------------ */

struct walk_dump_ctx {
	FILE *out;
	umem_inspect_format_t fmt;
	unsigned max_entries;
	size_t emitted;
	int first;
};

static int
walk_dump_cb(const umem_buffer_info_t *info, void *arg)
{
	struct walk_dump_ctx *ctx = arg;
	if (ctx->max_entries && ctx->emitted >= ctx->max_entries)
		return (1);
	if (ctx->fmt == UMEM_FMT_JSON) {
		(void) fprintf(ctx->out,
		    "%s{\"addr\":\"%p\",\"size\":%zu,\"cache\":",
		    ctx->first ? "" : ",", info->addr, info->size);
		json_escape(ctx->out,
		    info->cache_name ? info->cache_name : "");
		(void) fprintf(ctx->out,
		    ",\"state\":%d,\"thread\":%" PRIu64
		    ",\"timestamp\":%" PRIu64 ",\"stack\":",
		    info->state, info->thread, info->timestamp);
		print_stack_json(ctx->out, info);
		(void) fputc('}', ctx->out);
		ctx->first = 0;
	} else {
		const char *state_s = info->state == UMEM_BUF_ALLOCATED
		    ? "ALLOC"
		    : info->state == UMEM_BUF_FREE
		    ? "FREE "
		    : "?";
		(void) fprintf(ctx->out, "%s %p size=%zu cache=%s\n",
		    state_s, info->addr, info->size,
		    info->cache_name ? info->cache_name : "?");
		print_stack_text(ctx->out, info, "    ");
	}
	ctx->emitted++;
	return (0);
}

size_t
umem_walk_dump(FILE *out, const char *kind, umem_inspect_format_t fmt,
    unsigned max_entries)
{
	struct walk_dump_ctx ctx = { 0 };
	if (out == NULL)
		out = stderr;
	ctx.out = out;
	ctx.fmt = fmt;
	ctx.max_entries = max_entries;
	ctx.first = 1;

	(void) umem_stacktrace_init();

	if (fmt == UMEM_FMT_JSON)
		(void) fputs("{\"entries\":[", out);

	if (kind == NULL || strcmp(kind, "allocated") == 0) {
		(void) umem_walk_allocated(walk_dump_cb, &ctx);
	} else if (strcmp(kind, "freed") == 0) {
		(void) umem_walk_freed(walk_dump_cb, &ctx);
	} else if (strcmp(kind, "log") == 0) {
		(void) umem_walk_log(walk_dump_cb, &ctx);
	} else {
		if (fmt == UMEM_FMT_JSON)
			(void) fputs("]}\n", out);
		else
			(void) fprintf(out,
			    "unknown walk kind '%s'; "
			    "expected allocated|freed|log\n", kind);
		return (0);
	}

	if (fmt == UMEM_FMT_JSON)
		(void) fputs("]}\n", out);

	return (ctx.emitted);
}

/* ------------------------------------------------------------------------
 * Snapshot v2 (binary): self-describing, versioned, parseable offline.
 *
 * On-disk format (little-endian):
 *
 *   struct ump_v2_header {
 *       char     magic[4];        // "UMS2"
 *       uint32_t version;         // 2
 *       uint64_t timestamp_ns;
 *       uint32_t pointer_size;    // 4 or 8
 *       uint32_t stack_depth;     // umem_stack_depth
 *       uint64_t n_caches;
 *       uint64_t n_buffers;       // total live buffers walked
 *       uint64_t n_log_records;
 *   };
 *
 *   followed by:
 *     n_caches  * struct ump_v2_cache_summary  // ::umastat
 *     n_buffers * struct ump_v2_buffer_record  // findleaks raw input
 *     n_log_records * struct ump_v2_log_record  // ::umem_log raw
 *
 * The umem_tool can re-run findleaks/log/status logic against the
 * snapshot offline without the original process being alive.
 * ------------------------------------------------------------------------ */

#define UMP_V2_MAGIC	"UMS2"
#define UMP_V2_VERSION	2

#pragma pack(push, 1)
struct ump_v2_header {
	char     magic[4];
	uint32_t version;
	uint64_t timestamp_ns;
	uint32_t pointer_size;
	uint32_t stack_depth;
	uint64_t n_caches;
	uint64_t n_buffers;
	uint64_t n_log_records;
};

struct ump_v2_cache_summary {
	char     name[UMEM_CACHE_NAMELEN + 1];
	uint64_t bufsize;
	uint64_t inuse;
	uint64_t total;
	uint64_t memory;
	uint64_t alloc_ops;
	uint64_t alloc_fail;
	uint64_t depot_contention;
	uint32_t flags;
	uint32_t cflags;
};

struct ump_v2_buffer_record {
	uint64_t addr;
	uint64_t size;
	uint64_t cache_idx;        /* index into the caches table */
	uint64_t timestamp_ns;
	uint64_t thread;
	uint32_t depth;
	uint32_t state;
	uint64_t stack[UMEM_INSPECT_MAX_STACK];
};

struct ump_v2_log_record {
	struct ump_v2_buffer_record b;
};
#pragma pack(pop)

struct snapshot_state {
	FILE *fp;
	umem_cache_t **cache_table;
	size_t cache_count;
	size_t cache_cap;
	uint64_t buffers_written;
	uint64_t logs_written;
	struct cached_set *cached;
	int err;
};

static uint64_t
snapshot_cache_idx(struct snapshot_state *st, umem_cache_t *cp)
{
	for (size_t i = 0; i < st->cache_count; i++)
		if (st->cache_table[i] == cp)
			return (i);
	return ((uint64_t)-1);
}

static void
snapshot_collect_cache(umem_cache_t *cp, void *arg)
{
	struct snapshot_state *st = arg;
	if (st->cache_count == st->cache_cap) {
		size_t ncap = st->cache_cap == 0 ? 64 : st->cache_cap * 2;
		umem_cache_t **n = realloc(st->cache_table,
		    ncap * sizeof (*n));
		if (n == NULL) {
			st->err = ENOMEM;
			return;
		}
		st->cache_table = n;
		st->cache_cap = ncap;
	}
	st->cache_table[st->cache_count++] = cp;
}

static void
snapshot_write_cache_summary(struct snapshot_state *st, umem_cache_t *cp)
{
	struct ump_v2_cache_summary cs;
	memset(&cs, 0, sizeof (cs));
	strncpy(cs.name, cp->cache_name, sizeof (cs.name) - 1);

	(void) mutex_lock(&cp->cache_lock);
	cs.bufsize = cp->cache_bufsize;
	cs.inuse = cp->cache_slab_alloc - cp->cache_slab_free;
	cs.total = cp->cache_buftotal;
	cs.memory = (cp->cache_slab_create - cp->cache_slab_destroy)
	    * cp->cache_slabsize;
	cs.alloc_ops = cp->cache_alloc_ops;
	cs.alloc_fail = cp->cache_alloc_fail;
	cs.depot_contention = cp->cache_depot_contention;
	cs.flags = (uint32_t)cp->cache_flags;
	cs.cflags = (uint32_t)cp->cache_cflags;
	(void) mutex_unlock(&cp->cache_lock);

	if (fwrite(&cs, sizeof (cs), 1, st->fp) != 1)
		st->err = errno ? errno : EIO;
}

static void
snapshot_buffer_to_record(const umem_buffer_info_t *info,
    struct snapshot_state *st, struct ump_v2_buffer_record *out)
{
	memset(out, 0, sizeof (*out));
	out->addr = (uint64_t)(uintptr_t)info->addr;
	out->size = info->size;
	out->cache_idx = snapshot_cache_idx(st, (umem_cache_t *)info->cache);
	out->timestamp_ns = info->timestamp;
	out->thread = info->thread;
	out->depth = (uint32_t)info->depth;
	out->state = (uint32_t)info->state;
	for (int i = 0; i < info->depth && i < UMEM_INSPECT_MAX_STACK; i++)
		out->stack[i] = (uint64_t)info->stack[i];
}

static int
snapshot_buffer_cb(const umem_buffer_info_t *info, void *arg)
{
	struct snapshot_state *st = arg;
	struct ump_v2_buffer_record rec;
	snapshot_buffer_to_record(info, st, &rec);
	/* If the address is currently sitting in a magazine, mark it
	 * UMEM_BUF_CACHED so offline tooling can subtract it the same
	 * way live findleaks does. */
	if (st->cached != NULL &&
	    cached_set_contains(st->cached, info->addr))
		rec.state = UMEM_BUF_CACHED;
	if (fwrite(&rec, sizeof (rec), 1, st->fp) != 1) {
		st->err = errno ? errno : EIO;
		return (1);
	}
	st->buffers_written++;
	return (0);
}

static int
snapshot_log_cb(const umem_buffer_info_t *info, void *arg)
{
	struct snapshot_state *st = arg;
	struct ump_v2_log_record rec;
	snapshot_buffer_to_record(info, st, &rec.b);
	if (fwrite(&rec, sizeof (rec), 1, st->fp) != 1) {
		st->err = errno ? errno : EIO;
		return (1);
	}
	st->logs_written++;
	return (0);
}

static int
snapshot_v2_write(const char *path)
{
	struct snapshot_state st;
	memset(&st, 0, sizeof (st));

	st.fp = fopen(path, "wb");
	if (st.fp == NULL)
		return (-1);

	/* Reserve header space; we'll seek back and write it last. */
	struct ump_v2_header hdr;
	memset(&hdr, 0, sizeof (hdr));
	if (fwrite(&hdr, sizeof (hdr), 1, st.fp) != 1) {
		st.err = EIO;
		goto out;
	}

	/* Build the magazine cached set so we can mark in-magazine
	 * buffers as UMEM_BUF_CACHED when serializing.  Offline
	 * findleaks then matches live findleaks exactly. */
	struct cached_set cs;
	memset(&cs, 0, sizeof (cs));
	cached_set_build_all(&cs);
	st.cached = &cs;

	/* Phase 1: enumerate caches into a stable index. */
	for_each_cache(snapshot_collect_cache, &st);
	if (st.err)
		goto out;

	/* Phase 2: write cache summary table. */
	for (size_t i = 0; i < st.cache_count && !st.err; i++)
		snapshot_write_cache_summary(&st, st.cache_table[i]);
	if (st.err)
		goto out;

	/* Phase 3: write all live buffers. */
	(void) umem_walk_allocated(snapshot_buffer_cb, &st);
	if (st.err)
		goto out;

	/* Phase 4: write transaction log records. */
	(void) umem_walk_log(snapshot_log_cb, &st);
	if (st.err)
		goto out;

	/* Phase 5: rewrite header with the totals. */
	memcpy(hdr.magic, UMP_V2_MAGIC, 4);
	hdr.version = UMP_V2_VERSION;
	hdr.timestamp_ns = (uint64_t)gethrtime();
	hdr.pointer_size = (uint32_t)sizeof (void *);
	hdr.stack_depth = umem_stack_depth;
	hdr.n_caches = st.cache_count;
	hdr.n_buffers = st.buffers_written;
	hdr.n_log_records = st.logs_written;
	if (fseek(st.fp, 0L, SEEK_SET) != 0 ||
	    fwrite(&hdr, sizeof (hdr), 1, st.fp) != 1) {
		st.err = errno ? errno : EIO;
		goto out;
	}

out:
	if (st.fp != NULL)
		(void) fclose(st.fp);
	free(st.cache_table);
	cached_set_destroy(&cs);
	if (st.err) {
		errno = st.err;
		return (-1);
	}
	return (0);
}

int
umem_inspect_snapshot(const char *path)
{
	if (path == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * Two formats: paths ending in .ums, .umsnap, .bin write the v2
	 * binary format.  Anything else gets the human-readable text
	 * dump for backward compatibility with the v1 stub.
	 */
	size_t plen = strlen(path);
	int binary = 0;
	static const char *bin_suffixes[] = { ".ums", ".umsnap", ".bin", NULL };
	for (int i = 0; bin_suffixes[i] != NULL; i++) {
		size_t slen = strlen(bin_suffixes[i]);
		if (plen >= slen &&
		    strcmp(path + plen - slen, bin_suffixes[i]) == 0) {
			binary = 1;
			break;
		}
	}

	if (binary)
		return (snapshot_v2_write(path));

	FILE *fp = fopen(path, "w");
	if (fp == NULL)
		return (-1);
	(void) fprintf(fp,
	    "# umem_inspect snapshot v%d (text)\n", UMEM_INSPECT_VERSION);
	(void) fputs("\n# ::umastat\n", fp);
	umem_status_dump(fp, UMEM_FMT_TEXT);
	(void) fputs("\n# ::findleaks\n", fp);
	(void) umem_findleaks(fp, UMEM_FMT_TEXT, 50);
	(void) fputs("\n# ::umem_log\n", fp);
	(void) umem_log_dump(fp, UMEM_FMT_TEXT, 500);
	(void) fclose(fp);
	return (0);
}

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
#include "umem_base.h"
#include "umem_inspect.h"
#include "umem_stacktrace.h"
#include "umem_ptc.h"
/*
 * umem_rseq.h DEFINES UMEM_RSEQ_AVAILABLE; it does not consume it.  Guarding
 * this include with #ifdef UMEM_RSEQ_AVAILABLE therefore never fires, which
 * silently compiled out the rseq magazine subtraction below and left every
 * rseq-resident buffer reported as outstanding -- the exact defect this was
 * meant to fix.  Test the same condition umem.c tests to decide the include.
 */
#if defined(__linux__) && defined(HAVE_LINUX_RSEQ_H)
#include "umem_rseq.h"
#endif
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

/* ------------------------------------------------------------------------
 * CACHE LIFETIME AND SNAPSHOT CONTRACT  (Phase 3 items 1, 2 and 7 of
 * docs/plans/2026-09-21-production-readiness.md).  Also stated for callers
 * in umem_inspect.h -- keep the two in agreement.
 *
 * C1. CACHE LIFETIME.  umem_cache_lock protects the global cache list
 *     linkage.  umem_cache_destroy() unlinks a cache under that lock and
 *     only then destroys its locks and frees the descriptor, so a walker
 *     that holds umem_cache_lock across the ENTIRE walk cannot follow a
 *     freed link or lock a freed cache.  Every cache-list walk in this file
 *     therefore holds umem_cache_lock for its whole duration.  This is the
 *     same rule umem_cache_applyall() in umem.c relies on.
 *
 *     Consequence: inspection blocks umem_cache_create()/umem_cache_destroy()
 *     while it collects.  It does not block allocation.
 *
 * C2. NO ALLOCATION, NO STDIO, NO USER CODE UNDER AN ALLOCATOR LOCK.
 *     Collection copies into a buffer that was allocated BEFORE any lock was
 *     taken; formatting, sorting, classification, file I/O and public walker
 *     callbacks all run with every allocator lock released.  Anything else
 *     deadlocks under malloc interposition, where a calloc()/fprintf() inside
 *     the walk re-enters the allocator and blocks on a lock the walk holds.
 *
 * C3. WHAT IS CONSISTENT.  Per cache, the collected buffer set is a snapshot
 *     taken under that cache's locks, so it is internally consistent.  Across
 *     caches it is NOT one instant: cache A is read before cache B, and
 *     allocation continues in between.  umem_cache_lock only freezes the set
 *     of caches, not their contents.
 *
 *     Because the destination buffer is sized before the locks are taken, a
 *     cache that grows past the estimate truncates the snapshot.  Collection
 *     detects that (it keeps counting past capacity), grows, and retries;
 *     after UMEM_SNAP_TRIES attempts it reports truncation rather than
 *     silently under-reporting.
 *
 * C4. WHAT MAY BE TORN.  Statistics counters (cache_slab_alloc,
 *     cache_buftotal, ...) are plain 64-bit fields read under cache_lock,
 *     which is also what the allocator updates them under, so each is whole;
 *     but a set of them read across two caches is not a single instant.  The
 *     rseq per-CPU magazine state (cache_rseq[]) has no mutex at all -- it is
 *     mutated from rseq critical sections -- so it is read best-effort and
 *     clamped.  The transaction log is read under lh_lock, which only
 *     protects chunk rotation; individual records are written under a per-CPU
 *     clh_lock, so a record can be observed half-written.  Records are
 *     filtered for plausibility, never trusted.
 *
 * C5. DEBUGGER USE.  These functions take allocator mutexes.  Calling them
 *     from a debugger against a target that was stopped while one of those
 *     mutexes was held will hang the call.  Retry, or take the snapshot from
 *     inside the process.
 * ------------------------------------------------------------------------ */

/* Bounded retries when the buffer population outgrows the pre-sized
 * snapshot buffer.  ponytail: a fixed retry count with honest truncation
 * reporting; a quiesce protocol is the upgrade path if truncation is ever
 * observed in practice. */
#define	UMEM_SNAP_TRIES		5
#define	UMEM_SNAP_SLACK		256

typedef void (*cache_visitor_t)(umem_cache_t *cp, void *arg);

/*
 * Walk the cache list with umem_cache_lock held for the whole walk (C1).
 * Visitors MUST NOT allocate, do stdio, or call user code (C2).
 */
static void
for_each_cache(cache_visitor_t v, void *arg)
{
	umem_cache_t *cp;
	unsigned safety = 0;

	(void) mutex_lock(&umem_cache_lock);
	cp = umem_null_cache.cache_next;
	while (cp != &umem_null_cache && safety++ < 65536) {
		v(cp, arg);
		cp = cp->cache_next;
	}
	(void) mutex_unlock(&umem_cache_lock);
}

/*
 * Take every lock of one cache, in THE ONE TRUE LOCK ORDER documented in
 * umem_fork.c: per-CPU cc_lock ascending, then the depot maglist locks, then
 * cache_lock.  Needed by any collection that reads more than one layer --
 * the cached-buffer set reads per-CPU magazines (cc_lock), depot lists
 * (ml_lock) and cache_magtype (cache_lock) and previously held only
 * cache_lock for all three.
 *
 * Caller holds umem_cache_lock, which is above all of these.
 */
static void
cache_lock_all(umem_cache_t *cp)
{
	int i;

	for (i = 0; i <= (int)cp->cache_cpu_mask; i++)
		(void) mutex_lock(&cp->cache_cpu[i].cc_lock);

	(void) mutex_lock(&cp->cache_full.ml_lock);
	for (i = 0; i < cp->cache_depot_ncpus; i++)
		(void) mutex_lock(&cp->cache_depot_full[i].ml_lock);

	(void) mutex_lock(&cp->cache_lock);
}

static void
cache_unlock_all(umem_cache_t *cp)
{
	int i;

	(void) mutex_unlock(&cp->cache_lock);

	for (i = cp->cache_depot_ncpus - 1; i >= 0; i--)
		(void) mutex_unlock(&cp->cache_depot_full[i].ml_lock);
	(void) mutex_unlock(&cp->cache_full.ml_lock);

	for (i = (int)cp->cache_cpu_mask; i >= 0; i--)
		(void) mutex_unlock(&cp->cache_cpu[i].cc_lock);
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

/* True if buf is on the slab's freelist.
 *
 * Slab freelist links are mangled (P5.4, UMEM_LINK_MANGLE in umem_impl.h):
 * demangle each link before following it.  Without this the walk dereferences
 * a mangled value, i.e. a wild address. */
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
		bcp = UMEM_LINK_DEMANGLE(&bcp->bc_next, bcp->bc_next);
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
 * Two-phase collection: COLLECT under locks into a pre-sized array, then
 * EMIT with every lock released (contract C2).
 *
 * The pre-sizing pass runs under the same locks, so the estimate is a real
 * count and not a guess; the buffer is allocated between the two passes with
 * no lock held.
 * ------------------------------------------------------------------------ */

struct snap {
	umem_buffer_info_t *recs;
	size_t cap;		/* records the buffer can hold */
	size_t n;		/* records actually stored */
	size_t needed;		/* records we would have stored (>= n) */
};

static void
snap_put(struct snap *s, const umem_buffer_info_t *info)
{
	s->needed++;
	if (s->n < s->cap)
		s->recs[s->n++] = *info;
}

static void
snap_free(struct snap *s)
{
	free(s->recs);
	s->recs = NULL;
	s->cap = s->n = s->needed = 0;
}

/*
 * Run `collect` repeatedly, growing the buffer until it holds everything.
 * `collect` must only call snap_put() and must not allocate (C2).
 *
 * Returns 0 on success, -1 if the snapshot is still truncated after
 * UMEM_SNAP_TRIES attempts (s->n < s->needed tells the caller how much).
 */
static int
snap_collect(struct snap *s, void (*collect)(struct snap *, void *), void *arg)
{
	int try;

	memset(s, 0, sizeof (*s));

	for (try = 0; try < UMEM_SNAP_TRIES; try++) {
		s->n = 0;
		s->needed = 0;
		collect(s, arg);
		if (s->needed <= s->cap)
			return (0);

		/* Grow past what we just measured and retry, with no lock
		 * held here -- this is the allocation C2 keeps out of the
		 * locked region. */
		size_t want = s->needed + s->needed / 4 + UMEM_SNAP_SLACK;
		umem_buffer_info_t *nr = realloc(s->recs,
		    want * sizeof (*nr));
		if (nr == NULL)
			break;
		s->recs = nr;
		s->cap = want;
	}
	return (-1);
}

/* ------------------------------------------------------------------------
 * Per-cache collectors.  Every one of these runs with umem_cache_lock held
 * by for_each_cache() and takes the cache's own locks itself.
 * ------------------------------------------------------------------------ */

static void
collect_allocated_cache(umem_cache_t *cp, void *arg)
{
	struct snap *s = arg;
	umem_buffer_info_t info;

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
				    UMEM_BUF_ALLOCATED))
					snap_put(s, &info);
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
				snap_put(s, &info);
			}
		}
	}

	(void) mutex_unlock(&cp->cache_lock);
}

static void
collect_freed_cache(umem_cache_t *cp, void *arg)
{
	struct snap *s = arg;
	umem_buffer_info_t info;
	umem_slab_t *sp;

	(void) mutex_lock(&cp->cache_lock);
	for (sp = cp->cache_nullslab.slab_next;
	    sp != &cp->cache_nullslab;
	    sp = sp->slab_next) {
		umem_bufctl_t *bcp = sp->slab_head;
		unsigned safety = 0;
		while (bcp != NULL && safety++ < (1u << 20)) {
			info_from_bufctl(cp, bcp, &info, UMEM_BUF_FREE);
			snap_put(s, &info);
			/* Mangled link; see slab_buf_is_free() (P5.4). */
			bcp = UMEM_LINK_DEMANGLE(&bcp->bc_next, bcp->bc_next);
		}
	}
	(void) mutex_unlock(&cp->cache_lock);
}

/* Only the caches findleaks reports on: skip allocator bookkeeping
 * (UMC_NOHASH) and vmem quantum caches (UMC_QCACHE), whose buffers ARE the
 * slabs of user caches and would double-count. */
static int
cache_is_user_visible(const umem_cache_t *cp)
{
	return ((cp->cache_cflags & (UMC_NOHASH | UMC_QCACHE)) == 0);
}

static void
collect_allocated_user_cache(umem_cache_t *cp, void *arg)
{
	if (cache_is_user_visible(cp))
		collect_allocated_cache(cp, arg);
}

static void
collect_allocated_all(struct snap *s, void *arg)
{
	(void) arg;
	for_each_cache(collect_allocated_cache, s);
}

static void
collect_allocated_user(struct snap *s, void *arg)
{
	(void) arg;
	for_each_cache(collect_allocated_user_cache, s);
}

static void
collect_freed_all(struct snap *s, void *arg)
{
	(void) arg;
	for_each_cache(collect_freed_cache, s);
}

/* ------------------------------------------------------------------------
 * Public walkers.
 *
 * Contract: the callback runs with NO allocator lock held (C2), against a
 * snapshot taken earlier.  It may therefore allocate, do I/O, and call back
 * into libumem.  A nonzero return stops the walk.
 * ------------------------------------------------------------------------ */

static size_t
walk_snapshot(void (*collect)(struct snap *, void *),
    umem_buffer_cb_t cb, void *arg)
{
	struct snap s;
	size_t i;

	(void) snap_collect(&s, collect, NULL);
	for (i = 0; i < s.n; i++) {
		if (cb(&s.recs[i], arg) != 0) {
			i++;
			break;
		}
	}
	snap_free(&s);
	return (i);
}

size_t
umem_walk_allocated(umem_buffer_cb_t cb, void *arg)
{
	return (walk_snapshot(collect_allocated_all, cb, arg));
}

size_t
umem_walk_freed(umem_buffer_cb_t cb, void *arg)
{
	return (walk_snapshot(collect_freed_all, cb, arg));
}

/* ------------------------------------------------------------------------
 * Transaction log walk.
 *
 * The log is a set of chunks; each chunk holds a dense sequence of
 * audit records.  We don't have a write head per-chunk exposed so we
 * scan the whole chunk in stride and filter by "looks like a valid
 * audit record" (non-NULL addr and cache pointer that matches a known
 * cache).  This is what mdb's ::umem_logs does.
 *
 * LOCKING (contract C4).  lh_lock only protects CHUNK ROTATION; records are
 * written under the per-CPU clh_lock.  Holding lh_lock therefore stops a
 * chunk being recycled underneath the scan, but an individual record can be
 * observed half-written.  That is why every field is range-checked and the
 * record is dropped unless addr, cache and timestamp are all plausible --
 * torn records are discarded, never trusted.  Taking every clh_lock as well
 * would give a truly stable log, at the cost of stalling every logging CPU;
 * the read is diagnostic, so the cheap option with explicit filtering is the
 * deliberate choice.
 *
 * ponytail: filter-and-drop rather than a full log quiesce.  Take the
 * clh_locks too if a torn record ever matters more than the stall does.
 *
 * The cache-list check runs under umem_cache_lock (C1) and the callback runs
 * with no lock at all (C2).
 * ------------------------------------------------------------------------ */

/* Caller holds umem_cache_lock. */
static int
is_known_cache_locked(umem_cache_t *candidate)
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

static void
collect_log(struct snap *s, void *arg)
{
	umem_log_header_t *lhp = umem_transaction_log;
	size_t rec_sz = audit_record_size();

	(void) arg;
	if (lhp == NULL || rec_sz == 0)
		return;

	/*
	 * umem_cache_lock OUTSIDE lh_lock: THE ONE TRUE LOCK ORDER in
	 * umem_fork.c puts the log headers last.  We need it for the whole
	 * scan because every record names a cache we have to validate, and
	 * because a cache must not be freed while we copy its name.
	 */
	(void) mutex_lock(&umem_cache_lock);
	(void) mutex_lock(&lhp->lh_lock);

	size_t chunksize = lhp->lh_chunksize;
	char *base = lhp->lh_base;
	int nchunks = lhp->lh_nchunks;

	if (base == NULL || chunksize == 0 || nchunks <= 0)
		goto out;

	for (int c = 0; c < nchunks; c++) {
		char *chunk = base + (size_t)c * chunksize;
		for (size_t off = 0; off + rec_sz <= chunksize; off += rec_sz) {
			umem_bufctl_audit_t *rec =
			    (umem_bufctl_audit_t *)(chunk + off);
			if (rec->bc_addr == NULL)
				continue;
			if (rec->bc_cache == NULL)
				continue;
			if (!is_known_cache_locked(rec->bc_cache))
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

			snap_put(s, &info);
		}
	}

out:
	(void) mutex_unlock(&lhp->lh_lock);
	(void) mutex_unlock(&umem_cache_lock);
}

size_t
umem_walk_log(umem_buffer_cb_t cb, void *arg)
{
	return (walk_snapshot(collect_log, cb, arg));
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
 *
 * STORAGE (contract C2).  This is a flat, pre-sized array, not a chained hash
 * table: it is populated while allocator locks are held, so it must not
 * allocate.  It reuses the same grow-and-retry protocol as struct snap --
 * fill up to capacity while counting what was needed, then grow outside the
 * locks and refill.  The previous version called calloc() per inserted
 * address with cache_lock held, which deadlocks under malloc interposition.
 *
 * Membership is by binary search after one sort, so insertion stays O(1)
 * under the locks and duplicates are collapsed afterwards.
 * ------------------------------------------------------------------------ */

struct cached_set {
	void **addrs;
	size_t cap;		/* addrs[] capacity */
	size_t n;		/* addresses stored */
	size_t needed;		/* addresses we would have stored */
	int sorted;
};

/* Append; no allocation, no dedup (dedup happens in cached_set_finish). */
static void
cached_set_add(struct cached_set *cs, void *addr)
{
	if (addr == NULL)
		return;
	cs->needed++;
	if (cs->n < cs->cap)
		cs->addrs[cs->n++] = addr;
}

static int
cached_addr_compare(const void *a, const void *b)
{
	uintptr_t ua = (uintptr_t)*(void *const *)a;
	uintptr_t ub = (uintptr_t)*(void *const *)b;
	if (ua < ub)
		return (-1);
	if (ua > ub)
		return (1);
	return (0);
}

/* Sort + unique.  Called with no lock held, once collection is complete. */
static void
cached_set_finish(struct cached_set *cs)
{
	if (cs->n > 1) {
		qsort(cs->addrs, cs->n, sizeof (cs->addrs[0]),
		    cached_addr_compare);
		size_t w = 1;
		for (size_t i = 1; i < cs->n; i++)
			if (cs->addrs[i] != cs->addrs[w - 1])
				cs->addrs[w++] = cs->addrs[i];
		cs->n = w;
	}
	cs->sorted = 1;
}

static int
cached_set_contains(const struct cached_set *cs, void *addr)
{
	size_t lo = 0, hi = cs->n;

	if (addr == NULL || cs->n == 0)
		return (0);
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (cs->addrs[mid] == addr)
			return (1);
		if ((uintptr_t)cs->addrs[mid] < (uintptr_t)addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (0);
}

static void
cached_set_destroy(struct cached_set *cs)
{
	free(cs->addrs);
	cs->addrs = NULL;
	cs->cap = cs->n = cs->needed = 0;
	cs->sorted = 0;
}

/*
 * Walk a magazine list and add up to `cap` rounds per magazine.  The
 * mag_round[] array is indexed [0..rounds-1] for the rounds in use.
 */
static void
cached_set_add_maglist(struct cached_set *cs, umem_magazine_t *mp,
    unsigned safety_max)
{
	unsigned safety = 0;
	/*
	 * Bound each magazine by ITS OWN capacity, not the cache's current
	 * mt_magsize: after a resize, stale smaller shells stay on these lists
	 * until popped and destroyed, and reading mt_magsize rounds from a
	 * 127-round shell reads 128 pointers past it into the next slab object
	 * -- garbage that the cached set then treats as CACHED buffers.  A
	 * full-list magazine holds exactly its capacity, so capacity is the
	 * right count here (the loaded/previous ones below carry a count).
	 */
	while (mp != NULL && safety++ < safety_max) {
		int cap = UMEM_MAGAZINE_CAPACITY(mp);
		for (int r = 0; r < cap; r++)
			cached_set_add(cs, mp->mag_round[r]);
		mp = (umem_magazine_t *)mp->mag_next;
	}
}

/*
 * Build the cached set for a single cache.
 *
 * Covers every retention site reachable from the cache descriptor:
 *   - the central depot's full magazine list
 *   - the per-CPU depot stripes' full magazine lists
 *   - each per-CPU loaded/previous magazine
 *   - each rseq per-CPU loaded/previous magazine (when rseq is compiled in
 *     and enabled)
 *
 * NOT covered, because they are thread-local with no process-wide registry:
 * PTC bins and PTC per-thread magazines (umem_ptc.h).  See
 * cached_set_unaccounted_note(); findleaks reports that gap explicitly rather
 * than silently counting those buffers as outstanding.
 *
 * LOCKING (contract C2/C4).  Takes EVERY lock of the cache in THE ONE TRUE
 * LOCK ORDER, because it reads three layers governed by three different
 * locks: per-CPU magazines (cc_lock), depot lists (ml_lock) and
 * cache_magtype (cache_lock).  It previously held only cache_lock while
 * reading all three.  The rseq arrays have no lock by construction -- they
 * are mutated from rseq critical sections -- so they are read best-effort
 * with clamped counts; a concurrently migrating CPU can make that one read
 * stale, which can only cause a buffer to be reported outstanding when it is
 * actually cached, never the reverse.
 */
static void
cached_set_build_cache(struct cached_set *cs, umem_cache_t *cp)
{
	if (cp->cache_magtype == NULL)
		return;
	int magsize = cp->cache_magtype->mt_magsize;
	if (magsize <= 0)
		return;

	cache_lock_all(cp);

	/* Central depot: a full magazine holds exactly its own capacity. */
	cached_set_add_maglist(cs, cp->cache_full.ml_list, 1u << 20);

	/* Per-CPU depot arrays. */
	if (cp->cache_depot_full != NULL && cp->cache_depot_ncpus > 0) {
		for (int i = 0; i < cp->cache_depot_ncpus; i++) {
			cached_set_add_maglist(cs,
			    cp->cache_depot_full[i].ml_list, 1u << 20);
		}
	}

	/*
	 * Per-CPU loaded/previous magazines: bounded by the count the CPU
	 * layer keeps AND by the shell's own capacity (P1.3b).
	 */
	for (uint32_t cpu = 0; cpu <= cp->cache_cpu_mask; cpu++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[cpu];
		if (ccp->cc_loaded != NULL && ccp->cc_rounds > 0) {
			int r = ccp->cc_rounds;
			int cap = UMEM_MAGAZINE_CAPACITY(ccp->cc_loaded);
			if (r > cap) r = cap;
			for (int i = 0; i < r; i++)
				cached_set_add(cs,
				    ccp->cc_loaded->mag_round[i]);
		}
		if (ccp->cc_ploaded != NULL && ccp->cc_prounds > 0) {
			int r = ccp->cc_prounds;
			int cap = UMEM_MAGAZINE_CAPACITY(ccp->cc_ploaded);
			if (r > cap) r = cap;
			for (int i = 0; i < r; i++)
				cached_set_add(cs,
				    ccp->cc_ploaded->mag_round[i]);
		}
	}

#ifdef UMEM_RSEQ_AVAILABLE
	/*
	 * rseq per-CPU magazines.  These were previously NOT subtracted, so in
	 * a default (rseq-enabled) build every buffer parked in an rseq
	 * magazine was reported as an outstanding allocation.
	 */
	if (cp->cache_rseq != NULL && umem_rseq_enabled) {
		int n = umem_rseq_get_ncpus();
		for (int i = 0; i < n; i++) {
			umem_rseq_cache_t *rc = &cp->cache_rseq[i];
			umem_magazine_t *mp;
			int r;

			/* Each shell bounds itself (P1.3b); see maglist above. */
			mp = (umem_magazine_t *)rc->loaded_mag;
			r = rc->rounds;
			if (mp != NULL && r > 0) {
				int cap = UMEM_MAGAZINE_CAPACITY(mp);
				if (r > cap) r = cap;
				for (int k = 0; k < r; k++)
					cached_set_add(cs, mp->mag_round[k]);
			}

			mp = (umem_magazine_t *)rc->previous_mag;
			r = rc->prounds;
			if (mp != NULL && r > 0) {
				int cap = UMEM_MAGAZINE_CAPACITY(mp);
				if (r > cap) r = cap;
				for (int k = 0; k < r; k++)
					cached_set_add(cs, mp->mag_round[k]);
			}
		}
	}
#endif

	cache_unlock_all(cp);
}

/*
 * True if a retention site exists that cached_set_build_all() cannot
 * enumerate, so the outstanding count is an upper bound rather than exact.
 *
 * Today that is exactly the PTC: bins and per-thread magazines live in
 * thread-local storage (umem_ptc.h: __thread umem_ptc_t *thread_ptc) with no
 * process-wide registry, so no other thread can reach them.  Making this
 * exact requires a PTC registry in umem_ptc.c, which is an allocator change
 * outside this file.
 *
 * ponytail: report the gap instead of guessing at it.  Subtract the PTC
 * properly once umem_ptc.c publishes its per-thread caches.
 */
static int
cached_set_has_unaccounted(void)
{
	return (umem_ptc_enabled != 0);
}

static void
cached_set_build_visit(umem_cache_t *cp, void *arg)
{
	cached_set_build_cache((struct cached_set *)arg, cp);
}

/*
 * Collect the cached set with the same grow-and-retry protocol as snap_*:
 * fill under the locks while counting, grow outside them, refill.  Ends with
 * the set sorted and deduplicated so cached_set_contains() works.
 */
static void
cached_set_build_all(struct cached_set *cs)
{
	int try;

	memset(cs, 0, sizeof (*cs));

	for (try = 0; try < UMEM_SNAP_TRIES; try++) {
		cs->n = 0;
		cs->needed = 0;
		for_each_cache(cached_set_build_visit, cs);
		if (cs->needed <= cs->cap)
			break;

		size_t want = cs->needed + cs->needed / 4 + UMEM_SNAP_SLACK;
		void **na = realloc(cs->addrs, want * sizeof (*na));
		if (na == NULL)
			break;
		cs->addrs = na;
		cs->cap = want;
	}

	cached_set_finish(cs);
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
 * Tell the user what the count does and does not include.  The subtraction is
 * only as good as the set of retention sites we can enumerate, and the honest
 * framing is "outstanding allocations", not "leaks": a buffer the application
 * legitimately still owns is outstanding too.
 */
static void
findleaks_emit_cache_note(FILE *out, umem_inspect_format_t fmt,
    size_t cached_skipped, int unaccounted)
{
	if (fmt != UMEM_FMT_TEXT)
		return;
	if (cached_skipped != 0)
		(void) fprintf(out,
		    "(subtracted %zu buffer%s resident in magazines / per-CPU "
		    "caches / rseq magazines)\n",
		    cached_skipped, cached_skipped == 1 ? "" : "s");
	if (unaccounted)
		(void) fputs(
		    "(the per-thread cache is enabled: buffers retained in "
		    "another thread's PTC bins or per-thread magazines cannot "
		    "be enumerated and are counted as outstanding.  Run with "
		    "UMEM_OPTIONS=tcache=0 for an exact count.)\n", out);
	(void) fputc('\n', out);
}

size_t
umem_findleaks(FILE *out, umem_inspect_format_t fmt, unsigned max_classes)
{
	struct leak_state st;
	struct cached_set cs;
	struct snap snap;
	int truncated;
	int unaccounted;

	if (out == NULL)
		out = stderr;
	if (max_classes == 0)
		max_classes = 50;

	(void) umem_stacktrace_init();
	memset(&st, 0, sizeof (st));

	/*
	 * Phase 1 (under locks): build the cached-buffer set so still-cached
	 * buffers can be subtracted.  Without this, any free() that landed in
	 * a magazine would look outstanding.
	 */
	cached_set_build_all(&cs);
	unaccounted = cached_set_has_unaccounted();

	/*
	 * Phase 2 (under locks): snapshot the allocated buffers of every
	 * user-visible cache.  Allocator-internal caches (UMC_NOHASH) and
	 * vmem quantum caches (UMC_QCACHE, whose buffers ARE user caches'
	 * slabs) are excluded; umem_walk_allocated() gives the full walk.
	 *
	 * This used to walk the cache list WITHOUT umem_cache_lock, so a
	 * concurrent umem_cache_destroy() could free a cache out from under
	 * the walk.  collect_allocated_user() holds it for the whole walk (C1).
	 */
	truncated = (snap_collect(&snap, collect_allocated_user, NULL) != 0);

	/*
	 * Phase 3 (NO locks held): classify.  leak_classify() calloc()s a
	 * class per distinct stack, which is exactly the allocation that must
	 * not happen inside the walk (C2).
	 */
	size_t visited = snap.n;
	size_t cached_skipped = 0;
	for (size_t i = 0; i < snap.n; i++) {
		if (cached_set_contains(&cs, snap.recs[i].addr)) {
			cached_skipped++;
			continue;
		}
		(void) leak_classify(&snap.recs[i], &st);
	}
	snap_free(&snap);

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
		    "\"ptc_unaccounted\":%s,\"truncated\":%s,"
		    "\"classes\":[",
		    UMEM_INSPECT_VERSION, st.total_count, st.total_bytes,
		    cached_skipped, unaccounted ? "true" : "false",
		    truncated ? "true" : "false");
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
		/*
		 * "outstanding allocations", not "leaks": every buffer here is
		 * one the allocator has handed out and not got back, which
		 * includes memory the application legitimately still owns.
		 * Grouping by stack is what makes it useful -- a class that
		 * grows across successive reports is the signal.
		 */
		(void) fprintf(out,
		    "findleaks: %zu outstanding allocation%s (%zu bytes) in "
		    "%zu distinct stack class%s\n",
		    st.total_count, st.total_count == 1 ? "" : "s",
		    st.total_bytes, st.nclasses,
		    st.nclasses == 1 ? "" : "es");
		findleaks_emit_cache_note(out, fmt, cached_skipped,
		    unaccounted);
		if (truncated)
			(void) fputs(
			    "warning: the allocation set grew faster than it "
			    "could be snapshotted; report is INCOMPLETE\n\n",
			    out);

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
 *
 * Collect every cache's counters under its cache_lock (and umem_cache_lock
 * for the list), then format with no lock held (C2).  This used to fprintf
 * directly from the visitor, i.e. run stdio -- which allocates -- under both
 * locks.
 * ------------------------------------------------------------------------ */

struct status_rec {
	char name[UMEM_CACHE_NAMELEN + 1];
	size_t bufsize;
	uint64_t inuse;		/* handed out by the SLAB layer; see below */
	uint64_t total;
	uint64_t mem;
	uint64_t alloc_ops;
	uint64_t alloc_fail;
	uint64_t depot_contention;
	int flags;
};

struct status_ctx {
	struct status_rec *recs;
	size_t cap;
	size_t n;
	size_t needed;
};

/*
 * Sum the per-CPU cc_alloc counters, which is what cache_alloc_ops is
 * supposed to hold.  cache_alloc_ops itself is only refreshed inside the
 * optional umem_magazine_tuning branch of umem_cache_update(), so reading the
 * field directly reports zero in a default build and a stale value otherwise.
 * Recomputing here makes the ALLOCS column mean what its header says.
 *
 * Caller holds cache_lock; cc_alloc is a plain counter written under cc_lock,
 * so the sum is approximate by construction (C4) -- it is a monotonically
 * increasing statistic, not an invariant.
 */
static uint64_t
cache_alloc_ops_now(umem_cache_t *cp)
{
	uint64_t allocs = 0;
	uint32_t ci;

	for (ci = 0; ci <= cp->cache_cpu_mask; ci++) {
		umem_cpu_cache_t *tc = (umem_cpu_cache_t *)((char *)cp +
		    umem_cpus[ci].cpu_cache_offset);
		allocs += tc->cc_alloc;
	}
	return (allocs);
}

static void
status_visit(umem_cache_t *cp, void *arg)
{
	struct status_ctx *ctx = arg;
	struct status_rec r;

	ctx->needed++;
	if (ctx->n >= ctx->cap)
		return;

	memset(&r, 0, sizeof (r));
	(void) mutex_lock(&cp->cache_lock);
	(void) strncpy(r.name, cp->cache_name, sizeof (r.name) - 1);
	r.bufsize = cp->cache_bufsize;
	r.inuse = cp->cache_slab_alloc - cp->cache_slab_free;
	r.total = cp->cache_buftotal;
	r.mem = (cp->cache_slab_create - cp->cache_slab_destroy)
	    * cp->cache_slabsize;
	r.alloc_ops = cache_alloc_ops_now(cp);
	r.alloc_fail = cp->cache_alloc_fail;
	r.depot_contention = cp->cache_depot_contention;
	r.flags = cp->cache_flags;
	(void) mutex_unlock(&cp->cache_lock);

	ctx->recs[ctx->n++] = r;
}

void
umem_status_dump(FILE *out, umem_inspect_format_t fmt)
{
	struct status_ctx ctx;
	int try;

	if (out == NULL)
		out = stderr;
	memset(&ctx, 0, sizeof (ctx));

	/* Phase 1: collect under the locks, growing outside them. */
	for (try = 0; try < UMEM_SNAP_TRIES; try++) {
		ctx.n = 0;
		ctx.needed = 0;
		for_each_cache(status_visit, &ctx);
		if (ctx.needed <= ctx.cap)
			break;
		struct status_rec *nr = realloc(ctx.recs,
		    (ctx.needed + 16) * sizeof (*nr));
		if (nr == NULL)
			break;
		ctx.recs = nr;
		ctx.cap = ctx.needed + 16;
	}

	/* Phase 2: emit with no lock held. */
	if (fmt == UMEM_FMT_JSON) {
		(void) fputs("{\"caches\":[", out);
		for (size_t i = 0; i < ctx.n; i++) {
			struct status_rec *r = &ctx.recs[i];
			(void) fprintf(out, "%s{\"name\":", i ? "," : "");
			json_escape(out, r->name);
			(void) fprintf(out,
			    ",\"bufsize\":%zu,\"inuse\":%" PRIu64
			    ",\"total\":%" PRIu64 ",\"memory\":%" PRIu64
			    ",\"alloc_ops\":%" PRIu64
			    ",\"alloc_fail\":%" PRIu64
			    ",\"depot_contention\":%" PRIu64
			    ",\"flags\":%d}",
			    r->bufsize, r->inuse, r->total, r->mem,
			    r->alloc_ops, r->alloc_fail,
			    r->depot_contention, r->flags);
		}
		(void) fputs("]}\n", out);
	} else {
		/*
		 * INUSE is cache_slab_alloc - cache_slab_free: buffers the
		 * SLAB layer has handed upward.  A buffer parked in a
		 * magazine, an rseq magazine or a PTC bin is free to the
		 * application but still counted here.  The header says so.
		 */
		(void) fprintf(out,
		    "%-24s %8s %8s %8s %12s %10s %6s\n",
		    "CACHE", "BUFSIZE", "HELD", "TOTAL", "MEMORY",
		    "ALLOCS", "FAIL");
		(void) fprintf(out,
		    "------------------------ -------- -------- -------- "
		    "------------ ---------- ------\n");
		for (size_t i = 0; i < ctx.n; i++) {
			struct status_rec *r = &ctx.recs[i];
			(void) fprintf(out,
			    "%-24.24s %8zu %8" PRIu64 " %8" PRIu64
			    " %12" PRIu64 " %10" PRIu64 " %6" PRIu64 "\n",
			    r->name, r->bufsize, r->inuse, r->total, r->mem,
			    r->alloc_ops, r->alloc_fail);
		}
		(void) fputs(
		    "\nHELD = buffers held above the slab layer "
		    "(includes magazine/PTC-resident buffers that the "
		    "application has already freed).\n", out);
	}

	free(ctx.recs);
}

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
	struct cached_set cs;

	if (addr == NULL || out == NULL) {
		errno = EINVAL;
		return (-1);
	}
	struct whatis_ctx ctx = { addr, out, {0}, 0, 0 };
	for_each_cache(whatis_visit, &ctx);
	if (!ctx.found) {
		if (!ctx.have_fallback) {
			errno = ENOENT;
			return (-1);
		}
		*out = ctx.fallback;
	}

	/*
	 * Distinguish CACHED from ALLOCATED.  umem_inspect.h documents
	 * UMEM_BUF_CACHED ("sitting in a magazine / PTC") and this function
	 * never returned it: the slab layer still considers a
	 * magazine-resident buffer handed out, and the freelist check above
	 * only sees the slab freelist, so a buffer the application has already
	 * freed was reported ALLOCATED.
	 *
	 * The magazine set is enumerable, so check it.  A PTC-resident buffer
	 * still reports ALLOCATED -- thread-local bins are unreachable from
	 * here (see cached_set_has_unaccounted()).
	 */
	if (out->state == UMEM_BUF_ALLOCATED) {
		cached_set_build_all(&cs);
		if (cached_set_contains(&cs, out->addr))
			out->state = UMEM_BUF_CACHED;
		cached_set_destroy(&cs);
	}
	return (0);
}

static const char *
buf_state_name(int state)
{
	switch (state) {
	case UMEM_BUF_ALLOCATED:	return ("ALLOCATED");
	case UMEM_BUF_FREE:		return ("FREE");
	case UMEM_BUF_CACHED:		return ("CACHED");
	default:			return ("UNKNOWN");
	}
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
	    info.size == 1 ? "" : "s", buf_state_name(info.state));
	if (info.state == UMEM_BUF_CACHED)
		(void) fputs("  (freed by the application; still resident in "
		    "a magazine, so the slab layer counts it as held)\n", out);
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
		    : info->state == UMEM_BUF_CACHED
		    ? "CACHD"
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
	struct ump_v2_cache_summary *cache_summary;
	size_t cache_count;
	size_t cache_cap;
	size_t cache_needed;
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

/*
 * Cache-summary collection, two-phase like everything else (C2): the summary
 * records are filled under the locks into a pre-sized table, then written.
 * This used to realloc() inside the locked cache walk and fwrite() while
 * holding cache_lock.
 */
static void
snapshot_collect_cache(umem_cache_t *cp, void *arg)
{
	struct snapshot_state *st = arg;
	struct ump_v2_cache_summary *cs;

	st->cache_needed++;
	if (st->cache_count >= st->cache_cap)
		return;

	st->cache_table[st->cache_count] = cp;
	cs = &st->cache_summary[st->cache_count];
	memset(cs, 0, sizeof (*cs));

	(void) mutex_lock(&cp->cache_lock);
	(void) strncpy(cs->name, cp->cache_name, sizeof (cs->name) - 1);
	cs->bufsize = cp->cache_bufsize;
	cs->inuse = cp->cache_slab_alloc - cp->cache_slab_free;
	cs->total = cp->cache_buftotal;
	cs->memory = (cp->cache_slab_create - cp->cache_slab_destroy)
	    * cp->cache_slabsize;
	cs->alloc_ops = cache_alloc_ops_now(cp);
	cs->alloc_fail = cp->cache_alloc_fail;
	cs->depot_contention = cp->cache_depot_contention;
	cs->flags = (uint32_t)cp->cache_flags;
	cs->cflags = (uint32_t)cp->cache_cflags;
	(void) mutex_unlock(&cp->cache_lock);

	st->cache_count++;
}

/* Collect all cache summaries, growing between attempts with no lock held. */
static int
snapshot_collect_caches(struct snapshot_state *st)
{
	int try;

	for (try = 0; try < UMEM_SNAP_TRIES; try++) {
		st->cache_count = 0;
		st->cache_needed = 0;
		for_each_cache(snapshot_collect_cache, st);
		if (st->cache_needed <= st->cache_cap)
			return (0);

		size_t want = st->cache_needed + 16;
		umem_cache_t **nt = realloc(st->cache_table,
		    want * sizeof (*nt));
		if (nt == NULL)
			return (ENOMEM);
		st->cache_table = nt;
		struct ump_v2_cache_summary *ns = realloc(st->cache_summary,
		    want * sizeof (*ns));
		if (ns == NULL)
			return (ENOMEM);
		st->cache_summary = ns;
		st->cache_cap = want;
	}
	return (0);	/* best effort: write what we have */
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

	/* P5.3: fdopen(umem_open_write()) rather than fopen(path, "wb"):
	 * O_NOFOLLOW + regular-file/owner checks on the fd, no O_TRUNC before
	 * them.  `path` reaches here from a debugger or umemctl, i.e. from
	 * outside.  See misc.c:umem_open_write. */
	int sfd = umem_open_write(path);
	if (sfd < 0)
		return (-1);
	st.fp = fdopen(sfd, "wb");
	if (st.fp == NULL) {
		(void) close(sfd);
		return (-1);
	}

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

	/* Phase 1: collect the cache summaries (locked) -- the index every
	 * buffer record refers to. */
	st.err = snapshot_collect_caches(&st);
	if (st.err)
		goto out;

	/* Phase 2: write the summary table (unlocked). */
	for (size_t i = 0; i < st.cache_count; i++) {
		if (fwrite(&st.cache_summary[i],
		    sizeof (st.cache_summary[i]), 1, st.fp) != 1) {
			st.err = errno ? errno : EIO;
			goto out;
		}
	}

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
	free(st.cache_summary);
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

	/* P5.3: same as snapshot_v2_write above -- O_NOFOLLOW-checked fd
	 * instead of fopen(path, "w"). */
	int tfd = umem_open_write(path);
	if (tfd < 0)
		return (-1);
	FILE *fp = fdopen(tfd, "w");
	if (fp == NULL) {
		(void) close(tfd);
		return (-1);
	}
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

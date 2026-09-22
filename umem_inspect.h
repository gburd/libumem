/*
 * umem_inspect.h -- runtime introspection API for libumem.
 *
 * Surfaces the information libumem already maintains under UMEM_DEBUG
 * so that it is accessible at runtime -- from application code, from a
 * debugger (gdb/lldb) via `call`, and from the umem_tool(1) CLI.
 *
 * The Solaris mdb workflow was:
 *     $ mdb -p <pid>
 *     > ::findleaks
 *     > ::umem_log | ::umem_logs -v
 *     > ::umastat
 *     > ::whatis <addr>
 *     > ::bufctl_audit <bufctl>
 *
 * This header provides equivalents that work on Linux/FreeBSD without
 * mdb.  Every ::foo dcmd has a umem_foo() C entry point here, plus a
 * JSON variant for tool consumption.
 *
 * Thread-safety and locking contract (implemented in umem_inspect.c, where
 * the clauses are labelled C1..C5):
 *
 *   C1 CACHE LIFETIME.  Every cache-list walk holds umem_cache_lock for its
 *      whole duration, so a cache cannot be destroyed underneath it.
 *      Inspection therefore BLOCKS umem_cache_create()/umem_cache_destroy()
 *      while it collects.  It does not block allocation.
 *
 *   C2 NO ALLOCATION UNDER ALLOCATOR LOCKS.  Collection copies into a buffer
 *      sized before any lock is taken; formatting, sorting, file I/O and YOUR
 *      CALLBACK all run with every allocator lock released.  A umem_walk_*()
 *      callback may therefore allocate, do I/O, and call back into libumem.
 *
 *   C3 SNAPSHOT SCOPE.  Per cache, the collected set is consistent.  ACROSS
 *      caches it is not one instant: cache A is read before cache B and
 *      allocation continues in between.  A set that outgrows the snapshot
 *      buffer is reported as truncated, never silently shortened.
 *
 *   C4 WHAT MAY BE TORN.  Counters are read under the lock that writes them,
 *      so each is whole, but a set of them is not a single instant.  The rseq
 *      per-CPU magazine state has no mutex and is read best-effort.  The
 *      transaction log's lh_lock covers only chunk rotation -- records are
 *      written under a per-CPU lock, so a record can be observed half-written;
 *      implausible records are dropped.
 *
 *   C5 DEBUGGER USE.  These functions take allocator mutexes.  Calling them
 *      from a debugger against a target stopped while one of those mutexes was
 *      held will hang the call.  Retry, or snapshot from inside the process.
 *
 * Debug requirements: most functions work at all UMEM_DEBUG levels but
 * produce richer output at higher levels.  Specifically:
 *   - findleaks requires UMF_AUDIT (UMEM_DEBUG=audit) for stack traces;
 *     without audit it still reports counts and cache names.
 *   - log_dump requires UMEM_LOGGING=transaction=<size> to be non-empty.
 *   - whatis/bufctl_audit work at any debug level but stack traces
 *     require UMF_AUDIT.
 */

#ifndef UMEM_INSPECT_H
#define UMEM_INSPECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Shared structures
 * ------------------------------------------------------------------------ */

#define	UMEM_INSPECT_MAX_STACK		32
#define	UMEM_INSPECT_VERSION		1

/*
 * Describes one buffer (allocated or freed) discovered during a walk.
 * Fields whose source is not enabled (e.g. stack[] without UMF_AUDIT)
 * are zeroed.
 */
typedef struct umem_buffer_info {
	void		*addr;			/* buffer address */
	size_t		size;			/* buffer size in bytes */
	const char	*cache_name;		/* cache that owns the buffer */
	void		*cache;			/* opaque umem_cache_t * */
	void		*slab;			/* opaque umem_slab_t * */
	void		*bufctl;		/* opaque bufctl pointer */
	uint64_t	timestamp;		/* bc_timestamp (ns) or 0 */
	uint64_t	thread;			/* thr_self() at last txn */
	int		depth;			/* # valid stack[] entries */
	int		state;			/* UMEM_BUF_ALLOC / _FREE / _UNKNOWN */
	uintptr_t	stack[UMEM_INSPECT_MAX_STACK];
} umem_buffer_info_t;

#define	UMEM_BUF_UNKNOWN	0
#define	UMEM_BUF_ALLOCATED	1	/* held above the slab layer */
#define	UMEM_BUF_FREE		2	/* on a slab freelist */
#define	UMEM_BUF_CACHED		3	/* freed, resident in a magazine */

/* Callback signature for umem_walk_*.  Returning non-zero stops the walk.
 * Runs with NO allocator lock held (C2): it may allocate and do I/O. */
typedef int (*umem_buffer_cb_t)(const umem_buffer_info_t *info, void *arg);

/* ------------------------------------------------------------------------
 * Output format selection
 * ------------------------------------------------------------------------ */

typedef enum umem_inspect_format {
	UMEM_FMT_TEXT = 0,	/* human-readable, default */
	UMEM_FMT_JSON = 1,	/* one JSON object per line, or a JSON array */
	UMEM_FMT_CSV  = 2	/* one allocation per row */
} umem_inspect_format_t;

/* ------------------------------------------------------------------------
 * Primary dcmds
 * ------------------------------------------------------------------------ */

/*
 * ::findleaks equivalent.  Reports OUTSTANDING ALLOCATIONS grouped by
 * allocation stack fingerprint, ranked by total bytes -- not proven leaks: a
 * buffer the application legitimately still owns is outstanding too.
 *
 * Buffers resident in per-CPU magazines, depot magazines and rseq per-CPU
 * magazines are subtracted (reported as cached_skipped).  NOT subtracted:
 * buffers retained in another thread's per-thread cache (PTC), because
 * thread-local bins have no process-wide registry -- the report sets
 * ptc_unaccounted when PTC is enabled.  Oversize allocations (served straight
 * from a vmem arena, bypassing the caches) are not accounted at all.
 *
 * Treat the count as an UPPER BOUND; the signal is a class that grows across
 * successive reports.
 *
 * Without UMF_AUDIT there are no stack traces, so classes cannot be
 * distinguished; counts per cache are still accurate.
 *
 * Returns the total number of outstanding buffers encountered.  Emits output
 * to `out`; if NULL, stderr is used.
 */
size_t umem_findleaks(FILE *out, umem_inspect_format_t fmt,
    unsigned max_classes);

/*
 * ::umem_log / ::umem_logs -v equivalent.  Walks the transaction log
 * (per-CPU ring buffer) and emits every audit record in chronological
 * order by bc_timestamp.  Requires UMEM_LOGGING=transaction=<size>.
 */
size_t umem_log_dump(FILE *out, umem_inspect_format_t fmt,
    unsigned max_records);

/*
 * ::umastat equivalent.  Per-cache summary: bufsize, buffers HELD above the
 * slab layer, total, memory in use, successful allocs, failed allocs, depot
 * contention.
 *
 * HELD is cache_slab_alloc - cache_slab_free.  A buffer the application has
 * already freed but which still sits in a magazine, rseq magazine or PTC bin
 * is counted: it is not a measure of application memory in use.
 */
void umem_status_dump(FILE *out, umem_inspect_format_t fmt);

/*
 * ::whatis <addr> equivalent.  Resolves a pointer to a cache + slab + buffer +
 * state (UMEM_BUF_ALLOCATED / _FREE / _CACHED).  Fills `out` on success.
 * Returns 0 on success, -1 if addr is not in any umem-owned region.
 *
 * A buffer retained in another thread's PTC bin reports ALLOCATED, since
 * thread-local bins are not enumerable from outside the owning thread.
 */
int umem_whatis(const void *addr, umem_buffer_info_t *out);

/*
 * ::bufctl_audit equivalent.  Pretty-prints the audit record (if any)
 * for `addr`.  Addr may be a buffer pointer, a bufctl pointer, or a
 * pointer into a log chunk; all three are resolved.
 */
int umem_bufctl_audit_dump(FILE *out, const void *addr);

/* ------------------------------------------------------------------------
 * Walkers (used by the dcmds above; also directly useful).
 * ------------------------------------------------------------------------ */

/*
 * Walk every allocated buffer in every cache.  For caches with UMF_HASH
 * this iterates cache_hash_table entries; for non-hashed caches it
 * walks the slab list.  Returns the number of buffers visited.
 */
size_t umem_walk_allocated(umem_buffer_cb_t cb, void *arg);

/*
 * Walk every buffer parked on a slab freelist.
 */
size_t umem_walk_freed(umem_buffer_cb_t cb, void *arg);

/*
 * Walk the transaction log in chronological order.
 */
size_t umem_walk_log(umem_buffer_cb_t cb, void *arg);

/*
 * ::walk equivalent: stream every entry in `kind` to `out` in the
 * requested format.  `kind` is one of "allocated", "freed", "log".
 * Returns the number of entries emitted.
 */
size_t umem_walk_dump(FILE *out, const char *kind,
    umem_inspect_format_t fmt, unsigned max_entries);

/* ------------------------------------------------------------------------
 * Event hooks (for debugger breakpoints).
 *
 * These are empty functions the allocator calls at well-defined points.
 * Set a breakpoint on one to break on the condition:
 *
 *   (gdb) break umem_event_alloc if size > 1048576
 *   (gdb) break umem_event_error
 *   (gdb) break umem_event_free  if buf == 0x7fff12340000
 *
 * ~1 cycle overhead per alloc/free when no breakpoint is set because
 * gcc cannot inline-optimize the extern call without LTO, but the call
 * is unconditional.  Gated on UMEM_INSPECT_EVENTS=1 in the env.
 * ------------------------------------------------------------------------ */

/* Called from allocator hot path when events are enabled. */
extern void umem_event_alloc(void *buf, size_t size, void *cache);
extern void umem_event_free(void *buf, size_t size, void *cache);
extern void umem_event_error(int code, void *buf, void *cache);

/* Enable/disable events.  Also set via UMEM_INSPECT_EVENTS=1 at process
 * start.  Must be set before the first allocation to take full effect,
 * but can be toggled later (existing buffers just won't have been seen). */
void umem_inspect_enable_events(int on);

/* ------------------------------------------------------------------------
 * Structured hook callback (C API, not breakpoint).
 * ------------------------------------------------------------------------ */

typedef enum {
	UMEM_EV_ALLOC = 0,
	UMEM_EV_FREE,
	UMEM_EV_DOUBLE_FREE,
	UMEM_EV_CORRUPT,
	UMEM_EV_ALLOC_FAIL,
	UMEM_EV__COUNT
} umem_event_t;

typedef void (*umem_event_cb_t)(umem_event_t ev,
    const umem_buffer_info_t *info, void *arg);

void umem_inspect_set_event_cb(umem_event_cb_t cb, void *arg,
    unsigned event_mask);

/* ------------------------------------------------------------------------
 * Snapshot / restore (for offline post-mortem).
 * ------------------------------------------------------------------------ */

/*
 * Serialize the current allocation state to `path` in a self-describing
 * binary format.  The umem_tool(1) command can dump/findleaks/status
 * against the snapshot without the original process being alive.
 */
int umem_inspect_snapshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* UMEM_INSPECT_H */

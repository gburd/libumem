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
 * Thread-safety: inspection functions acquire cache_lock on every cache
 * they visit.  They are safe to call while the target is running but
 * will serialize with concurrent allocations in each cache they touch.
 * For a wedged or crashed target, prefer calling from a debugger so that
 * no locks are actually required (the target is stopped).
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
#define	UMEM_BUF_ALLOCATED	1
#define	UMEM_BUF_FREE		2	/* on a slab freelist */
#define	UMEM_BUF_CACHED		3	/* sitting in a magazine / PTC */

/* Callback signature for umem_walk_*.  Returning non-zero stops the walk. */
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
 * ::findleaks equivalent.  Walks every cache that has UMF_HASH set and
 * enumerates live bufctls; when UMF_AUDIT is also set, groups them by
 * stack-trace fingerprint and reports the N largest leak classes.
 *
 * Without UMF_AUDIT, still reports counts per cache and a stack-less
 * summary (useful to point UMEM_DEBUG=audit at the offending workload).
 *
 * Returns the total number of live buffers encountered (possibly leaked).
 * Emits output to `out`; if NULL, stderr is used.
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
 * ::umastat equivalent.  Per-cache summary: bufsize, inuse, total,
 * memory in use, successful allocs, failed allocs, depot contention.
 */
void umem_status_dump(FILE *out, umem_inspect_format_t fmt);

/*
 * ::whatis <addr> equivalent.  Given any pointer, attempt to resolve it
 * to a cache + slab + buffer + state.  Fills `out` on success.  Returns
 * 0 on success, -1 if addr is not in any umem-owned region.
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

/*
 * Application allocator hooks for libumem
 *
 * This API allows applications to register custom allocators (like PostgreSQL's
 * palloc) with umem for tracking and debugging purposes.
 *
 * ------------------------------------------------------------------------
 * CONCURRENCY CONTRACT (what this API guarantees, and what it does not)
 * ------------------------------------------------------------------------
 *
 * L1. LIFETIME.  umem_hook_unregister() does not return until no thread is
 *     executing inside the hook.  Concretely: every umem_hook_track_*() call
 *     holds a reference on the hook across BOTH the user callback and the
 *     statistics update, and unregister unlinks the hook, marks it inactive
 *     (so no new reference can be taken) and then waits for the reference
 *     count to reach zero.  Therefore the caller MAY free or reuse the hook
 *     structure as soon as umem_hook_unregister() returns.
 *
 *     Corollary: if a hook callback blocks forever, unregister blocks with
 *     it.  That is the price of the guarantee; there is no forced revoke.
 *
 *     Corollary: a hook callback must not call umem_hook_unregister() on its
 *     own hook -- it would wait for itself.  Self-deadlock, not corruption.
 *
 * L2. NO USER CODE UNDER THE REGISTRY LOCK.  The registry mutex is never
 *     held while a user callback runs (hook_alloc/hook_free/hook_realloc, or
 *     a umem_hook_walk() callback).  A callback may therefore register,
 *     unregister, walk, dump, or track through this API, and may allocate.
 *
 * L3. STATISTICS ARE NOT A CONSISTENT SNAPSHOT.  Each counter update is
 *     serialized by the registry lock, but a reader that looks at two
 *     counters can see them from different points in time.  Read them only
 *     for reporting.
 *
 * L4. umem_hook_walk() visits every hook registered for the whole duration
 *     of the walk exactly once.  Hooks registered during the walk may or may
 *     not be visited; hooks unregistered during the walk are not visited
 *     after they go away.  A callback returning nonzero stops the walk.
 *     Walks are serialized against each other, so a walk callback must not
 *     start a nested walk (self-deadlock).
 *
 * L5. umem_hook_find() returns a bare pointer and takes no reference.  It is
 *     meant for looking up a hook you own.  If another component can
 *     unregister that hook concurrently, the pointer can go stale the instant
 *     this function returns; this API offers no way to pin it.
 */

#ifndef _UMEM_HOOKS_H
#define _UMEM_HOOKS_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hook function signatures
 */
typedef void *(*umem_hook_alloc_f)(size_t size, void *arg);
typedef void (*umem_hook_free_f)(void *ptr, void *arg);
typedef void *(*umem_hook_realloc_f)(void *ptr, size_t size, void *arg);

/*
 * Application allocator hook structure
 */
typedef struct umem_hook {
	/* Hook identification */
	const char *hook_name;		/* Name (e.g., "palloc") */

	/* Hook functions */
	umem_hook_alloc_f hook_alloc;	/* Allocation function */
	umem_hook_free_f hook_free;	/* Free function */
	umem_hook_realloc_f hook_realloc; /* Realloc function (optional) */

	/* Application-specific context */
	void *hook_arg;			/* Passed to all hook functions */

	/* Statistics (maintained by umem) */
	uint64_t alloc_count;		/* Number of allocations */
	uint64_t free_count;		/* Number of frees */
	uint64_t realloc_count;		/* Number of reallocs */
	uint64_t bytes_allocated;	/* Total bytes allocated */
	uint64_t bytes_freed;		/* Total bytes freed */
	uint64_t bytes_current;		/* Currently allocated bytes */
	uint64_t peak_bytes;		/* Peak memory usage */

	/* Internal use -- do not touch; all fields below are owned by the
	 * registry lock inside umem_hooks.c. */
	struct umem_hook *hook_next;	/* Next in hook list */
	struct umem_hook *hook_prev;	/* Previous in hook list */
	int hook_active;		/* Hook is active */
	int hook_refcnt;		/* In-flight track_*/walk callers (L1) */
	uint32_t hook_walk_gen;		/* Last walk that visited this hook */
} umem_hook_t;

/*
 * Hook registration
 *
 * umem_hook_unregister() drains in-flight callbacks before returning; see L1
 * above.  Freeing the hook structure immediately after it returns is safe.
 */
int umem_hook_register(umem_hook_t *hook);
void umem_hook_unregister(umem_hook_t *hook);

/*
 * Hook tracking functions
 *
 * Applications should call these to update statistics when using
 * the hook allocator.
 */
void *umem_hook_track_alloc(umem_hook_t *hook, size_t size);
void umem_hook_track_free(umem_hook_t *hook, void *ptr, size_t size);
void *umem_hook_track_realloc(umem_hook_t *hook, void *ptr,
    size_t old_size, size_t new_size);

/*
 * Hook statistics and debugging
 */
void umem_hook_dump(FILE *fp);
void umem_hook_dump_one(FILE *fp, umem_hook_t *hook);

/*
 * Look up a hook by name.  Returns a bare, unreferenced pointer; see L5.
 */
umem_hook_t *umem_hook_find(const char *name);

/*
 * Hook iterator
 *
 * Calls func(hook, arg) for each registered hook, with no registry lock held
 * (see L2/L4).  A nonzero return from func stops the walk.
 *
 * Returns 0 if every callback returned 0, -1 if a callback returned nonzero
 * (i.e. the walk was stopped early) or func was NULL.
 */
typedef int (*umem_hook_walk_f)(umem_hook_t *hook, void *arg);
int umem_hook_walk(umem_hook_walk_f func, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_HOOKS_H */

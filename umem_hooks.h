/*
 * Application allocator hooks for libumem
 *
 * This API allows applications to register custom allocators (like PostgreSQL's
 * palloc) with umem for tracking and debugging purposes.
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

	/* Internal use */
	struct umem_hook *hook_next;	/* Next in hook list */
	struct umem_hook *hook_prev;	/* Previous in hook list */
	int hook_active;		/* Hook is active */
} umem_hook_t;

/*
 * Hook registration
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
umem_hook_t *umem_hook_find(const char *name);

/*
 * Hook iterator
 *
 * Allows applications to iterate through all registered hooks.
 */
typedef int (*umem_hook_walk_f)(umem_hook_t *hook, void *arg);
int umem_hook_walk(umem_hook_walk_f func, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_HOOKS_H */

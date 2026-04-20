/*
 * umem_palloc.h - PostgreSQL MemoryContext backed by libumem
 *
 * Minimal PostgreSQL MemoryContext types for standalone compilation.
 * In a real integration, include postgres.h instead.
 *
 * This header demonstrates how to map PostgreSQL's per-context memory
 * management onto umem's slab allocator, gaining debug features like
 * buffer auditing, redzone checking, and guard pages on a per-context
 * basis.
 */

#ifndef UMEM_PALLOC_H
#define UMEM_PALLOC_H

#include <umem.h>
#include <stddef.h>

/*
 * Context creation flags -- each maps to a set of umem debug features.
 * These can be combined with bitwise OR.
 */
#define UMEM_CTX_DEBUG_AUDIT    0x01  /* enable UMF_AUDIT on this context */
#define UMEM_CTX_DEBUG_GUARDS   0x02  /* enable deadbeef+redzone */
#define UMEM_CTX_DEBUG_FIREWALL 0x04  /* enable guard pages */
#define UMEM_CTX_TRACK_OWNER    0x08  /* enable ownership tracking */

/*
 * Opaque context handle. Internally manages a set of umem caches
 * covering common allocation sizes (power-of-two size classes).
 */
typedef struct UmemContext UmemContext;

/*
 * Create a new memory context. name is used for diagnostics.
 * block_size is the maximum single allocation size supported
 * (allocations larger than block_size will fall through to
 * umem_alloc directly). flags is a combination of UMEM_CTX_*
 * constants.
 */
UmemContext *umem_context_create(const char *name,
    size_t block_size, int flags);

/* Allocate size bytes from context (uninitialized). */
void *umem_context_alloc(UmemContext *ctx, size_t size);

/* Allocate size bytes from context (zero-filled). */
void *umem_context_alloc0(UmemContext *ctx, size_t size);

/* Free a previous allocation. size must match the original request. */
void umem_context_free(UmemContext *ctx, void *ptr, size_t size);

/* Reallocate: grow or shrink. old_size must match original request. */
void *umem_context_realloc(UmemContext *ctx, void *ptr,
    size_t old_size, size_t new_size);

/* Destroy all caches, recreate them. All prior allocations invalid. */
void umem_context_reset(UmemContext *ctx);

/* Destroy the context and free all resources. */
void umem_context_delete(UmemContext *ctx);

/* Print context statistics to stderr. */
void umem_context_stats(UmemContext *ctx);

#endif /* UMEM_PALLOC_H */

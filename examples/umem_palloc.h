/*
 * umem_palloc.h - Budget-based memory contexts backed by libumem
 *
 * Provides PostgreSQL-style per-context memory management with:
 *   - Memory budgets with enforcement and backpressure
 *   - Pre-allocated backing via mmap(MAP_POPULATE)
 *   - Shared memory support for multi-process access
 *   - Parent/child context hierarchy
 *   - Per-context debug flags (audit, guards, ownership)
 *
 * Each UmemBudgetContext wraps a vmem arena. The budget caps how much
 * memory the arena can dispense. When the budget is exhausted, callers
 * either block (backpressure), get NULL, or abort, depending on flags.
 */

#ifndef UMEM_PALLOC_H
#define UMEM_PALLOC_H

#include <umem.h>
#include <sys/vmem.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

/* Context flags */
#define UMEM_BUDGET_PREALLOC   0x0001  /* mmap entire budget upfront */
#define UMEM_BUDGET_SHARED     0x0002  /* backed by POSIX shared memory */
#define UMEM_BUDGET_AUDIT      0x0010  /* enable UMF_AUDIT on caches */
#define UMEM_BUDGET_GUARDS     0x0020  /* enable redzone + deadbeef */
#define UMEM_BUDGET_OWN        0x0040  /* enable ownership tracking */
#define UMEM_BUDGET_NOWAIT     0x0100  /* return NULL when over budget */
#define UMEM_BUDGET_NOFAIL     0x0200  /* abort() when over budget */

typedef struct UmemBudgetContext UmemBudgetContext;

/*
 * Create a top-level budget context. budget is the maximum number of
 * bytes this context may have outstanding at any time.
 */
UmemBudgetContext *umem_budget_create(const char *name,
    size_t budget, int flags);

/*
 * Create a child context whose allocations count against both its own
 * budget and the parent's budget.
 */
UmemBudgetContext *umem_budget_create_child(
    UmemBudgetContext *parent, const char *name,
    size_t budget, int flags);

/* Destroy the context, its arena, and any backing memory. */
void umem_budget_delete(UmemBudgetContext *ctx);

/* Allocate size bytes (uninitialized). */
void *umem_budget_alloc(UmemBudgetContext *ctx, size_t size);

/* Allocate size bytes (zero-filled). */
void *umem_budget_alloc0(UmemBudgetContext *ctx, size_t size);

/* Free a previous allocation. size must match the original request. */
void umem_budget_free(UmemBudgetContext *ctx, void *ptr, size_t size);

/* Destroy and recreate the arena. All prior pointers are invalid. */
void umem_budget_reset(UmemBudgetContext *ctx);

/*
 * Shared memory contexts. umem_shared_create() creates a new POSIX
 * shared memory segment and builds a vmem arena on top of it.
 * umem_shared_attach() opens an existing segment by name.
 * umem_shared_detach() unmaps without unlinking the segment.
 */
UmemBudgetContext *umem_shared_create(const char *name,
    size_t size, int flags);
UmemBudgetContext *umem_shared_attach(const char *name);
void umem_shared_detach(UmemBudgetContext *ctx);

/* Query functions */
size_t umem_budget_used(UmemBudgetContext *ctx);
size_t umem_budget_available(UmemBudgetContext *ctx);
size_t umem_budget_peak(UmemBudgetContext *ctx);
int    umem_budget_under_pressure(UmemBudgetContext *ctx);
void   umem_budget_stats(UmemBudgetContext *ctx, FILE *fp);

#endif /* UMEM_PALLOC_H */

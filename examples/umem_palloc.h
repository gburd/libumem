/*
 * umem_palloc.h - Budget-based memory contexts backed by libumem
 *
 * EXAMPLE CODE.  This lives under examples/ because it is a demonstration
 * of building PostgreSQL-style memory contexts on vmem arenas, not a
 * supported part of the library.  It is not installed and not part of
 * libumem's API.
 *
 * Provides PostgreSQL-style per-context memory management with:
 *   - Memory budget *accounting* (see the warning below -- not enforcement)
 *   - Pre-allocated backing via mmap(MAP_POPULATE)
 *   - Shared memory support for multi-process access
 *   - Parent/child context hierarchy
 *   - Per-context debug flags (audit, guards, ownership)
 *
 * Each UmemBudgetContext wraps a vmem arena.
 */

#ifndef UMEM_PALLOC_H
#define UMEM_PALLOC_H

/*
 * EXPERIMENTAL API -- not production-ready, and NOT A BUDGET-ENFORCEMENT
 * MECHANISM.
 *
 * The "budget" is accounting, not a limit.  Do not use it as a memory cap,
 * an OOM guard, or a backpressure signal you depend on: allocations are
 * not reliably refused once the budget is exhausted, the accounting and
 * the allocation are not a single atomic decision, and the
 * UMEM_BUDGET_NOWAIT / UMEM_BUDGET_NOFAIL flags below do not make it one.
 * If you need a hard limit, impose it outside this API (cgroups,
 * setrlimit, or your own admission control).
 *
 * This API may change without notice.  See README.md.
 */
#if !defined(UMEM_ENABLE_EXPERIMENTAL) && !defined(_UMEM_INTERNAL)
#error "This header requires #define UMEM_ENABLE_EXPERIMENTAL before inclusion"
#endif

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
/*
 * The two flags below describe the INTENDED over-budget behaviour.  Neither
 * is reliably enforced today; see the warning at the top of this file.
 */
#define UMEM_BUDGET_NOWAIT     0x0100  /* intent: return NULL when over budget */
#define UMEM_BUDGET_NOFAIL     0x0200  /* intent: abort() when over budget */

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

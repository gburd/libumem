/*
 * umem_palloc.c - Budget-based memory contexts backed by libumem
 *
 * Demonstrates PostgreSQL-style per-context memory management with
 * memory budgets, backpressure, pre-allocation, shared memory, and
 * parent/child hierarchy.
 *
 * Build:  cd examples && make
 * Run:    LD_LIBRARY_PATH=../.libs ./umem_palloc_demo
 */

#define UMEM_ENABLE_EXPERIMENTAL
#include "umem_palloc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Import spans for dynamic (non-PREALLOC) arenas from libumem's heap
 * arena.  This is an exported libumem symbol (see vmem_base.h); we
 * declare it here rather than pulling in the internal header so the
 * example stays self-contained.
 */
extern vmem_t *vmem_heap_arena(vmem_alloc_t **, vmem_free_t **);

/* Pressure threshold: 90% of budget */
#define PRESSURE_PCT 90

/* Backpressure wait timeout (100ms) */
#define WAIT_TIMEOUT_MS 100

/* Maximum retry attempts before giving up */
#define MAX_RETRIES 50

struct UmemBudgetContext {
	char name[64];
	vmem_t *arena;
	size_t budget;
	size_t reserved;
	int flags;
	void *backing;
	size_t backing_size;

	/* Backpressure */
	pthread_mutex_t pressure_lock;
	pthread_cond_t pressure_cv;
	int waiters;

	/* Hierarchy */
	UmemBudgetContext *parent;
	UmemBudgetContext *children;
	UmemBudgetContext *next_sibling;

	/* Stats */
	uint64_t alloc_count;
	uint64_t free_count;
	uint64_t bytes_allocated;
	uint64_t wait_count;
	uint64_t peak_usage;

	/* Shared memory */
	char shm_name[64];
	int shm_fd;
};

/*
 * Allocate and initialize a context structure with defaults.
 */
static UmemBudgetContext *
ctx_alloc(const char *name, size_t budget, int flags)
{
	UmemBudgetContext *ctx;

	ctx = (UmemBudgetContext *)umem_zalloc(
	    sizeof(UmemBudgetContext), UMEM_DEFAULT);
	if (ctx == NULL)
		return (NULL);

	(void)snprintf(ctx->name, sizeof(ctx->name), "%.63s", name);
	ctx->budget = budget;
	ctx->flags = flags;
	ctx->shm_fd = -1;

	(void)pthread_mutex_init(&ctx->pressure_lock, NULL);
	(void)pthread_cond_init(&ctx->pressure_cv, NULL);

	return (ctx);
}

/*
 * Create a vmem arena backed by a pre-mapped region.
 * The region is mmap'd with MAP_POPULATE so pages are faulted in
 * immediately, avoiding page-fault latency during allocation.
 */
static int
create_prealloc_arena(UmemBudgetContext *ctx)
{
	ctx->backing_size = ctx->budget;
	ctx->backing = mmap(NULL, ctx->backing_size,
	    PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

	if (ctx->backing == MAP_FAILED) {
		ctx->backing = NULL;
		(void)fprintf(stderr,
		    "[umem] mmap failed for '%s': %s\n",
		    ctx->name, strerror(errno));
		return (-1);
	}

	ctx->arena = vmem_create(ctx->name,
	    ctx->backing, ctx->backing_size,
	    8,     /* quantum */
	    NULL,  /* no import (fixed size) */
	    NULL,  /* no release */
	    NULL,  /* no source */
	    0,     /* qcache_max */
	    VM_NOSLEEP);

	if (ctx->arena == NULL) {
		(void)munmap(ctx->backing, ctx->backing_size);
		ctx->backing = NULL;
		return (-1);
	}

	return (0);
}

/*
 * Create a vmem arena that grows on demand by importing spans from
 * libumem's heap arena.  Without a real source arena (and matching
 * import/release callbacks) an initially-empty, span-less arena has no
 * backing address space and every vmem_alloc() fails -- so we wire the
 * arena to vmem_heap_arena(), exactly as libumem's own internal arenas
 * do (see umem.c: umem_internal_arena et al.).
 *
 * The budget itself is still enforced in umem_budget_alloc() by checking
 * vmem_size(VMEM_ALLOC) before each allocation, not by capping the arena.
 */
static int
create_dynamic_arena(UmemBudgetContext *ctx)
{
	vmem_alloc_t *heap_alloc;
	vmem_free_t *heap_free;
	vmem_t *heap = vmem_heap_arena(&heap_alloc, &heap_free);

	if (heap == NULL)
		return (-1);

	ctx->arena = vmem_create(ctx->name,
	    NULL, 0,
	    8,             /* quantum */
	    heap_alloc,    /* import spans from the heap arena */
	    heap_free,     /* release spans back to it */
	    heap,          /* source arena */
	    0,
	    VM_NOSLEEP);

	return (ctx->arena == NULL ? -1 : 0);
}

/*
 * Link a child into the parent's child list.
 */
static void
link_child(UmemBudgetContext *parent, UmemBudgetContext *child)
{
	child->parent = parent;
	child->next_sibling = parent->children;
	parent->children = child;
}

/*
 * Unlink a child from its parent's child list.
 */
static void
unlink_child(UmemBudgetContext *ctx)
{
	UmemBudgetContext *parent = ctx->parent;
	UmemBudgetContext **pp;

	if (parent == NULL)
		return;

	for (pp = &parent->children; *pp != NULL;
	    pp = &(*pp)->next_sibling) {
		if (*pp == ctx) {
			*pp = ctx->next_sibling;
			break;
		}
	}
	ctx->parent = NULL;
	ctx->next_sibling = NULL;
}

/*
 * Track peak usage after a successful allocation.
 */
static void
update_peak(UmemBudgetContext *ctx, size_t used)
{
	if (used > ctx->peak_usage)
		ctx->peak_usage = used;
}

/*
 * Return the current usage for this context's arena.
 */
static size_t
arena_used(UmemBudgetContext *ctx)
{
	return (vmem_size(ctx->arena, VMEM_ALLOC));
}

/*
 * Check whether the parent hierarchy has room for an additional
 * `size` bytes. Returns 0 if ok, -1 if any ancestor would exceed
 * its budget.
 */
static int
check_parent_budget(UmemBudgetContext *ctx, size_t size)
{
	UmemBudgetContext *p;

	for (p = ctx->parent; p != NULL; p = p->parent) {
		if (arena_used(p) + p->reserved + size > p->budget)
			return (-1);
	}
	return (0);
}

/*
 * Build a timespec for the backpressure wait timeout.
 */
static void
make_timeout(struct timespec *ts)
{
	(void)clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_nsec += WAIT_TIMEOUT_MS * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += ts->tv_nsec / 1000000000L;
		ts->tv_nsec %= 1000000000L;
	}
}

UmemBudgetContext *
umem_budget_create(const char *name, size_t budget, int flags)
{
	UmemBudgetContext *ctx;
	int rc;

	ctx = ctx_alloc(name, budget, flags);
	if (ctx == NULL)
		return (NULL);

	if (flags & UMEM_BUDGET_PREALLOC)
		rc = create_prealloc_arena(ctx);
	else
		rc = create_dynamic_arena(ctx);

	if (rc != 0) {
		(void)pthread_mutex_destroy(&ctx->pressure_lock);
		(void)pthread_cond_destroy(&ctx->pressure_cv);
		umem_free(ctx, sizeof(UmemBudgetContext));
		return (NULL);
	}

	(void)fprintf(stderr, "[umem] context '%s' created "
	    "(budget=%zu%s)\n", ctx->name, ctx->budget,
	    (flags & UMEM_BUDGET_PREALLOC) ? " prealloc" : "");

	return (ctx);
}

UmemBudgetContext *
umem_budget_create_child(UmemBudgetContext *parent,
    const char *name, size_t budget, int flags)
{
	UmemBudgetContext *ctx;

	if (parent == NULL)
		return (umem_budget_create(name, budget, flags));

	if (budget > parent->budget) {
		(void)fprintf(stderr,
		    "[umem] child '%s' budget %zu exceeds "
		    "parent '%s' budget %zu\n",
		    name, budget, parent->name, parent->budget);
		return (NULL);
	}

	ctx = umem_budget_create(name, budget, flags);
	if (ctx == NULL)
		return (NULL);

	link_child(parent, ctx);

	(void)fprintf(stderr,
	    "[umem] context '%s' is child of '%s'\n",
	    ctx->name, parent->name);

	return (ctx);
}

void
umem_budget_delete(UmemBudgetContext *ctx)
{
	UmemBudgetContext *child;

	if (ctx == NULL)
		return;

	/* Recursively delete children */
	while ((child = ctx->children) != NULL) {
		ctx->children = child->next_sibling;
		child->parent = NULL;
		umem_budget_delete(child);
	}

	unlink_child(ctx);

	(void)fprintf(stderr, "[umem] deleting context '%s'\n",
	    ctx->name);

	if (ctx->arena != NULL)
		vmem_destroy(ctx->arena);

	if (ctx->backing != NULL)
		(void)munmap(ctx->backing, ctx->backing_size);

	(void)pthread_mutex_destroy(&ctx->pressure_lock);
	(void)pthread_cond_destroy(&ctx->pressure_cv);

	umem_free(ctx, sizeof(UmemBudgetContext));
}

void *
umem_budget_alloc(UmemBudgetContext *ctx, size_t size)
{
	void *ptr;
	size_t used;
	int retries = 0;

	if (size == 0)
		return (NULL);

	(void)pthread_mutex_lock(&ctx->pressure_lock);

	for (;;) {
		used = arena_used(ctx);

		/* Check this context's budget */
		if (used + size <= ctx->budget &&
		    check_parent_budget(ctx, size) == 0) {
			break;
		}

		/* Over budget: try reaping */
		(void)pthread_mutex_unlock(&ctx->pressure_lock);
		umem_reap();
		(void)pthread_mutex_lock(&ctx->pressure_lock);

		used = arena_used(ctx);
		if (used + size <= ctx->budget &&
		    check_parent_budget(ctx, size) == 0) {
			break;
		}

		/* Still over budget */
		if (ctx->flags & UMEM_BUDGET_NOWAIT) {
			(void)pthread_mutex_unlock(&ctx->pressure_lock);
			return (NULL);
		}

		if (ctx->flags & UMEM_BUDGET_NOFAIL) {
			(void)fprintf(stderr,
			    "[umem] NOFAIL: context '%s' exhausted "
			    "(%zu/%zu), aborting\n",
			    ctx->name, used, ctx->budget);
			abort();
		}

		/* Backpressure: wait for a free to make room */
		if (retries >= MAX_RETRIES) {
			(void)pthread_mutex_unlock(&ctx->pressure_lock);
			(void)fprintf(stderr,
			    "[umem] context '%s': budget exhausted "
			    "after %d retries\n",
			    ctx->name, retries);
			return (NULL);
		}

		struct timespec ts;
		make_timeout(&ts);

		ctx->waiters++;
		ctx->wait_count++;
		(void)pthread_cond_timedwait(
		    &ctx->pressure_cv, &ctx->pressure_lock, &ts);
		ctx->waiters--;
		retries++;
	}

	(void)pthread_mutex_unlock(&ctx->pressure_lock);

	ptr = vmem_alloc(ctx->arena, size, VM_NOSLEEP);
	if (ptr == NULL)
		return (NULL);

	(void)pthread_mutex_lock(&ctx->pressure_lock);
	ctx->alloc_count++;
	ctx->bytes_allocated += size;
	update_peak(ctx, arena_used(ctx));
	(void)pthread_mutex_unlock(&ctx->pressure_lock);

	return (ptr);
}

void *
umem_budget_alloc0(UmemBudgetContext *ctx, size_t size)
{
	void *ptr;

	ptr = umem_budget_alloc(ctx, size);
	if (ptr != NULL)
		(void)memset(ptr, 0, size);

	return (ptr);
}

void
umem_budget_free(UmemBudgetContext *ctx, void *ptr, size_t size)
{
	if (ptr == NULL)
		return;

	vmem_free(ctx->arena, ptr, size);

	(void)pthread_mutex_lock(&ctx->pressure_lock);
	ctx->free_count++;

	if (ctx->waiters > 0)
		(void)pthread_cond_broadcast(&ctx->pressure_cv);

	(void)pthread_mutex_unlock(&ctx->pressure_lock);
}

void
umem_budget_reset(UmemBudgetContext *ctx)
{
	(void)fprintf(stderr,
	    "[umem] resetting context '%s' "
	    "(allocs=%"PRIu64", frees=%"PRIu64", bytes=%"PRIu64")\n",
	    ctx->name, ctx->alloc_count,
	    ctx->free_count, ctx->bytes_allocated);

	/* Destroy and recreate the arena on the same backing */
	vmem_destroy(ctx->arena);
	ctx->arena = NULL;

	if (ctx->flags & UMEM_BUDGET_PREALLOC) {
		ctx->arena = vmem_create(ctx->name,
		    ctx->backing, ctx->backing_size,
		    8, NULL, NULL, NULL, 0, VM_NOSLEEP);
	} else {
		(void)create_dynamic_arena(ctx);
	}

	(void)pthread_mutex_lock(&ctx->pressure_lock);
	ctx->alloc_count = 0;
	ctx->free_count = 0;
	ctx->bytes_allocated = 0;

	if (ctx->waiters > 0)
		(void)pthread_cond_broadcast(&ctx->pressure_cv);

	(void)pthread_mutex_unlock(&ctx->pressure_lock);
}

/* ----------------------------------------------------------------
 * Shared memory support
 * ---------------------------------------------------------------- */

UmemBudgetContext *
umem_shared_create(const char *name, size_t size, int flags)
{
	UmemBudgetContext *ctx;
	int fd;

	ctx = ctx_alloc(name, size, flags | UMEM_BUDGET_SHARED);
	if (ctx == NULL)
		return (NULL);

	(void)snprintf(ctx->shm_name, sizeof(ctx->shm_name),
	    "/umem_%.56s", name);

	fd = shm_open(ctx->shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
	if (fd < 0) {
		(void)fprintf(stderr,
		    "[umem] shm_open('%s') failed: %s\n",
		    ctx->shm_name, strerror(errno));
		umem_free(ctx, sizeof(UmemBudgetContext));
		return (NULL);
	}

	if (ftruncate(fd, (off_t)size) != 0) {
		(void)fprintf(stderr,
		    "[umem] ftruncate failed: %s\n", strerror(errno));
		(void)close(fd);
		(void)shm_unlink(ctx->shm_name);
		umem_free(ctx, sizeof(UmemBudgetContext));
		return (NULL);
	}

	ctx->backing = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	if (ctx->backing == MAP_FAILED) {
		ctx->backing = NULL;
		(void)close(fd);
		(void)shm_unlink(ctx->shm_name);
		umem_free(ctx, sizeof(UmemBudgetContext));
		return (NULL);
	}

	ctx->backing_size = size;
	ctx->shm_fd = fd;

	ctx->arena = vmem_create(ctx->name,
	    ctx->backing, ctx->backing_size,
	    8, NULL, NULL, NULL, 0, VM_NOSLEEP);

	if (ctx->arena == NULL) {
		(void)munmap(ctx->backing, ctx->backing_size);
		(void)close(fd);
		(void)shm_unlink(ctx->shm_name);
		umem_free(ctx, sizeof(UmemBudgetContext));
		return (NULL);
	}

	(void)fprintf(stderr,
	    "[umem] shared context '%s' created (size=%zu, shm=%s)\n",
	    ctx->name, size, ctx->shm_name);

	return (ctx);
}

UmemBudgetContext *
umem_shared_attach(const char *name)
{
	UmemBudgetContext *ctx;
	char shm_name[64];
	int fd;
	struct stat st;

	(void)snprintf(shm_name, sizeof(shm_name),
	    "/umem_%.56s", name);

	fd = shm_open(shm_name, O_RDWR, 0600);
	if (fd < 0) {
		(void)fprintf(stderr,
		    "[umem] shm_open('%s') failed: %s\n",
		    shm_name, strerror(errno));
		return (NULL);
	}

	if (fstat(fd, &st) != 0) {
		(void)close(fd);
		return (NULL);
	}

	ctx = ctx_alloc(name, (size_t)st.st_size,
	    UMEM_BUDGET_SHARED);
	if (ctx == NULL) {
		(void)close(fd);
		return (NULL);
	}

	(void)snprintf(ctx->shm_name, sizeof(ctx->shm_name),
	    "%s", shm_name);
	ctx->shm_fd = fd;
	ctx->backing_size = (size_t)st.st_size;

	ctx->backing = mmap(NULL, ctx->backing_size,
	    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ctx->backing == MAP_FAILED) {
		ctx->backing = NULL;
		(void)close(fd);
		umem_free(ctx, sizeof(UmemBudgetContext));
		return (NULL);
	}

	ctx->arena = vmem_create(ctx->name,
	    ctx->backing, ctx->backing_size,
	    8, NULL, NULL, NULL, 0, VM_NOSLEEP);

	if (ctx->arena == NULL) {
		(void)munmap(ctx->backing, ctx->backing_size);
		(void)close(fd);
		umem_free(ctx, sizeof(UmemBudgetContext));
		return (NULL);
	}

	(void)fprintf(stderr,
	    "[umem] attached to shared context '%s' "
	    "(size=%zu, shm=%s)\n",
	    ctx->name, ctx->backing_size, ctx->shm_name);

	return (ctx);
}

void
umem_shared_detach(UmemBudgetContext *ctx)
{
	if (ctx == NULL)
		return;

	(void)fprintf(stderr,
	    "[umem] detaching shared context '%s'\n", ctx->name);

	if (ctx->arena != NULL)
		vmem_destroy(ctx->arena);

	if (ctx->backing != NULL)
		(void)munmap(ctx->backing, ctx->backing_size);

	if (ctx->shm_fd >= 0)
		(void)close(ctx->shm_fd);

	(void)pthread_mutex_destroy(&ctx->pressure_lock);
	(void)pthread_cond_destroy(&ctx->pressure_cv);

	umem_free(ctx, sizeof(UmemBudgetContext));
}

/* ----------------------------------------------------------------
 * Query functions
 * ---------------------------------------------------------------- */

size_t
umem_budget_used(UmemBudgetContext *ctx)
{
	return (arena_used(ctx));
}

size_t
umem_budget_available(UmemBudgetContext *ctx)
{
	size_t used = arena_used(ctx);

	return (used < ctx->budget ? ctx->budget - used : 0);
}

size_t
umem_budget_peak(UmemBudgetContext *ctx)
{
	return ((size_t)ctx->peak_usage);
}

int
umem_budget_under_pressure(UmemBudgetContext *ctx)
{
	size_t used = arena_used(ctx);

	return (used * 100 / ctx->budget >= PRESSURE_PCT);
}

void
umem_budget_stats(UmemBudgetContext *ctx, FILE *fp)
{
	size_t used = arena_used(ctx);

	(void)fprintf(fp,
	    "--- Context: %s ---\n"
	    "  Budget:         %zu\n"
	    "  Used:           %zu (%.1f%%)\n"
	    "  Available:      %zu\n"
	    "  Peak:           %zu\n"
	    "  Flags:          0x%04x",
	    ctx->name,
	    ctx->budget,
	    used, ctx->budget > 0 ?
	        (double)used * 100.0 / (double)ctx->budget : 0.0,
	    ctx->budget > used ? ctx->budget - used : 0,
	    (size_t)ctx->peak_usage,
	    ctx->flags);

	if (ctx->flags & UMEM_BUDGET_PREALLOC)
		(void)fprintf(fp, " PREALLOC");
	if (ctx->flags & UMEM_BUDGET_SHARED)
		(void)fprintf(fp, " SHARED");
	if (ctx->flags & UMEM_BUDGET_AUDIT)
		(void)fprintf(fp, " AUDIT");
	if (ctx->flags & UMEM_BUDGET_GUARDS)
		(void)fprintf(fp, " GUARDS");
	if (ctx->flags & UMEM_BUDGET_NOWAIT)
		(void)fprintf(fp, " NOWAIT");
	if (ctx->flags & UMEM_BUDGET_NOFAIL)
		(void)fprintf(fp, " NOFAIL");

	(void)fprintf(fp, "\n"
	    "  Allocations:    %" PRIu64 "\n"
	    "  Frees:          %" PRIu64 "\n"
	    "  Bytes alloc'd:  %" PRIu64 "\n"
	    "  Waits:          %" PRIu64 "\n"
	    "  Pressure:       %s\n",
	    ctx->alloc_count,
	    ctx->free_count,
	    ctx->bytes_allocated,
	    ctx->wait_count,
	    umem_budget_under_pressure(ctx) ? "YES" : "no");

	if (ctx->parent != NULL)
		(void)fprintf(fp, "  Parent:         %s\n",
		    ctx->parent->name);

	if (ctx->children != NULL) {
		UmemBudgetContext *c;
		(void)fprintf(fp, "  Children:      ");
		for (c = ctx->children; c != NULL;
		    c = c->next_sibling) {
			(void)fprintf(fp, " %s", c->name);
		}
		(void)fprintf(fp, "\n");
	}

	(void)fprintf(fp, "---\n");
}

/* ----------------------------------------------------------------
 * Demonstration
 * ---------------------------------------------------------------- */
#ifndef UMEM_PALLOC_NO_MAIN

/*
 * Thread argument for the backpressure demo. The freeing thread
 * sleeps briefly, then frees a block to wake up a blocked allocator.
 */
struct free_arg {
	UmemBudgetContext *ctx;
	void *ptr;
	size_t size;
};

static void *
free_thread(void *arg)
{
	struct free_arg *fa = (struct free_arg *)arg;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 };

	(void)fprintf(stderr,
	    "  [free_thread] sleeping 200ms before freeing...\n");
	(void)nanosleep(&ts, NULL);

	(void)fprintf(stderr,
	    "  [free_thread] freeing %zu bytes\n", fa->size);
	umem_budget_free(fa->ctx, fa->ptr, fa->size);

	return (NULL);
}

static void
demo_budget_hierarchy(void)
{
	UmemBudgetContext *top, *executor, *tuple;

	(void)fprintf(stderr,
	    "\n=== Budget Hierarchy ===\n");

	top = umem_budget_create("TopMemCtx",
	    512 * 1024, UMEM_BUDGET_PREALLOC);
	if (top == NULL)
		return;

	executor = umem_budget_create_child(top, "ExecutorCtx",
	    64 * 1024, UMEM_BUDGET_AUDIT);
	if (executor == NULL) {
		umem_budget_delete(top);
		return;
	}

	tuple = umem_budget_create_child(executor, "TupleCtx",
	    16 * 1024, UMEM_BUDGET_GUARDS | UMEM_BUDGET_NOWAIT);
	if (tuple == NULL) {
		umem_budget_delete(top);
		return;
	}

	/* Allocate from each level */
	void *t1 = umem_budget_alloc(top, 1024);
	void *e1 = umem_budget_alloc(executor, 512);
	void *u1 = umem_budget_alloc(tuple, 256);

	(void)fprintf(stderr, "\nAfter allocations:\n");
	umem_budget_stats(top, stderr);
	umem_budget_stats(executor, stderr);
	umem_budget_stats(tuple, stderr);

	umem_budget_free(tuple, u1, 256);
	umem_budget_free(executor, e1, 512);
	umem_budget_free(top, t1, 1024);

	umem_budget_delete(top);
}

static void
demo_backpressure(void)
{
	UmemBudgetContext *ctx;
	void *blocks[32];
	int i, count = 0;

	(void)fprintf(stderr,
	    "\n=== Backpressure Demo ===\n");

	/* Small budget so we can exhaust it quickly */
	ctx = umem_budget_create("PressureCtx",
	    4096, UMEM_BUDGET_PREALLOC | UMEM_BUDGET_NOWAIT);
	if (ctx == NULL)
		return;

	/* Fill up the budget */
	for (i = 0; i < 32; i++) {
		blocks[i] = umem_budget_alloc(ctx, 128);
		if (blocks[i] == NULL) {
			(void)fprintf(stderr,
			    "  Budget exhausted after %d blocks "
			    "(%zu/%zu used)\n",
			    i, umem_budget_used(ctx), ctx->budget);
			break;
		}
		count++;
	}

	(void)fprintf(stderr, "  Pressure: %s\n",
	    umem_budget_under_pressure(ctx) ? "YES" : "no");

	umem_budget_stats(ctx, stderr);

	/* Free everything */
	for (i = 0; i < count; i++)
		umem_budget_free(ctx, blocks[i], 128);

	umem_budget_delete(ctx);
}

static void
demo_backpressure_wait(void)
{
	UmemBudgetContext *ctx;
	void *block;
	pthread_t tid;
	struct free_arg fa;

	(void)fprintf(stderr,
	    "\n=== Backpressure Wait Demo ===\n");

	/*
	 * Create a tiny context (no NOWAIT, so alloc will block).
	 * Fill it, then have another thread free a block.
	 */
	ctx = umem_budget_create("WaitCtx",
	    512, UMEM_BUDGET_PREALLOC);
	if (ctx == NULL)
		return;

	/* Fill the budget */
	block = umem_budget_alloc(ctx, 256);
	void *block2 = umem_budget_alloc(ctx, 128);

	if (block == NULL || block2 == NULL) {
		(void)fprintf(stderr, "  Initial alloc failed\n");
		umem_budget_delete(ctx);
		return;
	}

	(void)fprintf(stderr,
	    "  Budget nearly full: %zu/%zu\n",
	    umem_budget_used(ctx), ctx->budget);

	/* Schedule a free from another thread */
	fa.ctx = ctx;
	fa.ptr = block2;
	fa.size = 128;

	if (pthread_create(&tid, NULL, free_thread, &fa) != 0) {
		(void)fprintf(stderr, "  pthread_create failed\n");
		umem_budget_free(ctx, block, 256);
		umem_budget_free(ctx, block2, 128);
		umem_budget_delete(ctx);
		return;
	}

	(void)fprintf(stderr,
	    "  Attempting alloc that exceeds budget "
	    "(will wait for free)...\n");

	void *block3 = umem_budget_alloc(ctx, 128);

	if (block3 != NULL) {
		(void)fprintf(stderr,
		    "  Alloc succeeded after backpressure wait\n");
		umem_budget_free(ctx, block3, 128);
	} else {
		(void)fprintf(stderr,
		    "  Alloc timed out (expected if budget too tight)\n");
	}

	(void)pthread_join(tid, NULL);

	umem_budget_stats(ctx, stderr);

	umem_budget_free(ctx, block, 256);
	umem_budget_delete(ctx);
}

static void
demo_shared_memory(void)
{
	UmemBudgetContext *creator, *attacher;

	(void)fprintf(stderr,
	    "\n=== Shared Memory Demo ===\n");

	creator = umem_shared_create("pgbuf",
	    64 * 1024, 0);
	if (creator == NULL)
		return;

	/* Allocate from the creator's view */
	void *p = umem_budget_alloc(creator, 512);

	if (p != NULL) {
		(void)memset(p, 0xAB, 512);
		(void)fprintf(stderr,
		    "  Creator allocated 512 bytes at %p\n", p);
	}

	umem_budget_stats(creator, stderr);

	/*
	 * In a real multi-process scenario, a child process would
	 * call umem_shared_attach("pgbuf") after fork(). Here we
	 * demonstrate the attach path in the same process.
	 */
	attacher = umem_shared_attach("pgbuf");
	if (attacher != NULL) {
		(void)fprintf(stderr,
		    "  Attacher sees shm, size=%zu\n",
		    attacher->backing_size);
		umem_shared_detach(attacher);
	}

	if (p != NULL)
		umem_budget_free(creator, p, 512);

	/* Clean up: detach and unlink */
	if (creator->shm_fd >= 0)
		(void)shm_unlink(creator->shm_name);

	umem_shared_detach(creator);
}

static void
demo_stats_summary(void)
{
	UmemBudgetContext *top, *exec, *sort;

	(void)fprintf(stderr,
	    "\n=== Stats Summary ===\n");

	top = umem_budget_create("TopMemCtx",
	    256 * 1024, 0);
	if (top == NULL)
		return;

	exec = umem_budget_create_child(top, "ExecutorCtx",
	    128 * 1024, UMEM_BUDGET_AUDIT);
	sort = umem_budget_create_child(exec, "SortCtx",
	    32 * 1024, UMEM_BUDGET_GUARDS);

	if (exec == NULL || sort == NULL) {
		umem_budget_delete(top);
		return;
	}

	/* Simulate workload */
	void *ptrs[10];
	for (int i = 0; i < 10; i++)
		ptrs[i] = umem_budget_alloc(sort, 64 * (size_t)(i + 1));

	for (int i = 0; i < 5; i++) {
		if (ptrs[i] != NULL)
			umem_budget_free(sort, ptrs[i],
			    64 * (size_t)(i + 1));
	}

	(void)fprintf(stderr, "\nAll context stats:\n\n");
	umem_budget_stats(top, stderr);
	(void)fprintf(stderr, "\n");
	umem_budget_stats(exec, stderr);
	(void)fprintf(stderr, "\n");
	umem_budget_stats(sort, stderr);

	umem_budget_delete(top);
}

int
main(void)
{
	(void)fprintf(stderr,
	    "libumem budget-based memory context demo\n"
	    "=========================================\n");

	demo_budget_hierarchy();
	demo_backpressure();
	demo_backpressure_wait();
	demo_shared_memory();
	demo_stats_summary();

	(void)fprintf(stderr, "\nDone.\n");
	return (0);
}
#endif /* UMEM_PALLOC_NO_MAIN */

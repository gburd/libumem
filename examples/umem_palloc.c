/*
 * umem_palloc.c - PostgreSQL MemoryContext backed by libumem
 *
 * Demonstrates how to map PostgreSQL-style per-context memory
 * management onto umem's slab allocator. Each UmemContext owns a
 * set of size-classed umem caches, and per-context debug flags
 * control which umem debug features are active.
 *
 * Build:  make
 * Run:    LD_LIBRARY_PATH=../.libs ./umem_palloc_demo
 *
 * To see debug output, set UMEM_DEBUG before running:
 *   UMEM_DEBUG=default LD_LIBRARY_PATH=../.libs ./umem_palloc_demo
 */

#include "umem_palloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Size classes for the per-context caches. Each context creates one
 * umem_cache per size class. Allocations are routed to the smallest
 * cache whose object size is >= the requested size.
 */
#define NUM_SIZE_CLASSES 10

static const size_t size_classes[NUM_SIZE_CLASSES] = {
	32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384
};

struct UmemContext {
	char name[64];
	size_t block_size;
	int flags;
	umem_cache_t *caches[NUM_SIZE_CLASSES];
	size_t alloc_count;
	size_t free_count;
	size_t bytes_allocated;
};

/*
 * Map UMEM_CTX_* flags to umem internal cache flags.
 * UMC_NODEBUG (0x00020000) disables debug; we omit it when
 * debug flags are requested.
 */
static int
ctx_flags_to_cache_flags(int ctx_flags)
{
	int cflags = 0;

	if (ctx_flags == 0)
		cflags |= UMC_NODEBUG;

	return (cflags);
}

/*
 * Build a cache name like "TopMemCtx_0256" for diagnostics.
 */
static void
make_cache_name(char *buf, size_t bufsz,
    const char *ctx_name, size_t obj_size)
{
	(void)snprintf(buf, bufsz, "%.32s_%04zu", ctx_name, obj_size);
}

/*
 * Create all size-class caches for a context.
 */
static void
create_caches(UmemContext *ctx)
{
	int cflags = ctx_flags_to_cache_flags(ctx->flags);
	char cname[64];

	for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
		if (size_classes[i] > ctx->block_size)
			break;

		make_cache_name(cname, sizeof(cname),
		    ctx->name, size_classes[i]);

		ctx->caches[i] = umem_cache_create(cname,
		    size_classes[i],  /* object size */
		    0,                /* alignment (default) */
		    NULL,             /* constructor */
		    NULL,             /* destructor */
		    NULL,             /* reclaim callback */
		    NULL,             /* callback arg */
		    NULL,             /* vmem source */
		    cflags);

		if (ctx->caches[i] == NULL) {
			(void)fprintf(stderr,
			    "umem_context_create: cache %s failed\n",
			    cname);
		}
	}
}

/*
 * Destroy all caches owned by a context.
 */
static void
destroy_caches(UmemContext *ctx)
{
	for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
		if (ctx->caches[i] != NULL) {
			umem_cache_destroy(ctx->caches[i]);
			ctx->caches[i] = NULL;
		}
	}
}

/*
 * Find the cache index for a given allocation size.
 * Returns -1 if the size exceeds the largest size class.
 */
static int
find_cache_index(UmemContext *ctx, size_t size)
{
	for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
		if (ctx->caches[i] == NULL)
			break;
		if (size <= size_classes[i])
			return (i);
	}
	return (-1);
}

UmemContext *
umem_context_create(const char *name, size_t block_size, int flags)
{
	UmemContext *ctx;

	ctx = (UmemContext *)umem_zalloc(sizeof(UmemContext), UMEM_DEFAULT);
	if (ctx == NULL)
		return (NULL);

	(void)snprintf(ctx->name, sizeof(ctx->name), "%.63s", name);
	ctx->block_size = block_size;
	ctx->flags = flags;

	create_caches(ctx);

	(void)fprintf(stderr, "[umem] context '%s' created", ctx->name);
	if (flags & UMEM_CTX_DEBUG_AUDIT)
		(void)fprintf(stderr, " +audit");
	if (flags & UMEM_CTX_DEBUG_GUARDS)
		(void)fprintf(stderr, " +guards");
	if (flags & UMEM_CTX_DEBUG_FIREWALL)
		(void)fprintf(stderr, " +firewall");
	(void)fprintf(stderr, "\n");

	return (ctx);
}

void *
umem_context_alloc(UmemContext *ctx, size_t size)
{
	void *ptr;
	int idx;

	if (size == 0)
		return (NULL);

	idx = find_cache_index(ctx, size);
	if (idx >= 0) {
		ptr = umem_cache_alloc(ctx->caches[idx], UMEM_DEFAULT);
	} else {
		ptr = umem_alloc(size, UMEM_DEFAULT);
	}

	if (ptr != NULL) {
		ctx->alloc_count++;
		ctx->bytes_allocated += size;
	}

	return (ptr);
}

void *
umem_context_alloc0(UmemContext *ctx, size_t size)
{
	void *ptr;

	ptr = umem_context_alloc(ctx, size);
	if (ptr != NULL)
		(void)memset(ptr, 0, size);

	return (ptr);
}

void
umem_context_free(UmemContext *ctx, void *ptr, size_t size)
{
	int idx;

	if (ptr == NULL)
		return;

	idx = find_cache_index(ctx, size);
	if (idx >= 0) {
		umem_cache_free(ctx->caches[idx], ptr);
	} else {
		umem_free(ptr, size);
	}

	ctx->free_count++;
}

void *
umem_context_realloc(UmemContext *ctx, void *ptr,
    size_t old_size, size_t new_size)
{
	void *newptr;

	if (new_size == 0) {
		umem_context_free(ctx, ptr, old_size);
		return (NULL);
	}

	if (ptr == NULL)
		return (umem_context_alloc(ctx, new_size));

	/*
	 * If old and new sizes map to the same cache, no copy needed.
	 */
	int old_idx = find_cache_index(ctx, old_size);
	int new_idx = find_cache_index(ctx, new_size);

	if (old_idx == new_idx && old_idx >= 0)
		return (ptr);

	newptr = umem_context_alloc(ctx, new_size);
	if (newptr == NULL)
		return (NULL);

	(void)memcpy(newptr, ptr,
	    old_size < new_size ? old_size : new_size);

	umem_context_free(ctx, ptr, old_size);

	return (newptr);
}

void
umem_context_reset(UmemContext *ctx)
{
	(void)fprintf(stderr, "[umem] resetting context '%s' "
	    "(%zu allocs, %zu frees, %zu bytes)\n",
	    ctx->name, ctx->alloc_count, ctx->free_count,
	    ctx->bytes_allocated);

	destroy_caches(ctx);
	create_caches(ctx);

	ctx->alloc_count = 0;
	ctx->free_count = 0;
	ctx->bytes_allocated = 0;
}

void
umem_context_delete(UmemContext *ctx)
{
	if (ctx == NULL)
		return;

	(void)fprintf(stderr, "[umem] deleting context '%s'\n", ctx->name);

	destroy_caches(ctx);
	umem_free(ctx, sizeof(UmemContext));
}

void
umem_context_stats(UmemContext *ctx)
{
	(void)fprintf(stderr,
	    "--- Context: %s ---\n"
	    "  Flags:          0x%02x\n"
	    "  Allocations:    %zu\n"
	    "  Frees:          %zu\n"
	    "  Outstanding:    %zu\n"
	    "  Bytes (approx): %zu\n",
	    ctx->name,
	    ctx->flags,
	    ctx->alloc_count,
	    ctx->free_count,
	    ctx->alloc_count - ctx->free_count,
	    ctx->bytes_allocated);

	(void)fprintf(stderr, "  Size-class caches:\n");
	for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
		if (ctx->caches[i] == NULL)
			break;
		(void)fprintf(stderr, "    [%5zu bytes] cache=%p\n",
		    size_classes[i], (void *)ctx->caches[i]);
	}
	(void)fprintf(stderr, "---\n");
}

/* ----------------------------------------------------------------
 * Demonstration main()
 * ---------------------------------------------------------------- */

static void
demo_basic_allocation(void)
{
	UmemContext *top;

	(void)fprintf(stderr, "\n=== Basic Allocation ===\n");

	top = umem_context_create("TopMemoryContext", 8192, 0);
	if (top == NULL) {
		(void)fprintf(stderr, "Failed to create TopMemoryContext\n");
		return;
	}

	/* Allocate various sizes */
	char *s1 = umem_context_alloc(top, 24);
	char *s2 = umem_context_alloc(top, 100);
	char *s3 = umem_context_alloc0(top, 512);

	(void)snprintf(s1, 24, "hello umem");
	(void)snprintf(s2, 100, "PostgreSQL query text goes here");
	/* s3 is already zeroed */

	(void)fprintf(stderr, "s1=%s  s2=%s  s3[0]=%d\n",
	    s1, s2, (int)s3[0]);

	umem_context_stats(top);

	umem_context_free(top, s1, 24);
	umem_context_free(top, s2, 100);
	umem_context_free(top, s3, 512);

	umem_context_delete(top);
}

static void
demo_audit_context(void)
{
	UmemContext *executor;

	(void)fprintf(stderr, "\n=== Audit-Enabled Context ===\n");

	executor = umem_context_create("ExecutorContext", 4096,
	    UMEM_CTX_DEBUG_AUDIT);
	if (executor == NULL)
		return;

	/* Simulate executor allocations */
	void *tuples[8];
	for (int i = 0; i < 8; i++)
		tuples[i] = umem_context_alloc(executor, 128);

	/* Free half of them */
	for (int i = 0; i < 4; i++)
		umem_context_free(executor, tuples[i], 128);

	umem_context_stats(executor);

	/* Reset discards everything */
	umem_context_reset(executor);

	umem_context_stats(executor);
	umem_context_delete(executor);
}

static void
demo_guards_context(void)
{
	UmemContext *tctx;

	(void)fprintf(stderr, "\n=== Guards-Enabled Context ===\n");

	tctx = umem_context_create("TupleContext", 2048,
	    UMEM_CTX_DEBUG_GUARDS);
	if (tctx == NULL)
		return;

	char *buf = umem_context_alloc(tctx, 64);
	(void)snprintf(buf, 64, "tuple data");

	/* Realloc from 64 to 256 */
	buf = umem_context_realloc(tctx, buf, 64, 256);
	(void)fprintf(stderr, "After realloc: buf=%s\n", buf);

	umem_context_stats(tctx);
	umem_context_free(tctx, buf, 256);
	umem_context_delete(tctx);
}

static void
demo_buffer_overflow(void)
{
	(void)fprintf(stderr, "\n=== Deliberate Buffer Overflow ===\n");
	(void)fprintf(stderr,
	    "(Run with UMEM_DEBUG=default to see full diagnostics)\n\n");

	/*
	 * This demo shows what WOULD happen with a buffer overflow.
	 * When UMEM_DEBUG=default is set, umem places a redzone sentinel
	 * (0xfeedface) after each allocation. Overwriting it is detected
	 * on the next free, producing a diagnostic with a stack trace:
	 *
	 *   umem allocator: buffer modified after being freed
	 *   modification occurred at offset 0x20 (0x41414141)
	 *   buffer=0x7f... bufctl=0x7f... cache: OverflowDemo_0032
	 *   previous transaction on buffer 0x7f...:
	 *     #0  0x7f... in umem_cache_free+0x1a ()
	 *     #1  0x7f... in umem_context_free+0x2b ()
	 *     #2  0x00... in demo_buffer_overflow+0x5b ()
	 *     ...
	 *
	 * To trigger this deliberately (will crash):
	 *
	 *   UmemContext *ctx = umem_context_create("OverflowDemo",
	 *       4096, UMEM_CTX_DEBUG_GUARDS);
	 *   char *buf = umem_context_alloc(ctx, 32);
	 *   memset(buf, 'A', 48);  // overflow by 16 bytes
	 *   umem_context_free(ctx, buf, 32);  // detected here
	 */

	(void)fprintf(stderr,
	    "Skipped (would crash). Set UMEM_DEBUG=default and\n"
	    "uncomment the overflow code to see detection in action.\n");
}

int
main(void)
{
	(void)fprintf(stderr,
	    "libumem PostgreSQL integration demo\n"
	    "====================================\n");

	demo_basic_allocation();
	demo_audit_context();
	demo_guards_context();
	demo_buffer_overflow();

	(void)fprintf(stderr, "\nDone.\n");
	return (0);
}

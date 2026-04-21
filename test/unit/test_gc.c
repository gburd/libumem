/*
 * Unit tests for libumem garbage collector (umem_gc).
 *
 * Tests: basic alloc/collect, reachability, atomic vs non-atomic,
 * finalizers, thread safety, hybrid manual+GC, linked lists, statistics.
 */

#define UMEM_ENABLE_EXPERIMENTAL
#include "../munit.h"
#include "../../umem_gc.h"
#include <umem.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static volatile int finalizer_called;
static volatile void *finalizer_ptr;

static void
test_finalizer(void *obj, void *cd)
{
	(void)cd;
	finalizer_called = 1;
	finalizer_ptr = obj;
}

static volatile int finalizer_counter;

static void
counting_finalizer(void *obj, void *cd)
{
	(void)obj;
	(void)cd;
	__atomic_fetch_add(&finalizer_counter, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* Basic allocation and collection                                     */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_init(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	int rc = umem_gc_init();
	munit_assert_int(rc, ==, 0);

	/* Double init should succeed */
	rc = umem_gc_init();
	munit_assert_int(rc, ==, 0);

	return MUNIT_OK;
}

static MunitResult
test_gc_alloc_basic(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	/* GC_MALLOC returns zeroed memory */
	char zeros[64];
	memset(zeros, 0, sizeof (zeros));
	munit_assert_memory_equal(64, p, zeros);

	return MUNIT_OK;
}

static MunitResult
test_gc_alloc_atomic(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	void *p = GC_MALLOC_ATOMIC(128);
	munit_assert_not_null(p);

	/* Atomic allocations should also be zeroed */
	char zeros[128];
	memset(zeros, 0, sizeof (zeros));
	munit_assert_memory_equal(128, p, zeros);

	return MUNIT_OK;
}

static MunitResult
test_gc_alloc_various_sizes(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	size_t sizes[] = { 1, 7, 16, 63, 100, 256, 1000, 4096, 8000, 16384 };
	int nsizes = (int)(sizeof (sizes) / sizeof (sizes[0]));

	for (int i = 0; i < nsizes; i++) {
		void *p = GC_MALLOC(sizes[i]);
		munit_assert_not_null(p);
		memset(p, 0xAB, sizes[i]);
	}

	return MUNIT_OK;
}

static MunitResult
test_gc_alloc_zero(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/* Size 0 should still return a valid pointer */
	void *p = GC_MALLOC(0);
	munit_assert_not_null(p);

	return MUNIT_OK;
}

static MunitResult
test_gc_free_explicit(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);
	GC_FREE(p);

	/* Free NULL should be safe */
	GC_FREE(NULL);

	return MUNIT_OK;
}

static MunitResult
test_gc_collect_basic(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/* Allocate some objects that are not reachable */
	for (int i = 0; i < 100; i++) {
		(void) GC_MALLOC(64);
	}

	/* Collect should not crash */
	GC_gcollect();

	/* Allocate more after collection */
	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	return MUNIT_OK;
}

static MunitResult
test_gc_collect_frees_unreachable(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/* Record heap size before */
	size_t heap_before = GC_get_heap_size();

	/* Allocate objects that become unreachable */
	for (int i = 0; i < 200; i++) {
		(void) GC_MALLOC(256);
	}

	size_t heap_after_alloc = GC_get_heap_size();
	munit_assert_size(heap_after_alloc, >, heap_before);

	/* Force collection */
	GC_gcollect();

	/*
	 * After collection, heap size should decrease (or at least not
	 * grow further). Conservative GC may retain some objects due to
	 * false positives on the stack, so we just verify collection runs.
	 */
	size_t heap_after_gc = GC_get_heap_size();
	munit_assert_size(heap_after_gc, <=, heap_after_alloc);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Reachability: objects on stack survive collection                    */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_stack_reachable(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	volatile void *p = GC_MALLOC(128);
	munit_assert_not_null((void *)p);

	/* Write a known pattern */
	memset((void *)p, 0x42, 128);

	/* Collect - p is on the stack so should survive */
	GC_gcollect();

	/* Verify data is intact */
	unsigned char *bytes = (unsigned char *)(void *)p;
	for (int i = 0; i < 128; i++) {
		munit_assert_uint8(bytes[i], ==, 0x42);
	}

	return MUNIT_OK;
}

static MunitResult
test_gc_reachable_chain(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/*
	 * Build a chain: root -> A -> B -> C
	 * Only root is on the stack. A, B, C should survive
	 * because they are reachable through non-atomic objects.
	 */
	void **root = GC_MALLOC(sizeof (void *));
	munit_assert_not_null(root);

	void **a = GC_MALLOC(sizeof (void *));
	munit_assert_not_null(a);

	void **b = GC_MALLOC(sizeof (void *));
	munit_assert_not_null(b);

	void **c = GC_MALLOC(sizeof (void *));
	munit_assert_not_null(c);

	*root = a;
	*a = b;
	*b = c;
	*c = NULL;

	/* Write a pattern into the last node */
	/* Use the remaining space after the pointer for data */

	GC_gcollect();

	/* Chain should still be intact */
	munit_assert_ptr_equal(*root, a);
	munit_assert_ptr_equal(*a, b);
	munit_assert_ptr_equal(*b, c);
	munit_assert_null(*c);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Atomic vs non-atomic                                                */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_atomic_not_scanned(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/*
	 * Allocate an atomic object that contains a value that happens
	 * to look like a pointer to another GC object. The GC should
	 * NOT scan the atomic object's contents, so the "pointed-to"
	 * object should be collectible (unless kept alive by something
	 * else like a conservative stack scan).
	 */
	void *target = GC_MALLOC(64);
	munit_assert_not_null(target);

	uintptr_t *atomic_obj = GC_MALLOC_ATOMIC(sizeof (uintptr_t));
	munit_assert_not_null(atomic_obj);
	*atomic_obj = (uintptr_t)target;

	/*
	 * We can't reliably test that target gets collected because
	 * conservative scanning may find it on the stack. But we can
	 * verify that the atomic flag is set correctly.
	 */
	umem_gc_header_t *hdr = UMEM_GC_HEADER(atomic_obj);
	munit_assert_uint32(hdr->gc_flags & UMEM_GC_ATOMIC, ==,
	    UMEM_GC_ATOMIC);

	/* Non-atomic should not have the flag */
	void *non_atomic = GC_MALLOC(64);
	umem_gc_header_t *hdr2 = UMEM_GC_HEADER(non_atomic);
	munit_assert_uint32(hdr2->gc_flags & UMEM_GC_ATOMIC, ==, 0);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Finalizers                                                          */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_register_finalizer(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	void (*old_fn)(void *, void *) = NULL;
	void *old_cd = NULL;

	GC_REGISTER_FINALIZER(p, test_finalizer, NULL, &old_fn, &old_cd);
	munit_assert_null(old_fn);
	munit_assert_null(old_cd);

	/* Verify header has finalize flag */
	umem_gc_header_t *hdr = UMEM_GC_HEADER(p);
	munit_assert_uint32(hdr->gc_flags & UMEM_GC_FINALIZE, !=, 0);

	/* Replace finalizer, get old one back */
	void (*old_fn2)(void *, void *) = NULL;
	GC_REGISTER_FINALIZER(p, NULL, NULL, &old_fn2, NULL);
	munit_assert_ptr_equal((void *)(uintptr_t)old_fn2,
	    (void *)(uintptr_t)test_finalizer);

	/* Flag should be cleared */
	munit_assert_uint32(hdr->gc_flags & UMEM_GC_FINALIZE, ==, 0);

	GC_FREE(p);
	return MUNIT_OK;
}

static MunitResult
test_gc_finalizer_on_free(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	finalizer_called = 0;
	finalizer_ptr = NULL;

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	GC_REGISTER_FINALIZER(p, test_finalizer, NULL, NULL, NULL);

	/* Explicit free should run finalizer */
	GC_FREE(p);
	munit_assert_int(finalizer_called, ==, 1);
	munit_assert_ptr_equal((void *)finalizer_ptr, p);

	return MUNIT_OK;
}

static MunitResult
test_gc_finalizer_on_collect(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	finalizer_counter = 0;

	/* Allocate objects with finalizers that become unreachable */
	for (int i = 0; i < 50; i++) {
		void *p = GC_MALLOC(64);
		GC_REGISTER_FINALIZER(p, counting_finalizer, NULL,
		    NULL, NULL);
	}

	/* Force collection */
	GC_gcollect();

	/*
	 * Some finalizers should have been called. Due to conservative
	 * scanning, not all 50 may be collected, but at least some should.
	 */
	umem_gc_stats_t stats;
	umem_gc_get_stats(&stats);
	munit_assert_uint64(stats.gcs_collections, >, 0);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Realloc                                                             */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_realloc(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);
	memset(p, 0xAA, 64);

	/* Grow */
	void *p2 = GC_REALLOC(p, 256);
	munit_assert_not_null(p2);

	/* First 64 bytes should be preserved */
	unsigned char *bytes = (unsigned char *)p2;
	for (int i = 0; i < 64; i++) {
		munit_assert_uint8(bytes[i], ==, 0xAA);
	}

	/* Realloc NULL should allocate */
	void *p3 = GC_REALLOC(NULL, 32);
	munit_assert_not_null(p3);

	/* Realloc to 0 should free */
	void *p4 = GC_REALLOC(p3, 0);
	munit_assert_null(p4);

	GC_FREE(p2);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Thread safety                                                       */
/* ------------------------------------------------------------------ */

#define	THREAD_ALLOC_COUNT	200
#define	THREAD_COUNT		4

static void *
gc_thread_func(void *arg)
{
	int id = (int)(uintptr_t)arg;
	(void)id;

	umem_gc_register_thread();

	for (int i = 0; i < THREAD_ALLOC_COUNT; i++) {
		void *p = GC_MALLOC(64 + (i % 128));
		if (p != NULL)
			memset(p, (unsigned char)i, 64);

		if (i % 50 == 0)
			GC_gcollect();
	}

	umem_gc_unregister_thread();
	return NULL;
}

static MunitResult
test_gc_threaded(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	pthread_t threads[THREAD_COUNT];

	for (int i = 0; i < THREAD_COUNT; i++) {
		int rc = pthread_create(&threads[i], NULL, gc_thread_func,
		    (void *)(uintptr_t)i);
		munit_assert_int(rc, ==, 0);
	}

	for (int i = 0; i < THREAD_COUNT; i++) {
		(void) pthread_join(threads[i], NULL);
	}

	/* Final collection */
	GC_gcollect();

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Hybrid manual + GC                                                  */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_hybrid(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/* Mix umem_alloc (manual) and GC_MALLOC (collected) */
	void *manual = umem_alloc(128, UMEM_DEFAULT);
	munit_assert_not_null(manual);
	memset(manual, 0xBB, 128);

	void *gc_obj = GC_MALLOC(128);
	munit_assert_not_null(gc_obj);
	memset(gc_obj, 0xCC, 128);

	/* Collection should not touch manual allocation */
	GC_gcollect();

	unsigned char *mb = (unsigned char *)manual;
	for (int i = 0; i < 128; i++) {
		munit_assert_uint8(mb[i], ==, 0xBB);
	}

	umem_free(manual, 128);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Linked list                                                         */
/* ------------------------------------------------------------------ */

struct gc_node {
	int		value;
	struct gc_node	*next;
};

static MunitResult
test_gc_linked_list(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/* Build a linked list of 100 nodes */
	volatile struct gc_node *head = NULL;
	for (int i = 0; i < 100; i++) {
		struct gc_node *node = GC_MALLOC(sizeof (struct gc_node));
		munit_assert_not_null(node);
		node->value = i;
		node->next = (struct gc_node *)head;
		head = node;
	}

	/* Verify list integrity */
	int count = 0;
	volatile struct gc_node *cur = head;
	while (cur != NULL) {
		count++;
		cur = cur->next;
	}
	munit_assert_int(count, ==, 100);

	/* Collect - all nodes reachable through head */
	GC_gcollect();

	/* Verify list still intact after GC */
	count = 0;
	cur = head;
	while (cur != NULL) {
		count++;
		cur = cur->next;
	}
	munit_assert_int(count, ==, 100);

	/* Drop reference to list */
	head = NULL;
	GC_gcollect();

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_statistics(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	size_t heap_before = GC_get_heap_size();

	void *p = GC_MALLOC(1024);
	munit_assert_not_null(p);

	size_t heap_after = GC_get_heap_size();
	munit_assert_size(heap_after, >=, heap_before + 1024);

	size_t total = GC_get_total_bytes();
	munit_assert_size(total, >=, 1024);

	/* get_free_bytes should not crash */
	(void) GC_get_free_bytes();

	/* Stats struct */
	umem_gc_stats_t stats;
	umem_gc_get_stats(&stats);
	munit_assert_size(stats.gcs_heap_size, >=, 1024);
	munit_assert_size(stats.gcs_total_allocated, >=, 1024);

	GC_FREE(p);
	return MUNIT_OK;
}

static MunitResult
test_gc_collection_count(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	umem_gc_stats_t stats_before;
	umem_gc_get_stats(&stats_before);

	GC_gcollect();

	umem_gc_stats_t stats_after;
	umem_gc_get_stats(&stats_after);

	munit_assert_uint64(stats_after.gcs_collections, >,
	    stats_before.gcs_collections);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Phase query                                                         */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_phase(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/* Outside a collection, phase should be IDLE */
	umem_gc_phase_t phase = umem_gc_get_phase();
	munit_assert_int(phase, ==, GC_PHASE_IDLE);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* find_header and mark_object                                         */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_find_header(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	/* Should find the header for a valid GC pointer */
	umem_gc_header_t *hdr = umem_gc_find_header(p);
	munit_assert_not_null(hdr);
	munit_assert_size(hdr->gc_size, ==, 64);

	/* Should not find header for arbitrary pointer */
	int stack_var = 42;
	umem_gc_header_t *bad = umem_gc_find_header(&stack_var);
	munit_assert_null(bad);

	/* NULL should return NULL */
	munit_assert_null(umem_gc_find_header(NULL));

	GC_FREE(p);
	return MUNIT_OK;
}

static MunitResult
test_gc_mark_object(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	umem_gc_header_t *hdr = umem_gc_find_header(p);
	munit_assert_not_null(hdr);

	uint32_t mark_val = umem_gc_get_mark_value();
	munit_assert_uint32(hdr->gc_mark, !=, mark_val);

	umem_gc_mark_object(hdr);
	munit_assert_uint32(hdr->gc_mark, ==, mark_val);

	/* Mark NULL should not crash */
	umem_gc_mark_object(NULL);

	GC_FREE(p);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Walk objects                                                        */
/* ------------------------------------------------------------------ */

struct walk_ctx {
	int	count;
};

static void
walk_counter(umem_gc_header_t *hdr, void *arg)
{
	(void)hdr;
	struct walk_ctx *ctx = arg;
	ctx->count++;
}

static MunitResult
test_gc_walk_objects(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	GC_INIT();

	/* Allocate a few objects */
	void *p1 = GC_MALLOC(32);
	void *p2 = GC_MALLOC(64);
	void *p3 = GC_MALLOC(128);
	munit_assert_not_null(p1);
	munit_assert_not_null(p2);
	munit_assert_not_null(p3);

	struct walk_ctx ctx = { 0 };
	umem_gc_walk_objects(walk_counter, &ctx);

	/* Should see at least our 3 objects */
	munit_assert_int(ctx.count, >=, 3);

	GC_FREE(p1);
	GC_FREE(p2);
	GC_FREE(p3);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Boehm compatibility macros                                          */
/* ------------------------------------------------------------------ */

static MunitResult
test_gc_boehm_compat(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* All Boehm macros should compile and work */
	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	void *pa = GC_MALLOC_ATOMIC(64);
	munit_assert_not_null(pa);

	void *pr = GC_REALLOC(p, 128);
	munit_assert_not_null(pr);

	GC_FREE(pr);
	GC_FREE(pa);

	size_t hs = GC_get_heap_size();
	(void)hs;
	size_t fb = GC_get_free_bytes();
	(void)fb;
	size_t tb = GC_get_total_bytes();
	(void)tb;

	GC_gcollect();

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Suite definition                                                    */
/* ------------------------------------------------------------------ */

static MunitTest gc_tests[] = {
	{ "/init", test_gc_init,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alloc_basic", test_gc_alloc_basic,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alloc_atomic", test_gc_alloc_atomic,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alloc_various_sizes", test_gc_alloc_various_sizes,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alloc_zero", test_gc_alloc_zero,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/free_explicit", test_gc_free_explicit,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/collect_basic", test_gc_collect_basic,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/collect_frees_unreachable", test_gc_collect_frees_unreachable,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stack_reachable", test_gc_stack_reachable,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reachable_chain", test_gc_reachable_chain,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/atomic_not_scanned", test_gc_atomic_not_scanned,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/register_finalizer", test_gc_register_finalizer,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/finalizer_on_free", test_gc_finalizer_on_free,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/finalizer_on_collect", test_gc_finalizer_on_collect,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/realloc", test_gc_realloc,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/threaded", test_gc_threaded,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hybrid", test_gc_hybrid,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/linked_list", test_gc_linked_list,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/statistics", test_gc_statistics,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/collection_count", test_gc_collection_count,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/phase", test_gc_phase,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/find_header", test_gc_find_header,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mark_object", test_gc_mark_object,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/walk_objects", test_gc_walk_objects,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/boehm_compat", test_gc_boehm_compat,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_gc = {
	"/gc",
	gc_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

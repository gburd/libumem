/*
 * Unit tests for the GC sparsemap (page-level hash map).
 */

#define UMEM_ENABLE_EXPERIMENTAL
#include "../munit.h"
#include "../../umem_sparsemap.h"
#include "../../umem_gc.h"
#include <umem.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Basic create/destroy                                                */
/* ------------------------------------------------------------------ */

static MunitResult
test_sparsemap_create_destroy(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_sparsemap_t *map = umem_sparsemap_create(0);
	munit_assert_not_null(map);
	munit_assert_size(umem_sparsemap_count(map), ==, 0);

	umem_sparsemap_destroy(map);

	/* Destroy NULL should be safe */
	umem_sparsemap_destroy(NULL);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Set and test                                                        */
/* ------------------------------------------------------------------ */

static MunitResult
test_sparsemap_set_test(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_sparsemap_t *map = umem_sparsemap_create(0);
	munit_assert_not_null(map);

	/* Allocate a real buffer to get a valid pointer */
	void *buf = umem_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(buf);

	/* Initially, the page should not be tracked */
	munit_assert_int(umem_sparsemap_test(map, buf), ==, 0);

	/* After set, the page should be tracked */
	int rc = umem_sparsemap_set(map, buf);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(umem_sparsemap_test(map, buf), !=, 0);
	munit_assert_size(umem_sparsemap_count(map), ==, 1);

	umem_free(buf, 64);
	umem_sparsemap_destroy(map);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Reference counting: multiple objects on the same page               */
/* ------------------------------------------------------------------ */

static MunitResult
test_sparsemap_refcount(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_sparsemap_t *map = umem_sparsemap_create(0);
	munit_assert_not_null(map);

	/*
	 * Allocate two small buffers that are likely on the same page.
	 * Use a single large allocation and derive two pointers from it.
	 */
	char *buf = umem_alloc(128, UMEM_DEFAULT);
	munit_assert_not_null(buf);

	void *ptr1 = buf;
	void *ptr2 = buf + 64;

	/* Set twice (same page) */
	munit_assert_int(umem_sparsemap_set(map, ptr1), ==, 0);
	munit_assert_int(umem_sparsemap_set(map, ptr2), ==, 0);

	/* Only one page entry, but count is still 1 (one page) */
	munit_assert_size(umem_sparsemap_count(map), ==, 1);

	/* Clear one reference - page should still be tracked */
	umem_sparsemap_clear(map, ptr1);
	munit_assert_int(umem_sparsemap_test(map, ptr2), !=, 0);
	munit_assert_size(umem_sparsemap_count(map), ==, 1);

	/* Clear second reference - page should be removed */
	umem_sparsemap_clear(map, ptr2);
	munit_assert_int(umem_sparsemap_test(map, ptr1), ==, 0);
	munit_assert_size(umem_sparsemap_count(map), ==, 0);

	umem_free(buf, 128);
	umem_sparsemap_destroy(map);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Clear on untracked page is a no-op                                  */
/* ------------------------------------------------------------------ */

static MunitResult
test_sparsemap_clear_untracked(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_sparsemap_t *map = umem_sparsemap_create(0);
	munit_assert_not_null(map);

	void *buf = umem_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(buf);

	/* Clearing a page that was never set should be safe */
	umem_sparsemap_clear(map, buf);
	munit_assert_size(umem_sparsemap_count(map), ==, 0);

	umem_free(buf, 64);
	umem_sparsemap_destroy(map);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test with NULL map                                                  */
/* ------------------------------------------------------------------ */

static MunitResult
test_sparsemap_null_map(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Test on NULL map should return 0 */
	int stack_var = 42;
	munit_assert_int(umem_sparsemap_test(NULL, &stack_var), ==, 0);
	munit_assert_size(umem_sparsemap_count(NULL), ==, 0);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Many entries (force resize)                                         */
/* ------------------------------------------------------------------ */

static MunitResult
test_sparsemap_resize(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Use a small initial capacity to force resizing */
	umem_sparsemap_t *map = umem_sparsemap_create(4);
	munit_assert_not_null(map);

	/*
	 * Insert entries for many distinct pages by using synthetic
	 * page-aligned addresses. Use 64K stride to guarantee each
	 * address lands on a separate page regardless of page size
	 * (4K on x86, 8K on SPARC, up to 64K on some ARM64).
	 */
	size_t n = 500;
	uintptr_t stride = 0x10000;	/* 64K between addresses */
	for (size_t i = 0; i < n; i++) {
		void *fake_ptr = (void *)(uintptr_t)(0x100000 + i * stride);
		int rc = umem_sparsemap_set(map, fake_ptr);
		munit_assert_int(rc, ==, 0);
	}

	munit_assert_size(umem_sparsemap_count(map), ==, n);

	/* Verify all pages are found */
	for (size_t i = 0; i < n; i++) {
		void *fake_ptr = (void *)(uintptr_t)(0x100000 + i * stride);
		munit_assert_int(umem_sparsemap_test(map, fake_ptr), !=, 0);
	}

	/* Verify a non-existent page is not found */
	void *missing = (void *)(uintptr_t)(0x100000 + n * stride);
	munit_assert_int(umem_sparsemap_test(map, missing), ==, 0);

	/* Clear all entries */
	for (size_t i = 0; i < n; i++) {
		void *fake_ptr = (void *)(uintptr_t)(0x100000 + i * stride);
		umem_sparsemap_clear(map, fake_ptr);
	}

	munit_assert_size(umem_sparsemap_count(map), ==, 0);

	umem_sparsemap_destroy(map);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Integration: GC alloc/free updates the sparsemap                    */
/* ------------------------------------------------------------------ */

static MunitResult
test_sparsemap_gc_integration(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/*
	 * Verify that umem_gc_find_header() works correctly with the
	 * sparsemap fast path. This tests the full integration.
	 */
	GC_INIT();

	void *p = GC_MALLOC(64);
	munit_assert_not_null(p);

	/* find_header should succeed for a valid GC pointer */
	umem_gc_header_t *hdr = umem_gc_find_header(p);
	munit_assert_not_null(hdr);
	munit_assert_size(hdr->gc_size, ==, 64);

	/* find_header should fail for a stack pointer (fast path reject) */
	int stack_var = 42;
	munit_assert_null(umem_gc_find_header(&stack_var));

	/* After freeing, find_header should fail */
	GC_FREE(p);

	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Suite definition                                                    */
/* ------------------------------------------------------------------ */

static MunitTest sparsemap_tests[] = {
	{ "/create_destroy", test_sparsemap_create_destroy,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/set_test", test_sparsemap_set_test,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/refcount", test_sparsemap_refcount,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/clear_untracked", test_sparsemap_clear_untracked,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/null_map", test_sparsemap_null_map,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/resize", test_sparsemap_resize,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/gc_integration", test_sparsemap_gc_integration,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_sparsemap = {
	"/sparsemap",
	sparsemap_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

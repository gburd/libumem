/*
 * Unit tests for Small-Buffer Optimization (SBO)
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../umem_ptc.h"
#include <string.h>

static MunitResult
test_sbo_basic_alloc(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Ensure umem is initialized (PTC subsystem needs it) */
	void *init = umem_alloc(8, UMEM_DEFAULT);
	if (init != NULL)
		umem_free(init, 8);

	if (!umem_sbo_enabled()) {
		return MUNIT_SKIP;
	}

	umem_sbo_reset();

	void *p = umem_sbo_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(p);

	/* Verify the pointer is 16-byte aligned */
	munit_assert_size((uintptr_t)p % 16, ==, 0);

	/* Write to allocated memory */
	memset(p, 0xAB, 64);

	return MUNIT_OK;
}

static MunitResult
test_sbo_multiple_allocs(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	if (!umem_sbo_enabled()) {
		return MUNIT_SKIP;
	}

	umem_sbo_reset();

	void *ptrs[16];
	for (int i = 0; i < 16; i++) {
		ptrs[i] = umem_sbo_alloc(32, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
		munit_assert_size((uintptr_t)ptrs[i] % 16, ==, 0);
	}

	/* Verify no overlaps */
	for (int i = 0; i < 15; i++) {
		munit_assert_ptr_not_equal(ptrs[i], ptrs[i + 1]);
		size_t gap = (size_t)((char *)ptrs[i + 1] - (char *)ptrs[i]);
		munit_assert_size(gap, >=, 32);
	}

	return MUNIT_OK;
}

static MunitResult
test_sbo_free_is_noop(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	if (!umem_sbo_enabled()) {
		return MUNIT_SKIP;
	}

	umem_sbo_reset();

	void *p = umem_sbo_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(p);

	/* umem_sbo_free should return 1 (recognized as SBO pointer) */
	int rc = umem_sbo_free(p, 64);
	munit_assert_int(rc, ==, 1);

	/* Non-SBO pointer should return 0 */
	void *heap = umem_alloc(64, UMEM_DEFAULT);
	rc = umem_sbo_free(heap, 64);
	munit_assert_int(rc, ==, 0);
	umem_free(heap, 64);

	return MUNIT_OK;
}

static MunitResult
test_sbo_too_large(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	if (!umem_sbo_enabled()) {
		return MUNIT_SKIP;
	}

	umem_sbo_reset();

	/* Allocations > UMEM_SBO_MAXALLOC should return NULL */
	void *p = umem_sbo_alloc(UMEM_SBO_MAXALLOC + 1, UMEM_DEFAULT);
	munit_assert_null(p);

	/* Zero-size allocation should return NULL */
	p = umem_sbo_alloc(0, UMEM_DEFAULT);
	munit_assert_null(p);

	return MUNIT_OK;
}

static MunitResult
test_sbo_exhaustion_resets(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	if (!umem_sbo_enabled()) {
		return MUNIT_SKIP;
	}

	umem_sbo_reset();

	/*
	 * Fill the buffer: 4096 / 128 = 32 allocations of max size.
	 * The 33rd should trigger an automatic reset and succeed.
	 */
	for (int i = 0; i < 32; i++) {
		void *p = umem_sbo_alloc(UMEM_SBO_MAXALLOC, UMEM_DEFAULT);
		munit_assert_not_null(p);
	}

	/* This should trigger reset and succeed */
	void *p = umem_sbo_alloc(UMEM_SBO_MAXALLOC, UMEM_DEFAULT);
	munit_assert_not_null(p);

	return MUNIT_OK;
}

static MunitResult
test_sbo_reset(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	if (!umem_sbo_enabled()) {
		return MUNIT_SKIP;
	}

	umem_sbo_reset();

	void *p1 = umem_sbo_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(p1);

	umem_sbo_reset();

	/* After reset, next alloc should reuse the buffer from the start */
	void *p2 = umem_sbo_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(p2);
	munit_assert_ptr_equal(p1, p2);

	return MUNIT_OK;
}

static MunitResult
test_sbo_alignment(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	if (!umem_sbo_enabled()) {
		return MUNIT_SKIP;
	}

	umem_sbo_reset();

	/* Allocate odd sizes — all should be 16-byte aligned */
	size_t sizes[] = {1, 3, 7, 13, 17, 31, 65, 100, 127};
	int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

	for (int i = 0; i < nsizes; i++) {
		void *p = umem_sbo_alloc(sizes[i], UMEM_DEFAULT);
		munit_assert_not_null(p);
		munit_assert_size((uintptr_t)p % 16, ==, 0);
	}

	return MUNIT_OK;
}

static MunitTest sbo_tests[] = {
	{ "/basic_alloc", test_sbo_basic_alloc,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/multiple_allocs", test_sbo_multiple_allocs,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/free_is_noop", test_sbo_free_is_noop,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/too_large", test_sbo_too_large,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exhaustion_resets", test_sbo_exhaustion_resets,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reset", test_sbo_reset,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alignment", test_sbo_alignment,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_sbo = {
	"/sbo",
	sbo_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

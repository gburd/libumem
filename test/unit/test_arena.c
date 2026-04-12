/*
 * Unit tests for the scoped arena allocator
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../umem_arena.h"
#include <string.h>

static MunitResult
test_arena_create_destroy(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_arena_t *arena = umem_arena_create(4096, UMEM_DEFAULT);
	munit_assert_not_null(arena);
	munit_assert_size(umem_arena_capacity(arena), >=, 4096);
	munit_assert_size(umem_arena_available(arena), ==,
	    umem_arena_capacity(arena));

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_basic_alloc(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_arena_t *arena = umem_arena_create(65536, UMEM_DEFAULT);
	munit_assert_not_null(arena);

	void *p = umem_arena_alloc(arena, 100);
	munit_assert_not_null(p);

	/* Should be 16-byte aligned */
	munit_assert_size((uintptr_t)p % 16, ==, 0);

	memset(p, 0x42, 100);

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_multiple_allocs(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_arena_t *arena = umem_arena_create(65536, UMEM_DEFAULT);
	munit_assert_not_null(arena);

	void *ptrs[100];
	for (int i = 0; i < 100; i++) {
		ptrs[i] = umem_arena_alloc(arena, 64);
		munit_assert_not_null(ptrs[i]);
		memset(ptrs[i], i & 0xFF, 64);
	}

	/* Verify no overlaps */
	for (int i = 0; i < 99; i++) {
		size_t gap = (size_t)((char *)ptrs[i + 1] - (char *)ptrs[i]);
		munit_assert_size(gap, >=, 64);
	}

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_exhaustion(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_arena_t *arena = umem_arena_create(256, UMEM_DEFAULT);
	munit_assert_not_null(arena);

	/* Fill the arena */
	size_t cap = umem_arena_capacity(arena);
	void *p = umem_arena_alloc(arena, cap);
	munit_assert_not_null(p);

	/* Next alloc should fail */
	void *p2 = umem_arena_alloc(arena, 1);
	munit_assert_null(p2);
	munit_assert_size(umem_arena_available(arena), ==, 0);

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_reset(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_arena_t *arena = umem_arena_create(4096, UMEM_DEFAULT);
	munit_assert_not_null(arena);

	size_t initial_avail = umem_arena_available(arena);

	void *p1 = umem_arena_alloc(arena, 1024);
	munit_assert_not_null(p1);
	munit_assert_size(umem_arena_available(arena), <, initial_avail);

	umem_arena_reset(arena);
	munit_assert_size(umem_arena_available(arena), ==, initial_avail);

	/* After reset, can allocate again from the beginning */
	void *p2 = umem_arena_alloc(arena, 1024);
	munit_assert_not_null(p2);
	munit_assert_ptr_equal(p1, p2);

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_zero_size(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Zero capacity should fail */
	umem_arena_t *arena = umem_arena_create(0, UMEM_DEFAULT);
	munit_assert_null(arena);

	/* Zero-size alloc should fail */
	arena = umem_arena_create(4096, UMEM_DEFAULT);
	munit_assert_not_null(arena);

	void *p = umem_arena_alloc(arena, 0);
	munit_assert_null(p);

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_null_safety(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* All functions should handle NULL gracefully */
	munit_assert_null(umem_arena_alloc(NULL, 100));
	munit_assert_size(umem_arena_available(NULL), ==, 0);
	munit_assert_size(umem_arena_capacity(NULL), ==, 0);
	umem_arena_reset(NULL);
	umem_arena_destroy(NULL);

	return MUNIT_OK;
}

static MunitResult
test_arena_alignment(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_arena_t *arena = umem_arena_create(65536, UMEM_DEFAULT);
	munit_assert_not_null(arena);

	/* Allocate odd sizes and check alignment */
	size_t sizes[] = {1, 3, 7, 13, 17, 31, 65, 100, 255, 1000};
	int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

	for (int i = 0; i < nsizes; i++) {
		void *p = umem_arena_alloc(arena, sizes[i]);
		munit_assert_not_null(p);
		munit_assert_size((uintptr_t)p % 16, ==, 0);
	}

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_large_capacity(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Create a 1MB arena */
	umem_arena_t *arena = umem_arena_create(1024 * 1024, UMEM_DEFAULT);
	munit_assert_not_null(arena);
	munit_assert_size(umem_arena_capacity(arena), >=, 1024 * 1024);

	/* Allocate a large chunk */
	void *p = umem_arena_alloc(arena, 512 * 1024);
	munit_assert_not_null(p);
	memset(p, 0xCC, 512 * 1024);

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitResult
test_arena_repeated_reset(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_arena_t *arena = umem_arena_create(4096, UMEM_DEFAULT);
	munit_assert_not_null(arena);

	for (int round = 0; round < 100; round++) {
		for (int i = 0; i < 10; i++) {
			void *p = umem_arena_alloc(arena, 32);
			munit_assert_not_null(p);
			memset(p, round & 0xFF, 32);
		}
		umem_arena_reset(arena);
	}

	umem_arena_destroy(arena);

	return MUNIT_OK;
}

static MunitTest arena_tests[] = {
	{ "/create_destroy", test_arena_create_destroy,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/basic_alloc", test_arena_basic_alloc,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/multiple_allocs", test_arena_multiple_allocs,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exhaustion", test_arena_exhaustion,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reset", test_arena_reset,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/zero_size", test_arena_zero_size,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/null_safety", test_arena_null_safety,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alignment", test_arena_alignment,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/large_capacity", test_arena_large_capacity,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/repeated_reset", test_arena_repeated_reset,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_arena = {
	"/arena",
	arena_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

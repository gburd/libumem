/*
 * Test for realloc() bootstrap pointer bug fix
 *
 * Bug: When realloc() grows a bootstrap allocation, it must copy only
 * MIN(old_size, new_size) bytes to avoid reading past the end of the
 * old buffer, which caused segfaults in git, vi, and other programs.
 *
 * This test verifies the fix by allocating small buffers and growing
 * them with realloc(), ensuring no segfault and data integrity.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../munit.h"

/*
 * Test realloc() growing a small allocation to a larger size.
 * This simulates the git bug: realloc(ptr, 14027) where ptr has 8000 bytes.
 */
static MunitResult
test_realloc_grow(const MunitParameter params[], void *user_data)
{
	char *ptr;
	size_t small_size = 8000;
	size_t large_size = 14027;
	size_t i;

	(void)params;
	(void)user_data;

	/* Allocate small buffer */
	ptr = malloc(small_size);
	munit_assert_not_null(ptr);

	/* Fill with pattern */
	memset(ptr, 'A', small_size);

	/* Grow it - this used to read past the end and segfault */
	ptr = realloc(ptr, large_size);
	munit_assert_not_null(ptr);

	/* Verify old data is preserved */
	for (i = 0; i < small_size; i++) {
		munit_assert_char(ptr[i], ==, 'A');
	}

	free(ptr);
	return MUNIT_OK;
}

/*
 * Test realloc() shrinking an allocation.
 * This should copy only the new (smaller) size.
 */
static MunitResult
test_realloc_shrink(const MunitParameter params[], void *user_data)
{
	char *ptr;
	size_t large_size = 10000;
	size_t small_size = 5000;
	size_t i;

	(void)params;
	(void)user_data;

	/* Allocate large buffer */
	ptr = malloc(large_size);
	munit_assert_not_null(ptr);

	/* Fill with pattern */
	memset(ptr, 'B', large_size);

	/* Shrink it */
	ptr = realloc(ptr, small_size);
	munit_assert_not_null(ptr);

	/* Verify data is preserved up to new size */
	for (i = 0; i < small_size; i++) {
		munit_assert_char(ptr[i], ==, 'B');
	}

	free(ptr);
	return MUNIT_OK;
}

/*
 * Test realloc() with zero new size (should free the pointer).
 */
static MunitResult
test_realloc_zero(const MunitParameter params[], void *user_data)
{
	char *ptr;

	(void)params;
	(void)user_data;

	ptr = malloc(100);
	munit_assert_not_null(ptr);

	/* realloc(ptr, 0) should act like free(ptr) and return NULL */
	ptr = realloc(ptr, 0);
	munit_assert_null(ptr);

	return MUNIT_OK;
}

/*
 * Test realloc() with NULL pointer (should act like malloc).
 */
static MunitResult
test_realloc_null(const MunitParameter params[], void *user_data)
{
	char *ptr;

	(void)params;
	(void)user_data;

	/* realloc(NULL, size) should act like malloc(size) */
	ptr = realloc(NULL, 1000);
	munit_assert_not_null(ptr);

	memset(ptr, 'C', 1000);
	free(ptr);

	return MUNIT_OK;
}

/*
 * Test multiple realloc() operations in sequence.
 */
static MunitResult
test_realloc_multiple(const MunitParameter params[], void *user_data)
{
	char *ptr;
	size_t i;

	(void)params;
	(void)user_data;

	/* Start with small allocation */
	ptr = malloc(100);
	munit_assert_not_null(ptr);
	memset(ptr, 'D', 100);

	/* Grow multiple times */
	ptr = realloc(ptr, 500);
	munit_assert_not_null(ptr);
	for (i = 0; i < 100; i++) {
		munit_assert_char(ptr[i], ==, 'D');
	}

	ptr = realloc(ptr, 2000);
	munit_assert_not_null(ptr);
	for (i = 0; i < 100; i++) {
		munit_assert_char(ptr[i], ==, 'D');
	}

	ptr = realloc(ptr, 10000);
	munit_assert_not_null(ptr);
	for (i = 0; i < 100; i++) {
		munit_assert_char(ptr[i], ==, 'D');
	}

	/* Shrink */
	ptr = realloc(ptr, 1000);
	munit_assert_not_null(ptr);
	for (i = 0; i < 100; i++) {
		munit_assert_char(ptr[i], ==, 'D');
	}

	free(ptr);
	return MUNIT_OK;
}

static MunitTest test_suite_tests[] = {
	{
		"/grow",
		test_realloc_grow,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/shrink",
		test_realloc_shrink,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/zero",
		test_realloc_zero,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/null",
		test_realloc_null,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/multiple",
		test_realloc_multiple,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite test_suite = {
	"/realloc_bootstrap",
	test_suite_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&test_suite, NULL, argc, argv);
}

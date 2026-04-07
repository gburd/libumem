/*
 * Tests for integer overflow fixes
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include "munit.h"

/* External function for testing */
extern void *calloc(size_t nelem, size_t elsize);

/* Test: calloc integer overflow protection */
static MunitResult
test_calloc_overflow_SIZE_MAX(const MunitParameter params[], void* data)
{
	/* Try to overflow: SIZE_MAX / 2 + 1 elements of size 2 */
	size_t nelem = (SIZE_MAX / 2) + 1;
	size_t elsize = 2;

	errno = 0;
	void *ptr = calloc(nelem, elsize);

	/* Should fail with ENOMEM */
	munit_assert_null(ptr);
	munit_assert_int(errno, ==, ENOMEM);

	return MUNIT_OK;
}

/* Test: calloc overflow with large values */
static MunitResult
test_calloc_overflow_large(const MunitParameter params[], void* data)
{
	/* Try to overflow with smaller but still overflowing values */
	size_t nelem = SIZE_MAX / 3;
	size_t elsize = 4;

	errno = 0;
	void *ptr = calloc(nelem, elsize);

	/* Should fail with ENOMEM */
	munit_assert_null(ptr);
	munit_assert_int(errno, ==, ENOMEM);

	return MUNIT_OK;
}

/* Test: calloc normal operation still works */
static MunitResult
test_calloc_normal(const MunitParameter params[], void* data)
{
	size_t nelem = 100;
	size_t elsize = 50;

	void *ptr = calloc(nelem, elsize);
	munit_assert_not_null(ptr);

	/* Verify zeroed */
	unsigned char *bytes = (unsigned char *)ptr;
	for (size_t i = 0; i < nelem * elsize; i++) {
		munit_assert_uchar(bytes[i], ==, 0);
	}

	free(ptr);
	return MUNIT_OK;
}

/* Test: calloc with zero elements */
static MunitResult
test_calloc_zero_nelem(const MunitParameter params[], void* data)
{
	void *ptr = calloc(0, 100);
	/* Either NULL or valid pointer acceptable */
	if (ptr != NULL) {
		free(ptr);
	}

	return MUNIT_OK;
}

/* Test: calloc with zero element size */
static MunitResult
test_calloc_zero_elsize(const MunitParameter params[], void* data)
{
	void *ptr = calloc(100, 0);
	/* Either NULL or valid pointer acceptable */
	if (ptr != NULL) {
		free(ptr);
	}

	return MUNIT_OK;
}

/* Test: calloc with both zero */
static MunitResult
test_calloc_both_zero(const MunitParameter params[], void* data)
{
	void *ptr = calloc(0, 0);
	/* Either NULL or valid pointer acceptable */
	if (ptr != NULL) {
		free(ptr);
	}

	return MUNIT_OK;
}

/* Test: calloc near SIZE_MAX boundary */
static MunitResult
test_calloc_near_max(const MunitParameter params[], void* data)
{
	/* Try values near SIZE_MAX */
	size_t nelem = SIZE_MAX / 1024;
	size_t elsize = 1025;  /* Should overflow */

	errno = 0;
	void *ptr = calloc(nelem, elsize);

	/* Should fail */
	munit_assert_null(ptr);
	munit_assert_int(errno, ==, ENOMEM);

	return MUNIT_OK;
}

/* Test: calloc with maximum safe values */
static MunitResult
test_calloc_max_safe(const MunitParameter params[], void* data)
{
	/* Maximum value that shouldn't overflow: SIZE_MAX / 2 elements of size 1 */
	size_t nelem = SIZE_MAX / 2;
	size_t elsize = 1;

	void *ptr = calloc(nelem, elsize);
	/* May or may not succeed depending on available memory */
	/* This just verifies it doesn't crash */
	if (ptr != NULL) {
		free(ptr);
	}

	return MUNIT_OK;
}

/* Test suite */
static MunitTest overflow_fix_tests[] = {
	{ "/calloc_overflow_SIZE_MAX", test_calloc_overflow_SIZE_MAX, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/calloc_overflow_large", test_calloc_overflow_large, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/calloc_normal", test_calloc_normal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/calloc_zero_nelem", test_calloc_zero_nelem, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/calloc_zero_elsize", test_calloc_zero_elsize, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/calloc_both_zero", test_calloc_both_zero, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/calloc_near_max", test_calloc_near_max, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/calloc_max_safe", test_calloc_max_safe, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_overflow_fixes = {
	"/overflow_fixes", overflow_fix_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

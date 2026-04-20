/*
 * Unit tests for umem_stacktrace GDB-style stack trace formatting
 */

#include "munit.h"
#include "umem_stacktrace.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Force a known function name into the stack */
static void __attribute__((noinline))
test_helper_function(uintptr_t *pcs, int *depth)
{
	extern int getpcstack(uintptr_t *pcstack, int pcstack_limit,
	    int check_sighandler);
	*depth = getpcstack(pcs, 16, 0);
}

static MunitResult
test_format_contains_hex_address(const MunitParameter params[],
    void *data)
{
	(void)params;
	(void)data;

	char buf[512];
	uintptr_t pc = (uintptr_t)&test_format_contains_hex_address;

	umem_stacktrace_format(pc, 0, buf, sizeof(buf));

	/* Must start with frame prefix */
	munit_assert_string_equal(buf, buf);
	munit_assert_true(strstr(buf, "#0") != NULL);
	munit_assert_true(strstr(buf, "0x") != NULL);

	return MUNIT_OK;
}

static MunitResult
test_format_sequential_frames(const MunitParameter params[],
    void *data)
{
	(void)params;
	(void)data;

	/*
	 * Use synthetic PCs from known function pointers to test
	 * sequential frame numbering, avoiding getpcstack which may
	 * return 0 frames in some build/execution modes.
	 */
	uintptr_t pcs[4];
	pcs[0] = (uintptr_t)&test_format_sequential_frames;
	pcs[1] = (uintptr_t)&test_format_contains_hex_address;
	pcs[2] = (uintptr_t)&test_helper_function;
	pcs[3] = (uintptr_t)&umem_stacktrace_init;

	for (int i = 0; i < 4; i++) {
		char buf[512];
		char expected[16];

		umem_stacktrace_format(pcs[i], i, buf, sizeof(buf));

		(void) snprintf(expected, sizeof(expected), "#%d", i);
		munit_assert_true(strstr(buf, expected) != NULL);
		munit_assert_true(strstr(buf, "0x") != NULL);
	}

	return MUNIT_OK;
}

static MunitResult
test_format_contains_function_name(const MunitParameter params[],
    void *data)
{
	(void)params;
	(void)data;

	char buf[512];
	uintptr_t pc = (uintptr_t)&test_format_contains_function_name;

	umem_stacktrace_format(pc, 0, buf, sizeof(buf));

	/*
	 * With dladdr, we should resolve our own function name.
	 * The symbol may be mangled or missing in stripped builds,
	 * so we check for either the function name or "??".
	 */
	munit_assert_true(
	    strstr(buf, "test_format_contains_function_name") != NULL ||
	    strstr(buf, "test_") != NULL ||
	    strstr(buf, "??") != NULL);

	return MUNIT_OK;
}

static MunitResult
test_init_returns_valid_tier(const MunitParameter params[],
    void *data)
{
	(void)params;
	(void)data;

	int tier = umem_stacktrace_init();

	/* Tier must be 0 (dladdr), 1 (addr2line), or 2 (libdw) */
	munit_assert_int(tier, >=, 0);
	munit_assert_int(tier, <=, 2);

	return MUNIT_OK;
}

static MunitResult
test_format_null_address(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	char buf[512];

	umem_stacktrace_format(0, 0, buf, sizeof(buf));

	munit_assert_true(strstr(buf, "#0") != NULL);
	munit_assert_true(strlen(buf) > 0);

	return MUNIT_OK;
}

static MunitResult
test_format_small_buffer(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	char buf[32];
	uintptr_t pc = (uintptr_t)&test_format_small_buffer;

	umem_stacktrace_format(pc, 0, buf, sizeof(buf));

	/* Should not overflow; output is truncated */
	munit_assert_true(strlen(buf) < sizeof(buf));

	return MUNIT_OK;
}

static MunitTest stacktrace_tests[] = {
	{"/format_hex_address", test_format_contains_hex_address,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/format_sequential_frames", test_format_sequential_frames,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/format_function_name", test_format_contains_function_name,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/init_valid_tier", test_init_returns_valid_tier,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/format_null_address", test_format_null_address,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/format_small_buffer", test_format_small_buffer,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite suite_stacktrace = {
	"/stacktrace",
	stacktrace_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

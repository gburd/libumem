/*
 * Unit tests for umem debug modes
 *
 * Note: Some debug modes (like REDZONE/FIREWALL) may abort on violations,
 * so those tests need careful handling.
 */

#include "../munit.h"
#include "../../umem.h"
#include <string.h>
#include <stdlib.h>

/* Test: UMF_AUDIT enables audit tracking */
static MunitResult test_audit_enabled(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Set UMEM_DEBUG environment variable */
    setenv("UMEM_DEBUG", "audit", 1);

    /* Allocate with audit enabled */
    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* With audit, allocations are tracked with stack traces */
    /* We can't easily verify this without exposing internal APIs,
     * but at least verify it doesn't crash */

    umem_free(ptr, 128);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: UMF_DEADBEEF fills freed memory */
static MunitResult test_deadbeef(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "deadbeef", 1);

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Fill with known pattern */
    memset(ptr, 0x42, 128);

    /* Free - should fill with 0xdeadbeef pattern */
    umem_free(ptr, 128);

    /* Note: We can't safely read freed memory to verify the pattern,
     * as it may be reused. This test just ensures deadbeef mode
     * doesn't crash. */

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: UMF_CONTENTS sets initial contents */
static MunitResult test_contents(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* contents=0xab sets initial pattern */
    setenv("UMEM_DEBUG", "contents=0xab", 1);

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Check if memory has the pattern (may not be guaranteed) */
    /* This is more of a smoke test */

    umem_free(ptr, 128);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: multiple debug flags */
static MunitResult test_multiple_flags(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "audit,deadbeef", 1);

    void *ptr = umem_alloc(256, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    memset(ptr, 0xFF, 256);
    umem_free(ptr, 256);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: default debug mode */
static MunitResult test_default_mode(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "default", 1);

    /* Default enables common debug features */
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    umem_free(ptr, 64);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: debug mode with cache */
static MunitResult test_debug_with_cache(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "audit", 1);

    umem_cache_t *cache = umem_cache_create(
        "debug_cache",
        64,
        0, NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    umem_cache_free(cache, obj);
    umem_cache_destroy(cache);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: verbose debug logging */
static MunitResult test_verbose(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "verbose", 1);

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    umem_free(ptr, 128);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Note: REDZONE and FIREWALL tests are omitted because they can abort
 * the process on buffer overflows. These should be tested manually or
 * in subprocess tests. */

/* Test array */
static MunitTest debug_tests[] = {
    { "/audit", test_audit_enabled, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/deadbeef", test_deadbeef, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/contents", test_contents, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple_flags", test_multiple_flags, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/default", test_default_mode, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/with_cache", test_debug_with_cache, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verbose", test_verbose, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_debug = {
    "/umem_debug",
    debug_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

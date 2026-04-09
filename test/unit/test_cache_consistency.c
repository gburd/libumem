/*
 * Cache consistency checking tests
 *
 * Validates internal umem data structures remain consistent
 * across various operations. Similar to jemalloc/tcmalloc tests.
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../umem_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External verification function */
extern int umem_verify_all_caches(void);

/* Helper to initialize umem */
static void ensure_umem_initialized(void) {
    static int initialized = 0;
    if (!initialized) {
        void *ptr = umem_alloc(64, UMEM_DEFAULT);
        if (ptr) {
            umem_free(ptr, 64);
            initialized = 1;
        }
    }
}

/* Test: Basic cache verification */
static MunitResult test_verify_after_init(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    ensure_umem_initialized();

    /* Verify all caches are consistent after initialization */
    int errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    return MUNIT_OK;
}

/* Test: Verification after simple alloc/free */
static MunitResult test_verify_after_alloc_free(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    ensure_umem_initialized();

    /* Do some allocations */
    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = umem_alloc(64, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }

    /* Free them */
    for (int i = 0; i < 10; i++) {
        umem_free(ptrs[i], 64);
    }

    /* Verify consistency */
    int errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    return MUNIT_OK;
}

/* Test: Verification with cache create/destroy */
static MunitResult test_verify_with_custom_cache(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    ensure_umem_initialized();

    /* Create a custom cache */
    umem_cache_t *cp = umem_cache_create(
        "test_verify", 128, 8,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    /* Verify consistency after creation */
    int errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    /* Allocate from cache */
    void *ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }

    /* Verify consistency after allocations */
    errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    /* Free */
    for (int i = 0; i < 5; i++) {
        umem_cache_free(cp, ptrs[i]);
    }

    /* Verify consistency after frees */
    errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    /* Destroy cache */
    umem_cache_destroy(cp);

    /* Final verification */
    errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    return MUNIT_OK;
}

/* Test: Verification with mixed sizes */
static MunitResult test_verify_mixed_sizes(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    ensure_umem_initialized();

    /* Allocate various sizes */
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    void *ptrs[9];

    for (int i = 0; i < 9; i++) {
        ptrs[i] = umem_alloc(sizes[i], UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }

    /* Verify consistency */
    int errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    /* Free in reverse order */
    for (int i = 8; i >= 0; i--) {
        umem_free(ptrs[i], sizes[i]);
    }

    /* Final verification */
    errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    return MUNIT_OK;
}

/* Test: Verification with aligned allocations */
static MunitResult test_verify_aligned(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    ensure_umem_initialized();

    /* Allocate with various alignments */
    void *p1 = umem_alloc_align(128, 16, UMEM_DEFAULT);
    void *p2 = umem_alloc_align(128, 64, UMEM_DEFAULT);
    void *p3 = umem_alloc_align(128, 256, UMEM_DEFAULT);

    munit_assert_not_null(p1);
    munit_assert_not_null(p2);
    munit_assert_not_null(p3);

    /* Verify consistency */
    int errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    /* Free */
    umem_free_align(p1, 128);
    umem_free_align(p2, 128);
    umem_free_align(p3, 128);

    /* Final verification */
    errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    return MUNIT_OK;
}

/* Test: Verification stress test */
static MunitResult test_verify_stress(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    ensure_umem_initialized();

    /* Allocate and free many small buffers */
    for (int iter = 0; iter < 100; iter++) {
        void *ptrs[20];
        for (int i = 0; i < 20; i++) {
            ptrs[i] = umem_alloc(32, UMEM_DEFAULT);
        }
        for (int i = 0; i < 20; i++) {
            umem_free(ptrs[i], 32);
        }
    }

    /* Verify consistency after stress */
    int errors = umem_verify_all_caches();
    munit_assert_int(errors, ==, 0);

    return MUNIT_OK;
}

/* Test array */
static MunitTest consistency_tests[] = {
    { "/verify_after_init", test_verify_after_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verify_after_alloc_free", test_verify_after_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verify_with_custom_cache", test_verify_with_custom_cache, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verify_mixed_sizes", test_verify_mixed_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verify_aligned", test_verify_aligned, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verify_stress", test_verify_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_cache_consistency = {
    "/cache_consistency",
    consistency_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

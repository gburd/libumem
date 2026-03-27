/*
 * Unit tests for umem_alloc, umem_free, umem_zalloc
 */

#include "../munit.h"
#include "../../umem.h"
#include <string.h>

/* Test: basic allocation and free */
static MunitResult test_alloc_free(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Write to the memory */
    memset(ptr, 0x42, 64);

    umem_free(ptr, 64);

    return MUNIT_OK;
}

/* Test: umem_zalloc zeros memory */
static MunitResult test_zalloc_zeros(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t size = 1024;
    void *ptr = umem_zalloc(size, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Verify all bytes are zero */
    unsigned char *bytes = (unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        munit_assert_uint8(bytes[i], ==, 0);
    }

    umem_free(ptr, size);

    return MUNIT_OK;
}

/* Test: various allocation sizes */
static MunitResult test_various_sizes(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        void *ptr = umem_alloc(sizes[i], UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        /* Touch the memory */
        memset(ptr, i & 0xFF, sizes[i]);

        umem_free(ptr, sizes[i]);
    }

    return MUNIT_OK;
}

/* Test: many small allocations */
static MunitResult test_many_small(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define NUM_ALLOCS 1000
    void *ptrs[NUM_ALLOCS];

    /* Allocate */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = umem_alloc(32, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }

    /* Free */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        umem_free(ptrs[i], 32);
    }

    return MUNIT_OK;
}

/* Test: UMEM_NOFAIL never returns NULL */
static MunitResult test_nofail(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_alloc(128, UMEM_NOFAIL);
    munit_assert_not_null(ptr);

    umem_free(ptr, 128);

    return MUNIT_OK;
}

/* Test: allocations are properly aligned */
static MunitResult test_alignment(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    for (int i = 0; i < 100; i++) {
        void *ptr = umem_alloc(i + 1, UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        /* Should be at least pointer-aligned */
        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % sizeof(void*), ==, 0);

        umem_free(ptr, i + 1);
    }

    return MUNIT_OK;
}

/* Test: allocations don't overlap */
static MunitResult test_no_overlap(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define NUM_PTRS 100
    void *ptrs[NUM_PTRS];
    size_t size = 64;

    /* Allocate and mark each allocation */
    for (int i = 0; i < NUM_PTRS; i++) {
        ptrs[i] = umem_alloc(size, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
        memset(ptrs[i], i & 0xFF, size);
    }

    /* Verify each allocation still has its marker */
    for (int i = 0; i < NUM_PTRS; i++) {
        unsigned char *bytes = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < size; j++) {
            munit_assert_uint8(bytes[j], ==, (unsigned char)(i & 0xFF));
        }
    }

    /* Free */
    for (int i = 0; i < NUM_PTRS; i++) {
        umem_free(ptrs[i], size);
    }

    return MUNIT_OK;
}

/* Test: free with size 0 (umem allows this) */
static MunitResult test_free_size_zero(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* umem_free with size 0 should work */
    umem_free(ptr, 0);

    return MUNIT_OK;
}

/* Test: allocate max reasonable size */
static MunitResult test_large_allocation(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* 1 MB allocation */
    size_t size = 1024 * 1024;
    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Touch pages to ensure they're allocated */
    unsigned char *bytes = (unsigned char *)ptr;
    for (size_t i = 0; i < size; i += 4096) {
        bytes[i] = 0xFF;
    }

    umem_free(ptr, size);

    return MUNIT_OK;
}

/* Test: rapid alloc/free pattern */
static MunitResult test_rapid_alloc_free(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    for (int i = 0; i < 10000; i++) {
        void *ptr = umem_alloc(64, UMEM_DEFAULT);
        munit_assert_not_null(ptr);
        umem_free(ptr, 64);
    }

    return MUNIT_OK;
}

/* Test array */
static MunitTest alloc_tests[] = {
    { "/alloc_free", test_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/zalloc_zeros", test_zalloc_zeros, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/various_sizes", test_various_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/many_small", test_many_small, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nofail", test_nofail, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/alignment", test_alignment, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/no_overlap", test_no_overlap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/free_size_zero", test_free_size_zero, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/large_allocation", test_large_allocation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/rapid_alloc_free", test_rapid_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_alloc = {
    "/umem_alloc",
    alloc_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

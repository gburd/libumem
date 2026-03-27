/*
 * Unit tests for umem_alloc_align and umem_free_align
 */

#include "../munit.h"
#include "../../umem.h"
#include <string.h>

/* Test: basic aligned allocation */
static MunitResult test_align_basic(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t size = 128;
    size_t align = 64;

    void *ptr = umem_alloc_align(size, align, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Verify alignment */
    uintptr_t addr = (uintptr_t)ptr;
    munit_assert_uint64(addr % align, ==, 0);

    /* Use the memory */
    memset(ptr, 0x42, size);

    umem_free_align(ptr, size);

    return MUNIT_OK;
}

/* Test: various power-of-2 alignments */
static MunitResult test_align_powers_of_2(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t alignments[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int num_aligns = sizeof(alignments) / sizeof(alignments[0]);

    for (int i = 0; i < num_aligns; i++) {
        size_t align = alignments[i];
        size_t size = align * 2;  /* Size larger than alignment */

        void *ptr = umem_alloc_align(size, align, UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % align, ==, 0);

        umem_free_align(ptr, size);
    }

    return MUNIT_OK;
}

/* Test: page-aligned allocation */
static MunitResult test_align_page(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t page_size = 4096;
    size_t size = page_size * 4;

    void *ptr = umem_alloc_align(size, page_size, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    uintptr_t addr = (uintptr_t)ptr;
    munit_assert_uint64(addr % page_size, ==, 0);

    /* Touch all pages */
    for (size_t i = 0; i < size; i += page_size) {
        ((char*)ptr)[i] = 0xFF;
    }

    umem_free_align(ptr, size);

    return MUNIT_OK;
}

/* Test: small size with large alignment */
static MunitResult test_align_small_size_large_align(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t size = 16;
    size_t align = 1024;

    void *ptr = umem_alloc_align(size, align, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    uintptr_t addr = (uintptr_t)ptr;
    munit_assert_uint64(addr % align, ==, 0);

    umem_free_align(ptr, size);

    return MUNIT_OK;
}

/* Test: multiple aligned allocations don't overlap */
static MunitResult test_align_no_overlap(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define NUM_PTRS 50
    void *ptrs[NUM_PTRS];
    size_t size = 128;
    size_t align = 128;

    /* Allocate and mark */
    for (int i = 0; i < NUM_PTRS; i++) {
        ptrs[i] = umem_alloc_align(size, align, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);

        /* Verify alignment */
        uintptr_t addr = (uintptr_t)ptrs[i];
        munit_assert_uint64(addr % align, ==, 0);

        memset(ptrs[i], i & 0xFF, size);
    }

    /* Verify marks */
    for (int i = 0; i < NUM_PTRS; i++) {
        unsigned char *bytes = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < size; j++) {
            munit_assert_uint8(bytes[j], ==, (unsigned char)(i & 0xFF));
        }
    }

    /* Free */
    for (int i = 0; i < NUM_PTRS; i++) {
        umem_free_align(ptrs[i], size);
    }

    return MUNIT_OK;
}

/* Test: UMEM_NOFAIL with alignment */
static MunitResult test_align_nofail(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_alloc_align(256, 256, UMEM_NOFAIL);
    munit_assert_not_null(ptr);

    uintptr_t addr = (uintptr_t)ptr;
    munit_assert_uint64(addr % 256, ==, 0);

    umem_free_align(ptr, 256);

    return MUNIT_OK;
}

/* Test: large aligned allocation */
static MunitResult test_align_large(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t size = 1024 * 1024;  /* 1 MB */
    size_t align = 4096;

    void *ptr = umem_alloc_align(size, align, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    uintptr_t addr = (uintptr_t)ptr;
    munit_assert_uint64(addr % align, ==, 0);

    /* Touch memory */
    memset(ptr, 0, size);

    umem_free_align(ptr, size);

    return MUNIT_OK;
}

/* Test: various sizes with same alignment */
static MunitResult test_align_various_sizes(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t align = 64;
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        void *ptr = umem_alloc_align(sizes[i], align, UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % align, ==, 0);

        umem_free_align(ptr, sizes[i]);
    }

    return MUNIT_OK;
}

/* Test: repeated alloc/free with alignment */
static MunitResult test_align_repeated(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t size = 256;
    size_t align = 256;

    for (int i = 0; i < 1000; i++) {
        void *ptr = umem_alloc_align(size, align, UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % align, ==, 0);

        umem_free_align(ptr, size);
    }

    return MUNIT_OK;
}

/* Test array */
static MunitTest align_tests[] = {
    { "/basic", test_align_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/powers_of_2", test_align_powers_of_2, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/page_aligned", test_align_page, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/small_size_large_align", test_align_small_size_large_align, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/no_overlap", test_align_no_overlap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nofail", test_align_nofail, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/large", test_align_large, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/various_sizes", test_align_various_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/repeated", test_align_repeated, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_align = {
    "/umem_align",
    align_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

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

/* Test: free with size 0 (only NULL, 0 is safe) */
static MunitResult test_free_size_zero(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* umem_free with correct size */
    umem_free(ptr, 128);

    /* umem_free(NULL, 0) is explicitly safe */
    umem_free(NULL, 0);

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

/* Test: zero-size allocation */
static MunitResult test_alloc_zero_size(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_alloc(0, UMEM_DEFAULT);
    /* Zero-size allocation should either return non-NULL or NULL
     * Both are valid behaviors. If non-NULL, free it. */
    if (ptr != NULL) {
        umem_free(ptr, 0);
    }

    return MUNIT_OK;
}

/* Test: allocation larger than UMEM_MAXBUF (goes through vmem) */
static MunitResult test_alloc_huge(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* UMEM_MAXBUF is 131072 (128KB), allocate 200KB */
    size_t size = 200 * 1024;
    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Touch some pages to ensure the allocation is real */
    unsigned char *bytes = (unsigned char *)ptr;
    for (size_t i = 0; i < size; i += 4096) {
        bytes[i] = 0xAA;
    }

    /* Verify we can read back */
    for (size_t i = 0; i < size; i += 4096) {
        munit_assert_uint8(bytes[i], ==, 0xAA);
    }

    umem_free(ptr, size);

    return MUNIT_OK;
}

/* Test: freeing NULL pointer */
static MunitResult test_free_null(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Only umem_free(NULL, 0) is explicitly safe per umem.c:2327-2328
     * umem_free(NULL, size) where size > 0 is not safe and will crash */
    umem_free(NULL, 0);

    return MUNIT_OK;
}

/* Test: stress test with mixed random sizes */
static MunitResult test_alloc_stress_mixed_sizes(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define STRESS_ITERATIONS 1000
    void *ptrs[STRESS_ITERATIONS];
    size_t sizes[STRESS_ITERATIONS];

    /* Use simple pseudo-random for reproducibility */
    unsigned int seed = 42;

    /* Allocate random sizes */
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        /* Generate size between 1 and 8192 bytes */
        seed = seed * 1103515245 + 12345;
        sizes[i] = (seed % 8192) + 1;

        ptrs[i] = umem_alloc(sizes[i], UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);

        /* Touch the memory */
        memset(ptrs[i], (i & 0xFF), sizes[i]);
    }

    /* Free in same order */
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        umem_free(ptrs[i], sizes[i]);
    }

    return MUNIT_OK;
}

/* Test: memory reuse after free */
static MunitResult test_reuse_freed_memory(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t size = 128;
    void *ptr1, *ptr2, *ptr3;

    /* Allocate and free same size multiple times */
    ptr1 = umem_alloc(size, UMEM_DEFAULT);
    munit_assert_not_null(ptr1);
    memset(ptr1, 0xAA, size);
    umem_free(ptr1, size);

    ptr2 = umem_alloc(size, UMEM_DEFAULT);
    munit_assert_not_null(ptr2);
    memset(ptr2, 0xBB, size);
    umem_free(ptr2, size);

    ptr3 = umem_alloc(size, UMEM_DEFAULT);
    munit_assert_not_null(ptr3);

    /* With a good allocator, we should often see the same address reused
     * This is not guaranteed but indicates good cache behavior */
    /* Just verify we got valid memory - don't assert on reuse */
    memset(ptr3, 0xCC, size);
    umem_free(ptr3, size);

    return MUNIT_OK;
}

/* Test: umem_reap() doesn't crash */
static MunitResult test_umem_reap(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Allocate some memory to have something to reap */
    #define REAP_ALLOCS 100
    void *ptrs[REAP_ALLOCS];

    for (int i = 0; i < REAP_ALLOCS; i++) {
        ptrs[i] = umem_alloc(1024, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }

    /* Free half of them */
    for (int i = 0; i < REAP_ALLOCS / 2; i++) {
        umem_free(ptrs[i], 1024);
    }

    /* Call umem_reap() - should not crash */
    umem_reap();

    /* Free the rest */
    for (int i = REAP_ALLOCS / 2; i < REAP_ALLOCS; i++) {
        umem_free(ptrs[i], 1024);
    }

    /* Call umem_reap() again */
    umem_reap();

    return MUNIT_OK;
}

/* Test: different free patterns */
static MunitResult test_allocation_patterns(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define PATTERN_COUNT 50
    void *ptrs[PATTERN_COUNT];
    size_t size = 256;

    /* Pattern 1: Allocate all, free sequentially */
    for (int i = 0; i < PATTERN_COUNT; i++) {
        ptrs[i] = umem_alloc(size, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }
    for (int i = 0; i < PATTERN_COUNT; i++) {
        umem_free(ptrs[i], size);
    }

    /* Pattern 2: Allocate all, free in reverse order */
    for (int i = 0; i < PATTERN_COUNT; i++) {
        ptrs[i] = umem_alloc(size, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }
    for (int i = PATTERN_COUNT - 1; i >= 0; i--) {
        umem_free(ptrs[i], size);
    }

    /* Pattern 3: Allocate all, free in interleaved pattern (even first, then odd) */
    for (int i = 0; i < PATTERN_COUNT; i++) {
        ptrs[i] = umem_alloc(size, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }
    for (int i = 0; i < PATTERN_COUNT; i += 2) {
        umem_free(ptrs[i], size);
    }
    for (int i = 1; i < PATTERN_COUNT; i += 2) {
        umem_free(ptrs[i], size);
    }

    /* Pattern 4: Allocate all, free in pseudo-random order */
    for (int i = 0; i < PATTERN_COUNT; i++) {
        ptrs[i] = umem_alloc(size, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }
    /* Simple shuffle using pseudo-random swaps */
    unsigned int seed = 123;
    for (int i = 0; i < PATTERN_COUNT; i++) {
        seed = seed * 1103515245 + 12345;
        int j = seed % PATTERN_COUNT;
        void *temp = ptrs[i];
        ptrs[i] = ptrs[j];
        ptrs[j] = temp;
    }
    for (int i = 0; i < PATTERN_COUNT; i++) {
        umem_free(ptrs[i], size);
    }

    return MUNIT_OK;
}

/* Test: umem_nofail_callback */
static int nofail_callback_invoked = 0;

static int test_nofail_callback_handler(void) {
    nofail_callback_invoked = 1;
    /* Return RETRY to allow allocation to proceed if possible */
    return UMEM_CALLBACK_RETRY;
}

static MunitResult test_umem_nofail_callback(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Register callback */
    umem_nofail_callback(test_nofail_callback_handler);

    /* Reset flag */
    nofail_callback_invoked = 0;

    /* Allocate with UMEM_NOFAIL - should succeed without triggering callback
     * in normal memory conditions */
    void *ptr = umem_alloc(4096, UMEM_NOFAIL);
    munit_assert_not_null(ptr);
    umem_free(ptr, 4096);

    /* We can't easily trigger out-of-memory to test callback invocation
     * in a unit test, but we've at least verified the callback registration
     * doesn't crash and UMEM_NOFAIL allocations work */

    /* Clean up: set callback back to NULL */
    umem_nofail_callback(NULL);

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
    { "/alloc_zero_size", test_alloc_zero_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/alloc_huge", test_alloc_huge, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/free_null", test_free_null, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/alloc_stress_mixed_sizes", test_alloc_stress_mixed_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/reuse_freed_memory", test_reuse_freed_memory, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/umem_reap", test_umem_reap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/allocation_patterns", test_allocation_patterns, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/umem_nofail_callback", test_umem_nofail_callback, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_alloc = {
    "/umem_alloc",
    alloc_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

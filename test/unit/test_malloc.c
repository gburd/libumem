/*
 * Unit tests for malloc.c - umem's malloc implementation
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../umem_base.h"
#include "../../umem_impl.h"
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>

/* Forward declarations for internal malloc.c functions */
extern void *umem_memalign(size_t align, size_t size);

/* Test: basic umem_malloc and umem_malloc_free */
static MunitResult test_malloc_basic(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_malloc(64);
    munit_assert_not_null(ptr);

    /* Write to the memory */
    memset(ptr, 0x42, 64);

    umem_malloc_free(ptr);

    return MUNIT_OK;
}

/* Test: umem_malloc with various sizes */
static MunitResult test_malloc_various_sizes(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t sizes[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        void *ptr = umem_malloc(sizes[i]);
        munit_assert_not_null(ptr);

        /* Touch the memory */
        memset(ptr, i & 0xFF, sizes[i]);

        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: umem_malloc with size 0 */
static MunitResult test_malloc_zero_size(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_malloc(0);
    /* Zero-size allocation may return non-NULL or NULL, both valid */
    if (ptr != NULL) {
        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: umem_malloc_free with NULL pointer */
static MunitResult test_malloc_free_null(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* free(NULL) should be safe */
    umem_malloc_free(NULL);

    return MUNIT_OK;
}

/* Test: small allocations (< UMEM_MAXBUF) */
static MunitResult test_malloc_small(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Test various small sizes that should go through umem cache */
    size_t small_sizes[] = {7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095};
    int num_sizes = sizeof(small_sizes) / sizeof(small_sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        void *ptr = umem_malloc(small_sizes[i]);
        munit_assert_not_null(ptr);
        memset(ptr, 0xAA, small_sizes[i]);
        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: large allocations (> UMEM_MAXBUF) */
static MunitResult test_malloc_large(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* UMEM_MAXBUF is 131072 (128KB), test larger allocations */
    size_t large_sizes[] = {
        UMEM_MAXBUF + 1,
        UMEM_MAXBUF + 1024,
        200 * 1024,     /* 200KB */
        500 * 1024,     /* 500KB */
        1024 * 1024     /* 1MB */
    };
    int num_sizes = sizeof(large_sizes) / sizeof(large_sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        void *ptr = umem_malloc(large_sizes[i]);
        munit_assert_not_null(ptr);

        /* Touch some pages to ensure allocation is real */
        unsigned char *bytes = (unsigned char *)ptr;
        for (size_t j = 0; j < large_sizes[i]; j += 4096) {
            bytes[j] = 0xBB;
        }

        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: allocation exactly at UMEM_MAXBUF boundary */
static MunitResult test_malloc_maxbuf_boundary(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Test at and around the UMEM_MAXBUF boundary */
    size_t sizes[] = {
        UMEM_MAXBUF - 1,
        UMEM_MAXBUF,
        UMEM_MAXBUF + 1
    };

    for (int i = 0; i < 3; i++) {
        void *ptr = umem_malloc(sizes[i]);
        munit_assert_not_null(ptr);
        memset(ptr, 0xCC, sizes[i]);
        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: umem_malloc alignment */
static MunitResult test_malloc_alignment(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    for (int i = 1; i <= 200; i++) {
        void *ptr = umem_malloc(i);
        munit_assert_not_null(ptr);

        /* Should be at least pointer-aligned */
        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % sizeof(void*), ==, 0);

        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

#ifdef _LP64
/* Test: large 64-bit allocations (> 4GB in size field) */
static MunitResult test_malloc_64bit_large(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Test allocation larger than UMEM_SECOND_ALIGN to trigger 64-bit path */
    size_t size = (size_t)UMEM_SECOND_ALIGN + 1024;
    void *ptr = umem_malloc(size);
    munit_assert_not_null(ptr);

    /* Touch the memory */
    memset(ptr, 0xDD, size);

    umem_malloc_free(ptr);

    return MUNIT_OK;
}
#endif

/* Test: umem_memalign with various alignments */
static MunitResult test_memalign_basic(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t alignments[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int num_aligns = sizeof(alignments) / sizeof(alignments[0]);

    for (int i = 0; i < num_aligns; i++) {
        void *ptr = umem_memalign(alignments[i], 128);
        munit_assert_not_null(ptr);

        /* Check alignment */
        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % alignments[i], ==, 0);

        /* Touch the memory */
        memset(ptr, 0xEE, 128);

        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: umem_memalign with invalid alignment (not power of 2) */
static MunitResult test_memalign_invalid_align(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Non-power-of-2 alignments should return NULL with EINVAL */
    void *ptr = umem_memalign(15, 128);
    munit_assert_null(ptr);
    munit_assert_int(errno, ==, EINVAL);

    ptr = umem_memalign(100, 128);
    munit_assert_null(ptr);
    munit_assert_int(errno, ==, EINVAL);

    return MUNIT_OK;
}

/* Test: umem_memalign with zero size */
static MunitResult test_memalign_zero_size(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_memalign(64, 0);
    munit_assert_null(ptr);
    munit_assert_int(errno, ==, EINVAL);

    return MUNIT_OK;
}

/* Test: umem_memalign with zero alignment */
static MunitResult test_memalign_zero_align(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    void *ptr = umem_memalign(0, 128);
    munit_assert_null(ptr);
    munit_assert_int(errno, ==, EINVAL);

    return MUNIT_OK;
}

/* Test: umem_memalign delegates to umem_malloc for small alignments */
static MunitResult test_memalign_small_align(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Alignments <= UMEM_ALIGN should use umem_malloc */
    void *ptr = umem_memalign(UMEM_ALIGN, 64);
    munit_assert_not_null(ptr);
    uintptr_t addr = (uintptr_t)ptr;
    munit_assert_uint64(addr % UMEM_ALIGN, ==, 0);
    umem_malloc_free(ptr);

    return MUNIT_OK;
}

/* Test: umem_memalign with large size and alignment */
static MunitResult test_memalign_large(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t align = 4096;
    size_t size = 64 * 1024;

    void *ptr = umem_memalign(align, size);
    munit_assert_not_null(ptr);

    /* Check alignment */
    uintptr_t addr = (uintptr_t)ptr;
    munit_assert_uint64(addr % align, ==, 0);

    /* Touch memory */
    memset(ptr, 0xFF, size);

    umem_malloc_free(ptr);

    return MUNIT_OK;
}

/* Test: many small allocations */
static MunitResult test_malloc_many_small(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define NUM_ALLOCS 1000
    void *ptrs[NUM_ALLOCS];

    /* Allocate */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = umem_malloc(32);
        munit_assert_not_null(ptrs[i]);
    }

    /* Free */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        umem_malloc_free(ptrs[i]);
    }

    return MUNIT_OK;
}

/* Test: mixed small and large allocations */
static MunitResult test_malloc_mixed_sizes(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define MIXED_COUNT 100
    void *ptrs[MIXED_COUNT];
    size_t sizes[MIXED_COUNT];

    /* Use simple pseudo-random for reproducibility */
    unsigned int seed = 42;

    /* Allocate mixed sizes */
    for (int i = 0; i < MIXED_COUNT; i++) {
        seed = seed * 1103515245 + 12345;
        /* Generate sizes from 16 bytes to 200KB */
        sizes[i] = 16 + (seed % (200 * 1024));

        ptrs[i] = umem_malloc(sizes[i]);
        munit_assert_not_null(ptrs[i]);

        /* Touch the memory */
        memset(ptrs[i], (i & 0xFF), sizes[i] > 1024 ? 1024 : sizes[i]);
    }

    /* Free all */
    for (int i = 0; i < MIXED_COUNT; i++) {
        umem_malloc_free(ptrs[i]);
    }

    return MUNIT_OK;
}

/* Test: rapid alloc/free pattern */
static MunitResult test_malloc_rapid_alloc_free(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    for (int i = 0; i < 10000; i++) {
        void *ptr = umem_malloc(64);
        munit_assert_not_null(ptr);
        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: allocations don't overlap */
static MunitResult test_malloc_no_overlap(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define OVERLAP_COUNT 100
    void *ptrs[OVERLAP_COUNT];
    size_t size = 128;

    /* Allocate and mark each allocation with unique pattern */
    for (int i = 0; i < OVERLAP_COUNT; i++) {
        ptrs[i] = umem_malloc(size);
        munit_assert_not_null(ptrs[i]);
        memset(ptrs[i], i & 0xFF, size);
    }

    /* Verify each allocation still has its marker */
    for (int i = 0; i < OVERLAP_COUNT; i++) {
        unsigned char *bytes = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < size; j++) {
            munit_assert_uint8(bytes[j], ==, (unsigned char)(i & 0xFF));
        }
    }

    /* Free all */
    for (int i = 0; i < OVERLAP_COUNT; i++) {
        umem_malloc_free(ptrs[i]);
    }

    return MUNIT_OK;
}

/* Test: free in different order than allocated */
static MunitResult test_malloc_free_patterns(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define PATTERN_COUNT 50
    void *ptrs[PATTERN_COUNT];
    size_t size = 256;

    /* Pattern 1: Allocate all, free in reverse order */
    for (int i = 0; i < PATTERN_COUNT; i++) {
        ptrs[i] = umem_malloc(size);
        munit_assert_not_null(ptrs[i]);
    }
    for (int i = PATTERN_COUNT - 1; i >= 0; i--) {
        umem_malloc_free(ptrs[i]);
    }

    /* Pattern 2: Allocate all, free even indices first */
    for (int i = 0; i < PATTERN_COUNT; i++) {
        ptrs[i] = umem_malloc(size);
        munit_assert_not_null(ptrs[i]);
    }
    for (int i = 0; i < PATTERN_COUNT; i += 2) {
        umem_malloc_free(ptrs[i]);
    }
    for (int i = 1; i < PATTERN_COUNT; i += 2) {
        umem_malloc_free(ptrs[i]);
    }

    return MUNIT_OK;
}

/* Test: memory reuse after free */
static MunitResult test_malloc_reuse(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t size = 128;
    void *ptr1, *ptr2, *ptr3;

    /* Allocate and free same size multiple times */
    ptr1 = umem_malloc(size);
    munit_assert_not_null(ptr1);
    memset(ptr1, 0xAA, size);
    umem_malloc_free(ptr1);

    ptr2 = umem_malloc(size);
    munit_assert_not_null(ptr2);
    memset(ptr2, 0xBB, size);
    umem_malloc_free(ptr2);

    ptr3 = umem_malloc(size);
    munit_assert_not_null(ptr3);
    memset(ptr3, 0xCC, size);
    umem_malloc_free(ptr3);

    return MUNIT_OK;
}

/* Test: errno preservation on success */
static MunitResult test_malloc_errno_preserved(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Set errno to a specific value */
    errno = 99;

    void *ptr = umem_malloc(128);
    munit_assert_not_null(ptr);

    /* errno should be preserved on success */
    munit_assert_int(errno, ==, 99);

    umem_malloc_free(ptr);

    /* errno should be preserved on free */
    munit_assert_int(errno, ==, 99);

    return MUNIT_OK;
}

/* Test: errno set to ENOMEM on large allocation failure */
static MunitResult test_malloc_errno_enomem(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Try to allocate a huge amount that might fail */
    /* SIZE_MAX - overhead should fail or succeed, both OK */
    errno = 0;
    void *ptr = umem_malloc(SIZE_MAX - 1024);
    if (ptr == NULL) {
        /* Should set errno to ENOMEM */
        munit_assert_int(errno, ==, ENOMEM);
    } else {
        /* If it succeeded somehow, free it */
        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: size overflow detection */
static MunitResult test_malloc_size_overflow(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Try to allocate SIZE_MAX which should overflow when adding overhead
     * On some systems this might succeed with vmem, so we accept both outcomes */
    errno = 0;
    void *ptr = umem_malloc(SIZE_MAX);
    if (ptr == NULL) {
        /* Expected: allocation failed due to overflow */
        munit_assert_int(errno, ==, ENOMEM);
    } else {
        /* If it succeeded, free it */
        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: memalign size overflow */
static MunitResult test_memalign_size_overflow(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    errno = 0;
    void *ptr = umem_memalign(64, SIZE_MAX);
    munit_assert_null(ptr);
    munit_assert_int(errno, ==, ENOMEM);

    return MUNIT_OK;
}

/* Test: memalign with various size/alignment combinations */
static MunitResult test_memalign_combinations(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    struct test_case {
        size_t align;
        size_t size;
    } cases[] = {
        {64, 1},
        {64, 63},
        {64, 64},
        {64, 65},
        {64, 128},
        {128, 1},
        {128, 127},
        {128, 128},
        {128, 256},
        {256, 1},
        {256, 512},
        {512, 1},
        {512, 1024},
        {1024, 1},
        {1024, 2048},
        {4096, 1},
        {4096, 8192},
    };

    int num_cases = sizeof(cases) / sizeof(cases[0]);

    for (int i = 0; i < num_cases; i++) {
        void *ptr = umem_memalign(cases[i].align, cases[i].size);
        munit_assert_not_null(ptr);

        /* Verify alignment */
        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % cases[i].align, ==, 0);

        /* Touch memory */
        memset(ptr, 0xFF, cases[i].size);

        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: multiple memalign allocations with same alignment */
static MunitResult test_memalign_multiple(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define MEMALIGN_COUNT 50
    void *ptrs[MEMALIGN_COUNT];
    size_t align = 256;
    size_t size = 512;

    /* Allocate */
    for (int i = 0; i < MEMALIGN_COUNT; i++) {
        ptrs[i] = umem_memalign(align, size);
        munit_assert_not_null(ptrs[i]);

        /* Verify alignment */
        uintptr_t addr = (uintptr_t)ptrs[i];
        munit_assert_uint64(addr % align, ==, 0);

        /* Mark memory */
        memset(ptrs[i], i & 0xFF, size);
    }

    /* Verify and free */
    for (int i = 0; i < MEMALIGN_COUNT; i++) {
        unsigned char *bytes = (unsigned char *)ptrs[i];
        munit_assert_uint8(bytes[0], ==, (unsigned char)(i & 0xFF));
        umem_malloc_free(ptrs[i]);
    }

    return MUNIT_OK;
}

/* Test: memalign with page-aligned requests */
static MunitResult test_memalign_page_aligned(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t page_size = 4096;
    size_t sizes[] = {page_size, page_size * 2, page_size * 4, page_size * 8};

    for (int i = 0; i < 4; i++) {
        void *ptr = umem_memalign(page_size, sizes[i]);
        munit_assert_not_null(ptr);

        /* Verify alignment */
        uintptr_t addr = (uintptr_t)ptr;
        munit_assert_uint64(addr % page_size, ==, 0);

        /* Touch all pages */
        unsigned char *bytes = (unsigned char *)ptr;
        for (size_t j = 0; j < sizes[i]; j += page_size) {
            bytes[j] = 0xAA;
        }

        umem_malloc_free(ptr);
    }

    return MUNIT_OK;
}

/* Test: mixed malloc and memalign allocations */
static MunitResult test_mixed_malloc_memalign(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define MIXED_MA_COUNT 100
    void *ptrs[MIXED_MA_COUNT];

    unsigned int seed = 123;

    /* Allocate mix of malloc and memalign */
    for (int i = 0; i < MIXED_MA_COUNT; i++) {
        seed = seed * 1103515245 + 12345;
        if (seed & 1) {
            /* Use malloc */
            ptrs[i] = umem_malloc(64 + (seed % 1024));
        } else {
            /* Use memalign */
            size_t align = 64 << (seed % 4);  /* 64, 128, 256, 512 */
            ptrs[i] = umem_memalign(align, 128 + (seed % 512));
        }
        munit_assert_not_null(ptrs[i]);
    }

    /* Free all */
    for (int i = 0; i < MIXED_MA_COUNT; i++) {
        umem_malloc_free(ptrs[i]);
    }

    return MUNIT_OK;
}

/* Test: allocation stress test */
static MunitResult test_malloc_stress(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define STRESS_COUNT 1000
    void *ptrs[STRESS_COUNT];
    size_t sizes[STRESS_COUNT];

    unsigned int seed = 456;

    /* Allocate random sizes */
    for (int i = 0; i < STRESS_COUNT; i++) {
        seed = seed * 1103515245 + 12345;
        sizes[i] = 1 + (seed % 16384);

        ptrs[i] = umem_malloc(sizes[i]);
        munit_assert_not_null(ptrs[i]);

        /* Touch memory */
        if (sizes[i] > 0) {
            ((unsigned char *)ptrs[i])[0] = 0xFF;
            ((unsigned char *)ptrs[i])[sizes[i] - 1] = 0xFF;
        }
    }

    /* Free in random order */
    for (int i = 0; i < STRESS_COUNT; i++) {
        seed = seed * 1103515245 + 12345;
        int j = seed % STRESS_COUNT;
        void *temp = ptrs[i];
        ptrs[i] = ptrs[j];
        ptrs[j] = temp;
    }

    for (int i = 0; i < STRESS_COUNT; i++) {
        umem_malloc_free(ptrs[i]);
    }

    return MUNIT_OK;
}

/* Test array */
static MunitTest malloc_tests[] = {
    { "/malloc_basic", test_malloc_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_various_sizes", test_malloc_various_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_zero_size", test_malloc_zero_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_free_null", test_malloc_free_null, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_small", test_malloc_small, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_large", test_malloc_large, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_maxbuf_boundary", test_malloc_maxbuf_boundary, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_alignment", test_malloc_alignment, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#ifdef _LP64
    { "/malloc_64bit_large", test_malloc_64bit_large, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
    { "/memalign_basic", test_memalign_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_invalid_align", test_memalign_invalid_align, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_zero_size", test_memalign_zero_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_zero_align", test_memalign_zero_align, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_small_align", test_memalign_small_align, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_large", test_memalign_large, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_many_small", test_malloc_many_small, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_mixed_sizes", test_malloc_mixed_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_rapid_alloc_free", test_malloc_rapid_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_no_overlap", test_malloc_no_overlap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_free_patterns", test_malloc_free_patterns, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_reuse", test_malloc_reuse, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_errno_preserved", test_malloc_errno_preserved, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_errno_enomem", test_malloc_errno_enomem, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_size_overflow", test_malloc_size_overflow, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_size_overflow", test_memalign_size_overflow, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_combinations", test_memalign_combinations, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_multiple", test_memalign_multiple, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/memalign_page_aligned", test_memalign_page_aligned, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/mixed_malloc_memalign", test_mixed_malloc_memalign, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_stress", test_malloc_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_malloc = {
    "/malloc",
    malloc_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

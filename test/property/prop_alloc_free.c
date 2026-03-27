/*
 * Property-based tests for allocation/deallocation
 *
 * Tests invariants that should hold for all inputs
 */

#include "../qc.h"
#include "../../umem.h"
#include <string.h>
#include <stdint.h>

/* Property: umem_alloc with UMEM_NOFAIL never returns NULL */
static bool prop_nofail_never_null(void *data) {
    (void)data;

    /* Generate random size between 1 and 8192 */
    size_t size = qc_gen_uint64(1, 8192);

    void *ptr = umem_alloc(size, UMEM_NOFAIL);
    bool result = (ptr != NULL);

    if (ptr) {
        umem_free(ptr, size);
    }

    return result;
}

/* Property: allocated memory is properly aligned */
static bool prop_alloc_aligned(void *data) {
    (void)data;

    size_t size = qc_gen_uint64(1, 4096);

    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    if (!ptr) {
        return true;  /* NULL is acceptable for UMEM_DEFAULT */
    }

    /* Check alignment - should be at least pointer-aligned */
    uintptr_t addr = (uintptr_t)ptr;
    bool aligned = (addr % sizeof(void*)) == 0;

    umem_free(ptr, size);

    return aligned;
}

/* Property: umem_zalloc returns zeroed memory */
static bool prop_zalloc_zeros(void *data) {
    (void)data;

    size_t size = qc_gen_uint64(8, 1024);

    void *ptr = umem_zalloc(size, UMEM_DEFAULT);
    if (!ptr) {
        return true;
    }

    /* Check all bytes are zero */
    unsigned char *bytes = (unsigned char *)ptr;
    bool all_zero = true;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }

    umem_free(ptr, size);

    return all_zero;
}

/* Property: allocated memory doesn't overlap */
static bool prop_no_overlap(void *data) {
    (void)data;

    #define NUM_PTRS 10
    void *ptrs[NUM_PTRS];
    size_t size = 64;
    bool success = true;

    /* Allocate and mark with unique patterns */
    for (int i = 0; i < NUM_PTRS; i++) {
        ptrs[i] = umem_alloc(size, UMEM_DEFAULT);
        if (!ptrs[i]) {
            success = false;
            goto cleanup;
        }
        memset(ptrs[i], i + 1, size);  /* Mark with i+1 */
    }

    /* Verify patterns haven't been overwritten */
    for (int i = 0; i < NUM_PTRS; i++) {
        unsigned char *bytes = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < size; j++) {
            if (bytes[j] != (unsigned char)(i + 1)) {
                success = false;
                goto cleanup;
            }
        }
    }

cleanup:
    for (int i = 0; i < NUM_PTRS; i++) {
        if (ptrs[i]) {
            umem_free(ptrs[i], size);
        }
    }

    return success;
}

/* Property: alloc/free roundtrip works */
static bool prop_alloc_free_roundtrip(void *data) {
    (void)data;

    size_t size = qc_gen_uint64(1, 8192);

    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    if (!ptr) {
        return true;
    }

    /* Write pattern */
    if (size >= 4) {
        *(uint32_t*)ptr = 0xDEADBEEF;
    }

    /* Free should not crash */
    umem_free(ptr, size);

    return true;
}

/* Property: multiple alloc/free cycles work */
static bool prop_repeated_alloc_free(void *data) {
    (void)data;

    size_t size = qc_gen_uint64(16, 256);
    int iterations = qc_gen_int(10, 100);

    for (int i = 0; i < iterations; i++) {
        void *ptr = umem_alloc(size, UMEM_DEFAULT);
        if (!ptr) {
            return false;
        }
        memset(ptr, 0xFF, size);
        umem_free(ptr, size);
    }

    return true;
}

/* Property: aligned allocation maintains alignment */
static bool prop_align_maintains_alignment(void *data) {
    (void)data;

    /* Power of 2 alignment */
    size_t align_pow = qc_gen_uint64(3, 12);  /* 8 to 4096 */
    size_t align = 1UL << align_pow;
    size_t size = align * qc_gen_uint64(1, 4);

    void *ptr = umem_alloc_align(size, align, UMEM_DEFAULT);
    if (!ptr) {
        return true;
    }

    uintptr_t addr = (uintptr_t)ptr;
    bool aligned = (addr % align) == 0;

    umem_free_align(ptr, size);

    return aligned;
}

/* Property: cache allocation works consistently */
static bool prop_cache_alloc_works(void *data) {
    (void)data;

    size_t obj_size = qc_gen_uint64(16, 512);

    umem_cache_t *cache = umem_cache_create(
        "prop_cache",
        obj_size,
        0, NULL, NULL, NULL, NULL, NULL, 0
    );

    if (!cache) {
        return false;
    }

    /* Allocate some objects */
    void *objs[10];
    for (int i = 0; i < 10; i++) {
        objs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
        if (!objs[i]) {
            goto cleanup;
        }
    }

    /* Free objects */
    for (int i = 0; i < 10; i++) {
        umem_cache_free(cache, objs[i]);
    }

    umem_cache_destroy(cache);
    return true;

cleanup:
    for (int i = 0; i < 10; i++) {
        if (objs[i]) {
            umem_cache_free(cache, objs[i]);
        }
    }
    umem_cache_destroy(cache);
    return false;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    qc_init();

    printf("Property-based tests for umem allocation/deallocation\n");
    printf("======================================================\n\n");

    qc_property("UMEM_NOFAIL never returns NULL",
                prop_nofail_never_null, NULL, 100);

    qc_property("Allocations are properly aligned",
                prop_alloc_aligned, NULL, 1000);

    qc_property("umem_zalloc returns zeroed memory",
                prop_zalloc_zeros, NULL, 100);

    qc_property("Allocations don't overlap",
                prop_no_overlap, NULL, 100);

    qc_property("Alloc/free roundtrip works",
                prop_alloc_free_roundtrip, NULL, 1000);

    qc_property("Repeated alloc/free cycles work",
                prop_repeated_alloc_free, NULL, 100);

    qc_property("Aligned allocations maintain alignment",
                prop_align_maintains_alignment, NULL, 100);

    qc_property("Cache allocation works consistently",
                prop_cache_alloc_works, NULL, 50);

    return qc_results();
}

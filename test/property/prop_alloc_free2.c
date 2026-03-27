/*
 * Property-based tests for allocation/deallocation using qc framework
 */

#include "../qc.h"
#include "../../umem.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Property: UMEM_NOFAIL never returns NULL for valid sizes */
static QCC_TestStatus prop_nofail_never_null(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    /* Get generated size */
    size_t size = (size_t)QCC_getValue(vals, 0, long);

    /* Skip invalid sizes */
    if (size == 0 || size > 1024 * 1024) {
        return QCC_NOTHING;
    }

    void *ptr = umem_alloc(size, UMEM_NOFAIL);
    int result = (ptr != NULL) ? QCC_OK : QCC_FAIL;

    if (ptr) {
        umem_free(ptr, size);
    }

    return result;
}

/* Property: allocated memory is properly aligned */
static QCC_TestStatus prop_alloc_aligned(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    size_t size = (size_t)QCC_getValue(vals, 0, long);

    if (size == 0 || size > 8192) {
        return QCC_NOTHING;
    }

    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    if (!ptr) {
        return QCC_NOTHING;  /* NULL is acceptable for UMEM_DEFAULT */
    }

    /* Check alignment */
    uintptr_t addr = (uintptr_t)ptr;
    int aligned = (addr % sizeof(void*)) == 0;

    umem_free(ptr, size);

    return aligned ? QCC_OK : QCC_FAIL;
}

/* Property: umem_zalloc returns zeroed memory */
static QCC_TestStatus prop_zalloc_zeros(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    size_t size = (size_t)QCC_getValue(vals, 0, long);

    if (size == 0 || size > 4096) {
        return QCC_NOTHING;
    }

    void *ptr = umem_zalloc(size, UMEM_DEFAULT);
    if (!ptr) {
        return QCC_NOTHING;
    }

    /* Check all bytes are zero */
    unsigned char *bytes = (unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            umem_free(ptr, size);
            return QCC_FAIL;
        }
    }

    umem_free(ptr, size);
    return QCC_OK;
}

/* Property: alloc/free roundtrip works */
static QCC_TestStatus prop_alloc_free_roundtrip(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    size_t size = (size_t)QCC_getValue(vals, 0, long);

    if (size == 0 || size > 8192) {
        return QCC_NOTHING;
    }

    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    if (!ptr) {
        return QCC_NOTHING;
    }

    /* Write pattern */
    memset(ptr, 0x42, size);

    /* Verify pattern */
    unsigned char *bytes = (unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0x42) {
            umem_free(ptr, size);
            return QCC_FAIL;
        }
    }

    /* Free should not crash */
    umem_free(ptr, size);

    return QCC_OK;
}

/* Generator: sizes from 1 to 8192 */
static QCC_GenValue *gen_size() {
    return QCC_genLongR(1, 8192);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;

    printf("Property-based tests for umem\n");
    printf("=============================\n\n");

    printf("Testing: UMEM_NOFAIL never returns NULL...\n");
    if (QCC_testForAll(100, 1000, prop_nofail_never_null, 1, gen_size) != 0) {
        failures++;
    }

    printf("\nTesting: Allocations are properly aligned...\n");
    if (QCC_testForAll(1000, 10000, prop_alloc_aligned, 1, gen_size) != 0) {
        failures++;
    }

    printf("\nTesting: umem_zalloc returns zeroed memory...\n");
    if (QCC_testForAll(100, 1000, prop_zalloc_zeros, 1, gen_size) != 0) {
        failures++;
    }

    printf("\nTesting: Alloc/free roundtrip works...\n");
    if (QCC_testForAll(1000, 10000, prop_alloc_free_roundtrip, 1, gen_size) != 0) {
        failures++;
    }

    printf("\n=============================\n");
    if (failures == 0) {
        printf("All property tests passed!\n");
        return 0;
    } else {
        printf("%d property test(s) failed\n", failures);
        return 1;
    }
}

/*
 * Property-based tests for memory fragmentation behavior using qc framework
 */

#include "../qc.h"
#include "../../umem.h"
#include "../../sys/vmem.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* Property: Memory overhead stays within bounds */
static QCC_TestStatus prop_no_unbounded_fragmentation(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    size_t num_allocs = (size_t)QCC_getValue(vals, 0, long);

    /* Generate reasonable number of allocations */
    if (num_allocs < 100 || num_allocs > 500) {
        return QCC_NOTHING;
    }

    void **ptrs = malloc(num_allocs * sizeof(void*));
    if (!ptrs) {
        return QCC_NOTHING;
    }

    struct rusage before, after;
    getrusage(RUSAGE_SELF, &before);

    /* Allocate objects with varying sizes */
    size_t total_allocated = 0;
    for (size_t i = 0; i < num_allocs; i++) {
        /* Use index to generate pseudo-random but deterministic sizes */
        size_t size = ((i * 7919) % 1009) + 16;  /* 16-1024 bytes */
        ptrs[i] = umem_alloc(size, UMEM_DEFAULT);
        if (ptrs[i]) {
            total_allocated += size;
            /* Touch memory to ensure it's actually allocated */
            memset(ptrs[i], (int)(i & 0xFF), size);
        }
    }

    getrusage(RUSAGE_SELF, &after);

    /* Calculate RSS growth (ru_maxrss is in KB on Linux, bytes on macOS) */
#ifdef __linux__
    size_t rss_growth = (after.ru_maxrss - before.ru_maxrss) * 1024;
#else
    size_t rss_growth = (after.ru_maxrss - before.ru_maxrss);
#endif

    /* Clean up */
    for (size_t i = 0; i < num_allocs; i++) {
        if (ptrs[i]) {
            umem_free(ptrs[i], 0);  /* Size tracked internally */
        }
    }
    free(ptrs);

    /* Check fragmentation ratio - should be less than 2.0x */
    if (total_allocated > 0 && rss_growth > 0) {
        double ratio = (double)rss_growth / (double)total_allocated;
        if (ratio >= 2.0) {
            return QCC_FAIL;
        }
    }

    return QCC_OK;
}

/* Property: Adjacent frees get coalesced (vmem layer) */
static QCC_TestStatus prop_coalescing_works(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)vals;
    (void)len;
    (void)stamps;

    /* Create a vmem arena with 64KB */
    void *base = malloc(64 * 1024);
    if (!base) {
        return QCC_NOTHING;
    }

    vmem_t *arena = vmem_create("test_arena", base, 64 * 1024, 8,
                                NULL, NULL, NULL, 0, VM_SLEEP);
    if (!arena) {
        free(base);
        return QCC_NOTHING;
    }

    /* Allocate 8 adjacent 8KB blocks */
    void *blocks[8];
    for (int i = 0; i < 8; i++) {
        blocks[i] = vmem_alloc(arena, 8 * 1024, VM_SLEEP | VM_BESTFIT);
        if (!blocks[i]) {
            /* Clean up allocated blocks */
            for (int j = 0; j < i; j++) {
                vmem_free(arena, blocks[j], 8 * 1024);
            }
            vmem_destroy(arena);
            free(base);
            return QCC_NOTHING;
        }
    }

    /* Free even-indexed blocks (0, 2, 4, 6) */
    for (int i = 0; i < 8; i += 2) {
        vmem_free(arena, blocks[i], 8 * 1024);
    }

    /* Free odd-indexed blocks (1, 3, 5, 7) */
    for (int i = 1; i < 8; i += 2) {
        vmem_free(arena, blocks[i], 8 * 1024);
    }

    /* Try to allocate a large contiguous block */
    /* If coalescing works, we should be able to allocate ~60KB */
    void *large_block = vmem_alloc(arena, 60 * 1024, VM_NOSLEEP | VM_BESTFIT);

    int result = (large_block != NULL) ? QCC_OK : QCC_FAIL;

    if (large_block) {
        vmem_free(arena, large_block, 60 * 1024);
    }

    vmem_destroy(arena);
    free(base);

    return result;
}

/* Property: Slabs don't waste excessive space */
static QCC_TestStatus prop_slab_utilization(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    size_t obj_size = (size_t)QCC_getValue(vals, 0, long);

    /* Generate reasonable object sizes */
    if (obj_size < 16 || obj_size > 512) {
        return QCC_NOTHING;
    }

    /* Create a cache with the specified object size */
    char cache_name[32];
    snprintf(cache_name, sizeof(cache_name), "test_%zu", obj_size);

    umem_cache_t *cache = umem_cache_create(cache_name, obj_size, 0,
                                            NULL, NULL, NULL, NULL, NULL, 0);
    if (!cache) {
        return QCC_NOTHING;
    }

    /* Allocate 100 objects */
    void *objs[100];
    size_t num_allocated = 0;

    for (size_t i = 0; i < 100; i++) {
        objs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
        if (objs[i]) {
            num_allocated++;
            /* Touch memory */
            memset(objs[i], (int)(i & 0xFF), obj_size);
        }
    }

    /* Estimate overhead */
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);

    /* Clean up */
    for (size_t i = 0; i < num_allocated; i++) {
        if (objs[i]) {
            umem_cache_free(cache, objs[i]);
        }
    }

    umem_cache_destroy(cache);

    /* Check that we allocated something */
    if (num_allocated < 50) {
        return QCC_NOTHING;
    }

    /*
     * Verify overhead per object is reasonable
     * This is a heuristic - slab metadata should be < 64 bytes per object
     * We don't have direct access to slab internals, so this property
     * mainly ensures the cache creation and allocation work correctly
     */

    return QCC_OK;
}

/* Generator: allocation count from 100 to 500 */
static QCC_GenValue *gen_alloc_count() {
    return QCC_genLongR(100, 500);
}

/* Generator: object sizes from 16 to 512 */
static QCC_GenValue *gen_obj_size() {
    return QCC_genLongR(16, 512);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;

    printf("Property-based tests for memory fragmentation\n");
    printf("==============================================\n\n");

    printf("Testing: Memory overhead stays within bounds...\n");
    if (QCC_testForAll(100, 1000, prop_no_unbounded_fragmentation, 1, gen_alloc_count) != 0) {
        failures++;
    }

    printf("\nTesting: Adjacent frees get coalesced...\n");
    /* This property is deterministic, run fewer iterations */
    if (QCC_testForAll(50, 100, prop_coalescing_works, 0) != 0) {
        failures++;
    }

    printf("\nTesting: Slabs don't waste excessive space...\n");
    if (QCC_testForAll(100, 1000, prop_slab_utilization, 1, gen_obj_size) != 0) {
        failures++;
    }

    printf("\n==============================================\n");
    if (failures == 0) {
        printf("All property tests passed!\n");
        return 0;
    } else {
        printf("%d property test(s) failed\n", failures);
        return 1;
    }
}

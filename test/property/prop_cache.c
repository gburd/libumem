/*
 * Property-based tests for umem_cache API using qc framework
 */

#include "../qc.h"
#include "../../umem.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Global counters for constructor/destructor tests */
static int g_constructor_calls = 0;
static int g_destructor_calls = 0;

/* Test constructor - increments counter */
static int test_constructor(void *buf, void *arg, int flags) {
    (void)arg;
    (void)flags;
    g_constructor_calls++;
    memset(buf, 0xAA, 64);
    return 0;
}

/* Test destructor - increments counter */
static void test_destructor(void *buf, void *arg) {
    (void)buf;
    (void)arg;
    g_destructor_calls++;
}

/* Property 1: Cache allocation with UMEM_NOFAIL never returns NULL */
static QCC_TestStatus prop_cache_alloc_succeeds(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    /* Generate random cache size: 16-1024 bytes */
    long size_val = *QCC_getValue(vals, 0, long*);
    if (size_val < 16 || size_val > 1024) {
        return QCC_NOTHING;
    }
    size_t size = (size_t)size_val;

    umem_cache_t *cache = umem_cache_create("test_alloc", size, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    if (!cache) {
        return QCC_FAIL;
    }

    void *obj = umem_cache_alloc(cache, UMEM_NOFAIL);
    int result = (obj != NULL) ? QCC_OK : QCC_FAIL;

    if (obj) {
        umem_cache_free(cache, obj);
    }
    umem_cache_destroy(cache);

    return result;
}

/* Property 2: Constructor invoked for each allocation */
static QCC_TestStatus prop_cache_constructor_called(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    /* Generate random allocation count: 1-20 */
    long count_val = *QCC_getValue(vals, 0, long*);
    if (count_val < 1 || count_val > 20) {
        return QCC_NOTHING;
    }
    int alloc_count = (int)count_val;

    g_constructor_calls = 0;

    umem_cache_t *cache = umem_cache_create("test_ctor", 64, 0,
        test_constructor, NULL, NULL, NULL, NULL, 0);
    if (!cache) {
        return QCC_FAIL;
    }

    void **objs = malloc(alloc_count * sizeof(void *));
    if (!objs) {
        umem_cache_destroy(cache);
        return QCC_FAIL;
    }

    /* Allocate objects */
    for (int i = 0; i < alloc_count; i++) {
        objs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
        if (!objs[i]) {
            for (int j = 0; j < i; j++) {
                umem_cache_free(cache, objs[j]);
            }
            free(objs);
            umem_cache_destroy(cache);
            return QCC_NOTHING;
        }
    }

    /* Constructor should be called at least once per allocation */
    int result = (g_constructor_calls >= alloc_count) ? QCC_OK : QCC_FAIL;

    /* Clean up */
    for (int i = 0; i < alloc_count; i++) {
        umem_cache_free(cache, objs[i]);
    }
    free(objs);
    umem_cache_destroy(cache);

    return result;
}

/* Property 3: Destructor invoked on cache destroy */
static QCC_TestStatus prop_cache_destructor_called(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    /* Generate random allocation count: 1-10 */
    long count_val = *QCC_getValue(vals, 0, long*);
    if (count_val < 1 || count_val > 10) {
        return QCC_NOTHING;
    }
    int alloc_count = (int)count_val;

    g_destructor_calls = 0;

    umem_cache_t *cache = umem_cache_create("test_dtor", 64, 0,
        NULL, test_destructor, NULL, NULL, NULL, 0);
    if (!cache) {
        return QCC_FAIL;
    }

    void **objs = malloc(alloc_count * sizeof(void *));
    if (!objs) {
        umem_cache_destroy(cache);
        return QCC_FAIL;
    }

    /* Allocate and free objects */
    for (int i = 0; i < alloc_count; i++) {
        objs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
        if (!objs[i]) {
            for (int j = 0; j < i; j++) {
                umem_cache_free(cache, objs[j]);
            }
            free(objs);
            umem_cache_destroy(cache);
            return QCC_NOTHING;
        }
    }

    for (int i = 0; i < alloc_count; i++) {
        umem_cache_free(cache, objs[i]);
    }
    free(objs);

    /* Destroy cache - this should trigger destructors */
    umem_cache_destroy(cache);

    /* Destructor should be called for cached objects */
    int result = (g_destructor_calls > 0) ? QCC_OK : QCC_FAIL;

    return result;
}

/* Property 4: Specified alignment (powers of 2: 8-256) always maintained */
static QCC_TestStatus prop_cache_alignment_preserved(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    /* Generate random alignment: power of 2 from 8 to 256 */
    long power_val = *QCC_getValue(vals, 0, long*);
    if (power_val < 3 || power_val > 8) {
        return QCC_NOTHING;
    }
    size_t alignment = 1UL << power_val;

    umem_cache_t *cache = umem_cache_create("test_align", 64, alignment,
        NULL, NULL, NULL, NULL, NULL, 0);
    if (!cache) {
        return QCC_FAIL;
    }

    /* Allocate multiple objects and check alignment */
    void *objs[10];
    int alloc_count = 0;
    int result = QCC_OK;

    for (int i = 0; i < 10; i++) {
        objs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
        if (!objs[i]) {
            break;
        }
        alloc_count++;

        /* Check alignment */
        uintptr_t addr = (uintptr_t)objs[i];
        if ((addr % alignment) != 0) {
            result = QCC_FAIL;
            break;
        }
    }

    /* Clean up */
    for (int i = 0; i < alloc_count; i++) {
        umem_cache_free(cache, objs[i]);
    }
    umem_cache_destroy(cache);

    return result;
}

/* Property 5: All objects from cache are same size (no size variation) */
static QCC_TestStatus prop_cache_size_consistency(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    /* Generate random cache size: 32-512 bytes */
    long size_val = *QCC_getValue(vals, 0, long*);
    if (size_val < 32 || size_val > 512) {
        return QCC_NOTHING;
    }
    size_t size = (size_t)size_val;

    umem_cache_t *cache = umem_cache_create("test_size", size, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    if (!cache) {
        return QCC_FAIL;
    }

    /* Allocate multiple objects */
    void *objs[10];
    int alloc_count = 0;

    for (int i = 0; i < 10; i++) {
        objs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
        if (!objs[i]) {
            break;
        }
        alloc_count++;
    }

    if (alloc_count < 2) {
        for (int i = 0; i < alloc_count; i++) {
            umem_cache_free(cache, objs[i]);
        }
        umem_cache_destroy(cache);
        return QCC_NOTHING;
    }

    /* Write unique pattern to each object and verify size */
    int result = QCC_OK;
    for (int i = 0; i < alloc_count; i++) {
        memset(objs[i], 0x40 + i, size);
    }

    /* Verify patterns are intact (no overlap) */
    for (int i = 0; i < alloc_count; i++) {
        unsigned char *bytes = (unsigned char *)objs[i];
        unsigned char expected = 0x40 + i;
        for (size_t j = 0; j < size; j++) {
            if (bytes[j] != expected) {
                result = QCC_FAIL;
                break;
            }
        }
        if (result == QCC_FAIL) break;
    }

    /* Clean up */
    for (int i = 0; i < alloc_count; i++) {
        umem_cache_free(cache, objs[i]);
    }
    umem_cache_destroy(cache);

    return result;
}

/* Property 6: Objects from different caches don't overlap (check address ranges) */
static QCC_TestStatus prop_cache_isolation(QCC_GenValue **vals, int len, QCC_Stamp **stamps) {
    (void)stamps;

    if (len < 1) return QCC_FAIL;

    /* Generate random sizes: 64-512 bytes */
    long size_val = *QCC_getValue(vals, 0, long*);
    if (size_val < 64 || size_val > 512) {
        return QCC_NOTHING;
    }
    size_t size1 = (size_t)size_val;
    size_t size2 = size1 + 32;  /* Different size for second cache */

    umem_cache_t *cache1 = umem_cache_create("test_iso1", size1, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    umem_cache_t *cache2 = umem_cache_create("test_iso2", size2, 0,
        NULL, NULL, NULL, NULL, NULL, 0);

    if (!cache1 || !cache2) {
        if (cache1) umem_cache_destroy(cache1);
        if (cache2) umem_cache_destroy(cache2);
        return QCC_FAIL;
    }

    /* Allocate objects from both caches */
    void *obj1 = umem_cache_alloc(cache1, UMEM_DEFAULT);
    void *obj2 = umem_cache_alloc(cache2, UMEM_DEFAULT);

    if (!obj1 || !obj2) {
        if (obj1) umem_cache_free(cache1, obj1);
        if (obj2) umem_cache_free(cache2, obj2);
        umem_cache_destroy(cache1);
        umem_cache_destroy(cache2);
        return QCC_NOTHING;
    }

    /* Check that address ranges don't overlap */
    uintptr_t addr1_start = (uintptr_t)obj1;
    uintptr_t addr1_end = addr1_start + size1;
    uintptr_t addr2_start = (uintptr_t)obj2;
    uintptr_t addr2_end = addr2_start + size2;

    int no_overlap = (addr1_end <= addr2_start) || (addr2_end <= addr1_start);
    int result = no_overlap ? QCC_OK : QCC_FAIL;

    /* Write patterns and verify isolation */
    if (result == QCC_OK) {
        memset(obj1, 0xBB, size1);
        memset(obj2, 0xCC, size2);

        unsigned char *bytes1 = (unsigned char *)obj1;
        unsigned char *bytes2 = (unsigned char *)obj2;

        for (size_t i = 0; i < size1; i++) {
            if (bytes1[i] != 0xBB) {
                result = QCC_FAIL;
                break;
            }
        }
        for (size_t i = 0; i < size2 && result == QCC_OK; i++) {
            if (bytes2[i] != 0xCC) {
                result = QCC_FAIL;
                break;
            }
        }
    }

    /* Clean up */
    umem_cache_free(cache1, obj1);
    umem_cache_free(cache2, obj2);
    umem_cache_destroy(cache1);
    umem_cache_destroy(cache2);

    return result;
}

/* Generators */
static QCC_GenValue *gen_cache_size() {
    return QCC_genLongR(16, 1024);
}

static QCC_GenValue *gen_alloc_count() {
    return QCC_genLongR(1, 20);
}

static QCC_GenValue *gen_alignment_power() {
    return QCC_genLongR(3, 8);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int failures = 0;

    printf("Property-based tests for umem_cache\n");
    printf("====================================\n\n");

    printf("Testing: Cache allocation with UMEM_NOFAIL never returns NULL...\n");
    if (QCC_testForAll(500, 5000, prop_cache_alloc_succeeds, 1, gen_cache_size) != 0) {
        failures++;
    }

    printf("\nTesting: Constructor invoked for each allocation...\n");
    if (QCC_testForAll(500, 5000, prop_cache_constructor_called, 1, gen_alloc_count) != 0) {
        failures++;
    }

    printf("\nTesting: Destructor invoked on cache destroy...\n");
    if (QCC_testForAll(500, 5000, prop_cache_destructor_called, 1, gen_alloc_count) != 0) {
        failures++;
    }

    printf("\nTesting: Specified alignment always maintained...\n");
    if (QCC_testForAll(500, 5000, prop_cache_alignment_preserved, 1, gen_alignment_power) != 0) {
        failures++;
    }

    printf("\nTesting: All objects from cache are same size...\n");
    if (QCC_testForAll(500, 5000, prop_cache_size_consistency, 1, gen_cache_size) != 0) {
        failures++;
    }

    printf("\nTesting: Objects from different caches don't overlap...\n");
    if (QCC_testForAll(500, 5000, prop_cache_isolation, 1, gen_cache_size) != 0) {
        failures++;
    }

    printf("\n====================================\n");
    if (failures == 0) {
        printf("All property tests passed!\n");
        return 0;
    } else {
        printf("%d property test(s) failed\n", failures);
        return 1;
    }
}

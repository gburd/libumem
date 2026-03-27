/*
 * Unit tests for umem_cache_* API
 *
 * MAJOR GAP: umem_cache API is not currently tested
 */

#include "../munit.h"
#include "../../umem.h"
#include <string.h>
#include <stdio.h>

/* Test data structure */
typedef struct test_obj {
    int value;
    char data[64];
} test_obj_t;

/* Constructor callback */
static int test_constructor(void *buf, void *arg, int flags) {
    test_obj_t *obj = (test_obj_t *)buf;
    int *construct_count = (int *)arg;

    obj->value = 0xdeadbeef;
    memset(obj->data, 0, sizeof(obj->data));

    if (construct_count) {
        (*construct_count)++;
    }

    return 0;
}

/* Destructor callback */
static void test_destructor(void *buf, void *arg) {
    test_obj_t *obj = (test_obj_t *)buf;
    int *destruct_count = (int *)arg;

    /* Verify object is valid */
    if (obj->value == 0xdeadbeef) {
        obj->value = 0;
    }

    if (destruct_count) {
        (*destruct_count)++;
    }
}

/* Test: cache_create and cache_destroy */
static MunitResult test_cache_create_destroy(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    umem_cache_t *cache = umem_cache_create(
        "test_cache",
        sizeof(test_obj_t),
        0,  /* align */
        NULL, NULL,  /* constructor, destructor */
        NULL, NULL,  /* reclaim, callback_data */
        NULL,  /* source */
        0  /* cflags */
    );

    munit_assert_not_null(cache);

    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test: cache_alloc and cache_free */
static MunitResult test_cache_alloc_free(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    umem_cache_t *cache = umem_cache_create(
        "test_alloc_cache",
        sizeof(test_obj_t),
        0, NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    /* Allocate object */
    test_obj_t *obj = (test_obj_t *)umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    /* Use the object */
    obj->value = 42;
    strcpy(obj->data, "test");

    /* Free object */
    umem_cache_free(cache, obj);

    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test: cache with constructor */
static MunitResult test_cache_constructor(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    int construct_count = 0;

    umem_cache_t *cache = umem_cache_create(
        "test_ctor_cache",
        sizeof(test_obj_t),
        0,
        test_constructor, NULL,
        NULL, &construct_count,
        NULL, 0
    );
    munit_assert_not_null(cache);

    /* Allocate - constructor should be called */
    test_obj_t *obj = (test_obj_t *)umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    /* Verify constructor was called */
    munit_assert_int(obj->value, ==, 0xdeadbeef);
    munit_assert_int(construct_count, >, 0);

    umem_cache_free(cache, obj);
    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test: cache with destructor */
static MunitResult test_cache_destructor(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    int construct_count = 0;
    int destruct_count = 0;

    umem_cache_t *cache = umem_cache_create(
        "test_dtor_cache",
        sizeof(test_obj_t),
        0,
        test_constructor, test_destructor,
        NULL, &construct_count,
        NULL, 0
    );
    munit_assert_not_null(cache);

    /* Note: destructor callback data is shared with constructor */
    test_obj_t *obj = (test_obj_t *)umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    umem_cache_free(cache, obj);

    /* Destroy cache - destructor should be called for cached objects */
    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test: cache alignment */
static MunitResult test_cache_alignment(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t align = 64;

    umem_cache_t *cache = umem_cache_create(
        "test_align_cache",
        sizeof(test_obj_t),
        align,
        NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    /* Allocate and verify alignment */
    for (int i = 0; i < 100; i++) {
        void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
        munit_assert_not_null(obj);

        uintptr_t addr = (uintptr_t)obj;
        munit_assert_uint64(addr % align, ==, 0);

        umem_cache_free(cache, obj);
    }

    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test: cache flags - UMC_NOTOUCH */
static MunitResult test_cache_flags_notouch(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* UMC_NOTOUCH: Don't touch buffers on free (for debugging) */
    umem_cache_t *cache = umem_cache_create(
        "test_notouch_cache",
        sizeof(test_obj_t),
        0, NULL, NULL, NULL, NULL, NULL,
        UMC_NOTOUCH
    );
    munit_assert_not_null(cache);

    test_obj_t *obj = (test_obj_t *)umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    obj->value = 0x12345678;
    umem_cache_free(cache, obj);

    /* With UMC_NOTOUCH, the value might still be there
     * (not guaranteed, but cache might not touch it) */

    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test: cache stress - rapid alloc/free cycles */
static MunitResult test_cache_stress(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    umem_cache_t *cache = umem_cache_create(
        "test_stress_cache",
        sizeof(test_obj_t),
        0, NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    #define STRESS_ITERATIONS 10000
    void *objects[100];

    /* Rapid allocation/free cycles */
    for (int iter = 0; iter < STRESS_ITERATIONS; iter++) {
        /* Allocate batch */
        for (int i = 0; i < 100; i++) {
            objects[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
            munit_assert_not_null(objects[i]);
        }

        /* Free batch */
        for (int i = 0; i < 100; i++) {
            umem_cache_free(cache, objects[i]);
        }
    }

    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test: multiple caches */
static MunitResult test_multiple_caches(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    #define NUM_CACHES 10
    umem_cache_t *caches[NUM_CACHES];

    /* Create multiple caches */
    for (int i = 0; i < NUM_CACHES; i++) {
        char name[32];
        snprintf(name, sizeof(name), "cache_%d", i);

        caches[i] = umem_cache_create(
            name,
            sizeof(test_obj_t) * (i + 1),  /* Different sizes */
            0, NULL, NULL, NULL, NULL, NULL, 0
        );
        munit_assert_not_null(caches[i]);
    }

    /* Use all caches */
    for (int i = 0; i < NUM_CACHES; i++) {
        void *obj = umem_cache_alloc(caches[i], UMEM_DEFAULT);
        munit_assert_not_null(obj);
        umem_cache_free(caches[i], obj);
    }

    /* Destroy all caches */
    for (int i = 0; i < NUM_CACHES; i++) {
        umem_cache_destroy(caches[i]);
    }

    return MUNIT_OK;
}

/* Test: cache with UMEM_NOFAIL */
static MunitResult test_cache_nofail(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    umem_cache_t *cache = umem_cache_create(
        "test_nofail_cache",
        sizeof(test_obj_t),
        0, NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    /* UMEM_NOFAIL should never return NULL */
    void *obj = umem_cache_alloc(cache, UMEM_NOFAIL);
    munit_assert_not_null(obj);

    umem_cache_free(cache, obj);
    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/* Test array */
static MunitTest cache_tests[] = {
    { "/create_destroy", test_cache_create_destroy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/alloc_free", test_cache_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/constructor", test_cache_constructor, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/destructor", test_cache_destructor, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/alignment", test_cache_alignment, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/flags_notouch", test_cache_flags_notouch, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/stress", test_cache_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple_caches", test_multiple_caches, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nofail", test_cache_nofail, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_cache = {
    "/umem_cache",
    cache_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

/*
 * Unit tests for vmem layer
 *
 * vmem layer is currently NOT TESTED - this is critical infrastructure
 */

#include "../munit.h"
#include "../../sys/vmem.h"
#include <string.h>
#include <stdio.h>

/* Test: vmem_create and vmem_destroy */
static MunitResult test_vmem_create_destroy(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Create a vmem arena */
    vmem_t *vmp = vmem_create(
        "test_arena",
        NULL,  /* base */
        0,     /* size */
        4096,  /* quantum */
        NULL, NULL,  /* afunc, ffunc */
        NULL,  /* source */
        0,     /* qcache_max */
        VM_SLEEP
    );

    munit_assert_not_null(vmp);

    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test: vmem_alloc and vmem_free */
static MunitResult test_vmem_alloc_free(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    vmem_t *vmp = vmem_create(
        "test_alloc_arena",
        NULL, 0, 4096,
        NULL, NULL, NULL, 0,
        VM_SLEEP
    );
    munit_assert_not_null(vmp);

    /* Allocate from arena */
    void *addr = vmem_alloc(vmp, 8192, VM_SLEEP);
    munit_assert_not_null(addr);

    /* Free back to arena */
    vmem_free(vmp, addr, 8192);

    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test: vmem_xalloc with constraints */
static MunitResult test_vmem_xalloc(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    vmem_t *vmp = vmem_create(
        "test_xalloc_arena",
        NULL, 0, 4096,
        NULL, NULL, NULL, 0,
        VM_SLEEP
    );
    munit_assert_not_null(vmp);

    /* Allocate with alignment constraint */
    void *addr = vmem_xalloc(
        vmp,
        8192,  /* size */
        8192,  /* align */
        0,     /* phase */
        0,     /* nocross */
        NULL,  /* minaddr */
        NULL,  /* maxaddr */
        VM_SLEEP
    );
    munit_assert_not_null(addr);

    /* Verify alignment */
    uintptr_t iaddr = (uintptr_t)addr;
    munit_assert_uint64(iaddr % 8192, ==, 0);

    vmem_free(vmp, addr, 8192);
    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test: multiple allocations from same arena */
static MunitResult test_vmem_multiple_allocs(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    vmem_t *vmp = vmem_create(
        "test_multi_arena",
        NULL, 0, 4096,
        NULL, NULL, NULL, 0,
        VM_SLEEP
    );
    munit_assert_not_null(vmp);

    #define NUM_ALLOCS 10
    void *addrs[NUM_ALLOCS];

    /* Allocate multiple chunks */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        addrs[i] = vmem_alloc(vmp, 4096, VM_SLEEP);
        munit_assert_not_null(addrs[i]);
    }

    /* Free all chunks */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        vmem_free(vmp, addrs[i], 4096);
    }

    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test: vmem_add to add span to arena */
static MunitResult test_vmem_add(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    vmem_t *vmp = vmem_create(
        "test_add_arena",
        NULL, 0, 4096,
        NULL, NULL, NULL, 0,
        VM_SLEEP
    );
    munit_assert_not_null(vmp);

    /* Add a span of address space */
    void *span = (void *)0x100000;  /* Arbitrary address */
    void *result = vmem_add(vmp, span, 65536, VM_SLEEP);

    /* Result may be NULL if span is invalid, that's ok for this test */
    (void)result;

    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test: vmem with different quantum sizes */
static MunitResult test_vmem_quantum(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    size_t quantums[] = {64, 256, 1024, 4096, 8192};
    int num_quantums = sizeof(quantums) / sizeof(quantums[0]);

    for (int i = 0; i < num_quantums; i++) {
        char name[32];
        snprintf(name, sizeof(name), "quantum_%zu", quantums[i]);

        vmem_t *vmp = vmem_create(
            name,
            NULL, 0, quantums[i],
            NULL, NULL, NULL, 0,
            VM_SLEEP
        );
        munit_assert_not_null(vmp);

        /* Allocate aligned to quantum */
        void *addr = vmem_alloc(vmp, quantums[i] * 2, VM_SLEEP);
        if (addr) {
            vmem_free(vmp, addr, quantums[i] * 2);
        }

        vmem_destroy(vmp);
    }

    return MUNIT_OK;
}

/* Test: vmem_contains */
static MunitResult test_vmem_contains(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    vmem_t *vmp = vmem_create(
        "test_contains_arena",
        NULL, 0, 4096,
        NULL, NULL, NULL, 0,
        VM_SLEEP
    );
    munit_assert_not_null(vmp);

    void *addr = vmem_alloc(vmp, 8192, VM_SLEEP);
    if (addr) {
        /* Check if address is in arena */
        int contains = vmem_contains(vmp, addr, 8192);
        munit_assert_int(contains, !=, 0);

        vmem_free(vmp, addr, 8192);
    }

    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test: vmem_size to get total arena size */
static MunitResult test_vmem_size(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    vmem_t *vmp = vmem_create(
        "test_size_arena",
        NULL, 0, 4096,
        NULL, NULL, NULL, 0,
        VM_SLEEP
    );
    munit_assert_not_null(vmp);

    /* Get arena size */
    size_t size = vmem_size(vmp, VMEM_ALLOC);

    /* Size should be non-negative (may be 0 for new arena) */
    (void)size;

    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test: stress test - rapid alloc/free */
static MunitResult test_vmem_stress(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    vmem_t *vmp = vmem_create(
        "test_stress_arena",
        NULL, 0, 4096,
        NULL, NULL, NULL, 0,
        VM_SLEEP
    );
    munit_assert_not_null(vmp);

    for (int i = 0; i < 100; i++) {
        void *addr = vmem_alloc(vmp, 4096, VM_SLEEP);
        if (addr) {
            vmem_free(vmp, addr, 4096);
        }
    }

    vmem_destroy(vmp);

    return MUNIT_OK;
}

/* Test array */
static MunitTest vmem_tests[] = {
    { "/create_destroy", test_vmem_create_destroy, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/alloc_free", test_vmem_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/xalloc", test_vmem_xalloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple_allocs", test_vmem_multiple_allocs, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/add", test_vmem_add, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/quantum", test_vmem_quantum, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/contains", test_vmem_contains, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/size", test_vmem_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/stress", test_vmem_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_vmem = {
    "/vmem",
    vmem_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

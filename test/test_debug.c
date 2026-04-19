/*
 * Debug mode coverage tests for libumem.
 *
 * This is a SEPARATE binary from test_main because UMEM_DEBUG must be
 * set before the first umem allocation (before umem_init() runs).
 * Setting it via setenv() after init has no effect.
 *
 * This binary sets UMEM_DEBUG=default before any umem call, then
 * exercises debug-mode code paths: buftag validation, redzone
 * checking, deadbeef patterns, audit trails, and content logging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>

#include "umem.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %-50s", name); \
    fflush(stdout); \
} while (0)

#define PASS() do { \
    tests_passed++; \
    printf("[ OK ]\n"); \
} while (0)

#define FAIL(msg) do { \
    printf("[FAIL] %s\n", msg); \
} while (0)

/*
 * Test: basic allocation works in debug mode
 */
static void test_debug_alloc_free(void)
{
    TEST("debug_alloc_free");
    void *p = umem_alloc(64, UMEM_DEFAULT);
    if (p == NULL) { FAIL("alloc returned NULL"); return; }
    memset(p, 0xAA, 64);
    umem_free(p, 64);
    PASS();
}

/*
 * Test: zalloc returns zeroed memory in debug mode
 */
static void test_debug_zalloc(void)
{
    TEST("debug_zalloc_zeros");
    void *p = umem_zalloc(128, UMEM_DEFAULT);
    if (p == NULL) { FAIL("zalloc returned NULL"); return; }
    unsigned char *cp = (unsigned char *)p;
    for (int i = 0; i < 128; i++) {
        if (cp[i] != 0) {
            FAIL("not zeroed");
            umem_free(p, 128);
            return;
        }
    }
    umem_free(p, 128);
    PASS();
}

/*
 * Test: cache with constructor/destructor in debug mode
 */
static int ctor_count = 0;
static int dtor_count = 0;

static int debug_ctor(void *buf, void *priv, int flags)
{
    (void)priv; (void)flags;
    memset(buf, 0, 64);
    ctor_count++;
    return 0;
}

static void debug_dtor(void *buf, void *priv)
{
    (void)buf; (void)priv;
    dtor_count++;
}

static void test_debug_cache_ctor_dtor(void)
{
    TEST("debug_cache_ctor_dtor");
    ctor_count = 0;
    dtor_count = 0;

    umem_cache_t *cp = umem_cache_create("debug_cd", 64, 0,
        debug_ctor, debug_dtor, NULL, NULL, NULL, 0);
    if (cp == NULL) { FAIL("cache create failed"); return; }

    void *ptrs[20];
    for (int i = 0; i < 20; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        if (ptrs[i] == NULL) { FAIL("cache alloc failed"); return; }
    }
    for (int i = 0; i < 20; i++)
        umem_cache_free(cp, ptrs[i]);

    umem_cache_destroy(cp);

    if (ctor_count < 20) { FAIL("ctor not called enough"); return; }
    if (dtor_count < 20) { FAIL("dtor not called enough"); return; }
    PASS();
}

/*
 * Test: various size allocations exercise debug buftag paths
 */
static void test_debug_various_sizes(void)
{
    TEST("debug_various_sizes");
    int sizes[] = {8, 16, 32, 48, 64, 128, 256, 512, 1024, 2048, 4096, 0};
    for (int i = 0; sizes[i] != 0; i++) {
        void *p = umem_alloc(sizes[i], UMEM_DEFAULT);
        if (p == NULL) {
            FAIL("alloc failed");
            return;
        }
        memset(p, 0xBB, sizes[i]);
        umem_free(p, sizes[i]);
    }
    PASS();
}

/*
 * Test: rapid alloc/free cycles in debug mode
 * Exercises buftag checking on hot paths.
 */
static void test_debug_rapid_cycle(void)
{
    TEST("debug_rapid_alloc_free_cycle");
    for (int cycle = 0; cycle < 100; cycle++) {
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) { FAIL("alloc failed"); return; }
        ((char *)p)[0] = (char)cycle;
        umem_free(p, 64);
    }
    PASS();
}

/*
 * Test: debug mode with NOFAIL flag
 */
static void test_debug_nofail(void)
{
    TEST("debug_nofail");
    void *p = umem_alloc(64, UMEM_NOFAIL);
    if (p == NULL) { FAIL("NOFAIL returned NULL"); return; }
    umem_free(p, 64);
    PASS();
}

/*
 * Test: cache with alignment in debug mode
 */
static void test_debug_cache_aligned(void)
{
    TEST("debug_cache_aligned");
    umem_cache_t *cp = umem_cache_create("debug_align", 100, 64,
        NULL, NULL, NULL, NULL, NULL, 0);
    if (cp == NULL) { FAIL("cache create failed"); return; }

    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        if (ptrs[i] == NULL) { FAIL("alloc failed"); return; }
        if (((uintptr_t)ptrs[i] & 63) != 0) {
            FAIL("misaligned");
            return;
        }
    }
    for (int i = 0; i < 10; i++)
        umem_cache_free(cp, ptrs[i]);

    umem_cache_destroy(cp);
    PASS();
}

/*
 * Test: large allocation in debug mode (oversize arena)
 */
static void test_debug_large_alloc(void)
{
    TEST("debug_large_alloc");
    void *p = umem_alloc(256 * 1024, UMEM_DEFAULT);
    if (p == NULL) { FAIL("large alloc failed"); return; }
    memset(p, 0xCC, 256 * 1024);
    umem_free(p, 256 * 1024);
    PASS();
}

/*
 * Test: reap in debug mode
 */
static void test_debug_reap(void)
{
    TEST("debug_reap");
    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = umem_alloc(64, UMEM_DEFAULT);
        if (ptrs[i] == NULL) { FAIL("alloc failed"); return; }
    }
    for (int i = 0; i < 100; i++)
        umem_free(ptrs[i], 64);

    umem_reap();
    PASS();
}

/*
 * Test: cache with UMC_NODEBUG flag skips debug
 */
static void test_debug_nodebug_flag(void)
{
    TEST("debug_nodebug_cache");
    umem_cache_t *cp = umem_cache_create("nodebug_test", 64, 0,
        NULL, NULL, NULL, NULL, NULL, UMC_NODEBUG);
    if (cp == NULL) { FAIL("cache create failed"); return; }

    void *p = umem_cache_alloc(cp, UMEM_DEFAULT);
    if (p == NULL) { FAIL("alloc failed"); return; }
    umem_cache_free(cp, p);
    umem_cache_destroy(cp);
    PASS();
}

/*
 * Test: multiple caches active simultaneously in debug mode
 */
static void test_debug_multi_cache(void)
{
    TEST("debug_multi_cache");
    umem_cache_t *c1 = umem_cache_create("dbg_mc1", 32, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    umem_cache_t *c2 = umem_cache_create("dbg_mc2", 128, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    umem_cache_t *c3 = umem_cache_create("dbg_mc3", 512, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    if (!c1 || !c2 || !c3) { FAIL("cache create failed"); return; }

    void *p1 = umem_cache_alloc(c1, UMEM_DEFAULT);
    void *p2 = umem_cache_alloc(c2, UMEM_DEFAULT);
    void *p3 = umem_cache_alloc(c3, UMEM_DEFAULT);
    if (!p1 || !p2 || !p3) { FAIL("alloc failed"); return; }

    umem_cache_free(c1, p1);
    umem_cache_free(c2, p2);
    umem_cache_free(c3, p3);

    umem_cache_destroy(c1);
    umem_cache_destroy(c2);
    umem_cache_destroy(c3);
    PASS();
}

int
main(void)
{
    /*
     * CRITICAL: Set UMEM_DEBUG before any umem call.
     * This must happen before umem_init() which is triggered
     * by the first allocation.
     */
    setenv("UMEM_DEBUG", "default", 1);
    setenv("UMEM_LOGGING", "transaction", 1);

    printf("libumem debug mode coverage tests\n");
    printf("UMEM_DEBUG=%s\n", getenv("UMEM_DEBUG"));
    printf("==================================\n\n");

    test_debug_alloc_free();
    test_debug_zalloc();
    test_debug_cache_ctor_dtor();
    test_debug_various_sizes();
    test_debug_rapid_cycle();
    test_debug_nofail();
    test_debug_cache_aligned();
    test_debug_large_alloc();
    test_debug_reap();
    test_debug_nodebug_flag();
    test_debug_multi_cache();

    printf("\n==================================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}

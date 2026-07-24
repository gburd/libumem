/*
 * Unit tests for umem debug modes
 *
 * Note: Some debug modes (like REDZONE/FIREWALL) may abort on violations,
 * so those tests use subprocess wrappers to verify the detection works.
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../umem_impl.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>

extern char **environ;

#ifndef UMEM_ENV_HELPER
#error "UMEM_ENV_HELPER path must be defined by the build (see Makefile.am)"
#endif

/*
 * Spawn the exec helper in a FRESH process with UMEM_DEBUG=<dbg> and
 * UMEM_ABORT=1 set, running the given detection probe (overflow /
 * double_free / uaf).  Returns the raw wait status.
 *
 * A working detector makes the child abort (SIGABRT) or exit non-zero.
 * If the child exits 0 the corruption went UNDETECTED (the probe survived).
 */
static int run_detection(const char *dbg, const char *probe) {
    size_t n = 0;
    for (char **e = environ; *e; e++) n++;
    char **envp = calloc(n + 3, sizeof(char *));
    if (envp == NULL) return -1;
    size_t idx = 0;
    /* Copy environ, dropping any pre-existing UMEM_DEBUG / UMEM_ABORT. */
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, "UMEM_DEBUG=", 11) == 0) continue;
        if (strncmp(*e, "UMEM_ABORT=", 11) == 0) continue;
        envp[idx++] = *e;
    }
    char dbgbuf[128];
    snprintf(dbgbuf, sizeof dbgbuf, "UMEM_DEBUG=%s", dbg);
    envp[idx++] = dbgbuf;
    envp[idx++] = (char *)"UMEM_ABORT=1";
    envp[idx] = NULL;

    char *args[] = { (char *)UMEM_ENV_HELPER, (char *)probe, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, UMEM_ENV_HELPER, NULL, NULL, args, envp);
    free(envp);
    if (rc != 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return status;
}

/* True if the child aborted (signal) or exited non-zero => detection fired. */
static int detection_fired(int status) {
    if (status < 0) return 0;
    if (WIFSIGNALED(status)) return 1;
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) return 1;
    return 0;  /* exited 0 => corruption survived undetected */
}


/* Test: UMF_AUDIT enables audit tracking */
static MunitResult test_audit_enabled(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Set UMEM_DEBUG environment variable */
    setenv("UMEM_DEBUG", "audit", 1);

    /* Allocate with audit enabled */
    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* With audit, allocations are tracked with stack traces */
    /* We can't easily verify this without exposing internal APIs,
     * but at least verify it doesn't crash */

    umem_free(ptr, 128);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: UMF_DEADBEEF fills freed memory */
static MunitResult test_deadbeef(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "deadbeef", 1);

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Fill with known pattern */
    memset(ptr, 0x42, 128);

    /* Free - should fill with 0xdeadbeef pattern */
    umem_free(ptr, 128);

    /* Note: We can't safely read freed memory to verify the pattern,
     * as it may be reused. This test just ensures deadbeef mode
     * doesn't crash. */

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: UMF_CONTENTS sets initial contents */
static MunitResult test_contents(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* contents=0xab sets initial pattern */
    setenv("UMEM_DEBUG", "contents=0xab", 1);

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Check if memory has the pattern (may not be guaranteed) */
    /* This is more of a smoke test */

    umem_free(ptr, 128);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: multiple debug flags */
static MunitResult test_multiple_flags(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "audit,deadbeef", 1);

    void *ptr = umem_alloc(256, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    memset(ptr, 0xFF, 256);
    umem_free(ptr, 256);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: default debug mode */
static MunitResult test_default_mode(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "default", 1);

    /* Default enables common debug features */
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    umem_free(ptr, 64);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: debug mode with cache */
static MunitResult test_debug_with_cache(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "audit", 1);

    umem_cache_t *cache = umem_cache_create(
        "debug_cache",
        64,
        0, NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    umem_cache_free(cache, obj);
    umem_cache_destroy(cache);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: verbose debug logging */
static MunitResult test_verbose(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "verbose", 1);

    void *ptr = umem_alloc(128, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    umem_free(ptr, 128);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: REDZONE detection.  Overflow past a guarded buffer must abort or
 * exit non-zero when freed under UMEM_DEBUG=guards + UMEM_ABORT=1.
 *
 * EXPECTED RED (WS-A A3): the review found overflow is NOT detected in a
 * fresh process.  This RED is the reproduction handed to Workstream B. */
static MunitResult test_redzone_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;
    int status = run_detection("guards", "overflow");
    munit_assert_true(detection_fired(status));
    return MUNIT_OK;
}

/* Test: FIREWALL detection.  With UMEM_DEBUG=firewall=<N>, writing past a
 * buffer >= N lands on an unmapped page and must fault (SIGSEGV).
 * Firewall requires a byte threshold (=minbytes) and places the buffer flush
 * against the guard page, so the probe allocates a page-sized buffer and
 * overruns it. */
static MunitResult test_firewall_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;
    int status = run_detection("firewall=4096", "firewall_overflow");
    munit_assert_true(detection_fired(status));
    return MUNIT_OK;
}

/* Test: UMF_LITE mode (lightweight debugging) */
static MunitResult test_lite_mode(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "lite", 1);

    /* Lite mode provides lightweight debugging without full audit overhead */
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    memset(ptr, 0xAA, 64);
    umem_free(ptr, 64);

    /* Test with cache as well */
    umem_cache_t *cache = umem_cache_create(
        "lite_cache",
        32,
        0, NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    umem_cache_free(cache, obj);
    umem_cache_destroy(cache);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: Audit with stack traces.  Runs in a FRESH process via the exec
 * helper so UMEM_DEBUG=audit (read once at init) actually takes effect --
 * setenv() in this already-initialized process is too late.  The helper
 * allocates a spread of sizes under audit and must succeed (exit 0). */
static MunitResult test_audit_stack_traces(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;
    int status = run_detection("audit", "audit_sizes");
    munit_assert_true(status >= 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);
    return MUNIT_OK;
}

/* Test: Double free detection.  Freeing the same buffer twice under
 * UMEM_DEBUG=guards + UMEM_ABORT=1 must abort / exit non-zero.
 * EXPECTED RED pending WS-B. */
static MunitResult test_double_free_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;
    int status = run_detection("guards", "double_free");
    munit_assert_true(detection_fired(status));
    return MUNIT_OK;
}

/* Test: Buffer corruption / use-after-free detection.  Touching freed
 * memory (DEADBEEF verify) then churning must abort under default debug.
 * EXPECTED RED pending WS-B. */
static MunitResult test_corruption_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;
    int status = run_detection("default", "uaf");
    munit_assert_true(detection_fired(status));
    return MUNIT_OK;
}

/* Test: Uninitialized pattern verification */
static MunitResult test_uninitialized_pattern(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "default", 1);

    /* Allocate a buffer - in debug mode it may be filled with UMEM_UNINITIALIZED_PATTERN */
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* We cannot reliably check the pattern since it might be overwritten
     * by the allocator or previous use, but we verify allocation works
     * with the debug flag that uses this pattern */

    /* Write known pattern */
    memset(ptr, 0x55, 64);

    /* Free it */
    umem_free(ptr, 64);

    /* Allocate again - should be filled with DEADBEEF pattern after previous free */
    void *ptr2 = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(ptr2);

    umem_free(ptr2, 64);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: Multiple debug flags combined */
static MunitResult test_debug_flags_combination(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Combine multiple debug flags: audit, deadbeef, and redzone checking */
    setenv("UMEM_DEBUG", "audit,deadbeef,guards", 1);

    /* Allocate with all debug features active */
    void *ptr = umem_alloc(256, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    /* Write data to buffer */
    memset(ptr, 0xCC, 256);

    /* Free - should apply deadbeef pattern and verify redzone */
    umem_free(ptr, 256);

    /* Test with cache to ensure debug flags work there too */
    umem_cache_t *cache = umem_cache_create(
        "multi_debug_cache",
        128,
        0, NULL, NULL, NULL, NULL, NULL, 0
    );
    munit_assert_not_null(cache);

    void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
    munit_assert_not_null(obj);

    memset(obj, 0xDD, 128);

    umem_cache_free(cache, obj);
    umem_cache_destroy(cache);

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test array */
static MunitTest debug_tests[] = {
    /* Original tests */
    { "/audit", test_audit_enabled, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/deadbeef", test_deadbeef, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/contents", test_contents, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple_flags", test_multiple_flags, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/default", test_default_mode, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/with_cache", test_debug_with_cache, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verbose", test_verbose, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

    /* New comprehensive debug tests */
    { "/redzone_detection", test_redzone_detection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/firewall_detection", test_firewall_detection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/lite_mode", test_lite_mode, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/audit_stack_traces", test_audit_stack_traces, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/double_free_detection", test_double_free_detection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/corruption_detection", test_corruption_detection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/uninitialized_pattern", test_uninitialized_pattern, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_flags_combination", test_debug_flags_combination, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_debug = {
    "/umem_debug",
    debug_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

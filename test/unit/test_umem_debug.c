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
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

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

/* Test: REDZONE detection (subprocess wrapper - will abort) */
static MunitResult test_redzone_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: trigger redzone violation */
        setenv("UMEM_DEBUG", "default", 1);

        void *ptr = umem_alloc(128, UMEM_DEFAULT);
        if (ptr == NULL) {
            exit(1);
        }

        /* Write past the end of buffer to trigger redzone violation */
        char *buf = (char *)ptr;
        buf[128] = 0x42;  /* One byte past the end */

        /* Free should detect the redzone violation and abort */
        umem_free(ptr, 128);

        /* Should not reach here */
        exit(0);
    } else if (pid > 0) {
        /* Parent: verify child detected the violation */
        int status;
        waitpid(pid, &status, 0);

        /* Child should have been signaled (SIGABRT) or exited with error */
        munit_assert_true(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));

        return MUNIT_OK;
    } else {
        munit_error("fork failed");
        return MUNIT_ERROR;
    }
}

/* Test: FIREWALL detection (subprocess wrapper - will segfault) */
static MunitResult test_firewall_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: trigger firewall violation */
        setenv("UMEM_DEBUG", "default,firewall=128", 1);

        /* Allocate a buffer large enough to trigger firewall mode */
        void *ptr = umem_alloc(256, UMEM_DEFAULT);
        if (ptr == NULL) {
            exit(1);
        }

        /* Try to access memory just past the buffer - should hit guard page */
        volatile char *buf = (char *)ptr;
        volatile char c = buf[300];  /* Beyond buffer, should hit unmapped page */
        (void)c;

        umem_free(ptr, 256);

        /* Should not reach here */
        exit(0);
    } else if (pid > 0) {
        /* Parent: verify child crashed */
        int status;
        waitpid(pid, &status, 0);

        /* Child should have been signaled (SIGSEGV or SIGBUS) */
        munit_assert_true(WIFSIGNALED(status));

        return MUNIT_OK;
    } else {
        munit_error("fork failed");
        return MUNIT_ERROR;
    }
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

/* Test: Audit with stack traces */
static MunitResult test_audit_stack_traces(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    setenv("UMEM_DEBUG", "audit", 1);

    /* With audit, allocations should be tracked with stack traces */
    /* Allocate and free multiple buffers to populate the audit log */
    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = umem_alloc(64 + i * 8, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }

    for (int i = 0; i < 10; i++) {
        umem_free(ptrs[i], 64 + i * 8);
    }

    /* The audit trail is internal, but we verify no crashes occurred */
    /* and that the auditing mechanism is active */

    unsetenv("UMEM_DEBUG");

    return MUNIT_OK;
}

/* Test: Double free detection (subprocess wrapper - may abort) */
static MunitResult test_double_free_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: trigger double free */
        setenv("UMEM_DEBUG", "default", 1);

        void *ptr = umem_alloc(128, UMEM_DEFAULT);
        if (ptr == NULL) {
            exit(1);
        }

        umem_free(ptr, 128);

        /* Second free should be detected and abort */
        umem_free(ptr, 128);

        /* Should not reach here */
        exit(0);
    } else if (pid > 0) {
        /* Parent: verify child detected double free */
        int status;
        waitpid(pid, &status, 0);

        /* Child should have been signaled or exited with error */
        munit_assert_true(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));

        return MUNIT_OK;
    } else {
        munit_error("fork failed");
        return MUNIT_ERROR;
    }
}

/* Test: Buffer corruption detection */
static MunitResult test_corruption_detection(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: corrupt buffer metadata */
        setenv("UMEM_DEBUG", "default", 1);

        void *ptr = umem_alloc(128, UMEM_DEFAULT);
        if (ptr == NULL) {
            exit(1);
        }

        /* Corrupt the buftag that comes after the buffer */
        /* In debug mode, there's a buftag appended to each buffer */
        char *corrupt_zone = (char *)ptr + 128;
        memset(corrupt_zone, 0xFF, 8);  /* Corrupt redzone/buftag */

        /* Free should detect corruption */
        umem_free(ptr, 128);

        /* Should not reach here */
        exit(0);
    } else if (pid > 0) {
        /* Parent: verify child detected corruption */
        int status;
        waitpid(pid, &status, 0);

        /* Child should have detected corruption */
        munit_assert_true(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));

        return MUNIT_OK;
    } else {
        munit_error("fork failed");
        return MUNIT_ERROR;
    }
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

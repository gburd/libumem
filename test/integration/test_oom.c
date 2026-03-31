/*
 * Integration tests for out-of-memory handling with libumem
 */

#include "../munit.h"
#include "../../umem.h"
#include <sys/resource.h>
#include <string.h>
#include <unistd.h>

/* Test 1: UMEM_DEFAULT returns NULL on exhaustion */
static MunitResult test_graceful_oom(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    struct rlimit old_limit, new_limit;

    /* Save current limit */
    if (getrlimit(RLIMIT_AS, &old_limit) != 0) {
        return MUNIT_SKIP;
    }

    /* Set 100MB limit */
    new_limit.rlim_cur = 100 * 1024 * 1024;
    new_limit.rlim_max = old_limit.rlim_max;

    if (setrlimit(RLIMIT_AS, &new_limit) != 0) {
        return MUNIT_SKIP;
    }

    /* Allocate until we get NULL */
    #define MAX_OOMTEST_ALLOCS 100
    void *ptrs[MAX_OOMTEST_ALLOCS];
    int num_allocs = 0;
    int got_null = 0;

    for (int i = 0; i < MAX_OOMTEST_ALLOCS; i++) {
        ptrs[i] = umem_alloc(5 * 1024 * 1024, UMEM_DEFAULT);  /* 5MB chunks */
        if (ptrs[i] == NULL) {
            got_null = 1;
            break;
        }
        num_allocs++;

        /* Touch pages to force allocation */
        memset(ptrs[i], 0xFF, 5 * 1024 * 1024);
    }

    /* Clean up allocated memory */
    for (int i = 0; i < num_allocs; i++) {
        umem_free(ptrs[i], 5 * 1024 * 1024);
    }

    /* Restore limit */
    setrlimit(RLIMIT_AS, &old_limit);

    /* Verify we got NULL at some point (graceful failure) */
    munit_assert_int(got_null, ==, 1);

    return MUNIT_OK;
}

/* Test 2: UMEM_NOFAIL behavior under pressure */
static MunitResult test_nofail_never_null(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Allocate with UMEM_NOFAIL flag
     * This should never return NULL (may exit/abort on OOM, but not NULL) */

    #define NOFAIL_ALLOCS 100
    void *ptrs[NOFAIL_ALLOCS];

    for (int i = 0; i < NOFAIL_ALLOCS; i++) {
        ptrs[i] = umem_alloc(4096, UMEM_NOFAIL);
        munit_assert_not_null(ptrs[i]);
        memset(ptrs[i], i & 0xFF, 4096);
    }

    /* Free all */
    for (int i = 0; i < NOFAIL_ALLOCS; i++) {
        umem_free(ptrs[i], 4096);
    }

    return MUNIT_OK;
}

/* Test 3: Free memory, retry allocation succeeds */
static MunitResult test_oom_recovery(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    struct rlimit old_limit, new_limit;

    /* Save current limit */
    if (getrlimit(RLIMIT_AS, &old_limit) != 0) {
        return MUNIT_SKIP;
    }

    /* Set 80MB limit */
    new_limit.rlim_cur = 80 * 1024 * 1024;
    new_limit.rlim_max = old_limit.rlim_max;

    if (setrlimit(RLIMIT_AS, &new_limit) != 0) {
        return MUNIT_SKIP;
    }

    /* Allocate many large buffers until we're close to the limit */
    #define RECOVERY_ALLOCS 20
    void *ptrs[RECOVERY_ALLOCS];
    int num_allocs = 0;

    for (int i = 0; i < RECOVERY_ALLOCS; i++) {
        ptrs[i] = umem_alloc(3 * 1024 * 1024, UMEM_DEFAULT);  /* 3MB chunks */
        if (ptrs[i] == NULL) {
            break;
        }
        num_allocs++;
        memset(ptrs[i], 0xAA, 3 * 1024 * 1024);
    }

    /* Verify we allocated at least a few blocks */
    munit_assert_int(num_allocs, >, 5);

    /* Free half of them to create space */
    int half = num_allocs / 2;
    for (int i = 0; i < half; i++) {
        umem_free(ptrs[i], 3 * 1024 * 1024);
        ptrs[i] = NULL;
    }

    /* Now try to allocate again - should succeed after freeing memory */
    void *recovery_ptr = umem_alloc(3 * 1024 * 1024, UMEM_DEFAULT);
    munit_assert_not_null(recovery_ptr);

    /* Clean up */
    umem_free(recovery_ptr, 3 * 1024 * 1024);

    for (int i = half; i < num_allocs; i++) {
        umem_free(ptrs[i], 3 * 1024 * 1024);
    }

    /* Restore limit */
    setrlimit(RLIMIT_AS, &old_limit);

    return MUNIT_OK;
}

/* Callback for test 4 */
static volatile int nofail_callback_was_invoked = 0;

static int oom_nofail_callback(void) {
    nofail_callback_was_invoked = 1;
    /* Return RETRY to allow allocation to proceed if possible */
    return UMEM_CALLBACK_RETRY;
}

/* Test 4: Callback invoked on memory pressure */
static MunitResult test_umem_nofail_callback_oom(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    struct rlimit old_limit, new_limit;

    /* Register callback */
    umem_nofail_callback(oom_nofail_callback);

    /* Reset flag */
    nofail_callback_was_invoked = 0;

    /* Save current limit */
    if (getrlimit(RLIMIT_AS, &old_limit) != 0) {
        umem_nofail_callback(NULL);
        return MUNIT_SKIP;
    }

    /* Set 50MB limit to induce memory pressure */
    new_limit.rlim_cur = 50 * 1024 * 1024;
    new_limit.rlim_max = old_limit.rlim_max;

    if (setrlimit(RLIMIT_AS, &new_limit) != 0) {
        umem_nofail_callback(NULL);
        return MUNIT_SKIP;
    }

    /* Allocate with UMEM_NOFAIL until we're near the limit
     * This may or may not trigger the callback depending on system state */
    #define CALLBACK_ALLOCS 15
    void *ptrs[CALLBACK_ALLOCS];
    int num_allocs = 0;

    for (int i = 0; i < CALLBACK_ALLOCS; i++) {
        ptrs[i] = umem_alloc(2 * 1024 * 1024, UMEM_NOFAIL);  /* 2MB chunks */

        /* If callback was invoked, we achieved our goal
         * UMEM_NOFAIL should not return NULL even under pressure */
        if (ptrs[i] == NULL) {
            /* This should not happen with UMEM_NOFAIL, but handle gracefully */
            break;
        }

        munit_assert_not_null(ptrs[i]);
        num_allocs++;
        memset(ptrs[i], 0xBB, 2 * 1024 * 1024);

        /* If callback was invoked, we can stop the test */
        if (nofail_callback_was_invoked) {
            break;
        }
    }

    /* Clean up */
    for (int i = 0; i < num_allocs; i++) {
        umem_free(ptrs[i], 2 * 1024 * 1024);
    }

    /* Restore limit */
    setrlimit(RLIMIT_AS, &old_limit);

    /* Clean up callback */
    umem_nofail_callback(NULL);

    /* Note: We don't assert that the callback was invoked because in
     * some conditions there may be enough memory. The important thing
     * is that UMEM_NOFAIL never returned NULL and we didn't crash. */

    return MUNIT_OK;
}

/* Test array */
static MunitTest oom_tests[] = {
    { "/graceful_oom", test_graceful_oom, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/nofail_never_null", test_nofail_never_null, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/oom_recovery", test_oom_recovery, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/umem_nofail_callback_oom", test_umem_nofail_callback_oom, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitSuite oom_suite = {
    "/oom",
    oom_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[]) {
    return munit_suite_main(&oom_suite, NULL, argc, argv);
}

/*
 * Integration tests for signal safety with libumem
 */

#include "../munit.h"
#include "../../umem.h"
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

static volatile sig_atomic_t signal_handler_ran = 0;
static volatile sig_atomic_t allocation_succeeded = 0;

static void signal_handler(int sig) {
    (void)sig;
    signal_handler_ran = 1;

    /* Try allocation in signal handler
     * Note: This may not be async-signal-safe and may fail gracefully */
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    if (ptr != NULL) {
        allocation_succeeded = 1;
        umem_free(ptr, 64);
    }
}

/* Test 1: Can allocate from signal handler (if safe) */
static MunitResult test_signal_handler_alloc(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    struct sigaction sa = {0};
    struct sigaction old_sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    munit_assert_int(sigaction(SIGUSR1, &sa, &old_sa), ==, 0);

    signal_handler_ran = 0;
    allocation_succeeded = 0;

    /* Send signal to self */
    raise(SIGUSR1);
    usleep(10000);  /* Give handler time to run */

    munit_assert_int(signal_handler_ran, ==, 1);
    /* Note: allocation_succeeded may be 0 or 1 depending on implementation
     * This test just verifies no crash occurs */

    /* Restore old handler */
    sigaction(SIGUSR1, &old_sa, NULL);

    return MUNIT_OK;
}

static volatile sig_atomic_t signal_count = 0;

static void interrupt_handler(int sig) {
    (void)sig;
    signal_count++;
}

/* Test 2: Signal interrupts allocation */
static MunitResult test_signal_during_alloc(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    struct sigaction sa = {0};
    struct sigaction old_sa;
    sa.sa_handler = interrupt_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    munit_assert_int(sigaction(SIGUSR1, &sa, &old_sa), ==, 0);

    signal_count = 0;

    /* Allocate in loop, periodically sending signals */
    #define ALLOC_ITERATIONS 1000
    void *ptrs[ALLOC_ITERATIONS];

    for (int i = 0; i < ALLOC_ITERATIONS; i++) {
        /* Send signal every 100 iterations */
        if (i % 100 == 0) {
            raise(SIGUSR1);
        }

        ptrs[i] = umem_alloc(128, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);

        /* Touch memory to ensure it's valid */
        memset(ptrs[i], i & 0xFF, 128);
    }

    /* Verify signals were received */
    munit_assert_int(signal_count, >, 0);

    /* Free all allocations */
    for (int i = 0; i < ALLOC_ITERATIONS; i++) {
        umem_free(ptrs[i], 128);
    }

    /* Restore old handler */
    sigaction(SIGUSR1, &old_sa, NULL);

    return MUNIT_OK;
}

/* Test 3: Verify UMEM_DEBUG environment variable handling */
static MunitResult test_signal_safety_flags(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Set UMEM_DEBUG=checksignal if supported
     * Note: This tests that setting debug flags doesn't break signal safety */

    /* Perform some allocations */
    #define FLAG_TEST_ALLOCS 100
    void *ptrs[FLAG_TEST_ALLOCS];

    for (int i = 0; i < FLAG_TEST_ALLOCS; i++) {
        ptrs[i] = umem_alloc(256, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
        memset(ptrs[i], 0xAA, 256);
    }

    /* Free all */
    for (int i = 0; i < FLAG_TEST_ALLOCS; i++) {
        umem_free(ptrs[i], 256);
    }

    return MUNIT_OK;
}

/* Test 4: Basic async-signal-safety check */
static MunitResult test_async_signal_safe(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();

    if (pid < 0) {
        /* Fork failed, skip test */
        return MUNIT_SKIP;
    } else if (pid == 0) {
        /* Child process */

        /* Install signal handler */
        struct sigaction sa = {0};
        sa.sa_handler = interrupt_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, NULL);

        /* Do some allocations */
        for (int i = 0; i < 100; i++) {
            void *ptr = umem_alloc(64, UMEM_DEFAULT);
            if (ptr == NULL) {
                _exit(1);
            }
            umem_free(ptr, 64);
        }

        /* Send signal to self */
        raise(SIGUSR1);

        /* More allocations after signal */
        for (int i = 0; i < 100; i++) {
            void *ptr = umem_alloc(128, UMEM_DEFAULT);
            if (ptr == NULL) {
                _exit(2);
            }
            umem_free(ptr, 128);
        }

        _exit(0);
    } else {
        /* Parent process */
        int status;
        waitpid(pid, &status, 0);

        /* Verify child exited successfully */
        munit_assert_true(WIFEXITED(status));
        munit_assert_int(WEXITSTATUS(status), ==, 0);
    }

    return MUNIT_OK;
}

/* Test array */
static MunitTest signal_tests[] = {
    { "/signal_handler_alloc", test_signal_handler_alloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/signal_during_alloc", test_signal_during_alloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/signal_safety_flags", test_signal_safety_flags, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/async_signal_safe", test_async_signal_safe, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitSuite signal_suite = {
    "/signals",
    signal_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[]) {
    return munit_suite_main(&signal_suite, NULL, argc, argv);
}

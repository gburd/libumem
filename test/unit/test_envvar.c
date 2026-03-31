/*
 * Unit tests for envvar.c - Environment variable parsing
 *
 * Challenge: envvar.c reads UMEM_* environment variables at initialization time.
 * Tests must use fork() to set env vars before libumem initializes.
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../umem_base.h"
#include "../../vmem_base.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* External declarations for testing */
extern uint32_t umem_flags;
extern uint32_t umem_stack_depth;
extern uint32_t umem_output;
extern size_t umem_content_maxsave;
extern size_t umem_minfirewall;
extern size_t umem_maxverify;
extern uint32_t umem_abort;
extern uint32_t umem_mtbf;
extern uint32_t umem_max_ncpus;
extern uint32_t umem_depot_contention;
extern uint32_t umem_reap_interval;
extern size_t umem_ptc_size;
extern uint32_t umem_logging;
extern size_t umem_transaction_log_size;
extern size_t umem_content_log_size;
extern size_t umem_failure_log_size;
extern size_t umem_slab_log_size;
#ifndef UMEM_STANDALONE
extern uint_t vmem_backend;
extern uint_t vmem_allocator;
extern size_t vmem_sbrk_minalloc;
extern size_t vmem_sbrk_pagesize;
#endif

/*
 * Helper function to test environment variables in a forked process.
 * This ensures umem initialization happens with the env var set.
 */
static int test_with_env(const char *var, const char *value, int (*test_func)(void)) {
    pid_t pid = fork();
    if (pid == -1) {
        return -1;  /* fork failed */
    }

    if (pid == 0) {
        /* Child process */
        if (value != NULL) {
            setenv(var, value, 1);
        }

        /* Initialize umem by allocating */
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) {
            exit(1);
        }
        umem_free(p, 64);

        /* Run test function if provided */
        int result = 0;
        if (test_func != NULL) {
            result = test_func();
        }

        exit(result);
    }

    /* Parent process */
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;  /* abnormal exit */
}

/* Test function: check if UMF_AUDIT flag is set */
static int check_audit_flag(void) {
    return (umem_flags & UMF_AUDIT) ? 0 : 1;
}

/* Test function: check if UMF_DEADBEEF flag is set */
static int check_deadbeef_flag(void) {
    return (umem_flags & UMF_DEADBEEF) ? 0 : 1;
}

/* Test function: check if UMF_REDZONE flag is set */
static int check_redzone_flag(void) {
    return (umem_flags & UMF_REDZONE) ? 0 : 1;
}

/* Test function: check if UMF_CONTENTS flag is set */
static int check_contents_flag(void) {
    return (umem_flags & UMF_CONTENTS) ? 0 : 1;
}

/* Test function: check if UMF_FIREWALL flag is set */
static int check_firewall_flag(void) {
    return (umem_flags & UMF_FIREWALL) ? 0 : 1;
}

/* Test function: check if UMF_LITE flag is set */
static int check_lite_flag(void) {
    return (umem_flags & UMF_LITE) ? 0 : 1;
}

/* Test function: check default flags (audit, contents, deadbeef, redzone) */
static int check_default_flags(void) {
    uint32_t expected = UMF_AUDIT | UMF_CONTENTS | UMF_DEADBEEF | UMF_REDZONE;
    return ((umem_flags & expected) == expected) ? 0 : 1;
}

/* Test: UMEM_DEBUG=audit */
static MunitResult test_debug_audit(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    int result = test_with_env("UMEM_DEBUG", "audit", check_audit_flag);
    munit_assert_int(result, ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=default (enables audit,contents,guards) */
static MunitResult test_debug_default(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    int result = test_with_env("UMEM_DEBUG", "default", check_default_flags);
    munit_assert_int(result, ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=guards (enables deadbeef and redzone) */
static MunitResult test_debug_guards(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    int result = test_with_env("UMEM_DEBUG", "guards", check_deadbeef_flag);
    munit_assert_int(result, ==, 0);

    result = test_with_env("UMEM_DEBUG", "guards", check_redzone_flag);
    munit_assert_int(result, ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=contents */
static MunitResult test_debug_contents(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    int result = test_with_env("UMEM_DEBUG", "contents", check_contents_flag);
    munit_assert_int(result, ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=lite */
static MunitResult test_debug_lite(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    int result = test_with_env("UMEM_DEBUG", "lite", check_lite_flag);
    munit_assert_int(result, ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG with multiple flags */
static MunitResult test_debug_multiple_flags(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "audit,guards", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check both flags are set */
        if ((umem_flags & UMF_AUDIT) == 0) exit(1);
        if ((umem_flags & UMF_DEADBEEF) == 0) exit(1);
        if ((umem_flags & UMF_REDZONE) == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=audit=15 (with argument for stack depth) */
static MunitResult test_debug_audit_with_arg(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "audit=15", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check audit flag is set and stack_depth is 15 */
        if ((umem_flags & UMF_AUDIT) == 0) exit(1);
        if (umem_stack_depth != 15) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=contents=256 (with byte count) */
static MunitResult test_debug_contents_with_size(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "contents=256", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check contents flag is set and maxsave is 256 */
        if ((umem_flags & UMF_CONTENTS) == 0) exit(1);
        if (umem_content_maxsave != 256) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=firewall=4096 */
static MunitResult test_debug_firewall(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "firewall=4096", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check firewall flag is set and minfirewall is 4096 */
        if ((umem_flags & UMF_FIREWALL) == 0) exit(1);
        if (umem_minfirewall != 4096) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=maxverify=1024 */
static MunitResult test_debug_maxverify(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "maxverify=1024", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check maxverify is set */
        if (umem_maxverify != 1024) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=noabort (clear flag) */
static MunitResult test_debug_noabort(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "noabort", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check abort flag is cleared */
        if (umem_abort != 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=verbose (sets output=1) */
static MunitResult test_debug_verbose(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "verbose", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check output is set to 1 */
        if (umem_output < 1) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=mtbf=100 */
static MunitResult test_debug_mtbf(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "mtbf=100", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check mtbf is set */
        if (umem_mtbf != 100) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=concurrency=8 */
static MunitResult test_options_concurrency(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "concurrency=8", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check max_ncpus is set */
        if (umem_max_ncpus != 8) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=max_contention=50 */
static MunitResult test_options_max_contention(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "max_contention=50", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check depot_contention is set */
        if (umem_depot_contention != 50) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=nomagazines */
static MunitResult test_options_nomagazines(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "nomagazines", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check UMF_NOMAGAZINE flag is set */
        if ((umem_flags & UMF_NOMAGAZINE) == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=reap_interval=30 */
static MunitResult test_options_reap_interval(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "reap_interval=30", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check reap_interval is set */
        if (umem_reap_interval != 30) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=perthread_cache=8192 */
static MunitResult test_options_perthread_cache(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "perthread_cache=8192", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check ptc_size is set */
        if (umem_ptc_size != 8192) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS with size suffixes (k, m, g) */
static MunitResult test_options_size_suffixes(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Test kilobytes */
    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "perthread_cache=8k", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        if (umem_ptc_size != 8 * 1024) exit(1);
        exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    /* Test megabytes */
    pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "perthread_cache=1m", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        if (umem_ptc_size != 1024 * 1024) exit(1);
        exit(0);
    }
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

#ifndef UMEM_STANDALONE
/* Test: UMEM_OPTIONS=backend=mmap */
static MunitResult test_options_backend_mmap(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "backend=mmap", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check backend is set to VMEM_BACKEND_MMAP */
        if ((vmem_backend & VMEM_BACKEND_MMAP) == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=backend=sbrk */
static MunitResult test_options_backend_sbrk(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "backend=sbrk", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check backend is set to VMEM_BACKEND_SBRK */
        if ((vmem_backend & VMEM_BACKEND_SBRK) == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=allocator=best */
static MunitResult test_options_allocator_best(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "allocator=best", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check allocator is VM_BESTFIT */
        if (vmem_allocator != VM_BESTFIT) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=allocator=first */
static MunitResult test_options_allocator_first(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "allocator=first", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check allocator is VM_FIRSTFIT */
        if (vmem_allocator != VM_FIRSTFIT) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=allocator=next */
static MunitResult test_options_allocator_next(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "allocator=next", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check allocator is VM_NEXTFIT */
        if (vmem_allocator != VM_NEXTFIT) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}
#endif /* !UMEM_STANDALONE */

/* Test: UMEM_LOGGING=transaction */
static MunitResult test_logging_transaction(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_LOGGING", "transaction", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check logging is enabled and transaction_log_size is set */
        if (umem_logging == 0) exit(1);
        if (umem_transaction_log_size == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=transaction=128k */
static MunitResult test_logging_transaction_with_size(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_LOGGING", "transaction=128k", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check transaction_log_size is 128k */
        if (umem_transaction_log_size != 128 * 1024) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=contents */
static MunitResult test_logging_contents(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_LOGGING", "contents", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check logging is enabled and contents flag is set */
        if (umem_logging == 0) exit(1);
        if ((umem_flags & UMF_CONTENTS) == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=fail */
static MunitResult test_logging_fail(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_LOGGING", "fail", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check logging is enabled and failure_log_size is set */
        if (umem_logging == 0) exit(1);
        if (umem_failure_log_size == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=slab */
static MunitResult test_logging_slab(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_LOGGING", "slab", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check logging is enabled and slab_log_size is set */
        if (umem_logging == 0) exit(1);
        if (umem_slab_log_size == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: Invalid UMEM_DEBUG value (should be ignored) */
static MunitResult test_debug_invalid_value(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "invalid_option", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Should still work, option is just ignored */
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: UMEM_DEBUG with invalid numeric argument */
static MunitResult test_debug_invalid_number(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "audit=notanumber", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Should still work, invalid arg is ignored */
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: Overflow in numeric parsing */
static MunitResult test_numeric_overflow(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        /* Set a value that would overflow uint32_t */
        setenv("UMEM_DEBUG", "audit=99999999999999999999", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Should still work, overflow value is rejected */
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: Empty option value */
static MunitResult test_empty_option(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: Whitespace handling */
static MunitResult test_whitespace_handling(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_DEBUG", "  audit  ,  guards  ", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Should parse correctly despite whitespace */
        if ((umem_flags & UMF_AUDIT) == 0) exit(1);
        if ((umem_flags & UMF_DEADBEEF) == 0) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: Multiple comma-separated options */
static MunitResult test_multiple_options(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "concurrency=4,nomagazines,reap_interval=15", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Check all options are applied */
        if (umem_max_ncpus != 4) exit(1);
        if ((umem_flags & UMF_NOMAGAZINE) == 0) exit(1);
        if (umem_reap_interval != 15) exit(1);

        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: Case sensitivity (k vs K for size suffixes) */
static MunitResult test_case_sensitivity(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    /* Test lowercase k */
    pid_t pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "perthread_cache=16k", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);
        if (umem_ptc_size != 16 * 1024) exit(1);
        exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    /* Test uppercase K */
    pid = fork();
    if (pid == 0) {
        setenv("UMEM_OPTIONS", "perthread_cache=16K", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);
        if (umem_ptc_size != 16 * 1024) exit(1);
        exit(0);
    }
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test: Size suffix overflow detection */
static MunitResult test_size_overflow(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    pid_t pid = fork();
    if (pid == 0) {
        /* Try to set a size that would overflow when multiplied */
        setenv("UMEM_OPTIONS", "perthread_cache=99999999999g", 1);
        void *p = umem_alloc(64, UMEM_DEFAULT);
        if (p == NULL) exit(1);
        umem_free(p, 64);

        /* Should still work, overflow value is rejected */
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    munit_assert_true(WIFEXITED(status));
    munit_assert_int(WEXITSTATUS(status), ==, 0);

    return MUNIT_OK;
}

/* Test array */
static MunitTest envvar_tests[] = {
    /* UMEM_DEBUG tests */
    { "/debug_audit", test_debug_audit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_default", test_debug_default, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_guards", test_debug_guards, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_contents", test_debug_contents, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_lite", test_debug_lite, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_multiple_flags", test_debug_multiple_flags, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_audit_with_arg", test_debug_audit_with_arg, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_contents_with_size", test_debug_contents_with_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_firewall", test_debug_firewall, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_maxverify", test_debug_maxverify, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_noabort", test_debug_noabort, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_verbose", test_debug_verbose, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_mtbf", test_debug_mtbf, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

    /* UMEM_OPTIONS tests */
    { "/options_concurrency", test_options_concurrency, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_max_contention", test_options_max_contention, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_nomagazines", test_options_nomagazines, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_reap_interval", test_options_reap_interval, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_perthread_cache", test_options_perthread_cache, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_size_suffixes", test_options_size_suffixes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#ifndef UMEM_STANDALONE
    { "/options_backend_mmap", test_options_backend_mmap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_backend_sbrk", test_options_backend_sbrk, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_allocator_best", test_options_allocator_best, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_allocator_first", test_options_allocator_first, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/options_allocator_next", test_options_allocator_next, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif

    /* UMEM_LOGGING tests */
    { "/logging_transaction", test_logging_transaction, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/logging_transaction_with_size", test_logging_transaction_with_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/logging_contents", test_logging_contents, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/logging_fail", test_logging_fail, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/logging_slab", test_logging_slab, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

    /* Error handling tests */
    { "/debug_invalid_value", test_debug_invalid_value, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/debug_invalid_number", test_debug_invalid_number, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/numeric_overflow", test_numeric_overflow, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/empty_option", test_empty_option, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/whitespace_handling", test_whitespace_handling, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple_options", test_multiple_options, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/case_sensitivity", test_case_sensitivity, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/size_overflow", test_size_overflow, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_envvar = {
    "/envvar",
    envvar_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

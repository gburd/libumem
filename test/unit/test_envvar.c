/*
 * Unit tests for envvar.c - Environment variable parsing
 *
 * envvar.c reads UMEM_* environment variables once at initialization time.
 * fork()+setenv() in the child is too late (umem is already initialized in
 * the parent), so each probe runs in a FRESH process via the exec helper
 * (test/unit/umem_env_helper, path in UMEM_ENV_HELPER).  The helper sets the
 * env var, forces init, then checks the resulting flag/global.
 */

#include "../munit.h"
#include "../../umem.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

#ifndef UMEM_ENV_HELPER
#error "UMEM_ENV_HELPER path must be defined by the build (see Makefile.am)"
#endif

static int test_with_env2(const char *var, const char *value,
                          const char *optname, const char *expected);

/*
 * Spawn the exec helper with `var=value` added to the inherited environment
 * (so LD_LIBRARY_PATH etc. survive), run `check [arg]` in that fresh process,
 * and return its exit status (WEXITSTATUS), or -1 on abnormal exit.
 */
static int test_with_env(const char *var, const char *value,
                         const char *check, const char *arg) {
    /* Build envp = environ + optional "var=value". */
    size_t n = 0;
    for (char **e = environ; *e; e++) n++;
    char **envp = calloc(n + 2, sizeof(char *));
    if (envp == NULL) return -1;

    char envbuf[512];
    size_t idx = 0;
    /* Copy environ, dropping any pre-existing setting of `var`. */
    size_t varlen = var ? strlen(var) : 0;
    for (char **e = environ; *e; e++) {
        if (var && strncmp(*e, var, varlen) == 0 && (*e)[varlen] == '=')
            continue;
        envp[idx++] = *e;
    }
    if (var != NULL && value != NULL) {
        snprintf(envbuf, sizeof envbuf, "%s=%s", var, value);
        envp[idx++] = envbuf;
    }
    envp[idx] = NULL;

    char *args[4];
    int a = 0;
    args[a++] = (char *)UMEM_ENV_HELPER;
    args[a++] = (char *)check;
    if (arg != NULL) args[a++] = (char *)arg;
    args[a] = NULL;

    pid_t pid;
    int rc = posix_spawn(&pid, UMEM_ENV_HELPER, NULL, NULL, args, envp);
    free(envp);
    if (rc != 0) return -1;

    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;  /* signalled / abnormal */
}

/* UMF_* flag masks (from umem_impl.h). */
#define M_AUDIT     "0x1"
#define M_DEADBEEF  "0x2"
#define M_REDZONE   "0x4"
#define M_CONTENTS  "0x8"
#define M_NOMAGAZINE "0x20"
#define M_FIREWALL  "0x40"
#define M_LITE      "0x100"

/* Test: UMEM_DEBUG=audit -> UMF_AUDIT */
static MunitResult test_debug_audit(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "audit", "flag", M_AUDIT), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=default -> audit|contents|deadbeef|redzone (0xf) */
static MunitResult test_debug_default(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "default", "flag", "0xf"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=guards -> deadbeef + redzone */
static MunitResult test_debug_guards(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "guards", "flag", M_DEADBEEF), ==, 0);
    munit_assert_int(test_with_env("UMEM_DEBUG", "guards", "flag", M_REDZONE), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=contents -> UMF_CONTENTS */
static MunitResult test_debug_contents(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "contents", "flag", M_CONTENTS), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=lite -> UMF_LITE */
static MunitResult test_debug_lite(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "lite", "flag", M_LITE), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=audit,guards -> audit|deadbeef|redzone (0x7) */
static MunitResult test_debug_multiple_flags(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "audit,guards", "flag", "0x7"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=audit=15 -> stack_depth == 15 (and audit set) */
static MunitResult test_debug_audit_with_arg(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "audit=15", "flag", M_AUDIT), ==, 0);
    munit_assert_int(test_with_env2("UMEM_DEBUG", "audit=15", "stack_depth", "15"), ==, 0);
    return MUNIT_OK;
}

/* Helper for opt checks (name expected). */
static int test_with_env2(const char *var, const char *value,
                          const char *optname, const char *expected) {
    /* posix_spawn helper with `opt <optname> <expected>`. */
    size_t n = 0;
    for (char **e = environ; *e; e++) n++;
    char **envp = calloc(n + 2, sizeof(char *));
    if (envp == NULL) return -1;
    char envbuf[512];
    size_t idx = 0, varlen = strlen(var);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, var, varlen) == 0 && (*e)[varlen] == '=') continue;
        envp[idx++] = *e;
    }
    snprintf(envbuf, sizeof envbuf, "%s=%s", var, value);
    envp[idx++] = envbuf;
    envp[idx] = NULL;
    char *args[] = { (char *)UMEM_ENV_HELPER, "opt",
                     (char *)optname, (char *)expected, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, UMEM_ENV_HELPER, NULL, NULL, args, envp);
    free(envp);
    if (rc != 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Test: UMEM_DEBUG=contents=256 -> content_maxsave == 256 */
static MunitResult test_debug_contents_with_size(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "contents=256", "flag", M_CONTENTS), ==, 0);
    munit_assert_int(test_with_env2("UMEM_DEBUG", "contents=256", "content_maxsave", "256"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=firewall=4096 -> firewall flag + minfirewall == 4096 */
static MunitResult test_debug_firewall(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "firewall=4096", "flag", M_FIREWALL), ==, 0);
    munit_assert_int(test_with_env2("UMEM_DEBUG", "firewall=4096", "minfirewall", "4096"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=maxverify=1024 -> maxverify == 1024 */
static MunitResult test_debug_maxverify(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_DEBUG", "maxverify=1024", "maxverify", "1024"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=noabort -> abort == 0 */
static MunitResult test_debug_noabort(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_DEBUG", "noabort", "abort", "0"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=verbose -> output >= 1 (we set to exactly 1) */
static MunitResult test_debug_verbose(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_DEBUG", "verbose", "output", "1"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_DEBUG=mtbf=100 -> mtbf == 100 */
static MunitResult test_debug_mtbf(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_DEBUG", "mtbf=100", "mtbf", "100"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=concurrency=8 -> max_ncpus == 8 */
static MunitResult test_options_concurrency(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "concurrency=8", "max_ncpus", "8"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=max_contention=50 -> depot_contention == 50 */
static MunitResult test_options_max_contention(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "max_contention=50", "depot_contention", "50"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=nomagazines -> UMF_NOMAGAZINE */
static MunitResult test_options_nomagazines(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_OPTIONS", "nomagazines", "flag", M_NOMAGAZINE), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=reap_interval=30 -> reap_interval == 30 */
static MunitResult test_options_reap_interval(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "reap_interval=30", "reap_interval", "30"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=perthread_cache=8192 -> ptc_size == 8192 */
static MunitResult test_options_perthread_cache(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "perthread_cache=8192", "ptc_size", "8192"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS size suffixes (k, m) -> ptc_size scaled */
static MunitResult test_options_size_suffixes(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "perthread_cache=8k", "ptc_size", "8192"), ==, 0);
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "perthread_cache=1m", "ptc_size", "1048576"), ==, 0);
    return MUNIT_OK;
}

#ifndef UMEM_STANDALONE
/* Test: UMEM_OPTIONS=backend=mmap -> VMEM_BACKEND_MMAP set */
static MunitResult test_options_backend_mmap(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "backend=mmap", "backend", "0x2"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=backend=sbrk -> VMEM_BACKEND_SBRK set */
static MunitResult test_options_backend_sbrk(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "backend=sbrk", "backend", "0x1"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=allocator=best -> VM_BESTFIT */
static MunitResult test_options_allocator_best(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "allocator=best", "allocator", "0x100"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=allocator=first -> VM_FIRSTFIT */
static MunitResult test_options_allocator_first(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "allocator=first", "allocator", "0x200"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_OPTIONS=allocator=next -> VM_NEXTFIT */
static MunitResult test_options_allocator_next(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "allocator=next", "allocator", "0x400"), ==, 0);
    return MUNIT_OK;
}
#endif /* !UMEM_STANDALONE */

/* Test: UMEM_LOGGING=transaction -> logging enabled + transaction_log_size > 0 */
static MunitResult test_logging_transaction(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_LOGGING", "transaction", "logging", "1"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=transaction=128k -> transaction_log_size == 128k */
static MunitResult test_logging_transaction_with_size(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_LOGGING", "transaction=128k", "transaction_log_size", "131072"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=contents -> logging enabled + UMF_CONTENTS */
static MunitResult test_logging_contents(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_LOGGING", "contents", "flag", M_CONTENTS), ==, 0);
    munit_assert_int(test_with_env2("UMEM_LOGGING", "contents", "logging", "1"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=fail -> logging enabled + failure_log_size > 0 */
static MunitResult test_logging_fail(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_LOGGING", "fail", "logging", "1"), ==, 0);
    return MUNIT_OK;
}

/* Test: UMEM_LOGGING=slab -> logging enabled */
static MunitResult test_logging_slab(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_LOGGING", "slab", "logging", "1"), ==, 0);
    return MUNIT_OK;
}

/* Test: invalid UMEM_DEBUG value is ignored (init still succeeds -> alive 0) */
static MunitResult test_debug_invalid_value(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "invalid_option", "alive", NULL), ==, 0);
    return MUNIT_OK;
}

/* Test: invalid numeric arg is ignored (init still succeeds) */
static MunitResult test_debug_invalid_number(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "audit=notanumber", "alive", NULL), ==, 0);
    return MUNIT_OK;
}

/* Test: numeric overflow value rejected (init still succeeds) */
static MunitResult test_numeric_overflow(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "audit=99999999999999999999", "alive", NULL), ==, 0);
    return MUNIT_OK;
}

/* Test: empty option value (init still succeeds) */
static MunitResult test_empty_option(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_DEBUG", "", "alive", NULL), ==, 0);
    return MUNIT_OK;
}

/* Test: whitespace around flags parsed correctly */
static MunitResult test_whitespace_handling(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    /* audit|deadbeef|redzone all set => mask 0x7 */
    munit_assert_int(test_with_env("UMEM_DEBUG", "  audit  ,  guards  ", "flag", "0x7"), ==, 0);
    return MUNIT_OK;
}

/* Test: multiple comma-separated options all applied */
static MunitResult test_multiple_options(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "concurrency=4,nomagazines,reap_interval=15", "max_ncpus", "4"), ==, 0);
    munit_assert_int(test_with_env("UMEM_OPTIONS", "concurrency=4,nomagazines,reap_interval=15", "flag", M_NOMAGAZINE), ==, 0);
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "concurrency=4,nomagazines,reap_interval=15", "reap_interval", "15"), ==, 0);
    return MUNIT_OK;
}

/* Test: size suffix case insensitivity (k vs K) */
static MunitResult test_case_sensitivity(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "perthread_cache=16k", "ptc_size", "16384"), ==, 0);
    munit_assert_int(test_with_env2("UMEM_OPTIONS", "perthread_cache=16K", "ptc_size", "16384"), ==, 0);
    return MUNIT_OK;
}

/* Test: size suffix overflow rejected (init still succeeds) */
static MunitResult test_size_overflow(const MunitParameter params[], void* data) {
    (void)params; (void)data;
    munit_assert_int(test_with_env("UMEM_OPTIONS", "perthread_cache=99999999999g", "alive", NULL), ==, 0);
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

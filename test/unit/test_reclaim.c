/*
 * Reclaim regressions (P1.4, P1.5) driven through exec helpers.
 *
 * The interesting cases need UMEM_DEBUG / UMEM_OPTIONS settings, which umem
 * reads once at init, so each one runs in a fresh process.  Helper exit
 * status: 0 pass, 1 fail, 2 usage, 77 precondition not met (reported as
 * MUNIT_SKIP, never as a pass).
 *
 * Pre-fix behaviour these cover:
 *   destroy       leaks the retained empty slab's backing span (P1.4)
 *   hash_guards   aborts with "boundary tag corrupted" on a valid alloc (P1.5a)
 *   big_quantum   second pass gets a truncated freelist (P1.5b)
 *   race          reclaim publishes slab_state unlocked (P1.5c)
 *   reap_reentry  reap inside an update pass self-deadlocks on
 *                 umem_cache_lock (found by the race test; pre-existing)
 */

#include "../munit.h"

#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef UMEM_RECLAIM_DESTROY_HELPER
#error "UMEM_RECLAIM_DESTROY_HELPER path must be defined by the build"
#endif
#ifndef UMEM_RECLAIM_REUSE_HELPER
#error "UMEM_RECLAIM_REUSE_HELPER path must be defined by the build"
#endif

extern char **environ;

/*
 * Spawn `helper [arg]` with the given UMEM_* variables added to the
 * inherited environment (so LD_LIBRARY_PATH survives), and return its exit
 * status, or -1 if it died abnormally (which is how a corruption abort
 * arrives).  vars/vals are NULL-terminated parallel arrays.
 */
static int
run_helper(const char *helper, const char *arg,
    const char *const *vars, const char *const *vals)
{
    size_t n = 0, nvars = 0, i;
    for (char **e = environ; *e; e++) n++;
    while (vars != NULL && vars[nvars] != NULL) nvars++;

    char **envp = calloc(n + nvars + 1, sizeof(char *));
    if (envp == NULL) return -1;
    char (*bufs)[512] = calloc(nvars ? nvars : 1, sizeof(*bufs));
    if (bufs == NULL) { free(envp); return -1; }

    size_t idx = 0;
    for (char **e = environ; *e; e++) {
        int shadowed = 0;
        for (i = 0; i < nvars; i++) {
            size_t vlen = strlen(vars[i]);
            if (strncmp(*e, vars[i], vlen) == 0 && (*e)[vlen] == '=') {
                shadowed = 1;
                break;
            }
        }
        if (!shadowed) envp[idx++] = *e;
    }
    for (i = 0; i < nvars; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "%s=%s", vars[i], vals[i]);
        envp[idx++] = bufs[i];
    }
    envp[idx] = NULL;

    char *args[3];
    int a = 0;
    args[a++] = (char *)helper;
    if (arg != NULL) args[a++] = (char *)arg;
    args[a] = NULL;

    pid_t pid;
    int rc = posix_spawn(&pid, helper, NULL, NULL, args, envp);
    free(envp);
    free(bufs);
    if (rc != 0) return -1;

    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;  /* signalled: abort() from the corruption check counts here */
}

/* Map a helper status onto a munit result, keeping SKIP distinct from PASS. */
static MunitResult
helper_result(int status)
{
    if (status == 77) return MUNIT_SKIP;
    munit_assert_int(status, ==, 0);
    return MUNIT_OK;
}

/*
 * P1.4: create UMC_NOMAGAZINE cache, alloc one, free it, destroy -- the
 * retained empty slab's backing must be released.  Default options; the
 * helper measures its own per-cache arena.
 */
static MunitResult
test_reclaim_destroy_releases_slabs(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    return helper_result(run_helper(UMEM_RECLAIM_DESTROY_HELPER, NULL,
        NULL, NULL));
}

/*
 * P1.5a: UMEM_DEBUG=guards, 4072-byte object -> UMF_HASH + UMF_BUFTAG,
 * one-page slab.  reclaim_delay=0 and reap_interval=1 so the slab reaches
 * SLAB_CLEAN in one update pass instead of 30 seconds.
 */
static MunitResult
test_reclaim_hash_guards_reuse(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    static const char *const vars[] = { "UMEM_DEBUG", "UMEM_OPTIONS", NULL };
    static const char *const vals[] = { "guards",
        "reclaim=1,reclaim_delay=0,reap_interval=1", NULL };
    return helper_result(run_helper(UMEM_RECLAIM_REUSE_HELPER, "hash_guards",
        vars, vals));
}

/* P1.5b: 16 KiB-quantum arena -> multi-page non-hash slab, reused. */
static MunitResult
test_reclaim_big_quantum_reuse(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    static const char *const vars[] = { "UMEM_OPTIONS", NULL };
    static const char *const vals[] = {
        "reclaim=1,reclaim_delay=0,reap_interval=1", NULL };
    return helper_result(run_helper(UMEM_RECLAIM_REUSE_HELPER, "big_quantum",
        vars, vals));
}

/*
 * P1.5c: reclaim concurrent with alloc/free.  Under --enable-tsan the
 * unlocked slab_state store is reported directly; otherwise this is a
 * consistency/liveness check backed by the ASSERTs in
 * umem_cache_reclaim_pages().
 */
static MunitResult
test_reclaim_race(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    static const char *const vars[] = { "UMEM_OPTIONS", NULL };
    static const char *const vals[] = {
        "reclaim=1,reclaim_delay=0,reap_interval=1", NULL };
    return helper_result(run_helper(UMEM_RECLAIM_REUSE_HELPER, "race",
        vars, vals));
}

/*
 * P1.5c control: reap-driven update passes under churn.  The inner
 * umem_reap() reached from vmem_xalloc inside a pass that holds
 * umem_cache_lock must decline instead of deadlocking.  Without the
 * IN_UPDATE() guard in umem_reap() the helper hangs and this test times out
 * with the process still alive.
 */
static MunitResult
test_reclaim_reap_reentry(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    static const char *const vars[] = { "UMEM_OPTIONS", NULL };
    static const char *const vals[] = {
        "reclaim=1,reclaim_delay=0,reap_interval=1", NULL };
    return helper_result(run_helper(UMEM_RECLAIM_REUSE_HELPER, "reap_reentry",
        vars, vals));
}

static MunitTest reclaim_tests[] = {
    { "/destroy_releases_slabs", test_reclaim_destroy_releases_slabs,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/hash_guards_reuse", test_reclaim_hash_guards_reuse,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/big_quantum_reuse", test_reclaim_big_quantum_reuse,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/race", test_reclaim_race,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/reap_reentry", test_reclaim_reap_reentry,
      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_reclaim = {
    "/reclaim",
    reclaim_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

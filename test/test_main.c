/*
 * Unified test runner for libumem test suite
 */

#include "munit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cache verification function from umem_audit.c */
extern int umem_verify_all_caches(void);

/* External test suites */
extern MunitSuite suite_umem_alloc;
extern MunitSuite suite_umem_advanced;
extern MunitSuite suite_umem_cache;
extern MunitSuite suite_umem_align;
extern MunitSuite suite_umem_debug;
extern MunitSuite suite_umem_audit;
extern MunitSuite suite_vmem;
extern MunitSuite suite_vmem_sbrk;
extern MunitSuite suite_umem_hooks;
extern MunitSuite error_path_suite;
extern MunitSuite rare_flags_suite;
extern MunitSuite boundary_conditions_suite;
extern MunitSuite suite_malloc;
extern MunitSuite suite_envvar;
extern MunitSuite suite_umem_fail;
extern MunitSuite suite_overflow_fixes;
extern MunitSuite suite_magazine_tune;
extern MunitSuite suite_cache_consistency;
extern MunitSuite suite_umem_stats;
extern MunitSuite suite_depot_consistency;
extern MunitSuite suite_sbo;
extern MunitSuite suite_arena;
extern MunitSuite suite_umem_own;
extern MunitSuite suite_gc;
extern MunitSuite suite_sparsemap;
extern MunitSuite suite_coverage;
extern MunitSuite suite_profile;

static MunitSuite* test_suites[] = {
    &suite_umem_alloc,
    &suite_umem_own,
    &suite_umem_advanced,
    &suite_umem_cache,
    &suite_umem_align,
    &suite_umem_audit,
    &suite_umem_hooks,
    &error_path_suite,
    &rare_flags_suite,
    &boundary_conditions_suite,
    &suite_malloc,
    &suite_envvar,
    &suite_umem_fail,
    &suite_overflow_fixes,
    &suite_magazine_tune,
    &suite_cache_consistency,
    &suite_umem_stats,
    &suite_depot_consistency,
    &suite_sbo,
    &suite_arena,
    &suite_gc,
    &suite_coverage,
    &suite_sparsemap,
    &suite_profile,
    &suite_vmem,
    /* vmem_sbrk: excluded — calls vmem_sbrk_arena() which conflicts
     * with the already-initialized umem sbrk arena in --no-fork mode.
     * Covered by make check (umem_test runs sbrk path separately). */
    &suite_umem_debug,
    NULL
};

static int skip_verify(void) {
    const char *env = getenv("UMEM_SKIP_VERIFY");
    return (env != NULL && strcmp(env, "1") == 0);
}

int main(int argc, char* argv[]) {
    int result = 0;
    int do_verify = !skip_verify();

    printf("libumem Test Suite\n");
    printf("==================\n");
    if (do_verify) {
        printf("  (cache verification enabled after each suite)\n");
    }
    printf("\n");

    for (int i = 0; test_suites[i] != NULL; i++) {
        MunitSuite* suite = test_suites[i];
        int suite_result = munit_suite_main(suite, NULL, argc, argv);
        if (suite_result != 0) {
            result = suite_result;
        }

        if (do_verify) {
            int errors = umem_verify_all_caches();
            if (errors != 0) {
                fprintf(stderr,
                    "FAIL: umem_verify_all_caches() found %d error(s) "
                    "after suite '%s'\n", errors, suite->prefix);
                result = 1;
            }
        }
    }

    if (result == 0) {
        printf("\n==================\n");
        printf("All tests passed!\n");
    } else {
        printf("\n==================\n");
        printf("Some tests failed\n");
    }

    return result;
}

/*
 * Unified test runner for libumem test suite
 */

#include "munit.h"
#include <stdio.h>
#include <stdlib.h>

/* External test suites */
extern MunitSuite suite_umem_alloc;
extern MunitSuite suite_umem_cache;
extern MunitSuite suite_umem_align;
extern MunitSuite suite_umem_debug;
extern MunitSuite suite_vmem;

static MunitSuite* test_suites[] = {
    &suite_umem_alloc,
    &suite_umem_cache,
    &suite_umem_align,
    &suite_umem_debug,
    &suite_vmem,
    NULL
};

int main(int argc, char* argv[]) {
    int result = 0;

    printf("libumem Test Suite\n");
    printf("==================\n\n");

    for (int i = 0; test_suites[i] != NULL; i++) {
        MunitSuite* suite = test_suites[i];
        int suite_result = munit_suite_main(suite, NULL, argc, argv);
        if (suite_result != 0) {
            result = suite_result;
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

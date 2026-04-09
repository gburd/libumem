/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * Benchmark for size class selection optimization.
 * Tests allocation performance for various sizes to establish baseline
 * and measure improvements from size class selection optimization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "umem.h"

#define ITERATIONS 10000000
#define WARMUP_ITERATIONS 1000000

static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Benchmark allocation/free for a specific size
 */
static void bench_size(size_t size, const char *label) {
    uint64_t start, end;
    double ns_per_op;
    void *ptrs[100];
    int i, j;

    /* Warmup */
    for (i = 0; i < WARMUP_ITERATIONS; i++) {
        void *p = umem_alloc(size, UMEM_DEFAULT);
        umem_free(p, size);
    }

    /* Actual benchmark - allocate and free in batches */
    start = get_nanos();
    for (i = 0; i < ITERATIONS; i += 100) {
        for (j = 0; j < 100; j++) {
            ptrs[j] = umem_alloc(size, UMEM_DEFAULT);
        }
        for (j = 0; j < 100; j++) {
            umem_free(ptrs[j], size);
        }
    }
    end = get_nanos();

    ns_per_op = (double)(end - start) / (ITERATIONS * 2.0);
    printf("%-20s %4zu bytes: %6.2f ns/op\n", label, size, ns_per_op);
}

/*
 * Benchmark mixed workload with various sizes
 */
static void bench_mixed_workload(void) {
    uint64_t start, end;
    double ns_per_op;
    void *ptrs[100];
    size_t sizes[] = {16, 32, 64, 128, 256};
    int i, j, size_idx;

    /* Warmup */
    for (i = 0; i < WARMUP_ITERATIONS; i++) {
        size_t size = sizes[i % 5];
        void *p = umem_alloc(size, UMEM_DEFAULT);
        umem_free(p, size);
    }

    /* Actual benchmark - mixed sizes */
    start = get_nanos();
    for (i = 0; i < ITERATIONS; i += 100) {
        for (j = 0; j < 100; j++) {
            size_idx = (i + j) % 5;
            ptrs[j] = umem_alloc(sizes[size_idx], UMEM_DEFAULT);
        }
        for (j = 0; j < 100; j++) {
            size_idx = (i + j) % 5;
            umem_free(ptrs[j], sizes[size_idx]);
        }
    }
    end = get_nanos();

    ns_per_op = (double)(end - start) / (ITERATIONS * 2.0);
    printf("%-20s           : %6.2f ns/op\n", "Mixed workload", ns_per_op);
}

int main(int argc, char **argv) {
    int verbose = 0;

    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
    }

    printf("Size Class Selection Benchmark\n");
    printf("==============================\n");
    printf("Iterations: %d per size\n\n", ITERATIONS);

    /* Test common sizes */
    bench_size(16, "Very small");
    bench_size(32, "Common small (32)");
    bench_size(64, "Common small (64)");
    bench_size(128, "Medium");
    bench_size(256, "Large");
    bench_size(512, "XL");
    bench_size(1024, "XXL");
    bench_size(2048, "3XL");
    bench_size(4096, "4XL");

    printf("\n");
    bench_mixed_workload();

    if (verbose) {
        printf("\nNotes:\n");
        printf("- Lower ns/op is better\n");
        printf("- Target: <10ns per operation for sizes <= 256 bytes\n");
        printf("- Expected improvement from optimization: 3-10%%\n");
    }

    return 0;
}

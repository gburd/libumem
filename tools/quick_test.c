/*
 * Quick test to verify malloc works and measure basic overhead
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main() {
    const int N = 100;
    void *ptrs[100];

    printf("Testing %d allocations of 64 bytes...\n", N);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc(64);
        if (!ptrs[i]) {
            fprintf(stderr, "malloc failed at iteration %d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < N; i++) {
        free(ptrs[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    long ns = (end.tv_sec - start.tv_sec) * 1000000000L + (end.tv_nsec - start.tv_nsec);
    double avg_ns = (double)ns / N;

    printf("Total time: %ld ns\n", ns);
    printf("Average:    %.1f ns per malloc+free pair\n", avg_ns);
    printf("✓ Test passed\n");

    return 0;
}

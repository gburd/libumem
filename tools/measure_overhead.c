/*
 * Microbenchmark to measure malloc/free overhead
 * Compile: gcc -O2 -o measure_overhead measure_overhead.c
 * Run: ./measure_overhead [iterations]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

#define RDTSC() ({ \
    uint32_t lo, hi; \
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); \
    ((uint64_t)hi << 32) | lo; \
})

/* Measure malloc + free latency */
void benchmark_malloc_free(size_t size, size_t iterations) {
    uint64_t *latencies = calloc(iterations, sizeof(uint64_t));
    void **ptrs = calloc(iterations, sizeof(void*));

    if (!latencies || !ptrs) {
        fprintf(stderr, "Failed to allocate test arrays\n");
        return;
    }

    /* Warmup */
    for (size_t i = 0; i < 1000; i++) {
        void *p = malloc(size);
        free(p);
    }

    /* Measure allocations */
    for (size_t i = 0; i < iterations; i++) {
        uint64_t start = RDTSC();
        ptrs[i] = malloc(size);
        uint64_t end = RDTSC();
        latencies[i] = end - start;
    }

    /* Measure frees */
    for (size_t i = 0; i < iterations; i++) {
        free(ptrs[i]);
    }

    /* Calculate statistics */
    uint64_t sum = 0;
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;

    for (size_t i = 0; i < iterations; i++) {
        uint64_t lat = latencies[i];
        sum += lat;
        if (lat < min) min = lat;
        if (lat > max) max = lat;
    }

    /* Sort for percentiles */
    for (size_t i = 0; i < iterations - 1; i++) {
        for (size_t j = i + 1; j < iterations; j++) {
            if (latencies[j] < latencies[i]) {
                uint64_t tmp = latencies[i];
                latencies[i] = latencies[j];
                latencies[j] = tmp;
            }
        }
    }

    double avg = (double)sum / iterations;
    uint64_t p50 = latencies[iterations / 2];
    uint64_t p95 = latencies[(iterations * 95) / 100];
    uint64_t p99 = latencies[(iterations * 99) / 100];

    printf("Size %zu bytes (%zu iterations):\n", size, iterations);
    printf("  Min:     %lu cycles\n", min);
    printf("  Avg:     %.1f cycles\n", avg);
    printf("  Median:  %lu cycles\n", p50);
    printf("  P95:     %lu cycles\n", p95);
    printf("  P99:     %lu cycles\n", p99);
    printf("  Max:     %lu cycles\n\n", max);

    free(latencies);
    free(ptrs);
}

/* Measure TLS access overhead */
__thread int tls_var __attribute__((tls_model("initial-exec")));

void benchmark_tls_access(size_t iterations) {
    uint64_t start = RDTSC();

    for (size_t i = 0; i < iterations; i++) {
        int val = tls_var;
        tls_var = val + 1;
        tls_var = val;
        (void)val;  /* Prevent optimization */
    }

    uint64_t end = RDTSC();
    double avg = (double)(end - start) / (iterations * 3);

    printf("TLS access overhead: %.2f cycles per access\n\n", avg);
}

int main(int argc, char **argv) {
    size_t iterations = 10000;

    if (argc > 1) {
        iterations = atoi(argv[1]);
    }

    printf("=== Malloc/Free Overhead Measurement ===\n\n");

    /* Measure TLS overhead first */
    benchmark_tls_access(100000);

    /* Test various allocation sizes */
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        benchmark_malloc_free(sizes[i], iterations);
    }

    return 0;
}

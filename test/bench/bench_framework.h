/*
 * Benchmarking framework for memory allocators
 * Uses tdigest for accurate latency percentile tracking
 */

#ifndef BENCH_FRAMEWORK_H
#define BENCH_FRAMEWORK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

/* Forward declaration of tdigest */
typedef struct td_histogram td_histogram_t;

/* Allocator function pointers for testing different implementations */
typedef struct allocator_ops {
    const char *name;
    void* (*alloc)(size_t);
    void* (*calloc)(size_t, size_t);
    void* (*realloc)(void*, size_t);
    void (*free)(void*);
    void (*cleanup)(void);  /* Optional cleanup between runs */
} allocator_ops_t;

/* Benchmark result statistics */
typedef struct bench_stats {
    const char *allocator_name;
    const char *workload_name;

    /* Throughput */
    uint64_t total_operations;
    double elapsed_seconds;
    double ops_per_second;

    /* Latency (nanoseconds) */
    double latency_p50;
    double latency_p90;
    double latency_p99;
    double latency_p999;
    double latency_min;
    double latency_max;
    double latency_mean;

    /* Memory */
    size_t peak_rss_bytes;
    size_t current_rss_bytes;
    size_t bytes_allocated;
    size_t bytes_freed;
    double fragmentation_ratio;  /* RSS / allocated */

    /* Thread count (for multithreaded tests) */
    int thread_count;
} bench_stats_t;

/* Workload function signature */
typedef void (*workload_fn)(allocator_ops_t *ops, bench_stats_t *stats, void *config);

/* Workload configuration */
typedef struct workload_config {
    const char *name;
    workload_fn fn;
    int thread_count;
    uint64_t operation_count;
    size_t min_size;
    size_t max_size;
    void *custom_data;
} workload_config_t;

/* Time measurement helpers */
static inline uint64_t bench_get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Memory measurement */
size_t bench_get_rss_bytes(void);

/* Run a single benchmark */
int bench_run(allocator_ops_t *ops, workload_config_t *workload, bench_stats_t *stats);

/* Print results in human-readable format */
void bench_print_stats(const bench_stats_t *stats);

/* Print results in CSV format for analysis */
void bench_print_csv_header(void);
void bench_print_csv_row(const bench_stats_t *stats);

/* Standard workloads */
void workload_single_thread(allocator_ops_t *ops, bench_stats_t *stats, void *config);
void workload_multi_thread(allocator_ops_t *ops, bench_stats_t *stats, void *config);
void workload_producer_consumer(allocator_ops_t *ops, bench_stats_t *stats, void *config);
void workload_fragmentation(allocator_ops_t *ops, bench_stats_t *stats, void *config);

/* Allocator implementations */
extern allocator_ops_t allocator_libc;
extern allocator_ops_t allocator_umem;
extern allocator_ops_t allocator_jemalloc;
extern allocator_ops_t allocator_tcmalloc;
extern allocator_ops_t allocator_mimalloc;

#endif /* BENCH_FRAMEWORK_H */

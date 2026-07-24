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

/* CPU usage from getrusage */
typedef struct bench_cpu_usage {
    double user_ms;
    double sys_ms;
} bench_cpu_usage_t;

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

    /* CPU overhead */
    bench_cpu_usage_t cpu_usage;

    /* Stability across repeated runs (set by bench_run_n).
     * ops_cov = stddev/mean of ops_per_second over the measured runs.
     * runs_measured = number of runs kept (warm-up runs excluded).
     * unstable = 1 if ops_cov > BENCH_UNSTABLE_COV (do not gate on it). */
    double ops_cov;
    int runs_measured;
    int unstable;
} bench_stats_t;

/* A run set with CoV above this fraction is flagged unstable. */
#define BENCH_UNSTABLE_COV 0.10

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

/* Read VmRSS from /proc/self/status (Linux only, returns bytes) */
size_t bench_get_vmrss_bytes(void);

/* CPU usage measurement */
bench_cpu_usage_t bench_get_cpu_usage(void);

/* Run a single benchmark (one run, no warm-up discard). */
int bench_run(allocator_ops_t *ops, workload_config_t *workload, bench_stats_t *stats);

/* Run a benchmark warmups+runs times: discard the warm-up runs, keep the
 * median (by ops_per_second) run's full stats, and fill ops_cov/runs_measured/
 * unstable across the kept runs. runs<=1 && warmups<=0 behaves like bench_run. */
int bench_run_n(allocator_ops_t *ops, workload_config_t *workload,
                bench_stats_t *stats, int warmups, int runs);

/* Print results in human-readable format */
void bench_print_stats(const bench_stats_t *stats);

/* Print results in CSV format for analysis */
void bench_print_csv_header(void);
void bench_print_csv_row(const bench_stats_t *stats);

/* Historical tracking: append result to TOML file */
int bench_append_history(const bench_stats_t *stats, const char *history_path);

/* Compare against historical results, return 1 if regression detected */
int bench_compare_history(const bench_stats_t *stats,
                          const char *history_path);

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

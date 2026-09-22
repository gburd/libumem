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

    /* Memory.
     *
     * peak_rss_bytes is the largest RSS observed DURING the run for the
     * workloads that sample it (fragmentation), not the RSS left over after
     * cleanup -- reading it after the final free reported a post-teardown
     * number and called it a peak.
     *
     * live_bytes_at_peak is the simultaneously-live allocated byte count at
     * the moment peak_rss_bytes was observed.  fragmentation_ratio is
     * peak_rss_bytes / live_bytes_at_peak, i.e. a ratio of two quantities
     * measured at the SAME instant.  It is only defined for workloads that
     * hold a live set; see has_fragmentation.
     *
     * has_fragmentation = 0 means this workload does not define a
     * fragmentation number and none is reported.  The allocate-and-
     * immediately-free workloads (single/multi/prodcons) never hold a
     * meaningful live set, and dividing RSS by CUMULATIVE allocation
     * traffic -- which is what they used to do -- yields a figure that
     * tends to zero the longer the run, so it is not reported at all. */
    size_t peak_rss_bytes;
    size_t current_rss_bytes;
    size_t bytes_allocated;       /* cumulative traffic, not live bytes */
    size_t bytes_freed;
    size_t live_bytes_at_peak;    /* live bytes when peak_rss was sampled */
    size_t peak_live_bytes;       /* max simultaneously-live bytes */
    double fragmentation_ratio;   /* peak RSS / live bytes at that instant */
    int has_fragmentation;        /* 0 => undefined for this workload */

    /* Thread count (for multithreaded tests).  This is the number of threads
     * that actually ran the workload, not the number requested: a 1-thread
     * workload asked for 192 threads must report 1. */
    int thread_count;

    /* CPU overhead */
    bench_cpu_usage_t cpu_usage;

        /* Stability across repeated runs (set by bench_run_n).
     * ops_cov = stddev/mean of ops_per_second over the measured runs.
     * runs_measured = number of runs kept (warm-up runs excluded).
     * unstable = 1 if ops_cov > BENCH_UNSTABLE_COV (do not gate on it).
     * ops_floor_raised = 1 if the requested per-thread budget was below
     * BENCH_MIN_OPS_PER_THREAD and was raised to it, so the reported total
     * is larger than the one asked for. */
    double ops_cov;
    int runs_measured;
    int unstable;
    int ops_floor_raised;
} bench_stats_t;

/* Workload function signature */
typedef void (*workload_fn)(allocator_ops_t *ops, bench_stats_t *stats, void *config);

/* A run set with CoV above this fraction is flagged unstable. */
#define BENCH_UNSTABLE_COV 0.10

/*
 * Minimum work per thread.  A point that runs for microseconds measures
 * scheduling noise, not the allocator: the 192-thread multi points published
 * before 2026-09-22 measured ~52k total operations in ~3.8ms with >27% CoV
 * because the operation budget was divided by the thread count twice (once in
 * matrix.sh, once again in bench_main.c).  bench_run refuses to run a point
 * below this floor and raises it instead, recording that it did so.
 */
#define BENCH_MIN_OPS_PER_THREAD 100000

/* Workload configuration.
 *
 * operation_count is TOTAL operations for the whole workload, across all
 * threads -- never per-thread.  Each workload divides by its own thread
 * count internally.  Callers must NOT pre-divide (that was the P2.1 double
 * division).  bench_main's -n option and matrix.sh's -n option are both
 * total-ops for the same reason. */
typedef struct workload_config {
    const char *name;
    workload_fn fn;
    int thread_count;
    uint64_t operation_count;   /* TOTAL across all threads */
    size_t min_size;
    size_t max_size;
    void *custom_data;
} workload_config_t;

/* Per-thread share of the total budget, never below the work floor.
 * Returns the per-thread count and, if raised is non-NULL, whether the floor
 * had to be applied (so the caller can report an honest total). */
static inline uint64_t bench_ops_per_thread(uint64_t total_ops, int nthreads,
                                            int *raised) {
    if (nthreads < 1) nthreads = 1;
    uint64_t per = total_ops / (uint64_t)nthreads;
    if (per < BENCH_MIN_OPS_PER_THREAD) {
        if (raised) *raised = 1;
        per = BENCH_MIN_OPS_PER_THREAD;
    } else if (raised) {
        *raised = 0;
    }
    return per;
}

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

/* Allocator implementations.  Every one but libc/umem is loaded via
 * dlopen(RTLD_LOCAL) in allocators.c's constructors; .alloc == NULL means
 * the library wasn't found at runtime (bench_main skips it, matrix.sh's
 * probe_alloc() detects it) -- there is no compile-time HAVE_* gate. */
extern allocator_ops_t allocator_libc;
extern allocator_ops_t allocator_umem;
extern allocator_ops_t allocator_jemalloc;
extern allocator_ops_t allocator_tcmalloc;
extern allocator_ops_t allocator_mimalloc;
extern allocator_ops_t allocator_snmalloc;
extern allocator_ops_t allocator_scudo;
extern allocator_ops_t allocator_rpmalloc;

#endif /* BENCH_FRAMEWORK_H */

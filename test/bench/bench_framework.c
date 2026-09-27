/*
 * Benchmarking framework implementation
 */

#include "bench_framework.h"
#include "../tdigest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <pthread.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/utsname.h>

/* Get RSS (Resident Set Size) in bytes */
size_t bench_get_rss_bytes(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __linux__
        return (size_t)usage.ru_maxrss * 1024;  /* Linux reports in KB */
#else
        return (size_t)usage.ru_maxrss;  /* BSD reports in bytes */
#endif
    }
    return 0;
}

#ifdef __linux__
/* Read one "Name:  <n> kB" field from /proc/self/status, in bytes. */
static size_t bench_read_status_field(const char *name) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t len = strlen(name);
    size_t val = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, name, len) == 0) {
            char *p = line + len;
            while (*p == ' ' || *p == '\t') p++;
            val = (size_t)strtoull(p, NULL, 10) * 1024;
            break;
        }
    }
    fclose(f);
    return val;
}
#endif

/* Read VmRSS from /proc/self/status (more accurate current RSS) */
size_t bench_get_vmrss_bytes(void) {
#ifdef __linux__
    return bench_read_status_field("VmRSS:");
#else
    return bench_get_rss_bytes();
#endif
}

/*
 * VmHWM: the process's RSS high-water mark.
 *
 * Reported alongside the fragmentation pair because RSS is near-monotonic
 * within a process: RSS at the live-set peak carries every earlier transient
 * that grew RSS and never returned it -- including RSS grown legitimately for
 * an earlier phase, and (in this multi-allocator harness) RSS from loading
 * other allocator libraries.  A reader comparing RSS-at-live-peak against
 * VmHWM can tell "the allocator holds 2x the live set" apart from "RSS was
 * already high before this phase".  A single ratio cannot distinguish those,
 * and this metric has already misled twice.
 *
 * ACCURACY LIMIT, measured not assumed: with CONFIG_SPLIT_RSS_COUNTING the
 * kernel batches per-thread RSS deltas and folds them into the mm-wide
 * counters only every 64 events or at task exit.  VmRSS and VmHWM are
 * therefore both approximate under many threads, and not mutually consistent
 * instant to instant -- on c7i.metal-48xl a VmRSS sample exceeded the
 * subsequently-read VmHWM by 0.9MB at 1 thread, rising monotonically to 63MB
 * at 192 threads.  Treat both as +/- tens of MB at high thread counts, and do
 * not build an assertion on VmHWM >= VmRSS.
 */
size_t bench_get_vmhwm_bytes(void) {
#ifdef __linux__
    return bench_read_status_field("VmHWM:");
#else
    return bench_get_rss_bytes();
#endif
}

/* Get CPU usage from getrusage */
bench_cpu_usage_t bench_get_cpu_usage(void) {
    bench_cpu_usage_t usage = {0, 0};
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        usage.user_ms = ru.ru_utime.tv_sec * 1000.0 +
                        ru.ru_utime.tv_usec / 1000.0;
        usage.sys_ms = ru.ru_stime.tv_sec * 1000.0 +
                       ru.ru_stime.tv_usec / 1000.0;
    }
    return usage;
}

/* Print statistics in human-readable format */
void bench_print_stats(const bench_stats_t *stats) {
    char rss_str[64], alloc_str[64];

    /* Convert bytes to human-readable format */
    if (stats->peak_rss_bytes < 1024) {
        snprintf(rss_str, sizeof(rss_str), "%zu B", stats->peak_rss_bytes);
    } else if (stats->peak_rss_bytes < 1024 * 1024) {
        snprintf(rss_str, sizeof(rss_str), "%.2f KB", stats->peak_rss_bytes / 1024.0);
    } else if (stats->peak_rss_bytes < 1024 * 1024 * 1024) {
        snprintf(rss_str, sizeof(rss_str), "%.2f MB", stats->peak_rss_bytes / (1024.0 * 1024.0));
    } else {
        snprintf(rss_str, sizeof(rss_str), "%.2f GB", stats->peak_rss_bytes / (1024.0 * 1024.0 * 1024.0));
    }

    if (stats->bytes_allocated < 1024) {
        snprintf(alloc_str, sizeof(alloc_str), "%zu B", stats->bytes_allocated);
    } else if (stats->bytes_allocated < 1024 * 1024) {
        snprintf(alloc_str, sizeof(alloc_str), "%.2f KB", stats->bytes_allocated / 1024.0);
    } else if (stats->bytes_allocated < 1024 * 1024 * 1024) {
        snprintf(alloc_str, sizeof(alloc_str), "%.2f MB", stats->bytes_allocated / (1024.0 * 1024.0));
    } else {
        snprintf(alloc_str, sizeof(alloc_str), "%.2f GB", stats->bytes_allocated / (1024.0 * 1024.0 * 1024.0));
    }

    printf("\n========================================\n");
    printf("Allocator: %s\n", stats->allocator_name);
    printf("Workload:  %s\n", stats->workload_name);
    printf("Threads:   %d\n", stats->thread_count);
    printf("========================================\n");
    printf("Throughput: %.2f ops/sec (%.2f s total)\n",
           stats->ops_per_second, stats->elapsed_seconds);
    printf("Operations: %lu total across %d thread%s\n",
           (unsigned long)stats->total_operations, stats->thread_count,
           stats->thread_count == 1 ? "" : "s");
    if (stats->ops_floor_raised) {
        printf("            [per-thread budget raised to the %d-op floor:"
               " the requested total was too small to measure]\n",
               BENCH_MIN_OPS_PER_THREAD);
    }
    if (stats->alloc_failures > 0) {
        printf("            [%llu ALLOCATION FAILURES: this run did less work"
               " than requested and is not comparable to one with none]\n",
               (unsigned long long)stats->alloc_failures);
    }
    if (stats->runs_measured > 1) {
        printf("Stability:  CoV %.2f%% over %d runs%s\n",
               stats->ops_cov * 100.0, stats->runs_measured,
               stats->unstable ? "  [UNSTABLE - do not gate]" : "");
    }
    printf("\nLatency (ns):\n");
    printf("  min:  %.0f\n", stats->latency_min);
    printf("  p50:  %.0f\n", stats->latency_p50);
    printf("  p90:  %.0f\n", stats->latency_p90);
    printf("  p99:  %.0f\n", stats->latency_p99);
    printf("  p999: %.0f\n", stats->latency_p999);
    printf("  max:  %.0f\n", stats->latency_max);
    printf("  mean: %.0f\n", stats->latency_mean);
    printf("\nMemory:\n");
    printf("  RSS:          %s%s\n", rss_str,
           stats->has_fragmentation ? " (peak during run)" : "");
    printf("  Allocated:    %s (cumulative traffic)\n", alloc_str);
    if (stats->has_fragmentation) {
        printf("  Live at peak: %.2f MB\n",
               stats->live_bytes_at_peak / (1024.0 * 1024.0));
        printf("  VmHWM:        %.2f MB (process RSS high-water mark)\n",
               stats->max_rss_bytes / (1024.0 * 1024.0));
        printf("  Live median:  %.2f MB over %zu samples\n",
               stats->frag_live_median / (1024.0 * 1024.0),
               stats->frag_samples);
        printf("  Fragmentation: %.2f at the live-set peak, %.2f median\n",
               stats->fragmentation_ratio, stats->frag_ratio_median);
        printf("    (RSS and live bytes sampled together; report the PAIR --\n"
               "     a lone ratio cannot separate allocator overhead from\n"
               "     RSS that was already high.)\n");
    } else {
        printf("  Fragmentation: n/a (this workload holds no live set; a"
               " ratio against cumulative traffic is meaningless)\n");
    }
    printf("\nCPU:\n");
    printf("  User: %.1f ms\n", stats->cpu_usage.user_ms);
    printf("  Sys:  %.1f ms\n", stats->cpu_usage.sys_ms);
    printf("========================================\n");
}

/* CSV output for analysis.
 *
 * Column names carry their unit so a consumer cannot mistake one for the
 * other (P2.1/P2.2):
 *   total_ops        operations completed, summed over ALL threads
 *   ops_per_thread   total_ops / threads, as actually run
 *   rss_at_live_peak RSS observed at the live-set peak
 *   vmhwm_bytes      process RSS high-water mark (VmHWM).  Compare against
 *                    rss_at_live_peak to tell real overhead from RSS that was
 *                    already high before this phase -- RSS is near-monotonic,
 *                    so the ratio's numerator carries history its denominator
 *                    does not.
 *   live_bytes_at_peak  live bytes at that same instant (the denominator)
 *   live_bytes_median   median of the sampled live series (it MOVES: threads
 *                    desynchronise at high counts)
 *   frag_median      median of the sampled rss/live series
 *   frag_samples     samples behind the two summaries; < 8 => frag columns are
 *                    left empty, because that is a single-sample observation
 *   frag             peak_rss_bytes / live_bytes_at_peak, or EMPTY when the
 *                    workload does not define one (see has_fragmentation)
 *   ops_floor_raised 1 if the per-thread budget was raised to the floor
 *   alloc_failures   allocations that returned NULL.  NONZERO MEANS THE RUN
 *                    COMPLETED LESS WORK THAN REQUESTED, so its throughput is
 *                    not comparable to a run with zero.  Never inferred from
 *                    total_ops alone.
 */
void bench_print_csv_header(void) {
    printf("allocator,workload,threads,total_ops,ops_per_thread,elapsed_sec,ops_per_sec,");
    printf("lat_min,lat_p50,lat_p90,lat_p99,lat_p999,lat_max,lat_mean,");
    printf("rss_at_live_peak,vmhwm_bytes,allocated_bytes,live_bytes_at_peak,");
    printf("live_bytes_median,frag,frag_median,frag_samples,");
    printf("cpu_user_ms,cpu_sys_ms,ops_cov,runs,unstable,ops_floor_raised,");
    printf("alloc_failures\n");
}

void bench_print_csv_row(const bench_stats_t *stats) {
    int nt = stats->thread_count > 0 ? stats->thread_count : 1;
    printf("%s,%s,%d,%lu,%lu,%.6f,%.2f,",
           stats->allocator_name, stats->workload_name, stats->thread_count,
           (unsigned long)stats->total_operations,
           (unsigned long)(stats->total_operations / (uint64_t)nt),
           stats->elapsed_seconds, stats->ops_per_second);
    printf("%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,",
           stats->latency_min, stats->latency_p50, stats->latency_p90,
           stats->latency_p99, stats->latency_p999, stats->latency_max, stats->latency_mean);
    printf("%zu,%zu,%zu,%zu,%zu,", stats->peak_rss_bytes, stats->max_rss_bytes,
           stats->bytes_allocated, stats->live_bytes_at_peak,
           stats->frag_live_median);
    /* Empty, not 0 and not 1.0: an undefined ratio must not look like a
     * measured one.  Both the live-peak ratio and the median of the ratio
     * series are emitted -- one number cannot show whether the pair is
     * "holds 2x the live set" or "RSS was already high". */
    if (stats->has_fragmentation)
        printf("%.4f,%.4f,%zu,", stats->fragmentation_ratio,
               stats->frag_ratio_median, stats->frag_samples);
    else
        printf(",,%zu,", stats->frag_samples);
    printf("%.1f,%.1f,%.4f,%d,%d,%d,%llu\n",
           stats->cpu_usage.user_ms, stats->cpu_usage.sys_ms,
           stats->ops_cov, stats->runs_measured, stats->unstable,
           stats->ops_floor_raised,
           (unsigned long long)stats->alloc_failures);
}

/* Thread context for multithreaded benchmarks */
typedef struct thread_context {
    int thread_id;
    allocator_ops_t *ops;
    workload_config_t *config;
    uint64_t ops_target;      /* this thread's share of the TOTAL budget */
    td_histogram_t *latency_hist;
    size_t bytes_allocated;
    size_t bytes_freed;
    uint64_t operations;
    uint64_t failures;                /* allocations that returned NULL */
    pthread_barrier_t *start_barrier;
} thread_context_t;

/*
 * Fill the start-barrier slots of worker threads that never started.
 *
 * The barrier is sized for nthreads workers + the driver.  When
 * pthread_create() fails partway, `missing` slots will never be filled, and
 * the old "absorb" loop had the DRIVER wait once per missing slot -- which
 * cannot work: a single thread waiting sequentially blocks on its first wait
 * with the barrier still short.  Observed 2026-09-23 (pthread_create failed
 * at frag thread 7 of 8 once umem's heap ceiling had used up the VMA budget
 * a thread stack needs): the run hung until killed.
 *
 * POSIX offers no way to shrink a barrier that may already have waiters, so
 * each missing slot is filled by a throwaway thread that does exactly one
 * wait.  Failure path only; if even those threads cannot be created the run
 * exits with a message rather than hanging.
 */
static void *barrier_filler(void *b) {
    pthread_barrier_wait((pthread_barrier_t *)b);
    return NULL;
}
static void bench_barrier_shrink(pthread_barrier_t *b, int expected, int have) {
    int missing = expected - have;
    if (missing <= 0) return;
    pthread_t *fill = calloc((size_t)missing, sizeof(*fill));
    int made = 0;
    for (int i = 0; fill && i < missing; i++) {
        if (pthread_create(&fill[i], NULL, barrier_filler, b) != 0) break;
        made++;
    }
    if (made < missing) {
        /* Cannot even create filler threads: nothing can release the
         * barrier.  Say so and exit rather than hang forever. */
        fprintf(stderr, "bench: %d worker thread(s) never started and %d "
                "filler thread(s) could not be created either; aborting the "
                "run rather than deadlocking on the start barrier\n",
                missing, missing - made);
        exit(2);
    }
    /* Fillers are joined after the barrier releases; detach is enough. */
    for (int i = 0; i < made; i++) pthread_detach(fill[i]);
    free(fill);
}

/* Single-threaded workload: allocate, use, free in loop.
 * operation_count is the total budget; this workload runs one thread, so the
 * per-thread count equals it (subject to the work floor). */
void workload_single_thread(allocator_ops_t *ops, bench_stats_t *stats, void *config) {
    workload_config_t *cfg = (workload_config_t *)config;
    td_histogram_t *hist;

    if (td_init(100.0, &hist) != 0) {
        fprintf(stderr, "Failed to initialize tdigest\n");
        return;
    }

    int raised = 0;
    uint64_t nops = bench_ops_per_thread(cfg->operation_count, 1, &raised);
    stats->ops_floor_raised = raised;
    stats->thread_count = 1;

    uint64_t start = bench_get_ns();
    size_t total_allocated = 0;
    uint64_t completed = 0;

    for (uint64_t i = 0; i < nops; i++) {
        size_t size = cfg->min_size;
        if (cfg->max_size > cfg->min_size) {
            size = cfg->min_size + (rand() % (cfg->max_size - cfg->min_size));
        }

        uint64_t alloc_start = bench_get_ns();
        void *ptr = ops->alloc(size);
        uint64_t alloc_end = bench_get_ns();

        if (!ptr) {
            stats->alloc_failures++;
        } else {
            /* Touch memory to ensure it's allocated */
            memset(ptr, 0x42, size);
            total_allocated += size;
            completed++;

            double latency = (double)(alloc_end - alloc_start);
            td_add(hist, latency, 1);

            ops->free(ptr);
        }
    }

    uint64_t end = bench_get_ns();

    /* Compute statistics.  total_operations is what actually completed, not
     * what was requested: an allocation that returned NULL is not an op. */
    stats->elapsed_seconds = (end - start) / 1e9;
    stats->total_operations = completed;
    stats->ops_per_second = (stats->elapsed_seconds > 0) ?
        (completed / stats->elapsed_seconds) : 0;
    stats->bytes_allocated = total_allocated;
    stats->bytes_freed = total_allocated;

    /* Latency percentiles */
    stats->latency_min = td_min(hist);
    stats->latency_max = td_max(hist);
    stats->latency_p50 = td_quantile(hist, 0.50);
    stats->latency_p90 = td_quantile(hist, 0.90);
    stats->latency_p99 = td_quantile(hist, 0.99);
    stats->latency_p999 = td_quantile(hist, 0.999);

    /* Mean from total/count */
    long long total_samples = td_size(hist);
    double sum = 0;
    td_compress(hist);
    int n = td_centroid_count(hist);
    for (int i = 0; i < n; i++) {
        sum += td_centroids_mean_at(hist, i) * td_centroids_weight_at(hist, i);
    }
    stats->latency_mean = (total_samples > 0) ? (sum / total_samples) : 0;

    /* Memory.  This workload frees every buffer immediately, so it holds no
     * live set and defines NO fragmentation ratio.  It used to report
     * peak_rss / CUMULATIVE bytes allocated, which falls towards zero the
     * longer the run and is not a fragmentation measure at all. */
    stats->peak_rss_bytes = bench_get_rss_bytes();
    stats->max_rss_bytes = stats->peak_rss_bytes;
    stats->current_rss_bytes = bench_get_vmrss_bytes();
    stats->live_bytes_at_peak = 0;
    stats->peak_live_bytes = 0;
    stats->fragmentation_ratio = 0.0;
    stats->has_fragmentation = 0;

    td_free(hist);
}

/* Multithreaded workload thread function */
static void* mt_worker_thread(void *arg) {
    thread_context_t *ctx = (thread_context_t *)arg;
    allocator_ops_t *ops = ctx->ops;
    workload_config_t *cfg = ctx->config;

    /* Wait for all threads to be ready */
    pthread_barrier_wait(ctx->start_barrier);

    /*
     * Per-thread PRNG seed. glibc rand() takes a PROCESS-GLOBAL internal
     * lock on every call; at high thread counts that lock -- not the
     * allocator -- dominates the multi workload (perf: 96% in
     * __lll_lock_*_private via rand()). Use rand_r() with a thread-local
     * seed so we measure the allocator, not glibc's rand lock.
     */
    unsigned int seed = 0x9e3779b9u ^ ((unsigned int)ctx->thread_id * 2654435761u);

    for (uint64_t i = 0; i < ctx->ops_target; i++) {
        size_t size = cfg->min_size;
        if (cfg->max_size > cfg->min_size) {
            size = cfg->min_size +
                   ((size_t)rand_r(&seed) % (cfg->max_size - cfg->min_size));
        }

        uint64_t alloc_start = bench_get_ns();
        void *ptr = ops->alloc(size);
        uint64_t alloc_end = bench_get_ns();

        if (!ptr) {
            ctx->failures++;
        } else {
            memset(ptr, 0x42 + ctx->thread_id, size);
            ctx->bytes_allocated += size;
            ctx->operations++;

            double latency = (double)(alloc_end - alloc_start);
            td_add(ctx->latency_hist, latency, 1);

            ops->free(ptr);
            ctx->bytes_freed += size;
        }
    }

    return NULL;
}

/* Multithreaded workload: all threads allocate/free concurrently.
 *
 * cfg->operation_count is the TOTAL budget across all threads; this function
 * divides it by the thread count, subject to BENCH_MIN_OPS_PER_THREAD.  It
 * is the ONLY place that division happens: bench_main.c used to divide as
 * well, on top of matrix.sh already dividing, so aggregate work shrank as
 * 1/threads^2 (P2.1). */
void workload_multi_thread(allocator_ops_t *ops, bench_stats_t *stats, void *config) {
    workload_config_t *cfg = (workload_config_t *)config;
    int nthreads = cfg->thread_count;
    if (nthreads < 1) nthreads = 1;

    int raised = 0;
    uint64_t per_thread = bench_ops_per_thread(cfg->operation_count, nthreads,
                                               &raised);
    stats->ops_floor_raised = raised;
    stats->thread_count = nthreads;

    pthread_t *threads = calloc(nthreads, sizeof(pthread_t));
    thread_context_t *contexts = calloc(nthreads, sizeof(thread_context_t));
    pthread_barrier_t start_barrier;

    if (!threads || !contexts) {
        fprintf(stderr, "bench: multi-thread driver allocation failed\n");
        free(threads);
        free(contexts);
        return;
    }

    pthread_barrier_init(&start_barrier, NULL, nthreads + 1);
    int started = 0;

    /* Initialize thread contexts */
    for (int i = 0; i < nthreads; i++) {
        contexts[i].thread_id = i;
        contexts[i].ops = ops;
        contexts[i].config = cfg;
        contexts[i].ops_target = per_thread;
        contexts[i].start_barrier = &start_barrier;
        contexts[i].bytes_allocated = 0;
        contexts[i].bytes_freed = 0;
        contexts[i].operations = 0;

        if (td_init(100.0, &contexts[i].latency_hist) != 0) {
            fprintf(stderr, "Failed to initialize tdigest for thread %d\n", i);
            break;
        }

        if (pthread_create(&threads[i], NULL, mt_worker_thread,
                           &contexts[i]) != 0) {
            /* A short run is not a smaller run: say so rather than silently
             * reporting fewer threads' worth of work as the requested point. */
            fprintf(stderr, "bench: pthread_create failed at thread %d of "
                    "%d\n", i, nthreads);
            break;
        }
        started++;
    }
    /* Threads that never started own barrier slots nobody will fill.  The
     * previous "absorb" loop had this thread wait once per missing slot --
     * but a barrier of N+1 parties cannot be satisfied by ONE thread waiting
     * sequentially: the first wait blocks forever with N parties arrived.
     * Observed 2026-09-23: pthread_create failed at frag thread 7 of 8 (the
     * umem heap ceiling had consumed the VMA budget the thread stack needed)
     * and the run hung until killed.  Since no started worker has passed the
     * barrier yet, it is safe to re-create it for the parties that exist. */
    bench_barrier_shrink(&start_barrier, nthreads + 1, started + 1);
    stats->thread_count = started > 0 ? started : nthreads;

    uint64_t start = bench_get_ns();
    pthread_barrier_wait(&start_barrier);  /* Start all threads */

    /* Wait for all threads */
    for (int i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t end = bench_get_ns();

    /* Aggregate results */
    td_histogram_t *combined_hist;
    if (td_init(100.0, &combined_hist) != 0) {
        fprintf(stderr, "Failed to create combined histogram\n");
        goto cleanup;
    }

    uint64_t total_ops = 0;
    size_t total_allocated = 0;
    size_t total_freed = 0;

    for (int i = 0; i < nthreads; i++) {
        td_merge(combined_hist, contexts[i].latency_hist);
        total_ops += contexts[i].operations;
        total_allocated += contexts[i].bytes_allocated;
        total_freed += contexts[i].bytes_freed;
        stats->alloc_failures += contexts[i].failures;
    }

    stats->elapsed_seconds = (end - start) / 1e9;
    stats->total_operations = total_ops;
    stats->ops_per_second = (stats->elapsed_seconds > 0) ?
        (total_ops / stats->elapsed_seconds) : 0;
    stats->bytes_allocated = total_allocated;
    stats->bytes_freed = total_freed;

    /* Latency percentiles */
    stats->latency_min = td_min(combined_hist);
    stats->latency_max = td_max(combined_hist);
    stats->latency_p50 = td_quantile(combined_hist, 0.50);
    stats->latency_p90 = td_quantile(combined_hist, 0.90);
    stats->latency_p99 = td_quantile(combined_hist, 0.99);
    stats->latency_p999 = td_quantile(combined_hist, 0.999);

    long long total_samples = td_size(combined_hist);
    double sum = 0;
    td_compress(combined_hist);
    int n = td_centroid_count(combined_hist);
    for (int i = 0; i < n; i++) {
        sum += td_centroids_mean_at(combined_hist, i) * td_centroids_weight_at(combined_hist, i);
    }
    stats->latency_mean = (total_samples > 0) ? (sum / total_samples) : 0;

    /* Memory.  Like single-thread, every buffer is freed immediately, so
     * there is no live set and NO fragmentation ratio is defined. */
    stats->peak_rss_bytes = bench_get_rss_bytes();
    stats->max_rss_bytes = stats->peak_rss_bytes;
    stats->current_rss_bytes = bench_get_vmrss_bytes();
    stats->live_bytes_at_peak = 0;
    stats->peak_live_bytes = 0;
    stats->fragmentation_ratio = 0.0;
    stats->has_fragmentation = 0;

    td_free(combined_hist);

cleanup:
    pthread_barrier_destroy(&start_barrier);
    for (int i = 0; i < nthreads; i++) {
        td_free(contexts[i].latency_hist);
    }
    free(threads);
    free(contexts);
}

/*
 * Interval mode (workload_config_t.intervals > 0): ONE time-bounded process,
 * per-interval stats.  Used by prodcons and frag.
 *
 * Ownership and ordering:
 *   - iv_clock_t.iv is written only by the driver (the thread that called the
 *     workload), monotonically 0 -> n; n means stop.  Workers read it
 *     relaxed: it carries no data to them, only "the interval changed".
 *   - Each worker owns its current-interval accumulators (digest, ops,
 *     failures, bytes).  On seeing iv > its own interval it moves them into
 *     its slot for that interval and publishes the digest pointer with a
 *     release store.  From then on the slot belongs to the driver, which
 *     reads it after an acquire load of that pointer, merges and frees it.
 *   - A worker publishes every interval exactly once, in order, including the
 *     last (when it sees iv == n) before it stops recording, so a driver
 *     waiting for slot k of every STARTED worker cannot miss one.  The
 *     digest pointer is non-NULL while a worker is still recording.
 *   - Workers poll between operations, never inside a timed alloc/free, so
 *     the poll costs every allocator the same and is not in any latency.
 *   - No harness allocation happens while intervals run.  Every digest a
 *     worker will ever record into (one per interval) and the driver's merge
 *     digests are allocated BEFORE the start barrier; the driver resets a
 *     merged digest instead of freeing it, and all are freed after the join.
 *     The first version td_free()'d each worker's digest in the driver as it
 *     merged it: at 384 threads on 8 vCPUs the driver then queued on 384
 *     busy glibc arena locks (gdb: iv_drive -> td_free -> _int_free_chunk ->
 *     __lll_lock_wait_private) and a 10 s libc frag interval took 40-67 s;
 *     it also put harness frees on the libc arm's own arenas, which is a
 *     cross-arm confound whatever the thread count.
 */
typedef struct {
    _Alignas(64) atomic_int iv;
    int n;
} iv_clock_t;

typedef struct {
    _Atomic(td_histogram_t *) hist; /* published digest (NULL until then) */
    td_histogram_t *spare;          /* preallocated: this worker records
                                     * interval k+1 into slot[k+1].spare */
    uint64_t ops;
    uint64_t failures;
    size_t bytes;
} iv_slot_t;

typedef struct {
    iv_clock_t *clock;      /* NULL => op-budget mode (interval code unused) */
    iv_slot_t *slots;       /* this worker's n slots */
    int mine;               /* interval this worker is accumulating into */
} iv_worker_t;

/* nworkers x n slots, each with its interval digest preallocated (slot 0's
 * spare is handed to the worker as its first digest by the caller).  Aborts
 * on failure: a harness that cannot build its own bookkeeping measures
 * nothing. */
static iv_slot_t *iv_slots_new(int nworkers, int n) {
    iv_slot_t *s = calloc((size_t)nworkers * (size_t)n, sizeof(*s));
    if (!s) abort();
    for (size_t i = 0; i < (size_t)nworkers * (size_t)n; i++)
        if (td_init(100.0, &s[i].spare) != 0) abort();
    return s;
}

/* After every worker has been joined: each digest is freed exactly once --
 * published ones were reset (not freed) by the driver, unpublished ones
 * (a worker that never started) are still only in .spare. */
static void iv_slots_free(iv_slot_t *s, int nworkers, int n) {
    if (!s) return;
    for (size_t i = 0; i < (size_t)nworkers * (size_t)n; i++)
        td_free(s[i].spare);
    free(s);
}

/* Returns 1 while recording, 0 once this worker has published its last
 * interval (the caller stops recording and winds down). */
static int iv_poll(iv_worker_t *w, td_histogram_t **hist, uint64_t *ops,
                   uint64_t *failures, size_t *bytes) {
    int now = atomic_load_explicit(&w->clock->iv, memory_order_relaxed);
    while (w->mine < now && w->mine < w->clock->n) {
        iv_slot_t *s = &w->slots[w->mine];
        td_histogram_t *h = *hist;
        s->ops = *ops; s->failures = *failures; s->bytes = *bytes;
        *ops = 0; *failures = 0; *bytes = 0;
        w->mine++;
        /* Next interval's digest was preallocated (see iv_clock_t). */
        *hist = w->mine < w->clock->n ? w->slots[w->mine].spare : NULL;
        atomic_store_explicit(&s->hist, h, memory_order_release);
    }
    return w->mine < w->clock->n;
}

struct frag_series;
static void frag_summarise(const struct frag_series *series,
                           struct bench_stats *stats);
/*
 * Frag live bytes, one slot per worker, one cache line per slot.
 *
 * Ownership: slot i is written ONLY by frag worker i (a relaxed store of its
 * private `held` after every alloc/free that changes it) and read by the
 * samplers (frag_sample, iv_sample), which sum all slots with one relaxed load
 * each.  There is no read-modify-write on any shared line in the per-op path.
 * The previous design did atomic_fetch_add/sub on ONE process-wide counter per
 * alloc and per free: at t=192 on c8g.metal-48xl, perf put 96.5 % of frag's
 * time in that one atomic (__aarch64_ldadd8_acq_rel) and < 1 % in the
 * allocator under test.
 *
 * The sum is not a snapshot: the slots are read one after another while
 * workers keep running, so it can be torn across workers by up to one batch
 * each.  That is acceptable for its only use, an RSS/live ratio sampled on a
 * timer or after a batch, which is already a noisy series summarised by its
 * peak and median (see struct frag_series); it is never torn WITHIN a worker.
 */
typedef struct {
    _Alignas(64) atomic_size_t v;
} frag_live_slot_t;

static size_t frag_live_sum(const frag_live_slot_t *s, int n) {
    size_t sum = 0;
    for (int i = 0; i < n; i++)
        sum += atomic_load_explicit(&s[i].v, memory_order_relaxed);
    return sum;
}

static void iv_drive(iv_clock_t *clk, const workload_config_t *cfg,
                     iv_slot_t *slots, const unsigned char *started,
                     int nworkers, frag_live_slot_t *live,
                     struct frag_series *series);

/*
 * Lock-free MPMC ring buffer for producer-consumer workload.
 * Uses CAS on head/tail for safe multi-producer/multi-consumer access.
 *
 * Topology: one ring per consumer, not one ring for everybody.  Producer i
 * and consumer j use ring (i % nrings) and (j % nrings), nrings = the number
 * of consumers (<= producers), so each ring has exactly one consumer and one
 * producer (two for the odd extra producer at odd thread counts).  Every free
 * is still cross-thread, but no ring line is shared by more than three
 * threads.  The previous single ring shared by all threads measured its own
 * head/tail CAS: at t=192 on c8g.metal-48xl perf put 79.6 % of prodcons's time
 * in __aarch64_cas8_relax (ring_pop, producer_thread), the allocator < 1 %.
 * RING_CAPACITY is per ring; the old single ring had 8192 for all threads.
 */
#define RING_CAPACITY 1024

typedef struct {
    _Alignas(64) atomic_uint_fast64_t seq;
    void *ptr;
    size_t size;
} ring_slot_t;

typedef struct {
    _Alignas(64) ring_slot_t slots[RING_CAPACITY];
    _Alignas(64) atomic_uint_fast64_t head;
    _Alignas(64) atomic_uint_fast64_t tail;
    atomic_int done;
} ring_buffer_t;

static void ring_init(ring_buffer_t *rb) {
    atomic_init(&rb->head, 0);
    atomic_init(&rb->tail, 0);
    atomic_init(&rb->done, 0);
    for (uint_fast64_t i = 0; i < RING_CAPACITY; i++) {
        atomic_init(&rb->slots[i].seq, i);
        rb->slots[i].ptr = NULL;
    }
}

static int ring_push(ring_buffer_t *rb, void *ptr, size_t sz) {
    uint_fast64_t pos;
    ring_slot_t *slot;
    for (;;) {
        pos = atomic_load_explicit(&rb->head, memory_order_relaxed);
        slot = &rb->slots[pos % RING_CAPACITY];
        uint_fast64_t seq = atomic_load_explicit(&slot->seq,
                                                  memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &rb->head, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return 0;  /* Full */
        }
    }
    slot->ptr = ptr;
    slot->size = sz;
    atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
    return 1;
}

static int ring_pop(ring_buffer_t *rb, void **ptr, size_t *sz) {
    uint_fast64_t pos;
    ring_slot_t *slot;
    for (;;) {
        pos = atomic_load_explicit(&rb->tail, memory_order_relaxed);
        slot = &rb->slots[pos % RING_CAPACITY];
        uint_fast64_t seq = atomic_load_explicit(&slot->seq,
                                                  memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &rb->tail, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return 0;  /* Empty */
        }
    }
    *ptr = slot->ptr;
    *sz = slot->size;
    atomic_store_explicit(&slot->seq, pos + RING_CAPACITY,
                          memory_order_release);
    return 1;
}

typedef struct {
    int thread_id;
    allocator_ops_t *ops;
    workload_config_t *config;
    uint64_t ops_target;      /* this producer's share of the TOTAL budget */
    ring_buffer_t *ring;      /* this thread's ring (see the topology note) */
    td_histogram_t *latency_hist;
    size_t bytes_allocated;
    uint64_t operations;
    uint64_t failures;                /* allocations that returned NULL */
    pthread_barrier_t *start_barrier;
    iv_worker_t iv;
} pc_context_t;

#define PC_POLL(ctx) iv_poll(&(ctx)->iv, &(ctx)->latency_hist, \
    &(ctx)->operations, &(ctx)->failures, &(ctx)->bytes_allocated)

static void *producer_thread(void *arg) {
    pc_context_t *ctx = (pc_context_t *)arg;
    allocator_ops_t *ops = ctx->ops;
    workload_config_t *cfg = ctx->config;
    unsigned int seed = (unsigned int)(ctx->thread_id + 1);

    pthread_barrier_wait(ctx->start_barrier);

    for (uint64_t i = 0; ctx->iv.clock || i < ctx->ops_target; i++) {
        if (ctx->iv.clock && !PC_POLL(ctx))
            break;
        size_t size = cfg->min_size;
        if (cfg->max_size > cfg->min_size) {
            size = cfg->min_size +
                   ((size_t)rand_r(&seed) % (cfg->max_size - cfg->min_size));
        }

        uint64_t t0 = bench_get_ns();
        void *ptr = ops->alloc(size);
        uint64_t t1 = bench_get_ns();

        if (!ptr) { ctx->failures++; continue; }
        memset(ptr, 0x42, size);

        td_add(ctx->latency_hist, (double)(t1 - t0), 1);
        ctx->bytes_allocated += size;
        ctx->operations++;

        while (!ring_push(ctx->ring, ptr, size)) {
            /* Spin briefly waiting for consumer to drain.  Keep publishing
             * intervals meanwhile so the driver never waits on a full ring. */
            if (ctx->iv.clock)
                (void)PC_POLL(ctx);
            sched_yield();
        }
    }
    return NULL;
}

static void *consumer_thread(void *arg) {
    pc_context_t *ctx = (pc_context_t *)arg;
    allocator_ops_t *ops = ctx->ops;

    pthread_barrier_wait(ctx->start_barrier);

    for (;;) {
        void *ptr;
        size_t sz;
        /* Interval mode: after the last interval, keep draining the ring
         * (untimed, uncounted) until the producers are done. */
        if (ctx->iv.clock && !PC_POLL(ctx)) {
            if (ring_pop(ctx->ring, &ptr, &sz))
                ops->free(ptr);
            else if (atomic_load(&ctx->ring->done))
                break;
            else
                sched_yield();
            continue;
        }
        if (ring_pop(ctx->ring, &ptr, &sz)) {
            uint64_t t0 = bench_get_ns();
            ops->free(ptr);
            uint64_t t1 = bench_get_ns();
            td_add(ctx->latency_hist, (double)(t1 - t0), 1);
            ctx->operations++;
        } else if (atomic_load(&ctx->ring->done)) {
            /* Drain remaining */
            while (ring_pop(ctx->ring, &ptr, &sz)) {
                ops->free(ptr);
                ctx->operations++;
            }
            break;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

/* Producer-consumer workload: N threads allocate, M threads free */
void workload_producer_consumer(allocator_ops_t *ops,
                                bench_stats_t *stats, void *config) {
    workload_config_t *cfg = (workload_config_t *)config;
    int nthreads = cfg->thread_count;
    int n_producers = (nthreads + 1) / 2;
    int n_consumers = nthreads - n_producers;
    if (n_consumers < 1) n_consumers = 1;
    int total = n_producers + n_consumers;

    int nrings = n_consumers;    /* one per consumer; see the topology note */
    ring_buffer_t *ring = aligned_alloc(_Alignof(ring_buffer_t),
                                        (size_t)nrings * sizeof(*ring));
    pthread_t *threads = calloc(total, sizeof(pthread_t));
    pc_context_t *contexts = calloc(total, sizeof(pc_context_t));
    pthread_barrier_t barrier;

    if (!ring || !threads || !contexts) {
        free(ring); free(threads); free(contexts);
        return;
    }

    for (int r = 0; r < nrings; r++)
        ring_init(&ring[r]);
    pthread_barrier_init(&barrier, NULL, total + 1);

    /* cfg->operation_count is the TOTAL allocation budget; split it across
     * the producers, subject to the per-thread work floor.  Consumers are
     * demand-driven (they free what producers enqueue), so the floor applies
     * to producers. */
    int raised = 0;
    uint64_t ops_per_producer =
        bench_ops_per_thread(cfg->operation_count, n_producers, &raised);
    stats->ops_floor_raised = raised;

    for (int i = 0; i < total; i++) {
        contexts[i].thread_id = i;
        contexts[i].ops = ops;
        contexts[i].config = cfg;
        contexts[i].ops_target = ops_per_producer;
        /* Producers are 0..n_producers-1, consumers follow. */
        contexts[i].ring = &ring[(i < n_producers ? i : i - n_producers)
                                 % nrings];
        contexts[i].start_barrier = &barrier;
        contexts[i].bytes_allocated = 0;
        contexts[i].operations = 0;
        if (td_init(100.0, &contexts[i].latency_hist) != 0) {
            fprintf(stderr, "bench: tdigest init failed for pc thread %d\n", i);
            goto cleanup;
        }
    }

    /* Interval mode: one clock, n slots per worker (see iv_clock_t). */
    iv_clock_t clk = { .n = cfg->intervals > 0 ? cfg->intervals : 0 };
    atomic_init(&clk.iv, 0);
    iv_slot_t *slots = NULL;
    unsigned char *pc_started = NULL;
    if (clk.n > 0) {
        slots = iv_slots_new(total, clk.n);
        pc_started = calloc((size_t)total, 1);
        if (!pc_started) abort();
        for (int i = 0; i < total; i++) {
            contexts[i].iv.clock = &clk;
            contexts[i].iv.slots = &slots[(size_t)i * (size_t)clk.n];
            contexts[i].iv.mine = 0;
            /* Record into the preallocated slot-0 digest (see iv_clock_t). */
            td_free(contexts[i].latency_hist);
            contexts[i].latency_hist = contexts[i].iv.slots[0].spare;
        }
    }

    int started_p = 0, started_c = 0;
    for (int i = 0; i < n_producers; i++) {
        if (pthread_create(&threads[i], NULL, producer_thread,
                           &contexts[i]) != 0) {
            fprintf(stderr, "bench: pthread_create failed at producer %d of "
                    "%d\n", i, n_producers);
            break;
        }
        if (pc_started) pc_started[i] = 1;
        started_p++;
    }
    for (int i = n_producers; i < total; i++) {
        if (pthread_create(&threads[i], NULL, consumer_thread,
                           &contexts[i]) != 0) {
            /* A ring without its consumer never drains, and its producers
             * would spin on it forever: measure nothing instead. */
            fprintf(stderr, "bench: pthread_create failed at consumer %d; "
                    "its ring would never drain\n", i - n_producers);
            abort();
        }
        if (pc_started) pc_started[i] = 1;
        started_c++;
    }
    /* See bench_barrier_shrink(): a sequential absorb loop deadlocks. */
    bench_barrier_shrink(&barrier, total + 1, started_p + started_c + 1);

    uint64_t start = bench_get_ns();
    pthread_barrier_wait(&barrier);

    if (clk.n > 0) {
        iv_drive(&clk, cfg, slots, pc_started, total, NULL, NULL);
        for (int i = 0; i < clk.n; i++)
            cfg->interval_out[i].allocator_name = ops->name;
    }

    /* Wait for producers */
    for (int i = 0; i < started_p; i++) {
        pthread_join(threads[i], NULL);
    }
    for (int r = 0; r < nrings; r++)
        atomic_store(&ring[r].done, 1);

    /* Wait for consumers */
    for (int i = n_producers; i < n_producers + started_c; i++) {
        pthread_join(threads[i], NULL);
    }
    uint64_t end = bench_get_ns();
    if (slots) {
        /* The workers' current digests are slot spares: freed below. */
        for (int i = 0; i < total; i++)
            contexts[i].latency_hist = NULL;
        iv_slots_free(slots, total, clk.n);
    }
    free(pc_started);

    /* Aggregate */
    td_histogram_t *combined;
    if (td_init(100.0, &combined) != 0) goto cleanup;

    uint64_t total_ops = 0;
    size_t total_alloc = 0;
    for (int i = 0; i < total; i++) {
        if (!contexts[i].latency_hist) continue;   /* interval mode: handed off */
        td_merge(combined, contexts[i].latency_hist);
        total_ops += contexts[i].operations;
        total_alloc += contexts[i].bytes_allocated;
        stats->alloc_failures += contexts[i].failures;
    }

    stats->elapsed_seconds = (end - start) / 1e9;
    stats->total_operations = total_ops;
    stats->ops_per_second = (stats->elapsed_seconds > 0) ?
        (total_ops / stats->elapsed_seconds) : 0;
    stats->bytes_allocated = total_alloc;
    stats->bytes_freed = total_alloc;
    stats->thread_count = started_p + started_c;

    stats->latency_min = td_min(combined);
    stats->latency_max = td_max(combined);
    stats->latency_p50 = td_quantile(combined, 0.50);
    stats->latency_p90 = td_quantile(combined, 0.90);
    stats->latency_p99 = td_quantile(combined, 0.99);
    stats->latency_p999 = td_quantile(combined, 0.999);

    long long total_samples = td_size(combined);
    double sum = 0;
    td_compress(combined);
    int n = td_centroid_count(combined);
    for (int i = 0; i < n; i++) {
        sum += td_centroids_mean_at(combined, i) *
               td_centroids_weight_at(combined, i);
    }
    stats->latency_mean = (total_samples > 0) ? (sum / total_samples) : 0;

    /* Memory.  Buffers cross a thread boundary but are freed as soon as a
     * consumer sees them; the live set is bounded by the ring, not by the
     * run's length, and is not measured here.  NO fragmentation ratio is
     * defined (it used to be RSS / cumulative traffic). */
    stats->peak_rss_bytes = bench_get_vmrss_bytes();
    stats->max_rss_bytes = stats->peak_rss_bytes;
    stats->current_rss_bytes = stats->peak_rss_bytes;
    stats->live_bytes_at_peak = 0;
    stats->peak_live_bytes = 0;
    stats->fragmentation_ratio = 0.0;
    stats->has_fragmentation = 0;

    td_free(combined);

cleanup:
    pthread_barrier_destroy(&barrier);
    for (int i = 0; i < total; i++) {
        if (contexts[i].latency_hist)
            td_free(contexts[i].latency_hist);
    }
    free(ring);
    free(threads);
    free(contexts);
}

/*
 * Fragmentation workload: build and churn a LIVE working set, and measure RSS
 * against the bytes that are actually live at that same instant.
 *
 * WHAT WAS WRONG BEFORE (P2.2)
 *   1. When the pool was full the code freed the new allocation immediately
 *      but still did `currently_held += sz`, so the "live bytes" denominator
 *      accumulated bytes that were not live.  peak_frag was a ratio against a
 *      denominator that grew without bound.
 *   2. peak_rss_bytes was read AFTER the final cleanup -- current RSS after
 *      teardown, not the RSS that corresponded to the peak ratio.
 *   3. The pool was capped at 4096 objects regardless of the requested work,
 *      so a "sustained" run never grew its working set; it just cycled the
 *      same small pool for longer.
 *   4. The workload ran on one thread while being reported and described as
 *      a 192-thread workload.
 *
 * WHAT IT DOES NOW
 *   Each thread owns a private live pool that GROWS with the requested work
 *   (pool capacity is derived from the per-thread budget), allocates in
 *   batches, frees a random ~50% of its own pool each round, and after every
 *   round samples (RSS, live bytes) TOGETHER.
 *
 * WHICH SAMPLE IS REPORTED, AND WHY NOT THE WORST RATIO
 *   The first version of this fix reported the sample with the highest
 *   rss/live RATIO.  That selection rule is itself biased and was caught by
 *   re-measurement: process RSS is near-monotonic (freeing rarely returns RSS
 *   to the OS), so maximising rss/live simply finds the sample where live
 *   bytes happened to dip LOWEST.  At high thread counts the threads
 *   desynchronise -- some in their free phase while others allocate -- so
 *   aggregate live bytes dip hard at some sample and the rule homes straight
 *   in on that dip.  Measured at 192 threads it produced a ratio of 505 while
 *   the implied RSS (ratio x live) stayed flat at ~1.1GB across every thread
 *   count: the ratio was tracking the denominator, not allocator overhead.
 *
 *   So the reported pair is the one at the LIVE-SET PEAK: the largest live
 *   byte count observed, and the RSS observed at that same instant.  That is a
 *   defined quantity -- "process RSS while the program was holding the most
 *   memory" -- and its denominator is the largest, not the luckiest, so it
 *   cannot be inflated by a sampling dip.  peak_rss_bytes is also reported
 *   separately as the maximum RSS seen at any sample, for reference.
 *
 *   Live bytes are summed across threads via an atomic, because RSS is a
 *   process-wide quantity: pairing process RSS with one thread's live bytes
 *   would be another mismatched ratio.
 *
 *   The requested thread count is honoured.  cfg->thread_count == 1 still
 *   runs single-threaded.
 */
struct frag_series;

typedef struct {
    int thread_id;
    allocator_ops_t *ops;
    workload_config_t *cfg;
    uint64_t ops_target;              /* per-thread share of the budget */
    size_t pool_cap;
    td_histogram_t *hist;
    size_t bytes_allocated;           /* cumulative traffic, this thread */
    uint64_t operations;
    uint64_t failures;                /* allocations that returned NULL */
    /* This worker's live-byte slot (written only here), all nlive slots
     * (read by frag_sample), and the shared sample series. */
    frag_live_slot_t *live;
    frag_live_slot_t *live_all;
    int nlive;
    pthread_mutex_t *peak_lock;
    struct frag_series *series;
    pthread_barrier_t *start_barrier;
    iv_worker_t iv;
} frag_context_t;

/*
 * The (RSS, live) sample series.
 *
 * Aggregate live bytes is a MOVING quantity, not a fixed one: at high thread
 * counts the threads desynchronise -- some in their free phase while others
 * allocate -- so any single sample is one draw from a noisy series.  An
 * earlier version of this code reported the single sample with the worst
 * rss/live ratio, which reliably found the deepest live-bytes dip and
 * reported a 505x "fragmentation" at 192 threads while the implied RSS stayed
 * flat at ~1.1GB across every thread count.
 *
 * So the series is retained and summarised: the pair at the live-set peak
 * (max live, and the RSS at that instant), plus the MEDIAN of the live series
 * and the median ratio, plus the count of samples.  A single sample never
 * becomes the headline again.
 */
#define FRAG_MAX_SAMPLES 4096
struct frag_series {
    size_t live[FRAG_MAX_SAMPLES];
    size_t rss[FRAG_MAX_SAMPLES];
    size_t n;
    size_t dropped;                   /* samples past the array's end */
};

/*
 * Sample (RSS, live bytes) as a pair.  Both are read as close together as
 * possible under the lock so the pair describes one instant.
 */
static void frag_sample(frag_context_t *ctx) {
    pthread_mutex_lock(ctx->peak_lock);
    /* Sum the slots inside the lock, next to the RSS read (the sum itself may
     * be torn across workers; see frag_live_slot_t). */
    size_t live = frag_live_sum(ctx->live_all, ctx->nlive);
    size_t rss = bench_get_vmrss_bytes();
    struct frag_series *sr = ctx->series;
    if (live > 0) {
        if (sr->n < FRAG_MAX_SAMPLES) {
            sr->live[sr->n] = live;
            sr->rss[sr->n] = rss;
            sr->n++;
        } else {
            sr->dropped++;
        }
    }
    pthread_mutex_unlock(ctx->peak_lock);
}

static int cmp_size(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void *frag_worker(void *arg) {
    frag_context_t *ctx = (frag_context_t *)arg;
    allocator_ops_t *ops = ctx->ops;
    workload_config_t *cfg = ctx->cfg;

    size_t min_sz = cfg->min_size < 8 ? 8 : cfg->min_size;
    /* No upper clamp: -s 16384:65536 must run 16-64 KB objects.  This used
     * to clamp to 4096, so every "large" frag cell silently ran <= 4 KB. */
    size_t max_sz = cfg->max_size;
    if (max_sz < min_sz) max_sz = min_sz;

    void **pool = calloc(ctx->pool_cap, sizeof(void *));
    size_t *pool_sz = calloc(ctx->pool_cap, sizeof(size_t));
    if (!pool || !pool_sz) {
        fprintf(stderr, "bench: frag pool allocation failed (cap %zu)\n",
                ctx->pool_cap);
        /* The driver's own allocation failed, so this thread does no work.
         * Counted as a failure so the run cannot report as clean. */
        ctx->failures++;
        free(pool); free(pool_sz);
        if (ctx->start_barrier) pthread_barrier_wait(ctx->start_barrier);
        /* Interval mode: the driver waits for every started worker's slot,
         * so publish (empty) intervals until the last one. */
        while (ctx->iv.clock && iv_poll(&ctx->iv, &ctx->hist, &ctx->operations,
                                        &ctx->failures, &ctx->bytes_allocated)) {
            struct timespec ts = { 0, 10000000L };
            nanosleep(&ts, NULL);
        }
        return NULL;
    }
    size_t pool_count = 0;
    size_t held = 0;                  /* this thread's live bytes */
    unsigned int seed = 42u ^ ((unsigned int)ctx->thread_id * 2654435761u);

    if (ctx->start_barrier) pthread_barrier_wait(ctx->start_barrier);

    uint64_t remaining = ctx->ops_target;
    /* Interval mode: run by time, polling the interval clock before every
     * operation (one relaxed load of a line written once per interval), so an
     * operation is counted in the interval it started in.  The driver samples
     * (RSS, live) on its timer instead of every worker after every batch (see
     * iv_drive). */
#define FRAG_POLL(ctx) iv_poll(&(ctx)->iv, &(ctx)->hist, &(ctx)->operations, \
    &(ctx)->failures, &(ctx)->bytes_allocated)
    while (ctx->iv.clock || remaining > 0) {
        /* Grow the live set: allocate a batch into free pool slots. */
        size_t room = ctx->pool_cap - pool_count;
        size_t batch = (ctx->iv.clock || remaining > room) ? room
                                                           : (size_t)remaining;
        if (batch == 0) batch = 1;    /* pool full: still do work, see below */

        for (size_t i = 0; i < batch; i++) {
            if (ctx->iv.clock && !FRAG_POLL(ctx))
                goto stop;
            size_t sz = min_sz +
                ((size_t)rand_r(&seed) % (max_sz - min_sz + 1));

            uint64_t t0 = bench_get_ns();
            void *ptr = ops->alloc(sz);
            uint64_t t1 = bench_get_ns();

            if (!ptr) {
                /* Not an operation, and not live bytes.  Counted, because a
                 * run that could not allocate did LESS WORK than asked and is
                 * therefore not comparable to one that did. */
                ctx->failures++;
                continue;
            }
            memset(ptr, 0xAB, sz);
            td_add(ctx->hist, (double)(t1 - t0), 1);
            ctx->operations++;
            ctx->bytes_allocated += sz;

            if (pool_count < ctx->pool_cap) {
                pool[pool_count] = ptr;
                pool_sz[pool_count] = sz;
                pool_count++;
                /* Live only when actually retained. */
                held += sz;
                atomic_store_explicit(&ctx->live->v, held,
                                      memory_order_relaxed);
            } else {
                /* Pool full: this buffer is freed immediately, so it is NOT
                 * live and must NOT enter the live-byte total.  Doing so was
                 * defect (1) above. */
                ops->free(ptr);
                ctx->operations++;
            }
        }
        remaining -= (remaining > batch) ? batch : remaining;

        /* Sample RSS and live bytes together, at the live-set maximum. */
        if (!ctx->iv.clock)
            frag_sample(ctx);

        /* Free a random ~50% of our own pool, creating the holes that make
         * this a fragmentation workload rather than a churn workload. */
        for (size_t i = 0; i < pool_count; ) {
            if (ctx->iv.clock && !FRAG_POLL(ctx))
                goto stop;
            if (rand_r(&seed) % 2 == 0) {
                uint64_t t0 = bench_get_ns();
                ops->free(pool[i]);
                uint64_t t1 = bench_get_ns();
                td_add(ctx->hist, (double)(t1 - t0), 1);
                ctx->operations++;

                held -= pool_sz[i];
                atomic_store_explicit(&ctx->live->v, held,
                                      memory_order_relaxed);
                pool[i] = pool[pool_count - 1];
                pool_sz[i] = pool_sz[pool_count - 1];
                pool_count--;
            } else {
                i++;
            }
        }
    }

stop:
    /* Release what is still live (untimed, uncounted). */
    for (size_t i = 0; i < pool_count; i++) {
        ops->free(pool[i]);
        held -= pool_sz[i];
    }
    atomic_store_explicit(&ctx->live->v, held, memory_order_relaxed);
    free(pool);
    free(pool_sz);
    return NULL;
}

void workload_fragmentation(allocator_ops_t *ops,
                            bench_stats_t *stats, void *config) {
    workload_config_t *cfg = (workload_config_t *)config;
    int nthreads = cfg->thread_count;
    if (nthreads < 1) nthreads = 1;

    int raised = 0;
    uint64_t per_thread = bench_ops_per_thread(cfg->operation_count, nthreads,
                                               &raised);
    stats->ops_floor_raised = raised;

    /*
     * The live pool grows with the work requested, so a longer run holds a
     * LARGER working set rather than cycling the same 4096 objects for
     * longer.  A quarter of the per-thread budget, clamped to a sane range:
     * below 1024 the ratio is dominated by the allocator's fixed overhead,
     * above 4M entries the bookkeeping itself dominates RSS.
     */
    size_t pool_cap = (size_t)(per_thread / 4);
    if (pool_cap < 1024) pool_cap = 1024;
    if (pool_cap > (size_t)4 << 20) pool_cap = (size_t)4 << 20;

    /* One cache line per worker (see frag_live_slot_t); zeroed, so a worker
     * that never starts contributes 0. */
    frag_live_slot_t *live = aligned_alloc(_Alignof(frag_live_slot_t),
                                           (size_t)nthreads * sizeof(*live));
    if (!live) {
        fprintf(stderr, "bench: frag live slots allocation failed\n");
        return;
    }
    for (int i = 0; i < nthreads; i++)
        atomic_init(&live[i].v, (size_t)0);
    pthread_mutex_t peak_lock = PTHREAD_MUTEX_INITIALIZER;
    struct frag_series *series = calloc(1, sizeof(*series));
    if (!series) {
        fprintf(stderr, "bench: frag sample series allocation failed\n");
        free(live);
        return;
    }

    frag_context_t *ctxs = calloc((size_t)nthreads, sizeof(*ctxs));
    pthread_t *threads = calloc((size_t)nthreads, sizeof(*threads));
    if (!ctxs || !threads) {
        fprintf(stderr, "bench: frag driver allocation failed\n");
        free(ctxs); free(threads); free(series); free(live);
        return;
    }

    pthread_barrier_t start_barrier;
    pthread_barrier_init(&start_barrier, NULL, (unsigned)nthreads + 1);

    /* Interval mode: one clock, n slots per worker (see iv_clock_t). */
    iv_clock_t iv_clk = { .n = cfg->intervals > 0 ? cfg->intervals : 0 };
    atomic_init(&iv_clk.iv, 0);
    iv_slot_t *iv_slots = NULL;
    if (iv_clk.n > 0) {
        iv_slots = iv_slots_new(nthreads, iv_clk.n);
    }

    for (int i = 0; i < nthreads; i++) {
        ctxs[i].thread_id = i;
        ctxs[i].ops = ops;
        ctxs[i].cfg = cfg;
        if (iv_clk.n > 0) {
            ctxs[i].iv.clock = &iv_clk;
            ctxs[i].iv.slots = &iv_slots[(size_t)i * (size_t)iv_clk.n];
        }
        ctxs[i].ops_target = per_thread;
        ctxs[i].pool_cap = pool_cap;
        ctxs[i].live = &live[i];
        ctxs[i].live_all = live;
        ctxs[i].nlive = nthreads;
        ctxs[i].peak_lock = &peak_lock;
        ctxs[i].series = series;
        ctxs[i].start_barrier = &start_barrier;
        if (iv_clk.n > 0) {
            /* Record into the preallocated slot-0 digest (see iv_clock_t). */
            ctxs[i].hist = ctxs[i].iv.slots[0].spare;
        } else if (td_init(100.0, &ctxs[i].hist) != 0) {
            fprintf(stderr, "bench: frag tdigest init failed (thread %d)\n", i);
            ctxs[i].hist = NULL;
            break;
        }
    }

    int started = 0;
    for (int i = 0; i < nthreads; i++) {
        if (ctxs[i].hist == NULL) break;
        if (pthread_create(&threads[i], NULL, frag_worker, &ctxs[i]) != 0) {
            fprintf(stderr, "bench: pthread_create failed at frag thread %d "
                    "of %d\n", i, nthreads);
            break;
        }
        started++;
    }
    /* See bench_barrier_shrink(): a sequential absorb loop deadlocks. */
    bench_barrier_shrink(&start_barrier, nthreads + 1, started + 1);

    uint64_t start = bench_get_ns();
    pthread_barrier_wait(&start_barrier);
    if (iv_clk.n > 0) {
        /* Workers 0..started-1 are exactly the started ones. */
        unsigned char *st = calloc((size_t)nthreads, 1);
        if (!st) abort();
        memset(st, 1, (size_t)started);
        iv_drive(&iv_clk, cfg, iv_slots, st, nthreads, live, series);
        free(st);
        for (int i = 0; i < iv_clk.n; i++)
            cfg->interval_out[i].allocator_name = ops->name;
    }
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    uint64_t end = bench_get_ns();
    if (iv_slots) {
        /* The workers' current digests are slot spares: freed below. */
        for (int i = 0; i < nthreads; i++)
            ctxs[i].hist = NULL;
        iv_slots_free(iv_slots, nthreads, iv_clk.n);
    }

    td_histogram_t *combined = NULL;
    uint64_t total_ops = 0;
    size_t total_allocated = 0;
    uint64_t total_failures = 0;
    if (td_init(100.0, &combined) == 0) {
        for (int i = 0; i < nthreads; i++) {
            if (ctxs[i].hist == NULL) continue;
            td_merge(combined, ctxs[i].hist);
            total_ops += ctxs[i].operations;
            total_allocated += ctxs[i].bytes_allocated;
            total_failures += ctxs[i].failures;
        }
    }
    stats->alloc_failures = total_failures;

    if (total_failures > 0) {
        fprintf(stderr, "bench: WARNING -- %llu allocation(s) FAILED during "
                "the fragmentation workload.  This run completed less work "
                "than requested; its throughput is not comparable to a run "
                "that did not, and the ratio is not a measurement of a "
                "healthy allocator.\n",
                (unsigned long long)total_failures);
    }

    /* Report the number of threads that actually ran, never the number
     * requested: this workload used to run one thread and be reported (and
     * documented) as 192-thread. */
    stats->thread_count = started > 0 ? started : 0;
    stats->elapsed_seconds = (end - start) / 1e9;
    stats->total_operations = total_ops;
    stats->ops_per_second = (stats->elapsed_seconds > 0) ?
        (total_ops / stats->elapsed_seconds) : 0;
    stats->bytes_allocated = total_allocated;
    stats->bytes_freed = total_allocated;

    if (combined != NULL) {
        stats->latency_min = td_min(combined);
        stats->latency_max = td_max(combined);
        stats->latency_p50 = td_quantile(combined, 0.50);
        stats->latency_p90 = td_quantile(combined, 0.90);
        stats->latency_p99 = td_quantile(combined, 0.99);
        stats->latency_p999 = td_quantile(combined, 0.999);

        long long total_samples = td_size(combined);
        double sum = 0;
        td_compress(combined);
        int n = td_centroid_count(combined);
        for (int i = 0; i < n; i++) {
            sum += td_centroids_mean_at(combined, i) *
                   td_centroids_weight_at(combined, i);
        }
        stats->latency_mean = (total_samples > 0) ? (sum / total_samples) : 0;
        td_free(combined);
    }

    /*
     * Summarise the (RSS, live) series.  Report the PAIR, not just a quotient:
     * a single ratio cannot distinguish "the allocator holds 2x the live set"
     * from "RSS was already high from an earlier phase", because RSS is
     * near-monotonic and its numerator carries history the denominator does
     * not.  peak_rss_bytes/live_bytes_at_peak give the pair at the live-set
     * peak; frag_live_median/frag_ratio_median summarise the series so one
     * noisy sample cannot become a headline; max_rss_bytes is VmHWM, the
     * process's own high-water mark, as the outer bound on the numerator.
     */
    frag_summarise(series, stats);
    free(series);
    free(live);

    for (int i = 0; i < nthreads; i++)
        if (ctxs[i].hist != NULL) td_free(ctxs[i].hist);
    pthread_barrier_destroy(&start_barrier);
    free(ctxs);
    free(threads);
}

/* The frag (RSS, live) series -> the stats' fragmentation fields.  Shared by
 * the op-budget run and by every interval of an interval-mode run. */
static void frag_summarise(const struct frag_series *series,
                           bench_stats_t *stats) {
    size_t peak_live = 0, rss_at_peak_live = 0;
    double ratio_median = 0.0;
    size_t live_median = 0;
    if (series->n > 0) {
        for (size_t i = 0; i < series->n; i++) {
            if (series->live[i] > peak_live) {
                peak_live = series->live[i];
                rss_at_peak_live = series->rss[i];
            }
        }
        size_t *lv = calloc(series->n, sizeof(*lv));
        double *rt = calloc(series->n, sizeof(*rt));
        if (lv && rt) {
            for (size_t i = 0; i < series->n; i++) {
                lv[i] = series->live[i];
                rt[i] = (double)series->rss[i] / (double)series->live[i];
            }
            qsort(lv, series->n, sizeof(*lv), cmp_size);
            live_median = lv[series->n / 2];
            /* median of the ratio series, sorted independently */
            for (size_t i = 0; i + 1 < series->n; i++)
                for (size_t j = 0; j + 1 < series->n - i; j++)
                    if (rt[j] > rt[j + 1]) {
                        double t = rt[j]; rt[j] = rt[j + 1]; rt[j + 1] = t;
                    }
            ratio_median = rt[series->n / 2];
        }
        free(lv); free(rt);
    }

    stats->peak_rss_bytes = rss_at_peak_live;
    stats->current_rss_bytes = bench_get_vmrss_bytes();
    stats->live_bytes_at_peak = peak_live;
    stats->peak_live_bytes = peak_live;
    stats->max_rss_bytes = bench_get_vmhwm_bytes();   /* VmHWM */
    stats->frag_live_median = live_median;
    stats->frag_ratio_median = ratio_median;
    stats->frag_samples = series->n;
    stats->fragmentation_ratio = (peak_live > 0) ?
        ((double)rss_at_peak_live / (double)peak_live) : 0.0;
    /* Only defined if we actually sampled a live set.  A handful of samples is
     * a single-sample observation dressed up as a series: say so by refusing
     * to call it defined below 8 samples. */
    stats->has_fragmentation =
        (peak_live > 0 && rss_at_peak_live > 0 && series->n >= 8) ? 1 : 0;
    if (series->dropped > 0) {
        fprintf(stderr, "bench: frag sample series full; %zu samples dropped "
                "(summary covers the first %zu)\n", series->dropped, series->n);
    }
}

/*
 * Interval-mode driver (see iv_clock_t for the ownership rules).  Runs on the
 * thread that released the start barrier.  It keeps the clock -- closing
 * interval k exactly at its deadline, fixing its elapsed time, CPU and memory
 * there, and advancing iv -- independently of collection: each worker's
 * published slot for k is merged whenever it appears, and k's row is emitted
 * (interval_done) once every started worker has published it.  Between
 * events it sleeps in <= 100 ms ticks and samples VmRSS (and, for frag, the
 * aggregate live bytes with it) into the open interval.
 *
 * Frag's (RSS, live) pairs are taken on this timer, not by the workers after
 * every batch as in op-budget mode: at 192 threads the per-batch sampler
 * overflowed FRAG_MAX_SAMPLES within seconds and serialised every worker on
 * peak_lock.  prodcons reports the largest VmRSS seen in the interval as
 * rss_at_live_peak (op-budget mode reads it once, after the run), and both
 * report it in the vmhwm column: the process VmHWM would include intervals the
 * caller discards (the warm-up), and every column of a kept row must come
 * from that interval alone.
 */
/* One (RSS, live) sample on the driver's tick, into the open interval. */
static void iv_sample(size_t *rss_max, const frag_live_slot_t *live,
                      int nlive, struct frag_series *series) {
    size_t rss = bench_get_vmrss_bytes();
    if (rss > *rss_max) *rss_max = rss;
    /* One relaxed load per worker; torn across workers (frag_live_slot_t). */
    size_t lv = series ? frag_live_sum(live, nlive) : 0;
    if (lv == 0) return;
    if (series->n < FRAG_MAX_SAMPLES) {
        series->live[series->n] = lv;
        series->rss[series->n] = rss;
        series->n++;
    } else {
        series->dropped++;
    }
}

/* Interval k's slots are all merged: derive its latency and rate, report it. */
static void iv_finish(const workload_config_t *cfg, int k,
                      td_histogram_t *comb, int nstarted) {
    bench_stats_t *st = &cfg->interval_out[k];
    st->workload_name = cfg->name;
    st->thread_count = nstarted;
    st->runs_measured = 1;
    st->ops_per_second = st->elapsed_seconds > 0 ?
        (double)st->total_operations / st->elapsed_seconds : 0;
    st->bytes_freed = st->bytes_allocated;
    if (td_size(comb) > 0) {
        st->latency_min = td_min(comb);
        st->latency_max = td_max(comb);
        st->latency_p50 = td_quantile(comb, 0.50);
        st->latency_p90 = td_quantile(comb, 0.90);
        st->latency_p99 = td_quantile(comb, 0.99);
        st->latency_p999 = td_quantile(comb, 0.999);
        double sum = 0;
        td_compress(comb);
        for (int i = 0; i < td_centroid_count(comb); i++)
            sum += td_centroids_mean_at(comb, i) *
                   td_centroids_weight_at(comb, i);
        st->latency_mean = sum / (double)td_size(comb);
    }
    if (cfg->interval_done)
        cfg->interval_done(st, k);
}

static void iv_drive(iv_clock_t *clk, const workload_config_t *cfg,
                     iv_slot_t *slots, const unsigned char *started,
                     int nworkers, frag_live_slot_t *live,
                     struct frag_series *series) {
    const uint64_t period = (uint64_t)(cfg->interval_sec * 1e9);
    const uint64_t tick = 100000000ull;
    const int n = clk->n;
    int nstarted = 0;
    for (int w = 0; w < nworkers; w++)
        nstarted += started[w] ? 1 : 0;

    /* cur: the interval being timed and sampled (n once all are closed).
     * col: the oldest closed interval not yet fully merged; cursor: the next
     * worker to merge for it.  col <= cur always; rows complete in order. */
    int cur = 0, col = 0, cursor = 0;
    td_histogram_t *comb = NULL;
    if (td_init(100.0, &comb) != 0) abort();   /* reused; see iv_clock_t */
    size_t rss_max = 0;
    if (series) { series->n = 0; series->dropped = 0; }
    uint64_t t_start = bench_get_ns(), t_end = t_start + period;
    bench_cpu_usage_t cpu0 = bench_get_cpu_usage();

    while (col < n) {
        uint64_t now = bench_get_ns();
        if (cur < n && now >= t_end) {
            /* Close interval cur.  Its time, CPU and memory are fixed here;
             * its work and latency arrive as each worker publishes. */
            bench_stats_t *st = &cfg->interval_out[cur];
            memset(st, 0, sizeof(*st));
            st->elapsed_seconds = (double)(now - t_start) / 1e9;
            bench_cpu_usage_t cpu1 = bench_get_cpu_usage();
            st->cpu_usage.user_ms = cpu1.user_ms - cpu0.user_ms;
            st->cpu_usage.sys_ms = cpu1.sys_ms - cpu0.sys_ms;
            cpu0 = cpu1;
            if (series)
                frag_summarise(series, st);
            else
                st->peak_rss_bytes = rss_max;
            st->max_rss_bytes = rss_max;
            st->current_rss_bytes = bench_get_vmrss_bytes();
            atomic_store_explicit(&clk->iv, cur + 1, memory_order_relaxed);
            cur++;
            t_start = now;
            t_end = now + period;
            rss_max = 0;
            if (series) { series->n = 0; series->dropped = 0; }
        }
        /* Merge what is published, never waiting: a worker stalled inside one
         * operation delays only its interval's ROW, not the next interval's
         * clock or samples.  (Observed: 384 threads on 8 vCPUs, one umem frag
         * operation took 12.7 s; a blocking collect lost that interval's
         * samples and stretched it to 11.7 s of a 10 s target.) */
        while (col < cur) {
            bench_stats_t *st = &cfg->interval_out[col];
            for (; cursor < nworkers; cursor++) {
                if (!started[cursor]) continue;
                iv_slot_t *s = &slots[(size_t)cursor * (size_t)n + (size_t)col];
                td_histogram_t *h =
                    atomic_load_explicit(&s->hist, memory_order_acquire);
                if (h == NULL) break;
                td_merge(comb, h);
                td_reset(h);   /* freed with its slot after the join */
                st->total_operations += s->ops;
                st->alloc_failures += s->failures;
                st->bytes_allocated += s->bytes;
            }
            if (cursor < nworkers) break;
            iv_finish(cfg, col, comb, nstarted);
            td_reset(comb);
            col++;
            cursor = 0;
        }
        if (col >= n) break;
        /* Sleep to the next tick or the deadline, then sample; after the last
         * deadline only stragglers' publishes are awaited. */
        now = bench_get_ns();
        uint64_t d = cur < n ? (t_end > now ? t_end - now : 0) : 1000000ull;
        if (d > tick) d = tick;
        struct timespec ts = { (time_t)(d / 1000000000ull),
                               (long)(d % 1000000000ull) };
        nanosleep(&ts, NULL);
        if (cur < n)
            iv_sample(&rss_max, live, nworkers, series);
    }
    td_free(comb);
}

/* Run a benchmark once (no warm-up discard, no repeats). */
int bench_run(allocator_ops_t *ops, workload_config_t *workload,
              bench_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->allocator_name = ops->name;
    stats->workload_name = workload->name;
    stats->thread_count = workload->thread_count;
    stats->runs_measured = 1;

    if (ops->cleanup) {
        ops->cleanup();
    }

    bench_cpu_usage_t cpu_before = bench_get_cpu_usage();

    workload->fn(ops, stats, workload);

    bench_cpu_usage_t cpu_after = bench_get_cpu_usage();
    stats->cpu_usage.user_ms = cpu_after.user_ms - cpu_before.user_ms;
    stats->cpu_usage.sys_ms = cpu_after.sys_ms - cpu_before.sys_ms;

    return 0;
}

/* qsort comparator on ops_per_second (ascending). */
static int cmp_ops(const void *a, const void *b) {
    double x = ((const bench_stats_t *)a)->ops_per_second;
    double y = ((const bench_stats_t *)b)->ops_per_second;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Run warmups+runs times, discard warm-ups, report the median run's full
 * stats plus the coefficient of variation of ops_per_second across the kept
 * runs. Flags unstable when CoV exceeds BENCH_UNSTABLE_COV so a caller knows
 * not to gate on that point. */
int bench_run_n(allocator_ops_t *ops, workload_config_t *workload,
                bench_stats_t *stats, int warmups, int runs) {
    if (runs < 1) runs = 1;
    if (warmups < 0) warmups = 0;

    /* Warm up: run and throw the numbers away (populates caches/magazines,
     * pages in the arena, lets the CPU reach steady frequency). */
    for (int w = 0; w < warmups; w++) {
        bench_stats_t scratch;
        bench_run(ops, workload, &scratch);
    }

    if (runs == 1) {
        bench_run(ops, workload, stats);
        stats->runs_measured = 1;
        stats->ops_cov = 0.0;
        stats->unstable = 0;
        return 0;
    }

    bench_stats_t *samples = calloc((size_t)runs, sizeof(*samples));
    if (!samples) return bench_run(ops, workload, stats);

    double sum = 0.0, sumsq = 0.0;
    for (int r = 0; r < runs; r++) {
        bench_run(ops, workload, &samples[r]);
        double v = samples[r].ops_per_second;
        sum += v;
        sumsq += v * v;
    }

    double mean = sum / runs;
    /* population stddev of ops/sec */
    double var = (sumsq / runs) - (mean * mean);
    if (var < 0) var = 0;
    double stddev = sqrt(var);
    double cov = (mean > 0) ? (stddev / mean) : 0.0;

    /* Median run by throughput: report its full latency/memory picture, not a
     * synthetic average that mixes percentiles from different runs. */
    qsort(samples, (size_t)runs, sizeof(*samples), cmp_ops);
    *stats = samples[runs / 2];
    stats->ops_cov = cov;
    stats->runs_measured = runs;
    stats->unstable = (cov > BENCH_UNSTABLE_COV) ? 1 : 0;

    free(samples);
    return 0;
}

/* Append benchmark result to TOML history file */
int bench_append_history(const bench_stats_t *stats,
                         const char *history_path) {
    FILE *f = fopen(history_path, "a");
    if (!f) {
        /* Try creating parent directory */
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", history_path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            char cmd[1100];
            snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
            if (system(cmd) != 0) return -1;
            f = fopen(history_path, "a");
        }
        if (!f) return -1;
    }

    /* Get commit hash */
    char commit[64] = "unknown";
    FILE *p = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (p) {
        if (fgets(commit, sizeof(commit), p)) {
            char *nl = strchr(commit, '\n');
            if (nl) *nl = '\0';
        }
        pclose(p);
    }

    /* Get date */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d", tm);

    /* Get platform info */
    char os_info[128] = "unknown";
    {
        struct utsname uts;
        if (uname(&uts) == 0)
            snprintf(os_info, sizeof(os_info), "%s %s", uts.sysname, uts.release);
    }

    const char *arch =
#if defined(__x86_64__) || defined(_M_X64)
        "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        "aarch64";
#elif defined(__riscv) && (__riscv_xlen == 64)
        "riscv64";
#elif defined(__sparc) || defined(__sparcv9)
        "sparcv9";
#elif defined(__i386__)
        "i386";
#else
        "unknown";
#endif

    char compiler[128] = "unknown";
#if defined(__clang__)
    snprintf(compiler, sizeof(compiler), "clang-%d.%d.%d",
             __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    snprintf(compiler, sizeof(compiler), "gcc-%d.%d.%d",
             __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    snprintf(compiler, sizeof(compiler), "msvc-%d", _MSC_VER);
#endif

    fprintf(f, "\n[[result]]\n");
    fprintf(f, "commit = \"%s\"\n", commit);
    fprintf(f, "date = \"%s\"\n", date);
    fprintf(f, "os = \"%s\"\n", os_info);
    fprintf(f, "arch = \"%s\"\n", arch);
    fprintf(f, "compiler = \"%s\"\n", compiler);
    fprintf(f, "allocator = \"%s\"\n", stats->allocator_name);
    fprintf(f, "workload = \"%s\"\n", stats->workload_name);
    fprintf(f, "ops_per_sec = %.0f\n", stats->ops_per_second);
    fprintf(f, "p99_ns = %.0f\n", stats->latency_p99);
    fprintf(f, "peak_rss_mb = %.1f\n",
            stats->peak_rss_bytes / (1024.0 * 1024.0));
    fprintf(f, "cpu_user_ms = %.0f\n", stats->cpu_usage.user_ms);
    fprintf(f, "cpu_sys_ms = %.0f\n", stats->cpu_usage.sys_ms);

    fclose(f);
    return 0;
}

/* Simple TOML parser: find last matching result for comparison.
 *
 * Bug fixed: previously prev_ops/prev_p99 were overwritten by every block
 * in the file (matching or not) and never reset between blocks, so the
 * comparison silently used whichever block happened to be LAST in the file
 * rather than the last block whose allocator+workload actually matched
 * (e.g. a single-thread baseline entry got diffed against a multi-thread
 * result if multi-thread came later in the file). Now prev_ops/prev_p99 are
 * only recorded once the current block's allocator+workload match, and are
 * reset to 0 at the start of every block. allocator/workload keys are
 * always written before ops_per_sec/p99_ns in bench_append_history(), so
 * this ordering assumption holds for files this module writes itself. */
int bench_compare_history(const bench_stats_t *stats,
                          const char *history_path) {
    FILE *f = fopen(history_path, "r");
    if (!f) return 0;

    char line[512];
    double prev_ops = 0;
    double prev_p99 = 0;
    int found = 0;
    int cur_matches = 0;
    char cur_alloc[128] = "";
    char cur_workload[128] = "";

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "[[result]]", 10) == 0) {
            cur_matches = 0;
            cur_alloc[0] = '\0';
            cur_workload[0] = '\0';
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;

        /* Trim key */
        char key[64];
        size_t klen = (size_t)(eq - line);
        if (klen >= sizeof(key)) continue;
        memcpy(key, line, klen);
        key[klen] = '\0';
        while (klen > 0 && key[klen-1] == ' ') key[--klen] = '\0';

        /* Get value (skip = and spaces/quotes) */
        char *val = eq + 1;
        while (*val == ' ' || *val == '"') val++;
        char *end = val + strlen(val) - 1;
        while (end > val && (*end == '\n' || *end == '"' || *end == ' '))
            *end-- = '\0';

        if (strcmp(key, "allocator") == 0) {
            snprintf(cur_alloc, sizeof(cur_alloc), "%s", val);
            cur_matches = cur_alloc[0] && cur_workload[0] &&
                strcmp(cur_alloc, stats->allocator_name) == 0 &&
                strcmp(cur_workload, stats->workload_name) == 0;
        } else if (strcmp(key, "workload") == 0) {
            snprintf(cur_workload, sizeof(cur_workload), "%s", val);
            cur_matches = cur_alloc[0] && cur_workload[0] &&
                strcmp(cur_alloc, stats->allocator_name) == 0 &&
                strcmp(cur_workload, stats->workload_name) == 0;
        } else if (cur_matches && strcmp(key, "ops_per_sec") == 0) {
            prev_ops = atof(val);
            found = 1;
        } else if (cur_matches && strcmp(key, "p99_ns") == 0) {
            prev_p99 = atof(val);
        }
    }
    fclose(f);

    if (!found) return 0;

    int regression = 0;

    if (prev_ops > 0) {
        double ops_change =
            (stats->ops_per_second - prev_ops) / prev_ops * 100.0;
        if (ops_change < -10.0) {
            printf("  REGRESSION: ops/sec %.0f -> %.0f (%.1f%%)\n",
                   prev_ops, stats->ops_per_second, ops_change);
            regression = 1;
        }
    }

    if (prev_p99 > 0) {
        double p99_change =
            (stats->latency_p99 - prev_p99) / prev_p99 * 100.0;
        if (p99_change > 10.0) {
            printf("  REGRESSION: p99 %.0f -> %.0f ns (+%.1f%%)\n",
                   prev_p99, stats->latency_p99, p99_change);
            regression = 1;
        }
    }

    return regression;
}

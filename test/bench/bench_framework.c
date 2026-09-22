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

/* Read VmRSS from /proc/self/status (more accurate current RSS) */
size_t bench_get_vmrss_bytes(void) {
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t rss = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            char *p = line + 6;
            while (*p == ' ' || *p == '\t') p++;
            rss = (size_t)strtoull(p, NULL, 10) * 1024;
            break;
        }
    }
    fclose(f);
    return rss;
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
        printf("  Fragmentation: %.2f (peak RSS / live bytes at that instant)\n",
               stats->fragmentation_ratio);
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
 *   peak_rss_bytes   peak RSS during the run where sampled
 *   live_bytes_at_peak  simultaneously-live allocated bytes at that instant
 *   frag             peak_rss_bytes / live_bytes_at_peak, or EMPTY when the
 *                    workload does not define one (see has_fragmentation)
 *   ops_floor_raised 1 if the per-thread budget was raised to the floor
 */
void bench_print_csv_header(void) {
    printf("allocator,workload,threads,total_ops,ops_per_thread,elapsed_sec,ops_per_sec,");
    printf("lat_min,lat_p50,lat_p90,lat_p99,lat_p999,lat_max,lat_mean,");
    printf("peak_rss_bytes,allocated_bytes,live_bytes_at_peak,frag,");
    printf("cpu_user_ms,cpu_sys_ms,ops_cov,runs,unstable,ops_floor_raised\n");
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
    printf("%zu,%zu,%zu,", stats->peak_rss_bytes, stats->bytes_allocated,
           stats->live_bytes_at_peak);
    /* Empty, not 0 and not 1.0: an undefined ratio must not look like a
     * measured one. */
    if (stats->has_fragmentation)
        printf("%.4f,", stats->fragmentation_ratio);
    else
        printf(",");
    printf("%.1f,%.1f,%.4f,%d,%d,%d\n",
           stats->cpu_usage.user_ms, stats->cpu_usage.sys_ms,
           stats->ops_cov, stats->runs_measured, stats->unstable,
           stats->ops_floor_raised);
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
    pthread_barrier_t *start_barrier;
} thread_context_t;

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

        if (ptr) {
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

        if (ptr) {
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
    /* Absorb the barrier slots of threads that never started, or the ones
     * that did would wait forever. */
    for (int i = started; i < nthreads; i++)
        pthread_barrier_wait(&start_barrier);
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
 * Lock-free MPMC ring buffer for producer-consumer workload.
 * Uses CAS on head/tail for safe multi-producer/multi-consumer access.
 */
#define RING_CAPACITY 8192

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
    ring_buffer_t *ring;
    td_histogram_t *latency_hist;
    size_t bytes_allocated;
    uint64_t operations;
    pthread_barrier_t *start_barrier;
} pc_context_t;

static void *producer_thread(void *arg) {
    pc_context_t *ctx = (pc_context_t *)arg;
    allocator_ops_t *ops = ctx->ops;
    workload_config_t *cfg = ctx->config;
    unsigned int seed = (unsigned int)(ctx->thread_id + 1);

    pthread_barrier_wait(ctx->start_barrier);

    for (uint64_t i = 0; i < ctx->ops_target; i++) {
        size_t size = cfg->min_size;
        if (cfg->max_size > cfg->min_size) {
            size = cfg->min_size +
                   ((size_t)rand_r(&seed) % (cfg->max_size - cfg->min_size));
        }

        uint64_t t0 = bench_get_ns();
        void *ptr = ops->alloc(size);
        uint64_t t1 = bench_get_ns();

        if (!ptr) continue;
        memset(ptr, 0x42, size);

        td_add(ctx->latency_hist, (double)(t1 - t0), 1);
        ctx->bytes_allocated += size;
        ctx->operations++;

        while (!ring_push(ctx->ring, ptr, size)) {
            /* Spin briefly waiting for consumer to drain */
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

    ring_buffer_t *ring = calloc(1, sizeof(ring_buffer_t));
    pthread_t *threads = calloc(total, sizeof(pthread_t));
    pc_context_t *contexts = calloc(total, sizeof(pc_context_t));
    pthread_barrier_t barrier;

    if (!ring || !threads || !contexts) {
        free(ring); free(threads); free(contexts);
        return;
    }

    ring_init(ring);
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
        contexts[i].ring = ring;
        contexts[i].start_barrier = &barrier;
        contexts[i].bytes_allocated = 0;
        contexts[i].operations = 0;
        if (td_init(100.0, &contexts[i].latency_hist) != 0) {
            fprintf(stderr, "bench: tdigest init failed for pc thread %d\n", i);
            goto cleanup;
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
        started_p++;
    }
    for (int i = n_producers; i < total; i++) {
        if (pthread_create(&threads[i], NULL, consumer_thread,
                           &contexts[i]) != 0) {
            fprintf(stderr, "bench: pthread_create failed at consumer %d\n",
                    i - n_producers);
            break;
        }
        started_c++;
    }
    for (int i = started_p + started_c; i < total; i++)
        pthread_barrier_wait(&barrier);

    uint64_t start = bench_get_ns();
    pthread_barrier_wait(&barrier);

    /* Wait for producers */
    for (int i = 0; i < started_p; i++) {
        pthread_join(threads[i], NULL);
    }
    atomic_store(&ring->done, 1);

    /* Wait for consumers */
    for (int i = n_producers; i < n_producers + started_c; i++) {
        pthread_join(threads[i], NULL);
    }
    uint64_t end = bench_get_ns();

    /* Aggregate */
    td_histogram_t *combined;
    if (td_init(100.0, &combined) != 0) goto cleanup;

    uint64_t total_ops = 0;
    size_t total_alloc = 0;
    for (int i = 0; i < total; i++) {
        td_merge(combined, contexts[i].latency_hist);
        total_ops += contexts[i].operations;
        total_alloc += contexts[i].bytes_allocated;
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
 *   round samples (RSS, live bytes) TOGETHER.  peak_rss_bytes and
 *   live_bytes_at_peak are the pair from the sample with the highest ratio,
 *   so the reported fragmentation is a ratio of two quantities measured at
 *   the same moment, and the RSS reported alongside it is the one that
 *   produced it.
 *
 *   Live bytes are summed across threads via an atomic, because RSS is a
 *   process-wide quantity: pairing process RSS with one thread's live bytes
 *   would be another mismatched ratio.
 *
 *   The requested thread count is honoured.  cfg->thread_count == 1 still
 *   runs single-threaded.
 */
typedef struct {
    int thread_id;
    allocator_ops_t *ops;
    workload_config_t *cfg;
    uint64_t ops_target;              /* per-thread share of the budget */
    size_t pool_cap;
    td_histogram_t *hist;
    size_t bytes_allocated;           /* cumulative traffic, this thread */
    uint64_t operations;
    int failed;                       /* an allocation returned NULL */
    /* Shared, process-wide live-byte accounting + peak sampling. */
    atomic_size_t *live_bytes;
    pthread_mutex_t *peak_lock;
    double *peak_frag;
    size_t *peak_rss;
    size_t *peak_live;
    size_t *max_live;
    pthread_barrier_t *start_barrier;
} frag_context_t;

/* Sample (RSS, live bytes) as a pair and keep the pair with the worst ratio.
 * Called between rounds, when the live set is at its local maximum. */
static void frag_sample(frag_context_t *ctx) {
    size_t live = atomic_load(ctx->live_bytes);
    if (live == 0) return;
    size_t rss = bench_get_vmrss_bytes();
    double frag = (double)rss / (double)live;

    pthread_mutex_lock(ctx->peak_lock);
    if (live > *ctx->max_live) *ctx->max_live = live;
    if (frag > *ctx->peak_frag) {
        *ctx->peak_frag = frag;
        *ctx->peak_rss = rss;      /* the RSS that produced this ratio */
        *ctx->peak_live = live;    /* ...and the live bytes at that instant */
    }
    pthread_mutex_unlock(ctx->peak_lock);
}

static void *frag_worker(void *arg) {
    frag_context_t *ctx = (frag_context_t *)arg;
    allocator_ops_t *ops = ctx->ops;
    workload_config_t *cfg = ctx->cfg;

    size_t min_sz = cfg->min_size < 8 ? 8 : cfg->min_size;
    size_t max_sz = cfg->max_size > 4096 ? 4096 : cfg->max_size;
    if (max_sz < min_sz) max_sz = min_sz;

    void **pool = calloc(ctx->pool_cap, sizeof(void *));
    size_t *pool_sz = calloc(ctx->pool_cap, sizeof(size_t));
    if (!pool || !pool_sz) {
        fprintf(stderr, "bench: frag pool allocation failed (cap %zu)\n",
                ctx->pool_cap);
        ctx->failed = 1;
        free(pool); free(pool_sz);
        if (ctx->start_barrier) pthread_barrier_wait(ctx->start_barrier);
        return NULL;
    }
    size_t pool_count = 0;
    size_t held = 0;                  /* this thread's live bytes */
    unsigned int seed = 42u ^ ((unsigned int)ctx->thread_id * 2654435761u);

    if (ctx->start_barrier) pthread_barrier_wait(ctx->start_barrier);

    uint64_t remaining = ctx->ops_target;
    while (remaining > 0) {
        /* Grow the live set: allocate a batch into free pool slots. */
        size_t room = ctx->pool_cap - pool_count;
        size_t batch = remaining > room ? room : (size_t)remaining;
        if (batch == 0) batch = 1;    /* pool full: still do work, see below */

        for (size_t i = 0; i < batch; i++) {
            size_t sz = min_sz +
                ((size_t)rand_r(&seed) % (max_sz - min_sz + 1));

            uint64_t t0 = bench_get_ns();
            void *ptr = ops->alloc(sz);
            uint64_t t1 = bench_get_ns();

            if (!ptr) {
                /* Not an operation, and not live bytes.  Report it: a run
                 * that could not allocate has not measured fragmentation. */
                ctx->failed = 1;
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
                atomic_fetch_add(ctx->live_bytes, sz);
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
        frag_sample(ctx);

        /* Free a random ~50% of our own pool, creating the holes that make
         * this a fragmentation workload rather than a churn workload. */
        for (size_t i = 0; i < pool_count; ) {
            if (rand_r(&seed) % 2 == 0) {
                uint64_t t0 = bench_get_ns();
                ops->free(pool[i]);
                uint64_t t1 = bench_get_ns();
                td_add(ctx->hist, (double)(t1 - t0), 1);
                ctx->operations++;

                held -= pool_sz[i];
                atomic_fetch_sub(ctx->live_bytes, pool_sz[i]);
                pool[i] = pool[pool_count - 1];
                pool_sz[i] = pool_sz[pool_count - 1];
                pool_count--;
            } else {
                i++;
            }
        }
    }

    /* Release what is still live. */
    for (size_t i = 0; i < pool_count; i++) {
        ops->free(pool[i]);
        held -= pool_sz[i];
        atomic_fetch_sub(ctx->live_bytes, pool_sz[i]);
    }
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

    atomic_size_t live_bytes;
    atomic_init(&live_bytes, (size_t)0);
    pthread_mutex_t peak_lock = PTHREAD_MUTEX_INITIALIZER;
    double peak_frag = 0;
    size_t peak_rss = 0, peak_live = 0, max_live = 0;

    frag_context_t *ctxs = calloc((size_t)nthreads, sizeof(*ctxs));
    pthread_t *threads = calloc((size_t)nthreads, sizeof(*threads));
    if (!ctxs || !threads) {
        fprintf(stderr, "bench: frag driver allocation failed\n");
        free(ctxs); free(threads);
        return;
    }

    pthread_barrier_t start_barrier;
    pthread_barrier_init(&start_barrier, NULL, (unsigned)nthreads + 1);

    for (int i = 0; i < nthreads; i++) {
        ctxs[i].thread_id = i;
        ctxs[i].ops = ops;
        ctxs[i].cfg = cfg;
        ctxs[i].ops_target = per_thread;
        ctxs[i].pool_cap = pool_cap;
        ctxs[i].live_bytes = &live_bytes;
        ctxs[i].peak_lock = &peak_lock;
        ctxs[i].peak_frag = &peak_frag;
        ctxs[i].peak_rss = &peak_rss;
        ctxs[i].peak_live = &peak_live;
        ctxs[i].max_live = &max_live;
        ctxs[i].start_barrier = &start_barrier;
        if (td_init(100.0, &ctxs[i].hist) != 0) {
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
    for (int i = started; i < nthreads; i++)
        pthread_barrier_wait(&start_barrier);

    uint64_t start = bench_get_ns();
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    uint64_t end = bench_get_ns();

    td_histogram_t *combined = NULL;
    uint64_t total_ops = 0;
    size_t total_allocated = 0;
    int any_failed = 0;
    if (td_init(100.0, &combined) == 0) {
        for (int i = 0; i < nthreads; i++) {
            if (ctxs[i].hist == NULL) continue;
            td_merge(combined, ctxs[i].hist);
            total_ops += ctxs[i].operations;
            total_allocated += ctxs[i].bytes_allocated;
            any_failed |= ctxs[i].failed;
        }
    }

    if (any_failed) {
        fprintf(stderr, "bench: WARNING -- at least one allocation failed "
                "during the fragmentation workload; the ratio below is not a "
                "measurement of a healthy allocator\n");
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

    /* The peak pair, measured together during the run -- NOT post-cleanup
     * RSS, and NOT a ratio against cumulative traffic. */
    stats->peak_rss_bytes = peak_rss;
    stats->current_rss_bytes = bench_get_vmrss_bytes();
    stats->live_bytes_at_peak = peak_live;
    stats->peak_live_bytes = max_live;
    stats->fragmentation_ratio = peak_frag;
    /* Only defined if we actually sampled a live set. */
    stats->has_fragmentation = (peak_live > 0 && peak_rss > 0) ? 1 : 0;

    for (int i = 0; i < nthreads; i++)
        if (ctxs[i].hist != NULL) td_free(ctxs[i].hist);
    pthread_barrier_destroy(&start_barrier);
    free(ctxs);
    free(threads);
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

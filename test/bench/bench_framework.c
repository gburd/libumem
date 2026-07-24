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
    if (stats->thread_count > 1) {
        printf("Threads:   %d\n", stats->thread_count);
    }
    printf("========================================\n");
    printf("Throughput: %.2f ops/sec (%.2f s total)\n",
           stats->ops_per_second, stats->elapsed_seconds);
    printf("Operations: %lu\n", (unsigned long)stats->total_operations);
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
    printf("  RSS:          %s\n", rss_str);
    printf("  Allocated:    %s\n", alloc_str);
    printf("  Fragmentation: %.2f\n", stats->fragmentation_ratio);
    printf("\nCPU:\n");
    printf("  User: %.1f ms\n", stats->cpu_usage.user_ms);
    printf("  Sys:  %.1f ms\n", stats->cpu_usage.sys_ms);
    printf("========================================\n");
}

/* CSV output for analysis */
void bench_print_csv_header(void) {
    printf("allocator,workload,threads,ops,elapsed_sec,ops_per_sec,");
    printf("lat_min,lat_p50,lat_p90,lat_p99,lat_p999,lat_max,lat_mean,");
    printf("rss_bytes,allocated_bytes,fragmentation,");
    printf("cpu_user_ms,cpu_sys_ms,ops_cov,runs,unstable\n");
}

void bench_print_csv_row(const bench_stats_t *stats) {
    printf("%s,%s,%d,%lu,%.6f,%.2f,",
           stats->allocator_name, stats->workload_name, stats->thread_count,
           (unsigned long)stats->total_operations, stats->elapsed_seconds, stats->ops_per_second);
    printf("%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,",
           stats->latency_min, stats->latency_p50, stats->latency_p90,
           stats->latency_p99, stats->latency_p999, stats->latency_max, stats->latency_mean);
    printf("%zu,%zu,%.2f,%.1f,%.1f,%.4f,%d,%d\n",
           stats->peak_rss_bytes, stats->bytes_allocated,
           stats->fragmentation_ratio,
           stats->cpu_usage.user_ms, stats->cpu_usage.sys_ms,
           stats->ops_cov, stats->runs_measured, stats->unstable);
}

/* Thread context for multithreaded benchmarks */
typedef struct thread_context {
    int thread_id;
    allocator_ops_t *ops;
    workload_config_t *config;
    td_histogram_t *latency_hist;
    size_t bytes_allocated;
    size_t bytes_freed;
    uint64_t operations;
    pthread_barrier_t *start_barrier;
} thread_context_t;

/* Single-threaded workload: allocate, use, free in loop */
void workload_single_thread(allocator_ops_t *ops, bench_stats_t *stats, void *config) {
    workload_config_t *cfg = (workload_config_t *)config;
    td_histogram_t *hist;

    if (td_init(100.0, &hist) != 0) {
        fprintf(stderr, "Failed to initialize tdigest\n");
        return;
    }

    uint64_t start = bench_get_ns();
    size_t total_allocated = 0;

    for (uint64_t i = 0; i < cfg->operation_count; i++) {
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

            double latency = (double)(alloc_end - alloc_start);
            td_add(hist, latency, 1);

            ops->free(ptr);
        }
    }

    uint64_t end = bench_get_ns();

    /* Compute statistics */
    stats->elapsed_seconds = (end - start) / 1e9;
    stats->total_operations = cfg->operation_count;
    stats->ops_per_second = cfg->operation_count / stats->elapsed_seconds;
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

    /* Memory */
    stats->peak_rss_bytes = bench_get_rss_bytes();
    stats->current_rss_bytes = stats->peak_rss_bytes;
    stats->fragmentation_ratio = (total_allocated > 0) ?
        ((double)stats->peak_rss_bytes / total_allocated) : 1.0;

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

    for (uint64_t i = 0; i < cfg->operation_count; i++) {
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

/* Multithreaded workload: all threads allocate/free concurrently */
void workload_multi_thread(allocator_ops_t *ops, bench_stats_t *stats, void *config) {
    workload_config_t *cfg = (workload_config_t *)config;
    int nthreads = cfg->thread_count;

    pthread_t *threads = calloc(nthreads, sizeof(pthread_t));
    thread_context_t *contexts = calloc(nthreads, sizeof(thread_context_t));
    pthread_barrier_t start_barrier;

    if (!threads || !contexts) {
        free(threads);
        free(contexts);
        return;
    }

    pthread_barrier_init(&start_barrier, NULL, nthreads + 1);

    /* Initialize thread contexts */
    for (int i = 0; i < nthreads; i++) {
        contexts[i].thread_id = i;
        contexts[i].ops = ops;
        contexts[i].config = cfg;
        contexts[i].start_barrier = &start_barrier;
        contexts[i].bytes_allocated = 0;
        contexts[i].bytes_freed = 0;
        contexts[i].operations = 0;

        if (td_init(100.0, &contexts[i].latency_hist) != 0) {
            fprintf(stderr, "Failed to initialize tdigest for thread %d\n", i);
            /* Cleanup and return */
            pthread_barrier_destroy(&start_barrier);
            free(threads);
            free(contexts);
            return;
        }

        pthread_create(&threads[i], NULL, mt_worker_thread, &contexts[i]);
    }

    uint64_t start = bench_get_ns();
    pthread_barrier_wait(&start_barrier);  /* Start all threads */

    /* Wait for all threads */
    for (int i = 0; i < nthreads; i++) {
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
    stats->ops_per_second = total_ops / stats->elapsed_seconds;
    stats->bytes_allocated = total_allocated;
    stats->bytes_freed = total_freed;
    stats->thread_count = nthreads;

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

    /* Memory */
    stats->peak_rss_bytes = bench_get_rss_bytes();
    stats->current_rss_bytes = stats->peak_rss_bytes;
    stats->fragmentation_ratio = (total_allocated > 0) ?
        ((double)stats->peak_rss_bytes / total_allocated) : 1.0;

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

    for (uint64_t i = 0; i < cfg->operation_count; i++) {
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

    uint64_t ops_per_producer = cfg->operation_count / (uint64_t)n_producers;

    for (int i = 0; i < total; i++) {
        contexts[i].thread_id = i;
        contexts[i].ops = ops;
        contexts[i].config = cfg;
        contexts[i].ring = ring;
        contexts[i].start_barrier = &barrier;
        contexts[i].bytes_allocated = 0;
        contexts[i].operations = 0;
        if (td_init(100.0, &contexts[i].latency_hist) != 0) {
            goto cleanup;
        }
    }

    /* Temporarily override per-producer op count */
    workload_config_t producer_cfg = *cfg;
    producer_cfg.operation_count = ops_per_producer;
    for (int i = 0; i < n_producers; i++) {
        contexts[i].config = &producer_cfg;
        pthread_create(&threads[i], NULL, producer_thread, &contexts[i]);
    }
    for (int i = n_producers; i < total; i++) {
        pthread_create(&threads[i], NULL, consumer_thread, &contexts[i]);
    }

    uint64_t start = bench_get_ns();
    pthread_barrier_wait(&barrier);

    /* Wait for producers */
    for (int i = 0; i < n_producers; i++) {
        pthread_join(threads[i], NULL);
    }
    atomic_store(&ring->done, 1);

    /* Wait for consumers */
    for (int i = n_producers; i < total; i++) {
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
    stats->ops_per_second = total_ops / stats->elapsed_seconds;
    stats->bytes_allocated = total_alloc;
    stats->bytes_freed = total_alloc;
    stats->thread_count = total;

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

    stats->peak_rss_bytes = bench_get_vmrss_bytes();
    stats->current_rss_bytes = stats->peak_rss_bytes;
    stats->fragmentation_ratio = (total_alloc > 0) ?
        ((double)stats->peak_rss_bytes / total_alloc) : 1.0;

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

/* Fragmentation workload: allocate random sizes, free 50%, repeat */
void workload_fragmentation(allocator_ops_t *ops,
                            bench_stats_t *stats, void *config) {
    workload_config_t *cfg = (workload_config_t *)config;
    td_histogram_t *hist;

    if (td_init(100.0, &hist) != 0) return;

    size_t min_sz = cfg->min_size < 8 ? 8 : cfg->min_size;
    size_t max_sz = cfg->max_size > 4096 ? 4096 : cfg->max_size;
    if (max_sz < min_sz) max_sz = min_sz;

    /* Pool of live allocations */
    size_t pool_cap = 4096;
    void **pool = calloc(pool_cap, sizeof(void *));
    size_t *pool_sz = calloc(pool_cap, sizeof(size_t));
    if (!pool || !pool_sz) {
        free(pool); free(pool_sz); td_free(hist); return;
    }
    size_t pool_count = 0;
    size_t total_allocated = 0;
    size_t currently_held = 0;
    uint64_t total_ops = 0;
    double peak_frag = 0;
    unsigned int seed = 42;

    uint64_t start = bench_get_ns();

    /* Run rounds: allocate batch, free ~50% */
    uint64_t remaining = cfg->operation_count;
    while (remaining > 0) {
        /* Allocate a batch */
        size_t batch = remaining > pool_cap ? pool_cap : remaining;
        for (size_t i = 0; i < batch; i++) {
            size_t sz = min_sz +
                ((size_t)rand_r(&seed) % (max_sz - min_sz + 1));

            uint64_t t0 = bench_get_ns();
            void *ptr = ops->alloc(sz);
            uint64_t t1 = bench_get_ns();

            if (!ptr) continue;
            memset(ptr, 0xAB, sz);
            td_add(hist, (double)(t1 - t0), 1);

            if (pool_count < pool_cap) {
                pool[pool_count] = ptr;
                pool_sz[pool_count] = sz;
                pool_count++;
            } else {
                ops->free(ptr);
            }
            total_allocated += sz;
            currently_held += sz;
            total_ops++;
        }
        remaining -= batch;

        /* Measure fragmentation at peak */
        size_t rss = bench_get_vmrss_bytes();
        if (currently_held > 0) {
            double frag = (double)rss / (double)currently_held;
            if (frag > peak_frag) peak_frag = frag;
        }

        /* Free random ~50% of pool */
        for (size_t i = 0; i < pool_count; ) {
            if (rand_r(&seed) % 2 == 0) {
                uint64_t t0 = bench_get_ns();
                ops->free(pool[i]);
                uint64_t t1 = bench_get_ns();
                td_add(hist, (double)(t1 - t0), 1);
                total_ops++;

                currently_held -= pool_sz[i];
                pool[i] = pool[pool_count - 1];
                pool_sz[i] = pool_sz[pool_count - 1];
                pool_count--;
            } else {
                i++;
            }
        }
    }

    /* Free remaining */
    for (size_t i = 0; i < pool_count; i++) {
        ops->free(pool[i]);
        currently_held -= pool_sz[i];
    }

    uint64_t end = bench_get_ns();

    stats->elapsed_seconds = (end - start) / 1e9;
    stats->total_operations = total_ops;
    stats->ops_per_second = total_ops / stats->elapsed_seconds;
    stats->bytes_allocated = total_allocated;
    stats->bytes_freed = total_allocated;
    stats->thread_count = 1;

    stats->latency_min = td_min(hist);
    stats->latency_max = td_max(hist);
    stats->latency_p50 = td_quantile(hist, 0.50);
    stats->latency_p90 = td_quantile(hist, 0.90);
    stats->latency_p99 = td_quantile(hist, 0.99);
    stats->latency_p999 = td_quantile(hist, 0.999);

    long long total_samples = td_size(hist);
    double sum = 0;
    td_compress(hist);
    int n = td_centroid_count(hist);
    for (int i = 0; i < n; i++) {
        sum += td_centroids_mean_at(hist, i) *
               td_centroids_weight_at(hist, i);
    }
    stats->latency_mean = (total_samples > 0) ? (sum / total_samples) : 0;

    stats->peak_rss_bytes = bench_get_vmrss_bytes();
    stats->current_rss_bytes = stats->peak_rss_bytes;
    stats->fragmentation_ratio = peak_frag > 0 ? peak_frag :
        ((total_allocated > 0) ?
         ((double)stats->peak_rss_bytes / total_allocated) : 1.0);

    td_free(hist);
    free(pool);
    free(pool_sz);
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

/* Simple TOML parser: find last matching result for comparison */
int bench_compare_history(const bench_stats_t *stats,
                          const char *history_path) {
    FILE *f = fopen(history_path, "r");
    if (!f) return 0;

    char line[512];
    double prev_ops = 0;
    double prev_p99 = 0;
    int found = 0;
    int in_matching = 0;
    char cur_alloc[128] = "";
    char cur_workload[128] = "";

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "[[result]]", 10) == 0) {
            /* Save previous matching block */
            if (in_matching && prev_ops > 0) found = 1;
            in_matching = 0;
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

        if (strcmp(key, "allocator") == 0)
            snprintf(cur_alloc, sizeof(cur_alloc), "%s", val);
        else if (strcmp(key, "workload") == 0)
            snprintf(cur_workload, sizeof(cur_workload), "%s", val);
        else if (strcmp(key, "ops_per_sec") == 0)
            prev_ops = atof(val);
        else if (strcmp(key, "p99_ns") == 0)
            prev_p99 = atof(val);

        if (cur_alloc[0] && cur_workload[0] &&
            strcmp(cur_alloc, stats->allocator_name) == 0 &&
            strcmp(cur_workload, stats->workload_name) == 0) {
            in_matching = 1;
        }
    }
    if (in_matching && prev_ops > 0) found = 1;
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

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
    printf("========================================\n");
}

/* CSV output for analysis */
void bench_print_csv_header(void) {
    printf("allocator,workload,threads,ops,elapsed_sec,ops_per_sec,");
    printf("lat_min,lat_p50,lat_p90,lat_p99,lat_p999,lat_max,lat_mean,");
    printf("rss_bytes,allocated_bytes,fragmentation\n");
}

void bench_print_csv_row(const bench_stats_t *stats) {
    printf("%s,%s,%d,%lu,%.6f,%.2f,",
           stats->allocator_name, stats->workload_name, stats->thread_count,
           (unsigned long)stats->total_operations, stats->elapsed_seconds, stats->ops_per_second);
    printf("%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,",
           stats->latency_min, stats->latency_p50, stats->latency_p90,
           stats->latency_p99, stats->latency_p999, stats->latency_max, stats->latency_mean);
    printf("%zu,%zu,%.2f\n",
           stats->peak_rss_bytes, stats->bytes_allocated, stats->fragmentation_ratio);
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

    for (uint64_t i = 0; i < cfg->operation_count; i++) {
        size_t size = cfg->min_size;
        if (cfg->max_size > cfg->min_size) {
            size = cfg->min_size + (rand() % (cfg->max_size - cfg->min_size));
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

/* Producer-consumer workload: separate alloc and free threads */
void workload_producer_consumer(allocator_ops_t *ops, bench_stats_t *stats, void *config) {
    /* TODO: Implement producer-consumer pattern */
    workload_single_thread(ops, stats, config);  /* Placeholder */
}

/* Fragmentation test: allocate various sizes, free pattern */
void workload_fragmentation(allocator_ops_t *ops, bench_stats_t *stats, void *config) {
    /* TODO: Implement fragmentation test */
    workload_single_thread(ops, stats, config);  /* Placeholder */
}

/* Run a benchmark */
int bench_run(allocator_ops_t *ops, workload_config_t *workload, bench_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->allocator_name = ops->name;
    stats->workload_name = workload->name;
    stats->thread_count = workload->thread_count;

    /* Optional cleanup before run */
    if (ops->cleanup) {
        ops->cleanup();
    }

    /* Run the workload */
    workload->fn(ops, stats, workload);

    return 0;
}

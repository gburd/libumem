/*
 * Performance benchmarks for per-CPU magazine caching
 *
 * Compares performance of current implementation vs per-CPU implementation
 * across different thread counts, allocation sizes, and NUMA configurations.
 */

#include "../../config.h"

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>

#include "../../umem.h"
#include "bench_framework.h"

#ifdef UMEM_PER_CPU_CACHE
#include "../../umem_impl.h"
#include "../../umem_percpu.h"
#endif

/*
 * Benchmark configuration
 */
typedef struct bench_config {
	int nthreads;
	int iterations;
	size_t size;
	int pin_cpus;
	int measure_numa;
} bench_config_t;

/*
 * Thread work structure
 */
typedef struct thread_work {
	umem_cache_t *cache;
	int thread_id;
	int iterations;
	int cpu_id;
	uint64_t alloc_ns;
	uint64_t free_ns;
	uint64_t total_ns;
} thread_work_t;

/*
 * Timing helpers
 */
static inline uint64_t
gettime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/*
 * Benchmark 1: Single-threaded baseline
 *
 * Measure single-threaded alloc/free performance to establish baseline.
 */
static void *
bench_single_thread_worker(void *arg)
{
	thread_work_t *work = (thread_work_t *)arg;
	void **bufs;
	uint64_t start, end;
	int i;

	bufs = malloc(work->iterations * sizeof(void *));
	if (bufs == NULL) {
		return NULL;
	}

	/* Allocation benchmark */
	start = gettime_ns();
	for (i = 0; i < work->iterations; i++) {
		bufs[i] = umem_cache_alloc(work->cache, UMEM_DEFAULT);
	}
	end = gettime_ns();
	work->alloc_ns = end - start;

	/* Free benchmark */
	start = gettime_ns();
	for (i = 0; i < work->iterations; i++) {
		umem_cache_free(work->cache, bufs[i]);
	}
	end = gettime_ns();
	work->free_ns = end - start;

	work->total_ns = work->alloc_ns + work->free_ns;

	free(bufs);
	return NULL;
}

static void
bench_single_thread(bench_config_t *config)
{
	umem_cache_t *cache;
	thread_work_t work;
	pthread_t thread;
	double alloc_ns_per_op, free_ns_per_op, total_ns_per_op;
	double ops_per_sec;

	printf("=== Benchmark 1: Single-threaded (size=%zu) ===\n", config->size);

	cache = umem_cache_create("bench_single", config->size, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	if (cache == NULL) {
		printf("Failed to create cache\n");
		return;
	}

	work.cache = cache;
	work.thread_id = 0;
	work.iterations = config->iterations;

	pthread_create(&thread, NULL, bench_single_thread_worker, &work);
	pthread_join(thread, NULL);

	alloc_ns_per_op = (double)work.alloc_ns / config->iterations;
	free_ns_per_op = (double)work.free_ns / config->iterations;
	total_ns_per_op = (double)work.total_ns / config->iterations;
	ops_per_sec = 1000000000.0 / total_ns_per_op;

	printf("Alloc:  %.2f ns/op\n", alloc_ns_per_op);
	printf("Free:   %.2f ns/op\n", free_ns_per_op);
	printf("Total:  %.2f ns/op\n", total_ns_per_op);
	printf("Throughput: %.2f Mops/sec\n\n", ops_per_sec / 1000000.0);

	umem_cache_destroy(cache);
}

/*
 * Benchmark 2: Multi-threaded scalability
 *
 * Test with 1, 2, 4, 8, 16, 32, 64 threads to measure scalability.
 */
static void *
bench_multi_thread_worker(void *arg)
{
	thread_work_t *work = (thread_work_t *)arg;
	void **bufs;
	uint64_t start, end;
	int i;

#ifdef __linux__
	/* Pin to CPU if requested */
	if (work->cpu_id >= 0) {
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(work->cpu_id, &cpuset);
		pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
	}
#endif

	bufs = malloc(work->iterations * sizeof(void *));
	if (bufs == NULL) {
		return NULL;
	}

	/* Warm up */
	for (i = 0; i < 100; i++) {
		void *p = umem_cache_alloc(work->cache, UMEM_DEFAULT);
		umem_cache_free(work->cache, p);
	}

	/* Benchmark */
	start = gettime_ns();

	for (i = 0; i < work->iterations; i++) {
		bufs[i] = umem_cache_alloc(work->cache, UMEM_DEFAULT);
	}

	for (i = 0; i < work->iterations; i++) {
		umem_cache_free(work->cache, bufs[i]);
	}

	end = gettime_ns();
	work->total_ns = end - start;

	free(bufs);
	return NULL;
}

static void
bench_multi_thread(bench_config_t *config)
{
	umem_cache_t *cache;
	pthread_t *threads;
	thread_work_t *work;
	uint64_t total_ops, total_ns, max_ns;
	double ops_per_sec, throughput_scale;
	int i;

	printf("=== Benchmark 2: Multi-threaded (threads=%d, size=%zu) ===\n",
	    config->nthreads, config->size);

	cache = umem_cache_create("bench_multi", config->size, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	if (cache == NULL) {
		printf("Failed to create cache\n");
		return;
	}

	threads = malloc(config->nthreads * sizeof(pthread_t));
	work = malloc(config->nthreads * sizeof(thread_work_t));

	/* Create threads */
	for (i = 0; i < config->nthreads; i++) {
		work[i].cache = cache;
		work[i].thread_id = i;
		work[i].iterations = config->iterations;
		work[i].cpu_id = config->pin_cpus ? i : -1;
		pthread_create(&threads[i], NULL, bench_multi_thread_worker, &work[i]);
	}

	/* Wait for threads */
	for (i = 0; i < config->nthreads; i++) {
		pthread_join(threads[i], NULL);
	}

	/* Calculate statistics */
	total_ops = (uint64_t)config->nthreads * config->iterations * 2; /* alloc + free */
	total_ns = 0;
	max_ns = 0;

	for (i = 0; i < config->nthreads; i++) {
		total_ns += work[i].total_ns;
		if (work[i].total_ns > max_ns) {
			max_ns = work[i].total_ns;
		}
	}

	ops_per_sec = (double)total_ops / ((double)max_ns / 1000000000.0);
	throughput_scale = ops_per_sec / (config->nthreads * 1000000.0);

	printf("Total operations: %llu\n", (unsigned long long)total_ops);
	printf("Max thread time:  %.2f ms\n", (double)max_ns / 1000000.0);
	printf("Throughput:       %.2f Mops/sec\n", ops_per_sec / 1000000.0);
	printf("Per-thread:       %.2f Mops/sec\n", throughput_scale);
	printf("Scalability:      %.2fx\n\n", throughput_scale * config->nthreads);

#ifdef UMEM_PER_CPU_CACHE
	/* Dump per-CPU statistics */
	if (umem_percpu_enabled) {
		printf("Per-CPU statistics:\n");
		umem_percpu_dump(cache);
		printf("\n");
	}
#endif

	free(threads);
	free(work);
	umem_cache_destroy(cache);
}

/*
 * Benchmark 3: Lock contention measurement
 *
 * Measure lock contention by tracking time spent waiting for locks.
 */
static void
bench_lock_contention(bench_config_t *config)
{
	printf("=== Benchmark 3: Lock Contention (threads=%d) ===\n",
	    config->nthreads);
	printf("Use 'perf record -e lock:contention_begin' to measure\n");
	printf("Run: perf record -e lock:contention_begin -a ./bench_percpu --contention\n\n");
}

/*
 * Benchmark 4: NUMA locality
 *
 * Measure NUMA locality using perf mem.
 */
static void
bench_numa_locality(bench_config_t *config)
{
	printf("=== Benchmark 4: NUMA Locality ===\n");

#ifdef UMEM_PER_CPU_CACHE
	if (!umem_numa_enabled) {
		printf("NUMA not available on this system\n\n");
		return;
	}

	printf("NUMA nodes: %d\n", umem_num_nodes);
	printf("Run: perf mem record -a ./bench_percpu --numa\n");
	printf("Then: perf mem report --stdio\n\n");
#else
	printf("Per-CPU caching not enabled\n\n");
#endif
}

/*
 * Benchmark 5: Size sweep
 *
 * Test different allocation sizes to find crossover points.
 */
static void
bench_size_sweep(bench_config_t *config)
{
	size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
	int i;

	printf("=== Benchmark 5: Size Sweep (threads=%d) ===\n", config->nthreads);

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		bench_config_t size_config = *config;
		size_config.size = sizes[i];
		bench_multi_thread(&size_config);
	}
}

/*
 * Benchmark 6: Thread sweep
 *
 * Test different thread counts to measure scalability curve.
 */
static void
bench_thread_sweep(bench_config_t *config)
{
	int thread_counts[] = {1, 2, 4, 8, 16, 32, 64};
	int i;

	printf("=== Benchmark 6: Thread Sweep (size=%zu) ===\n", config->size);

	for (i = 0; i < sizeof(thread_counts) / sizeof(thread_counts[0]); i++) {
		bench_config_t thread_config = *config;
		thread_config.nthreads = thread_counts[i];
		bench_multi_thread(&thread_config);
	}
}

/*
 * Main benchmark runner
 */
static void
usage(const char *progname)
{
	printf("Usage: %s [options]\n", progname);
	printf("Options:\n");
	printf("  --threads N      Number of threads (default: 8)\n");
	printf("  --iterations N   Iterations per thread (default: 100000)\n");
	printf("  --size N         Allocation size (default: 64)\n");
	printf("  --pin-cpus       Pin threads to CPUs\n");
	printf("  --single         Run single-threaded benchmark\n");
	printf("  --multi          Run multi-threaded benchmark\n");
	printf("  --contention     Run lock contention benchmark\n");
	printf("  --numa           Run NUMA locality benchmark\n");
	printf("  --size-sweep     Run size sweep benchmark\n");
	printf("  --thread-sweep   Run thread sweep benchmark\n");
	printf("  --all            Run all benchmarks\n");
	printf("  --help           Show this help\n");
}

int
main(int argc, char *argv[])
{
	bench_config_t config = {
		.nthreads = 8,
		.iterations = 100000,
		.size = 64,
		.pin_cpus = 0,
		.measure_numa = 0
	};
	int run_single = 0, run_multi = 0, run_contention = 0;
	int run_numa = 0, run_size_sweep = 0, run_thread_sweep = 0;
	int run_all = 0;
	int i;

	/* Parse arguments */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
			config.nthreads = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
			config.iterations = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			config.size = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--pin-cpus") == 0) {
			config.pin_cpus = 1;
		} else if (strcmp(argv[i], "--single") == 0) {
			run_single = 1;
		} else if (strcmp(argv[i], "--multi") == 0) {
			run_multi = 1;
		} else if (strcmp(argv[i], "--contention") == 0) {
			run_contention = 1;
		} else if (strcmp(argv[i], "--numa") == 0) {
			run_numa = 1;
		} else if (strcmp(argv[i], "--size-sweep") == 0) {
			run_size_sweep = 1;
		} else if (strcmp(argv[i], "--thread-sweep") == 0) {
			run_thread_sweep = 1;
		} else if (strcmp(argv[i], "--all") == 0) {
			run_all = 1;
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			printf("Unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	/* Default to running all if nothing specified */
	if (!run_single && !run_multi && !run_contention && !run_numa &&
	    !run_size_sweep && !run_thread_sweep && !run_all) {
		run_all = 1;
	}

	printf("Per-CPU Magazine Caching Benchmark\n");
	printf("===================================\n\n");

#ifdef UMEM_PER_CPU_CACHE
	printf("Per-CPU caching: %s\n",
	    umem_percpu_enabled ? "ENABLED" : "DISABLED");
	printf("NUMA awareness:  %s\n",
	    umem_numa_enabled ? "ENABLED" : "DISABLED");
#else
	printf("Per-CPU caching: NOT COMPILED\n");
#endif
	printf("System CPUs:     %d\n", (int)sysconf(_SC_NPROCESSORS_ONLN));
	printf("\n");

	/* Run benchmarks */
	if (run_all || run_single) {
		bench_single_thread(&config);
	}

	if (run_all || run_multi) {
		bench_multi_thread(&config);
	}

	if (run_all || run_contention) {
		bench_lock_contention(&config);
	}

	if (run_all || run_numa) {
		bench_numa_locality(&config);
	}

	if (run_all || run_size_sweep) {
		bench_size_sweep(&config);
	}

	if (run_all || run_thread_sweep) {
		bench_thread_sweep(&config);
	}

	return 0;
}

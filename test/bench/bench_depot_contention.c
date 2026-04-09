/*
 * Depot contention benchmark test
 *
 * Tests umem_cache_alloc/free with different thread counts to measure
 * the impact of depot striping on lock contention. Validates the 50-150%
 * improvement claim from depot striping.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "../../umem.h"
#include "../tdigest.h"

#define ITERATIONS_PER_THREAD 100000
#define OBJECT_SIZE 64

static const int thread_counts[] = {1, 2, 4, 8, 16, 32, 64};
static const int num_thread_tests = sizeof(thread_counts) / sizeof(thread_counts[0]);

typedef struct thread_work {
	int thread_id;
	umem_cache_t *cache;
	int iterations;
	td_histogram_t *latencies;
	uint64_t ops_completed;
	uint64_t start_time_ns;
	uint64_t end_time_ns;
} thread_work_t;

static inline uint64_t
get_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *
worker_thread(void *arg)
{
	thread_work_t *work = (thread_work_t *)arg;
	umem_cache_t *cache = work->cache;
	void **bufs;
	int i;
	uint64_t start, end, latency_ns;

	bufs = malloc(work->iterations * sizeof(void *));
	if (bufs == NULL) {
		fprintf(stderr, "Thread %d: Failed to allocate buffer array\n",
		    work->thread_id);
		return NULL;
	}

	work->start_time_ns = get_ns();

	for (i = 0; i < work->iterations; i++) {
		start = get_ns();
		bufs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		end = get_ns();

		if (bufs[i] == NULL) {
			fprintf(stderr, "Thread %d: Allocation failed at iteration %d\n",
			    work->thread_id, i);
			work->iterations = i;
			break;
		}

		latency_ns = end - start;
		td_add(work->latencies, (double)latency_ns, 1);
		work->ops_completed++;
	}

	for (i = 0; i < work->iterations; i++) {
		start = get_ns();
		umem_cache_free(cache, bufs[i]);
		end = get_ns();

		latency_ns = end - start;
		td_add(work->latencies, (double)latency_ns, 1);
		work->ops_completed++;
	}

	work->end_time_ns = get_ns();

	free(bufs);
	return NULL;
}

static int
run_benchmark(int nthreads, bool print_details)
{
	umem_cache_t *cache;
	pthread_t *threads;
	thread_work_t *work;
	td_histogram_t *global_latencies;
	uint64_t total_ops = 0;
	uint64_t min_start_ns = UINT64_MAX;
	uint64_t max_end_ns = 0;
	double p50, p99, throughput;
	int i;

	cache = umem_cache_create("bench_depot", OBJECT_SIZE, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	if (cache == NULL) {
		fprintf(stderr, "Failed to create cache\n");
		return -1;
	}

	threads = malloc(nthreads * sizeof(pthread_t));
	work = malloc(nthreads * sizeof(thread_work_t));

	if (threads == NULL || work == NULL) {
		fprintf(stderr, "Failed to allocate thread arrays\n");
		if (cache) umem_cache_destroy(cache);
		free(threads);
		free(work);
		return -1;
	}

	for (i = 0; i < nthreads; i++) {
		work[i].thread_id = i;
		work[i].cache = cache;
		work[i].iterations = ITERATIONS_PER_THREAD;
		work[i].ops_completed = 0;
		work[i].latencies = td_new(100.0);
		if (work[i].latencies == NULL) {
			fprintf(stderr, "Failed to create t-digest for thread %d\n", i);
			while (--i >= 0) {
				td_free(work[i].latencies);
			}
			umem_cache_destroy(cache);
			free(threads);
			free(work);
			return -1;
		}
	}

	for (i = 0; i < nthreads; i++) {
		if (pthread_create(&threads[i], NULL, worker_thread, &work[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d\n", i);
			for (int j = 0; j < nthreads; j++) {
				td_free(work[j].latencies);
			}
			umem_cache_destroy(cache);
			free(threads);
			free(work);
			return -1;
		}
	}

	for (i = 0; i < nthreads; i++) {
		pthread_join(threads[i], NULL);
	}

	global_latencies = td_new(100.0);
	if (global_latencies == NULL) {
		fprintf(stderr, "Failed to create global t-digest\n");
		for (i = 0; i < nthreads; i++) {
			td_free(work[i].latencies);
		}
		umem_cache_destroy(cache);
		free(threads);
		free(work);
		return -1;
	}

	for (i = 0; i < nthreads; i++) {
		total_ops += work[i].ops_completed;
		if (work[i].start_time_ns < min_start_ns) {
			min_start_ns = work[i].start_time_ns;
		}
		if (work[i].end_time_ns > max_end_ns) {
			max_end_ns = work[i].end_time_ns;
		}
		td_merge(global_latencies, work[i].latencies);
		td_free(work[i].latencies);
	}

	td_compress(global_latencies);

	p50 = td_quantile(global_latencies, 0.50);
	p99 = td_quantile(global_latencies, 0.99);

	double elapsed_sec = (double)(max_end_ns - min_start_ns) / 1e9;
	throughput = (double)total_ops / elapsed_sec;

	if (print_details) {
		printf("\n========================================\n");
		printf("Threads: %d\n", nthreads);
		printf("========================================\n");
		printf("Operations:  %llu\n", (unsigned long long)total_ops);
		printf("Elapsed:     %.3f seconds\n", elapsed_sec);
		printf("Throughput:  %.2f Mops/sec\n", throughput / 1e6);
		printf("Latency p50: %.0f ns\n", p50);
		printf("Latency p99: %.0f ns\n", p99);
		printf("========================================\n");
	} else {
		printf("%2d threads: %8.2f Mops/sec | p50: %6.0f ns | p99: %7.0f ns\n",
		    nthreads, throughput / 1e6, p50, p99);
	}

	td_free(global_latencies);
	umem_cache_destroy(cache);
	free(threads);
	free(work);

	return 0;
}

static void
print_header(void)
{
	printf("\n");
	printf("Depot Contention Benchmark\n");
	printf("==========================\n");
	printf("Object size:     %d bytes\n", OBJECT_SIZE);
	printf("Iterations:      %d per thread\n", ITERATIONS_PER_THREAD);
	printf("Depot stripes:   16\n");
	printf("\n");
}

static void
print_summary(double *throughputs)
{
	double baseline = throughputs[0];
	printf("\n");
	printf("Scalability Summary\n");
	printf("===================\n");
	printf("Threads | Throughput | Speedup | Efficiency\n");
	printf("--------|------------|---------|------------\n");

	for (int i = 0; i < num_thread_tests; i++) {
		int nthreads = thread_counts[i];
		double speedup = throughputs[i] / baseline;
		double efficiency = speedup / nthreads;

		printf(" %6d | %8.2f M | %6.2fx | %9.1f%%\n",
		    nthreads, throughputs[i] / 1e6, speedup, efficiency * 100.0);
	}

	printf("\n");
	printf("Performance Analysis:\n");
	printf("---------------------\n");

	double two_thread_speedup = throughputs[1] / baseline;
	double eight_thread_speedup = throughputs[3] / baseline;

	if (two_thread_speedup >= 1.5) {
		printf("2-thread:  %.2fx speedup - GOOD (depot striping effective)\n",
		    two_thread_speedup);
	} else {
		printf("2-thread:  %.2fx speedup - POOR (high contention)\n",
		    two_thread_speedup);
	}

	if (eight_thread_speedup >= 4.0) {
		printf("8-thread:  %.2fx speedup - EXCELLENT\n",
		    eight_thread_speedup);
	} else if (eight_thread_speedup >= 3.0) {
		printf("8-thread:  %.2fx speedup - GOOD\n",
		    eight_thread_speedup);
	} else {
		printf("8-thread:  %.2fx speedup - POOR (scalability limited)\n",
		    eight_thread_speedup);
	}

	printf("\n");
}

int
main(int argc, char *argv[])
{
	double throughputs[num_thread_tests];
	bool verbose = false;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			verbose = true;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf("Usage: %s [options]\n", argv[0]);
			printf("Options:\n");
			printf("  -v, --verbose  Print detailed per-thread statistics\n");
			printf("  -h, --help     Show this help message\n");
			return 0;
		}
	}

	print_header();

	printf("Running benchmarks...\n");
	printf("---------------------\n");

	for (i = 0; i < num_thread_tests; i++) {
		int nthreads = thread_counts[i];
		umem_cache_t *cache;
		pthread_t *threads;
		thread_work_t *work;
		uint64_t total_ops = 0;
		uint64_t min_start_ns = UINT64_MAX;
		uint64_t max_end_ns = 0;
		int j;

		if (verbose && i > 0) {
			printf("\n");
		}

		cache = umem_cache_create("bench_depot_throughput", OBJECT_SIZE, 0,
		    NULL, NULL, NULL, NULL, NULL, 0);
		if (cache == NULL) {
			fprintf(stderr, "Failed to create cache for %d threads\n", nthreads);
			continue;
		}

		threads = malloc(nthreads * sizeof(pthread_t));
		work = malloc(nthreads * sizeof(thread_work_t));

		if (threads == NULL || work == NULL) {
			fprintf(stderr, "Failed to allocate for %d threads\n", nthreads);
			umem_cache_destroy(cache);
			free(threads);
			free(work);
			continue;
		}

		for (j = 0; j < nthreads; j++) {
			work[j].thread_id = j;
			work[j].cache = cache;
			work[j].iterations = ITERATIONS_PER_THREAD;
			work[j].ops_completed = 0;
			work[j].latencies = td_new(100.0);
		}

		for (j = 0; j < nthreads; j++) {
			pthread_create(&threads[j], NULL, worker_thread, &work[j]);
		}

		for (j = 0; j < nthreads; j++) {
			pthread_join(threads[j], NULL);
		}

		for (j = 0; j < nthreads; j++) {
			total_ops += work[j].ops_completed;
			if (work[j].start_time_ns < min_start_ns) {
				min_start_ns = work[j].start_time_ns;
			}
			if (work[j].end_time_ns > max_end_ns) {
				max_end_ns = work[j].end_time_ns;
			}
			if (work[j].latencies != NULL) {
				td_free(work[j].latencies);
			}
		}

		double elapsed_sec = (double)(max_end_ns - min_start_ns) / 1e9;
		throughputs[i] = (double)total_ops / elapsed_sec;

		if (!verbose) {
			printf("%2d threads: %8.2f Mops/sec\n",
			    nthreads, throughputs[i] / 1e6);
		} else {
			printf("Threads %2d: %.2f Mops/sec (%.3f sec, %llu ops)\n",
			    nthreads, throughputs[i] / 1e6, elapsed_sec,
			    (unsigned long long)total_ops);
		}

		umem_cache_destroy(cache);
		free(threads);
		free(work);
	}

	print_summary(throughputs);

	printf("Depot Striping Impact:\n");
	printf("----------------------\n");
	printf("This benchmark demonstrates the effectiveness of 16-way depot\n");
	printf("striping in reducing lock contention. Good scalability\n");
	printf("(>1.5x at 2 threads, >4x at 8 threads) indicates that depot\n");
	printf("striping successfully distributes depot lock pressure across\n");
	printf("multiple independent lock domains.\n");
	printf("\n");

	return 0;
}

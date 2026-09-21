/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * Benchmark for experimental optimization features
 *
 * Tests rseq, NUMA, and HTM performance improvements.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "umem.h"

#ifdef UMEM_RSEQ_AVAILABLE
#include "umem_rseq.h"
#endif

#ifdef UMEM_NUMA_AVAILABLE
#include "umem_numa.h"
#endif

#ifdef UMEM_HTM_AVAILABLE
#include "umem_htm.h"
#endif

#define NUM_THREADS 16
#define OPS_PER_THREAD 1000000
#define ALLOC_SIZE 64

typedef struct {
	int thread_id;
	umem_cache_t *cache;
	uint64_t ops_completed;
	double elapsed_time;
} thread_data_t;

static void *
benchmark_thread(void *arg)
{
	thread_data_t *data = (thread_data_t *)arg;
	umem_cache_t *cache = data->cache;
	struct timespec start, end;
	void *ptrs[100];
	int i, j;

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (i = 0; i < OPS_PER_THREAD / 100; i++) {
		/* Allocate 100 objects */
		for (j = 0; j < 100; j++) {
			ptrs[j] = umem_cache_alloc(cache, UMEM_DEFAULT);
			if (ptrs[j] == NULL) {
				fprintf(stderr, "Allocation failed\n");
				return NULL;
			}
		}

		/* Free 100 objects */
		for (j = 0; j < 100; j++) {
			umem_cache_free(cache, ptrs[j]);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	data->ops_completed = OPS_PER_THREAD;
	data->elapsed_time = (end.tv_sec - start.tv_sec) +
	    (end.tv_nsec - start.tv_nsec) / 1e9;

	return NULL;
}

static void
run_benchmark(const char *name)
{
	pthread_t threads[NUM_THREADS];
	thread_data_t thread_data[NUM_THREADS];
	umem_cache_t *cache;
	struct timespec start, end;
	double total_time, total_ops;
	int i;

	printf("\n=== %s ===\n", name);

	/* Create cache */
	cache = umem_cache_create("bench_cache", ALLOC_SIZE, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	if (cache == NULL) {
		fprintf(stderr, "Failed to create cache\n");
		return;
	}

	/* Start threads */
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (i = 0; i < NUM_THREADS; i++) {
		thread_data[i].thread_id = i;
		thread_data[i].cache = cache;
		thread_data[i].ops_completed = 0;
		thread_data[i].elapsed_time = 0.0;

		if (pthread_create(&threads[i], NULL, benchmark_thread,
		    &thread_data[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d\n", i);
			return;
		}
	}

	/* Wait for threads */
	for (i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	total_time = (end.tv_sec - start.tv_sec) +
	    (end.tv_nsec - start.tv_nsec) / 1e9;

	/* Calculate statistics */
	total_ops = 0;
	for (i = 0; i < NUM_THREADS; i++) {
		total_ops += thread_data[i].ops_completed;
	}

	printf("Total time: %.3f seconds\n", total_time);
	printf("Total operations: %.0f\n", total_ops);
	printf("Throughput: %.2f Mops/s\n", total_ops / total_time / 1e6);
	printf("Per-thread average: %.2f Kops/s\n",
	    total_ops / NUM_THREADS / total_time / 1e3);

	/* Destroy cache */
	umem_cache_destroy(cache);
}

int
main(int argc, char **argv)
{
	printf("Experimental Features Benchmark\n");
	printf("================================\n");

	/* Check available features */
	printf("\nAvailable features:\n");

#ifdef UMEM_RSEQ_AVAILABLE
	if (umem_rseq_available()) {
		printf("  - RSEQ (Restartable Sequences)\n");
	}
#endif

#ifdef UMEM_NUMA_AVAILABLE
	if (umem_numa_available()) {
		printf("  - NUMA-aware allocation\n");
	}
#endif

#ifdef UMEM_HTM_AVAILABLE
	if (umem_htm_available()) {
		printf("  - HTM (Hardware Transactional Memory)\n");
	}
#endif

	/* Run baseline benchmark */
	run_benchmark("Baseline (standard allocation)");

#ifdef UMEM_RSEQ_AVAILABLE
	/* Test RSEQ if available */
	if (umem_rseq_available()) {
		if (umem_rseq_init() == 0) {
			run_benchmark("With RSEQ enabled");
			umem_rseq_dump();
			umem_rseq_fini();
		}
	}
#endif

#ifdef UMEM_NUMA_AVAILABLE
	/* Test NUMA if available */
	if (umem_numa_available()) {
		if (umem_numa_init() == 0) {
			run_benchmark("With NUMA awareness");
			umem_numa_dump_topology();
			umem_numa_fini();
		}
	}
#endif

#ifdef UMEM_HTM_AVAILABLE
	/* Test HTM if available */
	if (umem_htm_available()) {
		if (umem_htm_init() == 0) {
			run_benchmark("With HTM enabled");
			umem_htm_dump();
			umem_htm_fini();
		}
	}
#endif

	printf("\nBenchmark complete.\n");
	return 0;
}

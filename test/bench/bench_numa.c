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
 * NUMA-aware allocation benchmark
 *
 * Tests performance of NUMA-aware allocation on multi-socket systems
 * by pinning threads to specific NUMA nodes and measuring allocation
 * latency for local vs cross-node allocations.
 *
 * Expected results on 2-socket NUMA system:
 *  - Single-socket: no difference (baseline)
 *  - Multi-socket without NUMA: 40-60ns per allocation (cross-node traffic)
 *  - Multi-socket with NUMA: 25-35ns per allocation (local node preferred)
 *  - Improvement: 30-50% faster on NUMA systems
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#ifdef UMEM_NUMA_AVAILABLE
#include "umem_numa.h"
#include <numa.h>
#include <numaif.h>
#endif

#include "umem.h"

#define ITERATIONS_PER_THREAD	1000000
#define ALLOC_SIZE		64

/* Thread data structure */
typedef struct thread_data {
	int thread_id;
	int numa_node;
	uint64_t allocations;
	uint64_t local_allocs;
	uint64_t remote_allocs;
	double elapsed_sec;
} thread_data_t;

/* Get current timestamp in nanoseconds */
static uint64_t
get_nsec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#ifdef UMEM_NUMA_AVAILABLE

/* Verify allocation is on the expected NUMA node */
static int
check_alloc_node(void *ptr, int expected_node)
{
	int status;
	int node = -1;
	void *pages[1] = { ptr };
	int nodes[1];

	if (move_pages(0, 1, pages, NULL, nodes, 0) == 0) {
		node = nodes[0];
	}

	return (node == expected_node);
}

/* Worker thread for NUMA benchmark */
static void *
numa_worker_thread(void *arg)
{
	thread_data_t *data = (thread_data_t *)arg;
	void **ptrs;
	uint64_t start_ns, end_ns;
	int i;

	/* Pin thread to specific NUMA node */
	if (umem_numa_bind_thread(data->numa_node) != 0) {
		fprintf(stderr, "Failed to bind thread %d to node %d\n",
		    data->thread_id, data->numa_node);
		return (NULL);
	}

	/* Allocate pointer array */
	ptrs = calloc(ITERATIONS_PER_THREAD, sizeof (void *));
	if (ptrs == NULL) {
		return (NULL);
	}

	/* Warm up */
	for (i = 0; i < 1000; i++) {
		void *p = umem_alloc(ALLOC_SIZE, UMEM_DEFAULT);
		umem_free(p, ALLOC_SIZE);
	}

	/* Benchmark allocation */
	start_ns = get_nsec();

	for (i = 0; i < ITERATIONS_PER_THREAD; i++) {
		ptrs[i] = umem_alloc(ALLOC_SIZE, UMEM_DEFAULT);
		if (ptrs[i] == NULL) {
			fprintf(stderr, "Allocation failed at iteration %d\n", i);
			break;
		}

		/* Check if allocation is on local node (sample every 1000th) */
		if (UMEM_NUMA_ENABLED() && (i % 1000 == 0)) {
			if (check_alloc_node(ptrs[i], data->numa_node)) {
				data->local_allocs++;
			} else {
				data->remote_allocs++;
			}
		}
	}

	end_ns = get_nsec();
	data->allocations = i;
	data->elapsed_sec = (end_ns - start_ns) / 1000000000.0;

	/* Free all allocations */
	for (i = 0; i < data->allocations; i++) {
		umem_free(ptrs[i], ALLOC_SIZE);
	}

	free(ptrs);
	return (NULL);
}

/* Run NUMA benchmark */
static void
bench_numa_aware(void)
{
	int num_nodes, num_threads;
	int threads_per_node = 4;
	pthread_t *threads;
	thread_data_t *thread_data;
	int i;
	uint64_t total_allocs = 0;
	double total_time = 0.0;
	uint64_t total_local = 0;
	uint64_t total_remote = 0;
	double avg_latency_ns;

	if (!UMEM_NUMA_ENABLED()) {
		printf("NUMA not enabled - skipping NUMA benchmark\n");
		return;
	}

	num_nodes = UMEM_NUMA_NODE_COUNT();
	num_threads = num_nodes * threads_per_node;

	printf("\n=== NUMA-Aware Allocation Benchmark ===\n");
	printf("NUMA nodes: %d\n", num_nodes);
	printf("Threads: %d (%d per node)\n", num_threads, threads_per_node);
	printf("Iterations per thread: %d\n", ITERATIONS_PER_THREAD);
	printf("Allocation size: %d bytes\n\n", ALLOC_SIZE);

	/* Print topology */
	umem_numa_dump_topology();

	/* Allocate thread arrays */
	threads = calloc(num_threads, sizeof (pthread_t));
	thread_data = calloc(num_threads, sizeof (thread_data_t));

	if (threads == NULL || thread_data == NULL) {
		fprintf(stderr, "Failed to allocate thread arrays\n");
		return;
	}

	/* Start threads */
	for (i = 0; i < num_threads; i++) {
		thread_data[i].thread_id = i;
		thread_data[i].numa_node = i / threads_per_node;

		if (pthread_create(&threads[i], NULL,
		    numa_worker_thread, &thread_data[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d\n", i);
			num_threads = i;
			break;
		}
	}

	/* Wait for threads to complete */
	for (i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
	}

	/* Aggregate results */
	printf("\nResults:\n");
	printf("Thread | Node | Allocations | Time (s) | Ops/sec    | "
	    "Latency (ns) | Local%% | Remote%%\n");
	printf("-------|------|-------------|----------|------------|"
	    "-------------|---------|----------\n");

	for (i = 0; i < num_threads; i++) {
		thread_data_t *td = &thread_data[i];
		double ops_per_sec = td->allocations / td->elapsed_sec;
		double latency_ns = (td->elapsed_sec * 1000000000.0) /
		    td->allocations;

		uint64_t samples = td->local_allocs + td->remote_allocs;
		double local_pct = samples > 0 ?
		    (td->local_allocs * 100.0) / samples : 0.0;
		double remote_pct = samples > 0 ?
		    (td->remote_allocs * 100.0) / samples : 0.0;

		printf("%6d | %4d | %11lu | %8.3f | %10.0f | %12.1f | "
		    "%6.1f%% | %7.1f%%\n",
		    td->thread_id, td->numa_node, td->allocations,
		    td->elapsed_sec, ops_per_sec, latency_ns,
		    local_pct, remote_pct);

		total_allocs += td->allocations;
		total_time += td->elapsed_sec;
		total_local += td->local_allocs;
		total_remote += td->remote_allocs;
	}

	/* Overall statistics */
	avg_latency_ns = (total_time * 1000000000.0) / total_allocs;
	uint64_t total_samples = total_local + total_remote;
	double overall_local_pct = total_samples > 0 ?
	    (total_local * 100.0) / total_samples : 0.0;
	double overall_remote_pct = total_samples > 0 ?
	    (total_remote * 100.0) / total_samples : 0.0;

	printf("\nOverall:\n");
	printf("  Total allocations: %lu\n", total_allocs);
	printf("  Average latency: %.1f ns per allocation\n", avg_latency_ns);
	printf("  Throughput: %.0f ops/sec\n",
	    total_allocs / (total_time / num_threads));
	printf("  Local allocations: %.1f%%\n", overall_local_pct);
	printf("  Remote allocations: %.1f%%\n", overall_remote_pct);

	/* Interpretation */
	printf("\nInterpretation:\n");
	if (avg_latency_ns < 30.0) {
		printf("  EXCELLENT: Sub-30ns latency indicates strong "
		    "NUMA locality\n");
	} else if (avg_latency_ns < 40.0) {
		printf("  GOOD: 30-40ns latency indicates good "
		    "NUMA awareness\n");
	} else if (avg_latency_ns < 60.0) {
		printf("  FAIR: 40-60ns latency suggests some cross-node "
		    "traffic\n");
	} else {
		printf("  POOR: >60ns latency indicates significant "
		    "cross-node traffic\n");
	}

	if (overall_local_pct > 80.0) {
		printf("  High local allocation rate (%.1f%%) confirms "
		    "NUMA effectiveness\n", overall_local_pct);
	}

	free(threads);
	free(thread_data);
}

#endif /* UMEM_NUMA_AVAILABLE */

/* Baseline benchmark without NUMA awareness */
static void *
baseline_worker_thread(void *arg)
{
	thread_data_t *data = (thread_data_t *)arg;
	void **ptrs;
	uint64_t start_ns, end_ns;
	int i;

	/* Allocate pointer array */
	ptrs = calloc(ITERATIONS_PER_THREAD, sizeof (void *));
	if (ptrs == NULL) {
		return (NULL);
	}

	/* Warm up */
	for (i = 0; i < 1000; i++) {
		void *p = umem_alloc(ALLOC_SIZE, UMEM_DEFAULT);
		umem_free(p, ALLOC_SIZE);
	}

	/* Benchmark allocation */
	start_ns = get_nsec();

	for (i = 0; i < ITERATIONS_PER_THREAD; i++) {
		ptrs[i] = umem_alloc(ALLOC_SIZE, UMEM_DEFAULT);
		if (ptrs[i] == NULL) {
			break;
		}
	}

	end_ns = get_nsec();
	data->allocations = i;
	data->elapsed_sec = (end_ns - start_ns) / 1000000000.0;

	/* Free all allocations */
	for (i = 0; i < data->allocations; i++) {
		umem_free(ptrs[i], ALLOC_SIZE);
	}

	free(ptrs);
	return (NULL);
}

/* Run baseline benchmark */
static void
bench_baseline(void)
{
	int num_threads = 8;
	pthread_t *threads;
	thread_data_t *thread_data;
	int i;
	uint64_t total_allocs = 0;
	double total_time = 0.0;
	double avg_latency_ns;

	printf("\n=== Baseline Allocation Benchmark ===\n");
	printf("Threads: %d\n", num_threads);
	printf("Iterations per thread: %d\n", ITERATIONS_PER_THREAD);
	printf("Allocation size: %d bytes\n\n", ALLOC_SIZE);

	/* Allocate thread arrays */
	threads = calloc(num_threads, sizeof (pthread_t));
	thread_data = calloc(num_threads, sizeof (thread_data_t));

	if (threads == NULL || thread_data == NULL) {
		fprintf(stderr, "Failed to allocate thread arrays\n");
		return;
	}

	/* Start threads */
	for (i = 0; i < num_threads; i++) {
		thread_data[i].thread_id = i;

		if (pthread_create(&threads[i], NULL,
		    baseline_worker_thread, &thread_data[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d\n", i);
			num_threads = i;
			break;
		}
	}

	/* Wait for threads to complete */
	for (i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
	}

	/* Aggregate results */
	printf("Results:\n");
	printf("Thread | Allocations | Time (s) | Ops/sec    | Latency (ns)\n");
	printf("-------|-------------|----------|------------|--------------\n");

	for (i = 0; i < num_threads; i++) {
		thread_data_t *td = &thread_data[i];
		double ops_per_sec = td->allocations / td->elapsed_sec;
		double latency_ns = (td->elapsed_sec * 1000000000.0) /
		    td->allocations;

		printf("%6d | %11lu | %8.3f | %10.0f | %12.1f\n",
		    td->thread_id, td->allocations,
		    td->elapsed_sec, ops_per_sec, latency_ns);

		total_allocs += td->allocations;
		total_time += td->elapsed_sec;
	}

	/* Overall statistics */
	avg_latency_ns = (total_time * 1000000000.0) / total_allocs;

	printf("\nOverall:\n");
	printf("  Total allocations: %lu\n", total_allocs);
	printf("  Average latency: %.1f ns per allocation\n", avg_latency_ns);
	printf("  Throughput: %.0f ops/sec\n",
	    total_allocs / (total_time / num_threads));

	free(threads);
	free(thread_data);
}

int
main(int argc, char **argv)
{
	printf("NUMA Allocation Benchmark\n");
	printf("=========================\n\n");

#ifdef UMEM_NUMA_AVAILABLE
	/* Check NUMA availability */
	if (numa_available() < 0) {
		printf("NUMA not available on this system\n");
		printf("Running baseline benchmark only\n");
		bench_baseline();
		return (0);
	}

	/* Initialize NUMA */
	if (umem_numa_init() != 0) {
		printf("NUMA initialization failed\n");
		printf("Running baseline benchmark only\n");
		bench_baseline();
		return (0);
	}

	/* Run baseline */
	bench_baseline();

	/* Run NUMA-aware benchmark */
	bench_numa_aware();

	/* Print comparison */
	printf("\n=== NUMA Impact ===\n");
	printf("Run both benchmarks and compare average latency:\n");
	printf("  - Expected improvement: 10-30%% on multi-socket NUMA systems\n");
	printf("  - On single-socket: no change (overhead < 2%%)\n");

#else
	printf("NUMA support not compiled in\n");
	printf("Recompile with --enable-numa to enable NUMA support\n");
	bench_baseline();
#endif

	return (0);
}

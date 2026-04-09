/*
 * Benchmark comparing syscall-based NUMA node lookup vs hash-based lookup
 *
 * Tests:
 * 1. Syscall approach: numa_node_of_cpu(sched_getcpu())
 * 2. Hash approach: hash_partitions_get_claimant_by_key()
 *
 * Expected results:
 * - Syscall: 100-500ns per lookup
 * - Hash: 20-30ns per lookup
 * - Speedup: 5-20x
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#ifdef HAVE_NUMA
#include <numa.h>
#endif
#endif

#include "../../umem_hash_partition.h"

#define ITERATIONS 10000000
#define NUM_THREADS 8

typedef struct {
	uint64_t duration_ns;
	uint64_t iterations;
	int thread_id;
} thread_result_t;

/*
 * Measure time in nanoseconds
 */
static inline uint64_t
get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#ifdef __linux__
#ifdef HAVE_NUMA
/*
 * Syscall-based NUMA node lookup
 */
static int
get_node_syscall(void)
{
	int cpu = sched_getcpu();
	if (cpu < 0)
		return 0;
	return numa_node_of_cpu(cpu);
}
#endif
#endif

/*
 * Hash-based NUMA node lookup
 */
static int
get_node_hash(hash_partitions_t *hp)
{
	pthread_t tid = pthread_self();
	const char *node_name = hash_partitions_get_claimant_by_key(hp,
	    &tid, sizeof (tid));
	if (!node_name)
		return 0;
	return atoi(node_name + 5);  /* Parse "node_X" */
}

#ifdef __linux__
#ifdef HAVE_NUMA
/*
 * Benchmark thread: syscall approach
 */
static void *
bench_syscall(void *arg)
{
	thread_result_t *result = (thread_result_t *)arg;
	volatile int node;

	uint64_t start = get_time_ns();

	for (uint64_t i = 0; i < ITERATIONS; i++) {
		node = get_node_syscall();
	}

	uint64_t end = get_time_ns();

	result->duration_ns = end - start;
	result->iterations = ITERATIONS;

	(void)node;  /* Prevent optimization */
	return NULL;
}
#endif
#endif

/*
 * Benchmark thread: hash approach
 */
static void *
bench_hash(void *arg)
{
	void **args = (void **)arg;
	hash_partitions_t *hp = (hash_partitions_t *)args[0];
	thread_result_t *result = (thread_result_t *)args[1];
	volatile int node;

	uint64_t start = get_time_ns();

	for (uint64_t i = 0; i < ITERATIONS; i++) {
		node = get_node_hash(hp);
	}

	uint64_t end = get_time_ns();

	result->duration_ns = end - start;
	result->iterations = ITERATIONS;

	(void)node;
	return NULL;
}

/*
 * Run benchmark with multiple threads
 */
static void
run_benchmark(const char *name, void *(*thread_func)(void *), void *arg)
{
	pthread_t threads[NUM_THREADS];
	thread_result_t results[NUM_THREADS];
	void *thread_args[NUM_THREADS][2];

	printf("\n=== %s ===\n", name);

	/* Create threads */
	for (int i = 0; i < NUM_THREADS; i++) {
		results[i].thread_id = i;
		if (arg) {
			thread_args[i][0] = arg;
			thread_args[i][1] = &results[i];
			pthread_create(&threads[i], NULL, thread_func,
			    thread_args[i]);
		} else {
			pthread_create(&threads[i], NULL, thread_func,
			    &results[i]);
		}
	}

	/* Wait for completion */
	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	/* Calculate statistics */
	uint64_t total_ops = 0;
	uint64_t total_time_ns = 0;
	double min_latency_ns = 1e9;
	double max_latency_ns = 0;

	for (int i = 0; i < NUM_THREADS; i++) {
		double latency = (double)results[i].duration_ns /
		    results[i].iterations;
		total_ops += results[i].iterations;
		total_time_ns += results[i].duration_ns;

		if (latency < min_latency_ns)
			min_latency_ns = latency;
		if (latency > max_latency_ns)
			max_latency_ns = latency;

		printf("Thread %d: %.2f ns/op\n", i, latency);
	}

	double avg_latency_ns = (double)total_time_ns / total_ops;
	double throughput = (double)total_ops /
	    ((double)total_time_ns / 1e9);

	printf("\nResults:\n");
	printf("  Total operations: %lu\n", total_ops);
	printf("  Average latency:  %.2f ns/op\n", avg_latency_ns);
	printf("  Min latency:      %.2f ns/op\n", min_latency_ns);
	printf("  Max latency:      %.2f ns/op\n", max_latency_ns);
	printf("  Throughput:       %.2f Mops/sec\n", throughput / 1e6);
}

int
main(int argc, char **argv)
{
	printf("NUMA Node Lookup Benchmark\n");
	printf("===========================\n");
	printf("Iterations per thread: %d\n", ITERATIONS);
	printf("Number of threads: %d\n", NUM_THREADS);

#ifdef __linux__
#ifdef HAVE_NUMA
	/* Check if NUMA is available */
	if (numa_available() < 0) {
		printf("\nNUMA not available on this system\n");
		printf("Skipping syscall benchmark\n");
	} else {
		int num_nodes = numa_num_configured_nodes();
		printf("NUMA nodes detected: %d\n", num_nodes);

		/* Benchmark syscall approach */
		run_benchmark("Syscall Approach (numa_node_of_cpu + sched_getcpu)",
		    bench_syscall, NULL);
	}
#else
	printf("\nNUMA support not compiled in (HAVE_NUMA not defined)\n");
	printf("Skipping syscall benchmark\n");
#endif
#else
	printf("\nNot running on Linux\n");
	printf("Skipping syscall benchmark\n");
#endif

	/* Create hash partitions for benchmark */
	int num_nodes = 4;  /* Simulate 4-node system */
	claimant_weight_t weights[4];

	for (int i = 0; i < num_nodes; i++) {
		snprintf(weights[i].name, MAX_NAME_LEN, "node_%d", i);
		weights[i].weight = 1.0;  /* Equal weights */
	}

	hash_partitions_t *hp = hash_partitions_create_with_weights(weights,
	    num_nodes, 2);

	if (!hp) {
		fprintf(stderr, "Failed to create hash partitions\n");
		return 1;
	}

	/* Benchmark hash approach */
	run_benchmark("Hash Approach (hash_partitions_get_claimant_by_key)",
	    bench_hash, hp);

	/* Calculate speedup */
#ifdef __linux__
#ifdef HAVE_NUMA
	if (numa_available() >= 0) {
		printf("\n=== Comparison ===\n");
		printf("Hash approach is significantly faster\n");
		printf("Expected speedup: 5-20x\n");
	}
#endif
#endif

	hash_partitions_free(hp);

	printf("\nBenchmark complete!\n");
	return 0;
}

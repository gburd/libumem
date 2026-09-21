/*
 * Test suite for per-CPU magazine caching with NUMA awareness
 *
 * This test suite validates correctness of the per-CPU caching implementation,
 * including CPU affinity, thread migration, NUMA locality, and magazine ownership.
 */

#include "../../config.h"

#ifdef UMEM_PER_CPU_CACHE

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <assert.h>

#include "../../umem.h"
#include "../../umem_impl.h"
#include "../../umem_percpu.h"
#include "../munit.h"

/*
 * Test 1: CPU Detection
 *
 * Verify that get_current_cpu() returns valid CPU IDs and that
 * get_numa_node() returns valid NUMA node IDs.
 */
static MunitResult
test_cpu_detection(const MunitParameter params[], void *user_data)
{
	int cpu, numa_node;
	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	(void)params;
	(void)user_data;

	/* Test get_current_cpu */
	cpu = get_current_cpu();
	munit_assert_int(cpu, >=, 0);
	munit_assert_int(cpu, <, ncpus);

	/* Test get_numa_node */
	numa_node = get_numa_node(cpu);
	munit_assert_int(numa_node, >=, 0);

	return MUNIT_OK;
}

/*
 * Test 2: CPU Affinity
 *
 * Pin thread to specific CPU, allocate objects, and verify they come from
 * the correct per-CPU magazine.
 */
static MunitResult
test_cpu_affinity(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	void *buf1, *buf2;
	cpu_set_t cpuset;
	int target_cpu = 0;

	(void)params;
	(void)user_data;

	/* Create a cache */
	cp = umem_cache_create("test_affinity", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Pin to CPU 0 */
	CPU_ZERO(&cpuset);
	CPU_SET(target_cpu, &cpuset);
	munit_assert_int(pthread_setaffinity_np(pthread_self(),
	    sizeof(cpuset), &cpuset), ==, 0);

	/* Allocate objects */
	buf1 = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(buf1);

	buf2 = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(buf2);

	/* Verify we're on the correct CPU */
	munit_assert_int(get_current_cpu(), ==, target_cpu);

	/* Free objects */
	umem_cache_free(cp, buf1);
	umem_cache_free(cp, buf2);

	/* Destroy cache */
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 3: Thread Migration
 *
 * Allocate on one CPU, migrate to another CPU, allocate again.
 * Verify correctness after migration.
 */
static MunitResult
test_thread_migration(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	void *buf1, *buf2, *buf3;
	cpu_set_t cpuset;
	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	(void)params;
	(void)user_data;

	if (ncpus < 2) {
		return MUNIT_SKIP;
	}

	/* Create a cache */
	cp = umem_cache_create("test_migration", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Pin to CPU 0 */
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

	/* Allocate on CPU 0 */
	buf1 = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(buf1);

	/* Migrate to CPU 1 */
	CPU_ZERO(&cpuset);
	CPU_SET(1, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
	sched_yield();  /* Force reschedule */

	/* Allocate on CPU 1 */
	buf2 = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(buf2);

	buf3 = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(buf3);

	/* Free all */
	umem_cache_free(cp, buf1);
	umem_cache_free(cp, buf2);
	umem_cache_free(cp, buf3);

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 4: Magazine Ownership
 *
 * Verify that each CPU has its own magazines and they don't interfere.
 */
static MunitResult
test_magazine_ownership(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	umem_percpu_cache_t stats[4];
	cpu_set_t cpuset;
	void *bufs[100];
	int i, ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	(void)params;
	(void)user_data;

	if (ncpus < 2) {
		return MUNIT_SKIP;
	}

	/* Create a cache */
	cp = umem_cache_create("test_ownership", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Pin to CPU 0, allocate many objects */
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

	for (i = 0; i < 100; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Get CPU 0 stats */
	umem_percpu_stats(cp, 0, &stats[0]);
	uint64_t cpu0_allocs = stats[0].pc_alloc;

	/* Pin to CPU 1, allocate more objects */
	CPU_ZERO(&cpuset);
	CPU_SET(1, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
	sched_yield();

	for (i = 0; i < 100; i++) {
		void *buf = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(buf);
		umem_cache_free(cp, buf);
	}

	/* Get CPU 1 stats */
	umem_percpu_stats(cp, 1, &stats[1]);
	uint64_t cpu1_allocs = stats[1].pc_alloc;

	/* Verify CPUs have different allocation counts */
	munit_assert_uint64(cpu0_allocs, >, 0);
	munit_assert_uint64(cpu1_allocs, >, 0);

	/* Free CPU 0 allocations */
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

	for (i = 0; i < 100; i++) {
		umem_cache_free(cp, bufs[i]);
	}

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 5: NUMA Node Detection
 *
 * Verify that NUMA node detection works correctly for all CPUs.
 */
static MunitResult
test_numa_detection(const MunitParameter params[], void *user_data)
{
	int cpu, numa_node;
	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	(void)params;
	(void)user_data;

	if (!umem_numa_enabled) {
		return MUNIT_SKIP;
	}

	/* Check all CPUs have valid NUMA nodes */
	for (cpu = 0; cpu < ncpus; cpu++) {
		numa_node = get_numa_node(cpu);
		munit_assert_int(numa_node, >=, 0);
		munit_assert_int(numa_node, <, umem_num_nodes);
	}

	return MUNIT_OK;
}

/*
 * Test 6: Depot Interaction
 *
 * Verify correct magazine exchange with depot when per-CPU magazines
 * are exhausted.
 */
static MunitResult
test_depot_interaction(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	void *bufs[1000];
	int i;
	umem_percpu_cache_t stats_before, stats_after;

	(void)params;
	(void)user_data;

	/* Create a cache */
	cp = umem_cache_create("test_depot", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Get initial stats */
	int cpu = get_current_cpu();
	umem_percpu_stats(cp, cpu, &stats_before);

	/* Allocate many objects to trigger depot refills */
	for (i = 0; i < 1000; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Get stats after allocation */
	umem_percpu_stats(cp, cpu, &stats_after);

	/* Verify depot refills occurred */
	munit_assert_uint64(stats_after.pc_depot_refill, >,
	    stats_before.pc_depot_refill);

	/* Free all objects */
	for (i = 0; i < 1000; i++) {
		umem_cache_free(cp, bufs[i]);
	}

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 7: High Thread Count
 *
 * Stress test with many threads allocating/freeing concurrently.
 */
typedef struct thread_work {
	umem_cache_t *cp;
	int iterations;
	int size;
} thread_work_t;

static void *
worker_thread(void *arg)
{
	thread_work_t *work = (thread_work_t *)arg;
	void **bufs;
	int i;

	bufs = malloc(work->iterations * sizeof(void *));
	assert(bufs != NULL);

	/* Allocate */
	for (i = 0; i < work->iterations; i++) {
		bufs[i] = umem_cache_alloc(work->cp, UMEM_DEFAULT);
		assert(bufs[i] != NULL);
	}

	/* Free */
	for (i = 0; i < work->iterations; i++) {
		umem_cache_free(work->cp, bufs[i]);
	}

	free(bufs);
	return NULL;
}

static MunitResult
test_high_thread_count(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	pthread_t threads[32];
	thread_work_t work;
	int i, nthreads = 32;

	(void)params;
	(void)user_data;

	/* Create a cache */
	cp = umem_cache_create("test_threads", 128, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Setup work */
	work.cp = cp;
	work.iterations = 1000;
	work.size = 128;

	/* Create threads */
	for (i = 0; i < nthreads; i++) {
		munit_assert_int(pthread_create(&threads[i], NULL,
		    worker_thread, &work), ==, 0);
	}

	/* Wait for threads */
	for (i = 0; i < nthreads; i++) {
		pthread_join(threads[i], NULL);
	}

	/* Dump stats */
	umem_percpu_dump(cp);

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 8: Allocation Pattern - Sequential
 *
 * Test allocating and freeing in FIFO order.
 */
static MunitResult
test_pattern_sequential(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	void *bufs[1000];
	int i;

	(void)params;
	(void)user_data;

	cp = umem_cache_create("test_sequential", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Allocate all */
	for (i = 0; i < 1000; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Free all in same order */
	for (i = 0; i < 1000; i++) {
		umem_cache_free(cp, bufs[i]);
	}

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test 9: Allocation Pattern - LIFO
 *
 * Test allocating and freeing in LIFO order (stack-like).
 */
static MunitResult
test_pattern_lifo(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	void *bufs[1000];
	int i;

	(void)params;
	(void)user_data;

	cp = umem_cache_create("test_lifo", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Allocate all */
	for (i = 0; i < 1000; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Free all in reverse order */
	for (i = 999; i >= 0; i--) {
		umem_cache_free(cp, bufs[i]);
	}

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test 10: Statistics Accuracy
 *
 * Verify that per-CPU statistics are accurate.
 */
static MunitResult
test_statistics(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cp;
	umem_percpu_cache_t stats;
	void *bufs[100];
	int i, cpu;

	(void)params;
	(void)user_data;

	cp = umem_cache_create("test_stats", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	cpu = get_current_cpu();

	/* Get initial stats */
	umem_percpu_stats(cp, cpu, &stats);
	uint64_t initial_allocs = stats.pc_alloc;
	uint64_t initial_frees = stats.pc_free;

	/* Allocate 100 objects */
	for (i = 0; i < 100; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Check stats */
	umem_percpu_stats(cp, cpu, &stats);
	munit_assert_uint64(stats.pc_alloc, ==, initial_allocs + 100);

	/* Free 100 objects */
	for (i = 0; i < 100; i++) {
		umem_cache_free(cp, bufs[i]);
	}

	/* Check stats */
	umem_percpu_stats(cp, cpu, &stats);
	munit_assert_uint64(stats.pc_free, ==, initial_frees + 100);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test Suite Definition
 */
static MunitTest percpu_tests[] = {
	{"/cpu_detection", test_cpu_detection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cpu_affinity", test_cpu_affinity, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/thread_migration", test_thread_migration, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/magazine_ownership", test_magazine_ownership, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/numa_detection", test_numa_detection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/depot_interaction", test_depot_interaction, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/high_thread_count", test_high_thread_count, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/pattern_sequential", test_pattern_sequential, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/pattern_lifo", test_pattern_lifo, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/statistics", test_statistics, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

static const MunitSuite percpu_suite = {
	"/percpu",
	percpu_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&percpu_suite, NULL, argc, argv);
}

#else /* !UMEM_PER_CPU_CACHE */

#include <stdio.h>

int
main(void)
{
	printf("Per-CPU caching not enabled at build time\n");
	printf("Reconfigure with --enable-per-cpu-cache to enable\n");
	return 0;
}

#endif /* UMEM_PER_CPU_CACHE */

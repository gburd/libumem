/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * umem_ptc_bench.c - Performance benchmark suite for per-thread caching (PTC)
 *
 * PTC generates optimized ptcmalloc/ptcfree that replace malloc/free in the
 * PLT.  This benchmark tests both the umem_alloc/umem_free path (magazine
 * layer) and the malloc/free path (where PTC genasm applies).
 *
 * Measures:
 *   1. Single-threaded throughput (ops/sec) for various allocation sizes
 *      via both umem_alloc/umem_free and malloc/free
 *   2. Multi-threaded throughput scaling (2, 4, 8, 16, 32 threads)
 *   3. Latency percentiles (p50, p95, p99)
 *   4. Memory overhead tracking
 *   5. Cache hit rate estimation
 *
 * Usage:
 *   Default (PTC enabled):
 *     ./umem_ptc_bench
 *
 *   PTC disabled:
 *     UMEM_OPTIONS=perthread_cache=0 ./umem_ptc_bench
 *
 *   Compare both:
 *     ./umem_ptc_bench --compare
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdint.h>

#include "umem.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define BENCH_WARMUP_OPS	10000
#define BENCH_OPS		500000
#define LATENCY_OPS		100000
#define MAX_THREADS		32
#define LATENCY_SAMPLES		LATENCY_OPS

static const size_t alloc_sizes[] = { 16, 32, 64, 128, 256 };
#define N_SIZES (sizeof(alloc_sizes) / sizeof(alloc_sizes[0]))

static const int thread_counts[] = { 2, 4, 8, 16, 32 };
#define N_THREAD_COUNTS (sizeof(thread_counts) / sizeof(thread_counts[0]))

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double
ns_to_sec(uint64_t ns)
{
	return (double)ns / 1e9;
}

/* ------------------------------------------------------------------ */
/* Latency sample comparison for qsort                                 */
/* ------------------------------------------------------------------ */

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	if (va < vb) return -1;
	if (va > vb) return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* 1a. Single-threaded throughput (umem_alloc/umem_free)                */
/* ------------------------------------------------------------------ */

static void
benchmark_single_threaded(void)
{
	size_t s;

	printf("=== Single-threaded throughput (umem_alloc/umem_free) ===\n");
	printf("%-10s %15s %15s\n", "Size", "Ops/sec", "ns/op");
	printf("----------------------------------------------\n");

	for (s = 0; s < N_SIZES; s++) {
		size_t sz = alloc_sizes[s];
		int i;
		uint64_t t0, t1, elapsed;
		double ops_sec, ns_op;

		/* Warmup */
		for (i = 0; i < BENCH_WARMUP_OPS; i++) {
			void *p = umem_alloc(sz, UMEM_DEFAULT);
			if (p == NULL) {
				fprintf(stderr, "warmup alloc failed: "
				    "size=%zu\n", sz);
				return;
			}
			umem_free(p, sz);
		}

		/* Timed run: alloc + free pairs */
		t0 = now_ns();
		for (i = 0; i < BENCH_OPS; i++) {
			void *p = umem_alloc(sz, UMEM_DEFAULT);
			if (p == NULL) {
				fprintf(stderr, "bench alloc failed: "
				    "size=%zu\n", sz);
				return;
			}
			umem_free(p, sz);
		}
		t1 = now_ns();

		elapsed = t1 - t0;
		ops_sec = (double)BENCH_OPS / ns_to_sec(elapsed);
		ns_op = (double)elapsed / (double)BENCH_OPS;

		printf("%-10zu %15.0f %15.1f\n", sz, ops_sec, ns_op);
	}

	printf("\n");
}

/* ------------------------------------------------------------------ */
/* 1b. Single-threaded throughput (malloc/free -- PTC genasm path)      */
/* ------------------------------------------------------------------ */

/*
 * Use volatile function pointers to prevent the compiler from
 * optimizing away or inlining calls through the PLT.
 */
static void *(*volatile bench_malloc_ptr)(size_t);
static void (*volatile bench_free_ptr)(void *);

static void
init_bench_malloc(void)
{
	bench_malloc_ptr = malloc;
	bench_free_ptr = free;
}

static void
benchmark_single_threaded_malloc(void)
{
	size_t s;

	printf("=== Single-threaded throughput (malloc/free) ===\n");
	printf("%-10s %15s %15s\n", "Size", "Ops/sec", "ns/op");
	printf("----------------------------------------------\n");

	for (s = 0; s < N_SIZES; s++) {
		size_t sz = alloc_sizes[s];
		int i;
		uint64_t t0, t1, elapsed;
		double ops_sec, ns_op;

		/* Warmup */
		for (i = 0; i < BENCH_WARMUP_OPS; i++) {
			void *p = bench_malloc_ptr(sz);
			if (p != NULL)
				bench_free_ptr(p);
		}

		/* Timed run */
		t0 = now_ns();
		for (i = 0; i < BENCH_OPS; i++) {
			void *p = bench_malloc_ptr(sz);
			if (p != NULL)
				bench_free_ptr(p);
		}
		t1 = now_ns();

		elapsed = t1 - t0;
		ops_sec = (double)BENCH_OPS / ns_to_sec(elapsed);
		ns_op = (double)elapsed / (double)BENCH_OPS;

		printf("%-10zu %15.0f %15.1f\n", sz, ops_sec, ns_op);
	}

	printf("\n");
}

/* ------------------------------------------------------------------ */
/* 2. Multi-threaded throughput                                        */
/* ------------------------------------------------------------------ */

struct mt_args {
	size_t		size;
	int		ops_per_thread;
	int		use_malloc;	/* 0 = umem_alloc, 1 = malloc */
	uint64_t	elapsed_ns;	/* output */
};

static void *
mt_worker(void *arg)
{
	struct mt_args *a = (struct mt_args *)arg;
	size_t sz = a->size;
	int ops = a->ops_per_thread;
	int i;
	uint64_t t0, t1;

	/* Warmup */
	for (i = 0; i < BENCH_WARMUP_OPS / 10; i++) {
		if (a->use_malloc) {
			void *p = bench_malloc_ptr(sz);
			if (p != NULL)
				bench_free_ptr(p);
		} else {
			void *p = umem_alloc(sz, UMEM_DEFAULT);
			if (p != NULL)
				umem_free(p, sz);
		}
	}

	t0 = now_ns();
	if (a->use_malloc) {
		for (i = 0; i < ops; i++) {
			void *p = bench_malloc_ptr(sz);
			if (p != NULL)
				bench_free_ptr(p);
		}
	} else {
		for (i = 0; i < ops; i++) {
			void *p = umem_alloc(sz, UMEM_DEFAULT);
			if (p != NULL)
				umem_free(p, sz);
		}
	}
	t1 = now_ns();

	a->elapsed_ns = t1 - t0;
	return NULL;
}

static void
run_mt_bench(const char *label, int use_malloc)
{
	int tc;

	printf("=== Multi-threaded throughput (%s, size=64) ===\n", label);
	printf("%-10s %15s %15s\n", "Threads", "Total ops/sec", "ns/op");
	printf("----------------------------------------------\n");

	for (tc = 0; tc < (int)N_THREAD_COUNTS; tc++) {
		int nthreads = thread_counts[tc];
		int ops_per_thread = BENCH_OPS / nthreads;
		pthread_t threads[MAX_THREADS];
		struct mt_args args[MAX_THREADS];
		uint64_t wall_t0, wall_t1, wall_elapsed;
		int total_ops;
		double total_ops_sec, ns_op;
		int i, ret;

		if (nthreads > MAX_THREADS)
			continue;

		total_ops = ops_per_thread * nthreads;

		for (i = 0; i < nthreads; i++) {
			args[i].size = 64;
			args[i].ops_per_thread = ops_per_thread;
			args[i].use_malloc = use_malloc;
			args[i].elapsed_ns = 0;
		}

		wall_t0 = now_ns();

		for (i = 0; i < nthreads; i++) {
			ret = pthread_create(&threads[i], NULL, mt_worker,
			    &args[i]);
			if (ret != 0) {
				fprintf(stderr, "pthread_create failed: %s\n",
				    strerror(ret));
				return;
			}
		}

		for (i = 0; i < nthreads; i++)
			pthread_join(threads[i], NULL);

		wall_t1 = now_ns();
		wall_elapsed = wall_t1 - wall_t0;

		total_ops_sec = (double)total_ops / ns_to_sec(wall_elapsed);
		ns_op = (double)wall_elapsed / (double)total_ops;

		printf("%-10d %15.0f %15.1f\n", nthreads, total_ops_sec,
		    ns_op);
	}

	printf("\n");
}

static void
benchmark_multi_threaded(void)
{
	run_mt_bench("umem_alloc/umem_free", 0);
	run_mt_bench("malloc/free", 1);
}

/* ------------------------------------------------------------------ */
/* 3. Latency percentiles                                              */
/* ------------------------------------------------------------------ */

static void
benchmark_latency_percentiles(void)
{
	size_t s;

	printf("=== Latency percentiles (alloc+free, ns) ===\n");
	printf("%-10s %10s %10s %10s %10s %10s\n",
	    "Size", "min", "p50", "p95", "p99", "max");
	printf("-----------------------------------------------------------"
	    "---\n");

	for (s = 0; s < N_SIZES; s++) {
		size_t sz = alloc_sizes[s];
		uint64_t *samples;
		int i;
		int idx_p50, idx_p95, idx_p99;

		samples = (uint64_t *)malloc(
		    LATENCY_SAMPLES * sizeof(uint64_t));
		if (samples == NULL) {
			fprintf(stderr, "malloc for samples failed\n");
			return;
		}

		/* Warmup */
		for (i = 0; i < BENCH_WARMUP_OPS; i++) {
			void *p = umem_alloc(sz, UMEM_DEFAULT);
			if (p != NULL)
				umem_free(p, sz);
		}

		/* Collect samples */
		for (i = 0; i < LATENCY_SAMPLES; i++) {
			uint64_t t0, t1;
			void *p;

			t0 = now_ns();
			p = umem_alloc(sz, UMEM_DEFAULT);
			if (p != NULL)
				umem_free(p, sz);
			t1 = now_ns();

			samples[i] = t1 - t0;
		}

		qsort(samples, LATENCY_SAMPLES, sizeof(uint64_t), cmp_u64);

		idx_p50 = (int)(LATENCY_SAMPLES * 0.50);
		idx_p95 = (int)(LATENCY_SAMPLES * 0.95);
		idx_p99 = (int)(LATENCY_SAMPLES * 0.99);

		printf("%-10zu %10llu %10llu %10llu %10llu %10llu\n",
		    sz,
		    (unsigned long long)samples[0],
		    (unsigned long long)samples[idx_p50],
		    (unsigned long long)samples[idx_p95],
		    (unsigned long long)samples[idx_p99],
		    (unsigned long long)samples[LATENCY_SAMPLES - 1]);

		free(samples);
	}

	printf("\n");
}

/* ------------------------------------------------------------------ */
/* 4. Memory overhead tracking                                         */
/* ------------------------------------------------------------------ */

static void
benchmark_memory_overhead(void)
{
	/*
	 * Allocate a batch of buffers, hold them, then free.
	 * Report RSS delta as a rough measure of overhead.
	 * We read /proc/self/statm which gives pages; multiply by page size.
	 */
	const int N = 10000;
	const size_t sz = 64;
	void **ptrs;
	long page_sz;
	long rss_before, rss_after, rss_freed;
	FILE *fp;
	int i;
	unsigned long dummy, rss_pages;

	printf("=== Memory overhead (size=64, count=%d) ===\n", N);

	page_sz = sysconf(_SC_PAGESIZE);

	ptrs = (void **)malloc(N * sizeof(void *));
	if (ptrs == NULL) {
		fprintf(stderr, "malloc for ptrs failed\n");
		return;
	}

	/* Measure RSS before allocations */
	fp = fopen("/proc/self/statm", "r");
	if (fp == NULL) {
		printf("  (skipped: /proc/self/statm not available)\n\n");
		free(ptrs);
		return;
	}
	if (fscanf(fp, "%lu %lu", &dummy, &rss_pages) != 2) {
		fclose(fp);
		printf("  (skipped: cannot parse /proc/self/statm)\n\n");
		free(ptrs);
		return;
	}
	fclose(fp);
	rss_before = (long)rss_pages * page_sz;

	/* Allocate and hold */
	for (i = 0; i < N; i++) {
		ptrs[i] = umem_alloc(sz, UMEM_DEFAULT);
		if (ptrs[i] != NULL)
			memset(ptrs[i], 0xAA, sz);
	}

	fp = fopen("/proc/self/statm", "r");
	if (fp != NULL) {
		if (fscanf(fp, "%lu %lu", &dummy, &rss_pages) == 2)
			rss_after = (long)rss_pages * page_sz;
		else
			rss_after = rss_before;
		fclose(fp);
	} else {
		rss_after = rss_before;
	}

	/* Free all */
	for (i = 0; i < N; i++) {
		if (ptrs[i] != NULL)
			umem_free(ptrs[i], sz);
	}

	fp = fopen("/proc/self/statm", "r");
	if (fp != NULL) {
		if (fscanf(fp, "%lu %lu", &dummy, &rss_pages) == 2)
			rss_freed = (long)rss_pages * page_sz;
		else
			rss_freed = rss_after;
		fclose(fp);
	} else {
		rss_freed = rss_after;
	}

	printf("  Logical allocation:  %ld bytes (%d x %zu)\n",
	    (long)N * (long)sz, N, sz);
	printf("  RSS before alloc:    %ld bytes\n", rss_before);
	printf("  RSS after alloc:     %ld bytes (+%ld)\n",
	    rss_after, rss_after - rss_before);
	printf("  RSS after free:      %ld bytes (+%ld from baseline)\n",
	    rss_freed, rss_freed - rss_before);
	if (rss_after > rss_before) {
		double overhead = ((double)(rss_after - rss_before) /
		    (double)(N * sz) - 1.0) * 100.0;
		printf("  Overhead ratio:      %.1f%%\n", overhead);
	}
	printf("\n");

	free(ptrs);
}

/* ------------------------------------------------------------------ */
/* 5. Cache hit rate estimation                                        */
/* ------------------------------------------------------------------ */

/*
 * Estimate cache hit rate by measuring the fraction of alloc+free
 * operations that complete faster than a threshold.  PTC-serviced
 * operations are significantly faster than those going through the
 * full slab allocator path (which requires mutex acquisition).
 *
 * We calibrate the threshold by first measuring the median latency
 * of a warmed-up allocation loop; operations completing within 3x
 * the median are considered "cache hits".
 */
static void
benchmark_cache_hit_rate(void)
{
	const int N = LATENCY_OPS;
	const size_t sz = 64;
	uint64_t *samples;
	uint64_t median, threshold;
	int hits, i;
	double hit_rate;

	printf("=== Cache hit rate estimate (size=64) ===\n");

	samples = (uint64_t *)malloc(N * sizeof(uint64_t));
	if (samples == NULL) {
		fprintf(stderr, "malloc for samples failed\n");
		return;
	}

	/* Warmup */
	for (i = 0; i < BENCH_WARMUP_OPS; i++) {
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, sz);
	}

	/* Collect timing samples */
	for (i = 0; i < N; i++) {
		uint64_t t0, t1;
		void *p;

		t0 = now_ns();
		p = umem_alloc(sz, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, sz);
		t1 = now_ns();

		samples[i] = t1 - t0;
	}

	/* Sort and compute threshold */
	qsort(samples, N, sizeof(uint64_t), cmp_u64);
	median = samples[N / 2];
	threshold = median * 3;

	/* Count hits */
	hits = 0;
	for (i = 0; i < N; i++) {
		if (samples[i] <= threshold)
			hits++;
	}

	hit_rate = (double)hits / (double)N * 100.0;

	printf("  Median latency:      %llu ns\n",
	    (unsigned long long)median);
	printf("  Threshold (3x med):  %llu ns\n",
	    (unsigned long long)threshold);
	printf("  Cache hit rate:      %.1f%% (%d / %d)\n",
	    hit_rate, hits, N);

	if (hit_rate >= 90.0)
		printf("  Status:              PASS (>= 90%%)\n");
	else
		printf("  Status:              INFO (< 90%%, may be "
		    "expected without PTC)\n");

	printf("\n");

	free(samples);
}

/* ------------------------------------------------------------------ */
/* Comparison mode: run benchmark with and without PTC                 */
/* ------------------------------------------------------------------ */

/*
 * Run the benchmark in a child process with the given UMEM_OPTIONS.
 * The child inherits the parent's stdout so results are printed inline.
 */
static int
run_with_env(const char *label, const char *umem_opts, const char *prog)
{
	pid_t pid;
	int status;

	printf("###############################################\n");
	printf("# %s\n", label);
	if (umem_opts != NULL)
		printf("# UMEM_OPTIONS=%s\n", umem_opts);
	else
		printf("# UMEM_OPTIONS=(default)\n");
	printf("###############################################\n\n");
	fflush(stdout);
	fflush(stderr);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}

	if (pid == 0) {
		/* Child: set env and exec self without --compare */
		if (umem_opts != NULL)
			setenv("UMEM_OPTIONS", umem_opts, 1);
		else
			unsetenv("UMEM_OPTIONS");
		execl(prog, prog, (char *)NULL);
		perror("execl");
		_exit(127);
	}

	/* Parent: wait for child */
	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return 1;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s: child exited with status %d\n",
		    label, WEXITSTATUS(status));
		return 1;
	}

	printf("\n");
	fflush(stdout);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
	int compare_mode = 0;
	int i;

	/* Line-buffer stdout for consistent output in all modes */
	setvbuf(stdout, NULL, _IOLBF, 0);

	init_bench_malloc();

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--compare") == 0)
			compare_mode = 1;
	}

	if (compare_mode) {
		int rc = 0;
		printf("Running PTC performance comparison...\n\n");
		rc |= run_with_env("PTC ENABLED (default)",
		    NULL, argv[0]);
		rc |= run_with_env("PTC DISABLED (perthread_cache=0)",
		    "perthread_cache=0", argv[0]);
		return rc;
	}

	printf("umem PTC Performance Benchmark\n");
	printf("==============================\n");
	{
		const char *opts = getenv("UMEM_OPTIONS");
		if (opts != NULL)
			printf("UMEM_OPTIONS=%s\n", opts);
		else
			printf("UMEM_OPTIONS=(default, PTC enabled)\n");
	}
	printf("Operations per benchmark: %d\n", BENCH_OPS);
	printf("Latency samples:          %d\n", LATENCY_SAMPLES);
	printf("\n");

	benchmark_single_threaded();
	benchmark_single_threaded_malloc();
	benchmark_multi_threaded();
	benchmark_latency_percentiles();
	benchmark_memory_overhead();
	benchmark_cache_hit_rate();

	printf("Benchmark complete.\n");
	return 0;
}

/*
 * Debug overhead benchmark for libumem
 *
 * Measures the throughput impact of each UMEM_DEBUG mode.
 * Uses fork() to get a fresh umem init per mode (debug modes
 * must be set before initialization).
 *
 * Also supports --compare mode for competitor comparison
 * at various thread counts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <pthread.h>
#include <errno.h>

#include "../../umem.h"

/* Shared memory for child->parent result passing */
#include <sys/mman.h>

#define OPS_COUNT    1000000
#define ALLOC_SIZE   64
#define WARMUP_OPS   10000
#define LATENCY_SAMPLES 10000

/* --- Time helpers --- */

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* --- Result structure (shared via mmap) --- */

typedef struct {
	double ops_per_sec;
	uint64_t p99_ns;
	int ok;
} bench_result_t;

/* --- Latency tracking (simple sorted array) --- */

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	if (va < vb) return -1;
	if (va > vb) return 1;
	return 0;
}

/* --- Run benchmark in child process with given UMEM_DEBUG --- */

static void
run_debug_bench(const char *mode_env, bench_result_t *out)
{
	/* Warmup */
	for (int i = 0; i < WARMUP_OPS; i++) {
		void *p = umem_alloc(ALLOC_SIZE, UMEM_DEFAULT);
		if (p)
			umem_free(p, ALLOC_SIZE);
	}

	/* Collect latency samples */
	uint64_t *latencies = calloc(LATENCY_SAMPLES, sizeof(uint64_t));
	if (!latencies) {
		out->ok = 0;
		return;
	}
	int lat_idx = 0;
	int sample_every = OPS_COUNT / LATENCY_SAMPLES;
	if (sample_every < 1)
		sample_every = 1;

	uint64_t start = now_ns();

	for (int i = 0; i < OPS_COUNT; i++) {
		uint64_t t0 = now_ns();
		void *p = umem_alloc(ALLOC_SIZE, UMEM_DEFAULT);
		uint64_t t1 = now_ns();

		if (p) {
			memset(p, 0x42, ALLOC_SIZE);
			umem_free(p, ALLOC_SIZE);
		}

		if (i % sample_every == 0 && lat_idx < LATENCY_SAMPLES)
			latencies[lat_idx++] = t1 - t0;
	}

	uint64_t end = now_ns();
	double elapsed = (end - start) / 1e9;

	out->ops_per_sec = (double)OPS_COUNT / elapsed;

	/* Compute p99 */
	if (lat_idx > 0) {
		qsort(latencies, (size_t)lat_idx, sizeof(uint64_t), cmp_u64);
		int p99_idx = (int)((double)(lat_idx - 1) * 0.99);
		out->p99_ns = latencies[p99_idx];
	}

	out->ok = 1;
	free(latencies);
}

/* Fork a child, set UMEM_DEBUG, run benchmark, collect result */
static int
fork_and_bench(const char *mode_name, const char *mode_env,
    bench_result_t *result)
{
	bench_result_t *shared = mmap(NULL, sizeof(bench_result_t),
	    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		return -1;
	}
	memset(shared, 0, sizeof(*shared));

	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		munmap(shared, sizeof(bench_result_t));
		return -1;
	}

	if (pid == 0) {
		/* Child: set env and run */
		if (mode_env && mode_env[0]) {
			setenv("UMEM_DEBUG", mode_env, 1);
		} else {
			unsetenv("UMEM_DEBUG");
		}
		run_debug_bench(mode_env, shared);
		_exit(0);
	}

	/* Parent: wait */
	int status;
	waitpid(pid, &status, 0);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared->ok) {
		*result = *shared;
		munmap(shared, sizeof(bench_result_t));
		return 0;
	}

	munmap(shared, sizeof(bench_result_t));
	return -1;
}

/* --- Debug mode definitions --- */

typedef struct {
	const char *name;
	const char *env_val;
} debug_mode_t;

static const debug_mode_t debug_modes[] = {
	{ "none",     ""                         },
	{ "lite",     "lite"                     },
	{ "guards",   "guards"                   },
	{ "audit",    "audit"                    },
	{ "contents", "contents"                 },
	{ "default",  "default"                  },
	{ "firewall", "firewall"                 },
};

#define NUM_DEBUG_MODES \
	(int)(sizeof(debug_modes) / sizeof(debug_modes[0]))

static void
run_debug_overhead_bench(void)
{
	printf("%-14s %14s %8s %10s %10s\n",
	    "Mode", "Throughput", "Ratio", "p99 (ns)", "Overhead");
	printf("%-14s %14s %8s %10s %10s\n",
	    "----", "----------", "-----", "--------", "--------");

	bench_result_t baseline = { 0 };
	bench_result_t results[NUM_DEBUG_MODES];
	memset(results, 0, sizeof(results));

	for (int i = 0; i < NUM_DEBUG_MODES; i++) {
		int rc = fork_and_bench(debug_modes[i].name,
		    debug_modes[i].env_val, &results[i]);

		if (rc != 0 || !results[i].ok) {
			printf("%-14s %14s %8s %10s %10s\n",
			    debug_modes[i].name, "FAILED", "-", "-", "-");
			continue;
		}

		if (i == 0)
			baseline = results[i];

		double ratio = baseline.ops_per_sec > 0 ?
		    results[i].ops_per_sec / baseline.ops_per_sec : 0;
		double overhead = baseline.ops_per_sec > 0 ?
		    (1.0 - ratio) * 100.0 : 0;
		if (overhead < 0)
			overhead = 0;

		char throughput_str[32];
		if (results[i].ops_per_sec >= 1e6)
			snprintf(throughput_str, sizeof(throughput_str),
			    "%.1fM ops/s", results[i].ops_per_sec / 1e6);
		else
			snprintf(throughput_str, sizeof(throughput_str),
			    "%.0fK ops/s", results[i].ops_per_sec / 1e3);

		printf("%-14s %14s %7.2fx %9lu %9.0f%%\n",
		    debug_modes[i].name,
		    throughput_str,
		    ratio,
		    (unsigned long)results[i].p99_ns,
		    overhead);
	}
}

/* --- Competitor comparison --- */

typedef struct {
	int thread_count;
	uint64_t ops_per_thread;
	double *results_ops;  /* output: ops/sec per thread count */
} mt_bench_config_t;

static _Atomic uint64_t mt_total_ops;

static void *
mt_alloc_worker(void *arg)
{
	uint64_t ops_count = *(uint64_t *)arg;
	uint64_t done = 0;

	for (uint64_t i = 0; i < ops_count; i++) {
		void *p = malloc(ALLOC_SIZE);
		if (p) {
			memset(p, 0x42, ALLOC_SIZE);
			free(p);
			done++;
		}
	}

	atomic_fetch_add(&mt_total_ops, done);
	return NULL;
}

static double
run_mt_bench(int nthreads, uint64_t ops_per_thread)
{
	atomic_store(&mt_total_ops, 0);

	pthread_t *threads = calloc((size_t)nthreads, sizeof(pthread_t));
	if (!threads)
		return 0;

	uint64_t start = now_ns();

	for (int i = 0; i < nthreads; i++)
		pthread_create(&threads[i], NULL, mt_alloc_worker,
		    &ops_per_thread);
	for (int i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	uint64_t end = now_ns();
	double elapsed = (end - start) / 1e9;

	uint64_t total = atomic_load(&mt_total_ops);
	free(threads);
	return (double)total / elapsed;
}

static void *
mt_umem_worker(void *arg)
{
	uint64_t ops_count = *(uint64_t *)arg;
	uint64_t done = 0;

	for (uint64_t i = 0; i < ops_count; i++) {
		void *p = umem_alloc(ALLOC_SIZE, UMEM_DEFAULT);
		if (p) {
			memset(p, 0x42, ALLOC_SIZE);
			umem_free(p, ALLOC_SIZE);
			done++;
		}
	}

	atomic_fetch_add(&mt_total_ops, done);
	return NULL;
}

static double
run_mt_umem_bench(int nthreads, uint64_t ops_per_thread)
{
	atomic_store(&mt_total_ops, 0);

	pthread_t *threads = calloc((size_t)nthreads, sizeof(pthread_t));
	if (!threads)
		return 0;

	uint64_t start = now_ns();

	for (int i = 0; i < nthreads; i++)
		pthread_create(&threads[i], NULL, mt_umem_worker,
		    &ops_per_thread);
	for (int i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	uint64_t end = now_ns();
	double elapsed = (end - start) / 1e9;

	uint64_t total = atomic_load(&mt_total_ops);
	free(threads);
	return (double)total / elapsed;
}

static void
fmt_ops(char *buf, size_t bufsz, double ops)
{
	if (ops >= 1e6)
		snprintf(buf, bufsz, "%.1fM", ops / 1e6);
	else if (ops >= 1e3)
		snprintf(buf, bufsz, "%.0fK", ops / 1e3);
	else
		snprintf(buf, bufsz, "%.0f", ops);
}

static void
run_compare_bench(void)
{
	int thread_counts[] = { 1, 4, 16, 32 };
	int n_counts = (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));

	/* Cap thread counts to available CPUs */
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	for (int i = 0; i < n_counts; i++) {
		if (thread_counts[i] > ncpu * 2)
			thread_counts[i] = (int)(ncpu * 2);
	}

	/* Scale ops down for higher thread counts */
	uint64_t base_ops = 500000;

	printf("\n%-12s", "Allocator");
	for (int i = 0; i < n_counts; i++)
		printf(" %3dT ops/s  ", thread_counts[i]);
	printf("\n");

	printf("%-12s", "----------");
	for (int i = 0; i < n_counts; i++)
		printf(" -----------");
	printf("\n");

	/* glibc/system malloc */
	double glibc_results[4] = { 0 };
	printf("%-12s", "glibc");
	for (int i = 0; i < n_counts; i++) {
		uint64_t ops = base_ops / (uint64_t)thread_counts[i];
		if (ops < 10000)
			ops = 10000;
		glibc_results[i] = run_mt_bench(thread_counts[i], ops);
		char buf[32];
		fmt_ops(buf, sizeof(buf), glibc_results[i]);
		printf(" %11s", buf);
	}
	printf("\n");

	/* umem */
	double umem_results[4] = { 0 };
	printf("%-12s", "umem");
	for (int i = 0; i < n_counts; i++) {
		uint64_t ops = base_ops / (uint64_t)thread_counts[i];
		if (ops < 10000)
			ops = 10000;
		umem_results[i] = run_mt_umem_bench(thread_counts[i], ops);
		char buf[32];
		fmt_ops(buf, sizeof(buf), umem_results[i]);
		printf(" %11s", buf);
	}
	printf("\n");

	/* Ratio */
	printf("%-12s", "ratio");
	for (int i = 0; i < n_counts; i++) {
		double ratio = glibc_results[i] > 0 ?
		    umem_results[i] / glibc_results[i] : 0;
		printf(" %10.2fx", ratio);
	}
	printf("\n");
}

/* --- Main --- */

static void
print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n", prog);
	printf("  --compare    Run allocator comparison at various thread counts\n");
	printf("  -h           Show help\n");
}

int
main(int argc, char *argv[])
{
	bool do_compare = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--compare") == 0) {
			do_compare = true;
		} else if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}

	printf("=== libumem debug overhead benchmark ===\n");
	printf("Operations: %d x %d-byte alloc/free cycles\n\n",
	    OPS_COUNT, ALLOC_SIZE);

	run_debug_overhead_bench();

	if (do_compare)
		run_compare_bench();

	return 0;
}

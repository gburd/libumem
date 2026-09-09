/*
 * bench_affinity_vs_mutex - is CPU-affinity pinning (a candidate
 * migration-safe mechanism for arming the rseq reload slowpath without new
 * assembly -- see docs/results/2026-09-09-rseq-reload-analysis-v2.md,
 * alternative 2b) cheaper than the mutex it would replace?
 *
 * Measures, per iteration, under N contending threads:
 *   (a) mutex_lock(&m); ++counter; mutex_unlock(&m);
 *       -- what the existing cc_lock-guarded reload path already pays.
 *   (b) sched_setaffinity(self, {current_cpu}) ; ++counter ;
 *       sched_setaffinity(self, &original_mask)
 *       -- the pin/write/unpin sequence alternative 2b would need, in the
 *       BEST case (already on the target cpu, no actual migration). Any
 *       real reload (after a genuine migration) can only be MORE
 *       expensive than this best case (it may additionally trigger a
 *       real cross-CPU migration_cpu_stop). So if (b) >> (a) even here,
 *       the real-migration case is strictly worse and the mechanism is
 *       conclusively not a net win versus the existing lock.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static int nthreads = 8;
static long iters_per_thread = 200000;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile long g_counter = 0;

static double
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void *
mutex_worker(void *arg)
{
	(void)arg;
	for (long i = 0; i < iters_per_thread; i++) {
		pthread_mutex_lock(&g_mutex);
		g_counter++;
		pthread_mutex_unlock(&g_mutex);
	}
	return (NULL);
}

static void *
affinity_worker(void *arg)
{
	(void)arg;
	cpu_set_t orig;
	sched_getaffinity(0, sizeof(orig), &orig);
	for (long i = 0; i < iters_per_thread; i++) {
		int cpu = sched_getcpu();
		cpu_set_t pin;
		CPU_ZERO(&pin);
		CPU_SET(cpu, &pin);
		if (sched_setaffinity(0, sizeof(pin), &pin) != 0) {
			perror("sched_setaffinity(pin)");
			exit(1);
		}
		g_counter++;
		if (sched_setaffinity(0, sizeof(orig), &orig) != 0) {
			perror("sched_setaffinity(restore)");
			exit(1);
		}
	}
	return (NULL);
}

static double
run(void *(*fn)(void *))
{
	pthread_t *tids = calloc(nthreads, sizeof(pthread_t));
	g_counter = 0;
	double t0 = now_ns();
	for (int i = 0; i < nthreads; i++)
		pthread_create(&tids[i], NULL, fn, NULL);
	for (int i = 0; i < nthreads; i++)
		pthread_join(tids[i], NULL);
	double t1 = now_ns();
	free(tids);
	long total_ops = (long)nthreads * iters_per_thread;
	if (g_counter != total_ops) {
		fprintf(stderr, "WARNING: counter=%ld expected=%ld\n",
		    g_counter, total_ops);
	}
	return (t1 - t0) / (double)total_ops; /* ns/op */
}

int
main(int argc, char **argv)
{
	if (argc > 1)
		nthreads = atoi(argv[1]);
	if (argc > 2)
		iters_per_thread = atol(argv[2]);

	printf("# threads=%d iters/thread=%ld ncpu_online=%ld\n",
	    nthreads, iters_per_thread, sysconf(_SC_NPROCESSORS_ONLN));

	double mutex_ns = run(mutex_worker);
	printf("mutex_lock/unlock:            %8.1f ns/op\n", mutex_ns);

	double affinity_ns = run(affinity_worker);
	printf("sched_setaffinity pin+unpin:  %8.1f ns/op\n", affinity_ns);

	printf("ratio (affinity/mutex):        %8.1fx\n",
	    affinity_ns / mutex_ns);

	return (0);
}

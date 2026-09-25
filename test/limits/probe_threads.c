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
 * Phase 6 probe: THREAD COUNT.
 *
 * Spawn T threads.  Each does a little alloc/free (so its PTC is created and
 * populated: umem_ptc_get() allocates a umem_ptc_t per thread), then parks on
 * a barrier.  While parked, measure RSS per idle thread -- the per-thread
 * footprint the allocator alone adds, with the thread stack cost netted out
 * via the -DLIM_GLIBC build of the same program.  Then release all T at once
 * and time the exit drain (umem_ptc_destroy() flushes every bin and magazine
 * back to the depot under cc_lock / ml_lock).
 *
 * Also reports whether any thread saw sched_getcpu() >= umem_max_ncpus, i.e.
 * whether the CPU-hint mask can wrap on a box with more CPUs than the count
 * umem_init() took at startup (nthreads >> ncpus is the interesting regime).
 *
 * Usage: probe_threads <nthreads> [allocs_per_thread]
 */
#include "limits.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

static pthread_barrier_t ready, go;
static size_t per_thread = 1000;
static size_t probe_size_lo = 0;	/* P8.2c: if set, cycle [lo, hi] instead of 16-240 B */
static size_t probe_size_hi = 0;
static atomic_int max_cpu_seen;
static atomic_uint_fast64_t exit_worst_ns;

static void *
worker(void *arg)
{
	(void)arg;
	void *ptrs[64];
	size_t i, k;
	int cpu = sched_getcpu();
	int cur;

	do {
		cur = atomic_load(&max_cpu_seen);
	} while (cpu > cur && !atomic_compare_exchange_weak(&max_cpu_seen, &cur, cpu));

	/* Populate this thread's PTC across several small size classes. */
	for (k = 0; k < per_thread / 64 + 1; k++) {
		for (i = 0; i < 64; i++) {
			size_t sz;
			if (probe_size_hi) {
				/* P8.2c: sweep the tier so every class in
				 * [lo,hi] retains a magazine's worth. */
				size_t span = probe_size_hi - probe_size_lo;
				sz = probe_size_lo +
				    (span ? (i * span / 63) : 0);
			} else {
				sz = 16 + (i % 8) * 32;
			}
			ptrs[i] = ALLOC(sz);
			if (ptrs[i]) *(volatile char *)ptrs[i] = 1;
		}
		for (i = 0; i < 64; i++) {
			size_t sz;
			if (probe_size_hi) {
				size_t span = probe_size_hi - probe_size_lo;
				sz = probe_size_lo +
				    (span ? (i * span / 63) : 0);
			} else {
				sz = 16 + (i % 8) * 32;
			}
			if (ptrs[i]) FREE(ptrs[i], sz);
		}
	}
	pthread_barrier_wait(&ready);	/* main samples RSS here */
	pthread_barrier_wait(&go);
	return (NULL);
}

int
main(int argc, char **argv)
{
	int t = argc > 1 ? atoi(argv[1]) : 1000;
	if (argc > 2) per_thread = strtoul(argv[2], NULL, 10);
	/* P8.2c: probe_threads <n> <per_thread> <lo:hi>  e.g. 4096:8192 */
	if (argc > 3) {
		char *colon = strchr(argv[3], ':');
		probe_size_lo = strtoul(argv[3], NULL, 10);
		probe_size_hi = colon ? strtoul(colon + 1, NULL, 10)
		    : probe_size_lo;
	}
	pthread_t *th = calloc(t, sizeof (pthread_t));
	pthread_attr_t attr;
	int i, spawned = 0;
	uint64_t rss0, rss1, t0, t1;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);	/* small, fixed */
	pthread_barrier_init(&ready, NULL, t + 1);
	pthread_barrier_init(&go, NULL, t + 1);

	/* warm the allocator in main so init cost is not in the per-thread number */
	{ void *p = ALLOC(64); if (p) FREE(p, 64); }
	rss0 = rss_bytes();
	printf("%s threads=%d per_thread_allocs=%zu ncpus_online=%ld rss0=%.1fMB\n",
	    LIM_NAME, t, per_thread, sysconf(_SC_NPROCESSORS_ONLN), MB(rss0));

	t0 = now_ns();
	for (i = 0; i < t; i++) {
		int rc = pthread_create(&th[i], &attr, worker, NULL);
		if (rc != 0) {
			printf("pthread_create failed at %d: %s\n", i, strerror(rc));
			break;
		}
		spawned++;
	}
	if (spawned != t) {
		printf("only %d of %d threads; aborting probe\n", spawned, t);
		return (2);
	}
	pthread_barrier_wait(&ready);
	t1 = now_ns();
	rss1 = rss_bytes();
	printf("spawn+populate: %.2fs  rss=%.1fMB  delta=%.1fMB  per_thread=%.1fKB  max_cpu_seen=%d  vmas=%lu\n",
	    (t1 - t0) / 1e9, MB(rss1), MB(rss1 - rss0),
	    (double)(rss1 - rss0) / t / 1024.0, atomic_load(&max_cpu_seen),
	    count_vmas());
	fflush(stdout);

	/* Release everyone and time the exit storm. */
	t0 = now_ns();
	pthread_barrier_wait(&go);
	for (i = 0; i < t; i++)
		pthread_join(th[i], NULL);
	t1 = now_ns();
	printf("exit drain of %d threads: %.3fs (%.1fus/thread)  rss_after=%.1fMB\n",
	    t, (t1 - t0) / 1e9, (t1 - t0) / 1e3 / t, MB(rss_bytes()));

	/* Does main's own allocation stall while they were exiting? Measured
	 * indirectly: re-run a short exit storm while main allocates. */
	pthread_barrier_destroy(&ready); pthread_barrier_destroy(&go);
	pthread_barrier_init(&ready, NULL, t + 1);
	pthread_barrier_init(&go, NULL, t + 1);
	for (i = 0; i < t; i++)
		pthread_create(&th[i], &attr, worker, NULL);
	pthread_barrier_wait(&ready);
	pthread_barrier_wait(&go);
	uint64_t worst = 0, probes = 0;
	uint64_t end = now_ns() + 3ULL * 1000000000ULL;
	while (now_ns() < end) {
		uint64_t a = now_ns();
		void *p = ALLOC(64);
		uint64_t d = now_ns() - a;
		if (p) FREE(p, 64);
		if (d > worst) worst = d;
		probes++;
	}
	for (i = 0; i < t; i++)
		pthread_join(th[i], NULL);
	printf("main alloc during exit storm: %llu probes, worst %.1fus\n",
	    (unsigned long long)probes, worst / 1e3);
	(void)exit_worst_ns;
	return (0);
}

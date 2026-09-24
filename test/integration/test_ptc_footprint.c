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
 * P6.3 regression: per-thread cache footprint and exit-drain cost.
 *
 * THE DEFECT.  umem_ptc_t was 28 bins x 128 slots x 8 B for every thread,
 * 30.9 KB, though bins 13-27 only ever index 64 or 32 of those slots; with
 * the two per-thread magazines and the retained objects an idle thread cost
 * 56 KB against glibc's 17.5 KB.  And umem_ptc_destroy() returned each
 * retained object through _umem_cache_free(), one cc_lock per object, up to
 * ~600 per exiting thread: 32 us/thread at 1k threads, 423 us at 16k, during
 * which main's own umem_alloc saw a 36.6 ms stall.
 *
 * WHAT THIS TESTS, exactly.  Two oracles, both exact; no timing threshold.
 *
 *   1. sizeof (umem_ptc_t) <= 24 KB.  The struct is the footprint the
 *      allocator adds per thread; this bounds the slot pool at its PACKED
 *      size (pre-fix 30.9 KB, with 28 x 128 slots regardless of capacity).
 *      The limit was 12 KB when the bin capacities were also halved; the
 *      halving was reverted (see umem_ptc.h: the per-thread magazine layer
 *      behind the bins is never primed, so a smaller bin is a 28x cliff at
 *      its boundary), leaving packing alone: 128/64/32 slots = 19.2 KB pool
 *      + 28 line-separated bin records + magazines ~= 22.8 KB.  A
 *      compile-time fact checked at run time so the test says which build
 *      it ran against.
 *
 *   2. Hand-offs per drained bin == 1.  Linked against the probe build
 *      (-DUMEM_PTC_RESIZE_PROBE), the library counts the lock-taking
 *      hand-offs umem_ptc_destroy() makes and the non-empty bins it drains.
 *      Pre-fix each object was its own _umem_cache_free (handoffs = objects,
 *      ~600 per thread here); post-fix a bin is one umem_cache_free_batch
 *      (handoffs = bins, 8 per thread).  Exact and deterministic.
 *
 *   The per-thread exit-drain time is printed for the record but not gated:
 *   on an 8-vCPU box the 1,000-thread drain is 14-23 us/thread pre-fix and
 *   the post-fix figure is inside that spread, so a threshold there would be
 *   a coin toss (the 16k-thread metal figure in the plan is where it shows).
 *   The stranded-object invariant is test_ptc_thread_exit_drain_probe's.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "umem.h"
#include "umem_ptc.h"

#define NTHREADS	1000
#define PER_THREAD	1000
#define STRUCT_MAX	(24 * 1024)

static pthread_barrier_t ready, go;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static void *
worker(void *arg)
{
	void *ptrs[64];
	size_t i, k;

	(void) arg;
	for (k = 0; k < PER_THREAD / 64 + 1; k++) {
		for (i = 0; i < 64; i++) {
			ptrs[i] = umem_alloc(16 + (i % 8) * 32, UMEM_DEFAULT);
			if (ptrs[i] != NULL)
				*(volatile char *)ptrs[i] = 1;
		}
		for (i = 0; i < 64; i++)
			if (ptrs[i] != NULL)
				umem_free(ptrs[i], 16 + (i % 8) * 32);
	}
	pthread_barrier_wait(&ready);
	pthread_barrier_wait(&go);
	return (NULL);
}

int
main(void)
{
	pthread_t *th = calloc(NTHREADS, sizeof (pthread_t));
	pthread_attr_t attr;
	int i, spawned = 0, fails = 0;
	uint64_t t0, t1;
	double us_per;
	extern volatile long umem_ptc_probe_exit_handoffs;
	extern volatile long umem_ptc_probe_exit_bins;

	printf("sizeof(umem_ptc_t)=%zu bytes (limit %d)\n",
	    sizeof (umem_ptc_t), STRUCT_MAX);
	if (sizeof (umem_ptc_t) > STRUCT_MAX) {
		printf("FAIL: per-thread cache struct exceeds %d bytes\n",
		    STRUCT_MAX);
		fails++;
	}

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	pthread_barrier_init(&ready, NULL, NTHREADS + 1);
	pthread_barrier_init(&go, NULL, NTHREADS + 1);

	{ void *p = umem_alloc(64, UMEM_DEFAULT); if (p) umem_free(p, 64); }

	for (i = 0; i < NTHREADS; i++) {
		if (pthread_create(&th[i], &attr, worker, NULL) != 0)
			break;
		spawned++;
	}
	if (spawned != NTHREADS) {
		/* Cannot measure; do not report a number for a smaller run. */
		printf("SKIP: only %d of %d threads could be created\n",
		    spawned, NTHREADS);
		return (77);
	}
	pthread_barrier_wait(&ready);

	t0 = now_ns();
	pthread_barrier_wait(&go);
	for (i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);
	t1 = now_ns();
	us_per = (t1 - t0) / 1e3 / NTHREADS;

	printf("exit drain of %d threads: %.3f s, %.1f us/thread (not gated)\n",
	    NTHREADS, (t1 - t0) / 1e9, us_per);

	printf("exit hand-offs=%ld over bins=%ld (%.1f per bin; must be 1)\n",
	    umem_ptc_probe_exit_handoffs, umem_ptc_probe_exit_bins,
	    umem_ptc_probe_exit_bins ? (double)umem_ptc_probe_exit_handoffs /
	    umem_ptc_probe_exit_bins : 0.0);
	if (umem_ptc_probe_exit_bins == 0) {
		printf("FAIL: no bin was drained; the workload did not "
		    "populate a PTC\n");
		fails++;
	} else if (umem_ptc_probe_exit_handoffs != umem_ptc_probe_exit_bins) {
		printf("FAIL: %ld hand-offs for %ld bins -- the drain is "
		    "per-object, not per-bin\n", umem_ptc_probe_exit_handoffs,
		    umem_ptc_probe_exit_bins);
		fails++;
	}

	if (fails == 0)
		printf("RESULT: PASS (struct packed; one hand-off per bin at exit)\n");
	else
		printf("RESULT: FAIL (%d)\n", fails);
	return (fails ? 1 : 0);
}

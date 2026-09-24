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
 * P8.5 regression: a depot steal scan must not take the lock of a stripe
 * whose list is empty.
 *
 * THE DEFECT.  umem_depot_pop() and umem_depot_pop_trylock() locked the
 * stripe before looking at its list head.  A reload that misses its own
 * stripe scans the others -- min(ncpus, 8) of them on the PTC path, ALL of
 * them on the blocking path -- and on a workload whose frees do not keep up
 * with its allocations (frag: a growing live set, half freed per round)
 * nearly every stripe is empty nearly all the time, so every miss took and
 * released up to 2 x ncpus locks to read 2 x ncpus NULLs.  Eight threads
 * doing that at once hold each other's stripe locks: on c7i.2xlarge, frag
 * 16:64 t=8, the blocking pop found its OWN stripe's lock held 434k times
 * against 80k successful reloads (5.4 per success), perf 22 % in
 * pthread_mutex_trylock + 8 % unlock, and the PTC-path trylock scan failed
 * its way through 8 stripes on every miss.
 *
 * WHAT THIS TESTS, exactly.  Linked against the probe build
 * (-DUMEM_PTC_RESIZE_PROBE), where every stripe pop is counted and every
 * pop that acquired a stripe lock and then read a NULL head is counted
 * separately.  Eight threads run a frag-shaped loop (grow a live set, free
 * a random half, repeat) at 64 B for long enough to make ~10^5 depot pops.
 *
 *   PASS: locked-empty pops <= 1 % of all pops.
 *   FAIL: more.  Pre-fix it is 90 %+: almost every pop locks an empty
 *   stripe.  Post-fix the unlocked head check turns those away before the
 *   lock; the only locked-empties left are races where the head went NULL
 *   between the check and the lock, which are rare by construction.
 *
 * An exact count, not a timing threshold.  The throughput and p999
 * consequences are in the P8.5 STATUS entry; this fails on the mechanism.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "umem.h"

extern volatile long umem_ptc_probe_depot_pops;
extern volatile long umem_ptc_probe_depot_pops_empty;
extern volatile long umem_ptc_probe_depot_pops_locked_empty;

#define	NTHREADS	8
#define	OBJ		64
#define	POOL		200000
#define	ROUNDS		40

static void *
worker(void *arg)
{
	void **pool = malloc(sizeof (void *) * POOL);
	unsigned seed = 42u ^ ((unsigned)(uintptr_t)arg * 2654435761u);
	size_t count = 0, i;
	int r;

	if (pool == NULL)
		return ((void *)1);
	for (r = 0; r < ROUNDS; r++) {
		while (count < POOL) {
			pool[count] = umem_alloc(OBJ, UMEM_DEFAULT);
			if (pool[count] == NULL)
				return ((void *)1);
			count++;
		}
		for (i = 0; i < count; ) {
			if (rand_r(&seed) % 2 == 0) {
				umem_free(pool[i], OBJ);
				pool[i] = pool[--count];
			} else {
				i++;
			}
		}
	}
	for (i = 0; i < count; i++)
		umem_free(pool[i], OBJ);
	free(pool);
	return (NULL);
}

int
main(void)
{
	pthread_t th[NTHREADS];
	long pops, empty, locked_empty;
	double pct;
	int i, bad = 0;

	for (i = 0; i < NTHREADS; i++)
		(void) pthread_create(&th[i], NULL, worker,
		    (void *)(uintptr_t)i);
	for (i = 0; i < NTHREADS; i++) {
		void *rv;
		(void) pthread_join(th[i], &rv);
		if (rv != NULL)
			bad++;
	}
	if (bad) {
		printf("SKIP: %d worker(s) could not allocate\n", bad);
		return (77);
	}

	pops = umem_ptc_probe_depot_pops;
	empty = umem_ptc_probe_depot_pops_empty;
	locked_empty = umem_ptc_probe_depot_pops_locked_empty;
	if (pops < 10000) {
		printf("SKIP: only %ld depot pops; workload did not reach "
		    "the depot\n", pops);
		return (77);
	}
	pct = 100.0 * (double)locked_empty / (double)pops;
	printf("depot pops=%ld  found empty=%ld (%.1f%%)  LOCKED then found "
	    "empty=%ld (%.1f%%; must be <= 1%%)\n", pops, empty,
	    100.0 * (double)empty / (double)pops, locked_empty, pct);
	if (pct > 1.0) {
		printf("FAIL: %.1f%% of depot pops took a stripe lock to read "
		    "an empty list\n", pct);
		printf("RESULT: FAIL\n");
		return (1);
	}
	printf("RESULT: PASS (empty stripes are skipped without their lock)\n");
	return (0);
}

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
 * P8.6 regression: the per-thread magazine layer behind the PTC bins must
 * actually hold objects.
 *
 * THE DEFECT.  umem_ptc_mag_t is the L2 of the per-thread cache: when a bin
 * (128/64/32 slots) is full on free or empty on alloc, the object is meant to
 * cross into a thread-private magazine, lock-free, and only a whole magazine
 * at a time goes to the depot.  Nothing primed that layer.  It took magazines
 * from the depot by trylock only and never allocated one; the CPU layer
 * publishes an empty magazine to the depot only when its own loaded magazine
 * drains, which a steady alloc/free workload never does.  So on a cache one
 * thread cycles, every op past the bin did 1 + min(ncpus,
 * UMEM_DEPOT_STEAL_MAX) + 1 FAILED trylock/unlock pairs on empty stripes and
 * then cc_lock.  Measured on c7i.2xlarge, one thread, 512 B, alloc N then
 * free N: 157 Mpairs/s at N = 64, 5.5 at N = 128 (28x); perf 39 %
 * pthread_mutex_trylock + 34 % pthread_mutex_unlock.
 *
 * WHAT THIS TESTS, exactly.  Linked against the probe build
 * (-DUMEM_PTC_RESIZE_PROBE), where the library counts every
 * umem_depot_alloc_trylock() the PTC paths make.  One thread, one size
 * class whose bin holds `cap` objects, alloc N then free N, R rounds, with
 * N = 2 * cap so every round pushes `cap` objects past the bin on the free
 * side and pulls `cap` back on the alloc side.
 *
 *   Pre-fix: each of those 2 * cap crossings per round is a depot trylock
 *   attempt (no magazine to absorb them): attempts ~ 2 * cap * R.  Measured
 *   at the parent: exactly 2 * cap * R.
 *
 *   Post-fix: the first free-side miss allocates one empty magazine; after
 *   that the L2 holds the round's overflow (cap <= magsize for every class
 *   here, since 512 B gets 127-round magazines and 64 B 255), so the depot
 *   is touched only when a magazine actually fills or drains: attempts
 *   O(R * cap / magsize) -- for these parameters, a handful.
 *
 *   PASS: attempts <= 2 * R + 8  (the O(N/magsize) bound with slack for the
 *   first-miss allocation and the initial empty-list probes).
 *   FAIL: attempts > that.  Pre-fix it is 2 * cap * R = 128 * R for 512 B.
 *
 * An exact count, not a timing threshold: the throughput cliff that motivated
 * this is in test/bench/bench_pairs (a bare loop) and in the plan entry; this
 * test fails on the mechanism, on any box, at any load.
 *
 * Both 512 B (medium bin, 64 slots) and 64 B (small bin, 128 slots) are
 * tested: the fix is in the shared path, and the bin capacity differs.
 */

#include <stdio.h>
#include <stdlib.h>

#include "umem.h"
#include "umem_ptc.h"

extern volatile long umem_ptc_probe_depot_trylocks;

#define	ROUNDS	200

static int
run(size_t size, int cap)
{
	int n = 2 * cap;
	void **p = malloc(sizeof (void *) * (size_t)n);
	long before, after, attempts, bound;
	int r, i;

	/* Fill the bin once so the measured rounds start at a full bin. */
	for (i = 0; i < n; i++)
		p[i] = umem_alloc(size, UMEM_DEFAULT);
	for (i = 0; i < n; i++)
		umem_free(p[i], size);

	before = umem_ptc_probe_depot_trylocks;
	for (r = 0; r < ROUNDS; r++) {
		for (i = 0; i < n; i++) {
			p[i] = umem_alloc(size, UMEM_DEFAULT);
			if (p[i] == NULL) {
				printf("FAIL: alloc %zu returned NULL\n", size);
				return (1);
			}
		}
		for (i = 0; i < n; i++)
			umem_free(p[i], size);
	}
	after = umem_ptc_probe_depot_trylocks;
	attempts = after - before;
	bound = 2L * ROUNDS + 8;
	free(p);

	printf("size=%zu bin=%d N=%d rounds=%d: depot trylock attempts=%ld "
	    "(bound %ld; unprimed L2 gives %ld)\n", size, cap, n, ROUNDS,
	    attempts, bound, 2L * cap * ROUNDS);
	if (attempts > bound) {
		printf("FAIL: %ld depot trylock attempts for %d objects past "
		    "the bin per round -- the per-thread magazines are not "
		    "holding anything\n", attempts, cap);
		return (1);
	}
	return (0);
}

int
main(void)
{
	int fails = 0;
	int bin;

	{ void *w = umem_alloc(512, UMEM_DEFAULT); umem_free(w, 512); }

	bin = umem_ptc_size_to_bin(512);
	if (bin < 0) {
		printf("SKIP: PTC disabled or 512 B not a PTC class\n");
		return (77);
	}
	fails += run(512, ptc_bin_capacity(bin));
	bin = umem_ptc_size_to_bin(64);
	fails += run(64, ptc_bin_capacity(bin));

	if (fails == 0)
		printf("RESULT: PASS (per-thread magazines absorb bin "
		    "overflow)\n");
	else
		printf("RESULT: FAIL (%d)\n", fails);
	return (fails ? 1 : 0);
}

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
 * Regression: a thread must not lose cached objects when it exits (P1.3a).
 *
 * DEFECT (pre-fix): umem_ptc_destroy() called umem_ptc_bin_flush(), which
 * deliberately flushes only HALF a bin, exactly once per bin, and then freed
 * the PTC.  Every object still in a bin at that point lost its only reference
 * while the slab layer went on counting it as allocated.  A thread exiting
 * with 128 cached objects permanently leaked 64.  Thread churn made it
 * cumulative.
 *
 * HOW THIS DETECTS IT
 *   umem_cache_stats() reports cache_buftotal (buffers the slab layer has
 *   handed out) and the cache's allocated/free accounting.  We use the
 *   allocator's own view of outstanding buffers for a specific size class:
 *
 *     1. Warm up, then record a baseline.
 *     2. Spawn threads; each allocates N objects of one size class and frees
 *        them, so they land in that thread's PTC bin, then exits.
 *     3. After joining, and after a reap to push magazine contents down to the
 *        slab layer, the outstanding count must return to the baseline.
 *
 *   Pre-fix, each thread strands roughly half a bin, so the outstanding count
 *   climbs with every round and never comes back.
 *
 * The check is a bound, not an equality on a single round: the magazine and
 * depot layers legitimately retain objects.  What must not happen is unbounded
 * GROWTH across rounds, which is what losing references produces.  Running
 * several rounds and requiring the count to stop growing distinguishes
 * "retained in a cache" (bounded) from "lost" (monotonic).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem.h"
#include "umem_inspect.h"

#define OBJ_SIZE     64		/* one PTC-eligible size class */
#define PER_THREAD   96		/* enough to fill a bin and overflow it */
#define NTHREADS     8
#define ROUNDS       6

/* Count allocated buffers of our size class that the slab layer still
 * considers handed out.  umem_walk_allocated() is the documented read-only
 * walker; cached-but-free buffers are what we are hunting, and a lost object
 * stays in this set forever because nothing can return it. */
static size_t g_matched;

static int
count_cb(const umem_buffer_info_t *info, void *arg)
{
	(void) arg;
	if (info != NULL && info->size == OBJ_SIZE)
		g_matched++;
	return (0);
}

static size_t
outstanding(void)
{
	g_matched = 0;
	(void) umem_walk_allocated(count_cb, NULL);
	return (g_matched);
}

static void *
churn(void *arg)
{
	void **keep = calloc(PER_THREAD, sizeof(void *));
	int i;

	(void) arg;
	if (keep == NULL)
		return (NULL);

	/*
	 * Allocate then free, so the objects end up in THIS thread's PTC bin
	 * rather than being handed straight back.
	 */
	for (i = 0; i < PER_THREAD; i++) {
		keep[i] = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		if (keep[i] != NULL)
			memset(keep[i], 0x5a, OBJ_SIZE);
	}
	for (i = 0; i < PER_THREAD; i++) {
		if (keep[i] != NULL)
			umem_free(keep[i], OBJ_SIZE);
	}

	free(keep);
	return (NULL);		/* thread exits here: PTC destructor runs */
}

int
main(void)
{
	size_t counts[ROUNDS];
	int r, i;
	pthread_t th[NTHREADS];

	/* Warm up: create the cache and its magazines before measuring. */
	void *w = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
	if (w != NULL)
		umem_free(w, OBJ_SIZE);

	for (r = 0; r < ROUNDS; r++) {
		for (i = 0; i < NTHREADS; i++) {
			if (pthread_create(&th[i], NULL, churn, NULL) != 0) {
				fprintf(stderr, "pthread_create failed\n");
				return (2);
			}
		}
		for (i = 0; i < NTHREADS; i++)
			(void) pthread_join(th[i], NULL);

		/* Push magazine contents down so the slab view settles. */
		umem_reap();
		(void) sleep(1);

		counts[r] = outstanding();
		printf("round %d: outstanding(size=%d)=%zu\n", r, OBJ_SIZE,
		    counts[r]);
	}

	/*
	 * Verdict: the count must stop growing.  Compare the last two rounds
	 * against the middle of the run; retention is fine, monotonic growth
	 * proportional to thread count is the leak.
	 */
	size_t mid = counts[ROUNDS / 2];
	size_t last = counts[ROUNDS - 1];
	size_t per_round_leak_floor =
	    (size_t)(NTHREADS * (PER_THREAD / 4));	/* conservative */

	printf("mid(round %d)=%zu last(round %d)=%zu growth=%zd "
	    "leak_floor_per_round=%zu\n",
	    ROUNDS / 2, mid, ROUNDS - 1, last, (ssize_t)(last - mid),
	    per_round_leak_floor);

	if (last > mid + per_round_leak_floor) {
		printf("RESULT: FAIL (outstanding buffers keep growing across "
		    "rounds -- exiting threads are losing cached objects)\n");
		return (1);
	}

	printf("RESULT: PASS (outstanding buffers stabilised; exiting threads "
	    "return their cached objects)\n");
	return (0);
}

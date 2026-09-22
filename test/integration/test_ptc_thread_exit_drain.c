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
 *   umem_walk_allocated() counts buffers the slab layer still considers handed
 *   out.  A lost object stays in that set forever, because nothing holds a
 *   reference that could return it.
 *
 *     1. Warm up, then run several rounds.  Each round spawns threads that
 *        each cache PER_THREAD objects of one size class in their PTC bin and
 *        then exit.
 *     2. After joining and reaping, sample the outstanding count.
 *
 *   IMPORTANT -- WHY THIS NEEDS AN INTERNAL CONTROL
 *   The magazine, depot, and slab layers all legitimately retain freed
 *   objects, and that retention grows for a while before it plateaus.  A
 *   threshold on absolute growth therefore cannot distinguish "retained" from
 *   "lost": measured on a fixed build, this workload grows the outstanding
 *   count from ~315 to ~1092 over six rounds even with PTC COMPLETELY
 *   DISABLED, where no PTC object loss is possible.
 *
 *   So the test runs the same workload twice in one process: once with the PTC
 *   active and once with it bypassed (umem_ptc_enabled = 0), and compares.
 *   PTC-enabled growth must not exceed the PTC-disabled growth by more than a
 *   margin.  Pre-fix, each exiting thread stranded about half a bin, which put
 *   PTC-enabled growth far above the control; post-fix the two track each
 *   other.
 *
 *   This is deliberately a comparison and not an equality: both arms include
 *   ordinary allocator retention, which is what the control subtracts out.
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

/* Run ROUNDS rounds of thread churn and return how much the outstanding
 * count grew over the second half of the run. */
static ssize_t
measure_growth(const char *label)
{
	size_t counts[ROUNDS];
	int r, i;
	pthread_t th[NTHREADS];

	for (r = 0; r < ROUNDS; r++) {
		for (i = 0; i < NTHREADS; i++) {
			if (pthread_create(&th[i], NULL, churn, NULL) != 0) {
				fprintf(stderr, "pthread_create failed\n");
				exit(2);
			}
		}
		for (i = 0; i < NTHREADS; i++)
			(void) pthread_join(th[i], NULL);

		/* Push magazine contents down so the slab view settles. */
		umem_reap();
		(void) sleep(1);

		counts[r] = outstanding();
		printf("  %s round %d: outstanding(size=%d)=%zu\n", label, r,
		    OBJ_SIZE, counts[r]);
	}

	return ((ssize_t)counts[ROUNDS - 1] - (ssize_t)counts[ROUNDS / 2]);
}

int
main(void)
{
	/* umem_ptc_enabled is the library's own switch; flipping it lets us run
	 * the control arm in the same process, with the same caches already
	 * warm, so the comparison is not confounded by startup differences. */
	extern int umem_ptc_enabled;
	ssize_t growth_ptc, growth_noptc;

	/* Warm up: create the cache and its magazines before measuring. */
	void *w = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
	if (w != NULL)
		umem_free(w, OBJ_SIZE);

	printf("arm 1: PTC enabled (the path under test)\n");
	umem_ptc_enabled = 1;
	growth_ptc = measure_growth("ptc");

	printf("arm 2: PTC disabled (control -- no PTC loss is possible)\n");
	umem_ptc_enabled = 0;
	growth_noptc = measure_growth("noptc");

	/*
	 * Margin: one quarter of a bin per thread per round is far below the
	 * half-bin-per-thread the pre-fix code stranded, and comfortably above
	 * run-to-run noise in ordinary retention.
	 */
	ssize_t margin = (ssize_t)(NTHREADS * (PER_THREAD / 4));

	printf("growth: ptc=%zd control=%zd margin=%zd\n",
	    growth_ptc, growth_noptc, margin);

	if (growth_ptc > growth_noptc + margin) {
		printf("RESULT: FAIL (PTC-enabled growth exceeds the PTC-off "
		    "control by more than the margin -- exiting threads are "
		    "losing cached objects)\n");
		return (1);
	}

	printf("RESULT: PASS (PTC-enabled growth tracks the PTC-off control; "
	    "exiting threads return their cached objects)\n");
	return (0);
}

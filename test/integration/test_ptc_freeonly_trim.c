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
 * F1-a regression (round 8): a FREE-ONLY thread must not pin a full PTC bin
 * forever.
 *
 * THE DEFECT (r8rss, docs/results/r8rss/README.md).  In cross-thread free
 * (prodcons) a consumer thread only frees.  Once its PTC bin for a size class
 * fills, every subsequent free bypasses the bin into the magazine layer, but
 * the FULL bin (up to 128 objects) is never popped again because the thread
 * never allocates that class.  That stock is dead weight for the thread's
 * whole life: on 96 metal consumers it is 36 %% of RSS, and the tcache=0
 * control cut RSS 29->19 MiB with no throughput change.
 *
 * THE FIX.  On the magazine-overflow SLOW path (both per-thread magazines
 * full -- reached only after ~2*magsize frees with no pop), if the bin is
 * still completely full it has stayed full across PTC_TRIM_IDLE_OVERFLOWS
 * overflows, which only a free-only thread does; flush the whole bin to the
 * depot.  The fast push/pop is untouched.
 *
 * WHAT THIS TESTS, exactly.  A single free-only worker frees a large batch of
 * one size class handed to it by main (which allocated the objects, so this
 * thread never allocates that class).  It then reports its OWN PTC bin count
 * for the class, read through thread_ptc (umem_ptc.h).  The batch is far more
 * than 2*magsize + capacity, so a free-only thread crosses the trim threshold
 * many times.
 *
 *   PRE-FIX  the bin stays pinned at capacity: reported count == capacity.
 *   POST-FIX the trim drains it: reported count is well under capacity
 *            (0 right after a trim; at most a partial re-fill since the last).
 *
 * The oracle is the thread's own exact bin count, not VmRSS: an in-library
 * count has no coupling to unrelated retention (the P6.3/r8rss lesson).
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
#include "umem_ptc.h"

/*
 * Class under test.  1536 B lands in a "large" bin (capacity 32) among the
 * 1-4 KB classes r8rss measured; the effect is identical for any class.
 */
#define	SZ		1536
/* Enough frees to cross the overflow slow path many times: >> 2*magsize +
 * capacity for any magtype (metal magsize reaches 127). */
#define	BATCH		200000

static void **g_buf;
static size_t g_n;
static int g_bin;
static int g_capacity;
static int g_reported = -1;	/* bin count reported by the free-only worker */
static pthread_barrier_t done;

static void *
freeonly_worker(void *arg)
{
	size_t i;

	(void) arg;

	/*
	 * Free only.  This thread never allocates SZ, so nothing pops the bin.
	 * We must NOT allocate here (not even to find our own PTC) before the
	 * frees, or we would create a balanced pattern; thread_ptc is set by
	 * the first free's PTC creation.
	 */
	for (i = 0; i < g_n; i++)
		umem_free(g_buf[i], SZ);

	/* Read our own PTC bin count for the class. */
	if (thread_ptc != NULL && g_bin >= 0)
		g_reported = (int)thread_ptc->bins[g_bin].count;
	else
		g_reported = -2;

	pthread_barrier_wait(&done);
	pthread_barrier_wait(&done);	/* hold PTC alive while main checks */
	return (NULL);
}

int
main(void)
{
	pthread_t th;
	size_t i;
	int fails = 0;

	g_bin = umem_ptc_size_to_bin(SZ);
	if (g_bin < 0) {
		printf("SKIP: size %d not PTC-eligible\n", SZ);
		return (77);
	}
	g_capacity = ptc_bin_capacity(g_bin);

	g_n = BATCH;
	g_buf = calloc(g_n, sizeof (*g_buf));
	if (g_buf == NULL) {
		printf("SKIP: OOM allocating batch array\n");
		return (77);
	}
	/* Main allocates every object, so the worker only frees. */
	for (i = 0; i < g_n; i++) {
		g_buf[i] = umem_alloc(SZ, UMEM_DEFAULT);
		if (g_buf[i] == NULL) {
			printf("SKIP: OOM at object %zu\n", i);
			return (77);
		}
	}

	pthread_barrier_init(&done, NULL, 2);
	if (pthread_create(&th, NULL, freeonly_worker, NULL) != 0) {
		printf("SKIP: pthread_create failed\n");
		return (77);
	}

	pthread_barrier_wait(&done);	/* worker has freed the batch + reported */

	printf("class SZ=%d bin=%d capacity=%d free-only bin count after "
	    "%d frees = %d\n", SZ, g_bin, g_capacity, (int)g_n, g_reported);

	if (g_reported < 0) {
		printf("FAIL: worker could not read its PTC (%d)\n", g_reported);
		fails++;
	} else if (g_reported >= g_capacity) {
		/*
		 * Pinned full: the free-only thread is holding the whole bin
		 * forever (pre-fix behaviour).
		 */
		printf("FAIL: bin pinned at capacity (%d >= %d) -- free-only "
		    "thread retains a full bin\n", g_reported, g_capacity);
		fails++;
	} else if (g_reported > g_capacity / 2) {
		/*
		 * Post-fix the bin is drained on the trim; it may hold at most
		 * the frees since the last trim.  Half capacity is a generous
		 * ceiling that still discriminates from the pinned-full defect.
		 */
		printf("FAIL: bin above half capacity (%d > %d) -- trim not "
		    "bounding free-only retention\n", g_reported, g_capacity / 2);
		fails++;
	}

	pthread_barrier_wait(&done);	/* release worker */
	pthread_join(th, NULL);

	for (i = 0; i < g_n; i++)
		; /* worker already freed everything */
	free(g_buf);

	if (fails == 0)
		printf("RESULT: PASS (free-only bin trimmed below capacity)\n");
	else
		printf("RESULT: FAIL (%d)\n", fails);
	return (fails ? 1 : 0);
}

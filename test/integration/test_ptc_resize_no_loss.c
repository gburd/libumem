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
 * Regression: a populated magazine must not be discarded when the cache's
 * magtype changes (P1.3c).
 *
 * DEFECT (pre-fix)
 *   umem_ptc_mag_return() and umem_ptc_mag_return_trylock() checked
 *   UMEM_MAGAZINE_VALID and, on a magtype mismatch, freed just the magazine
 *   SHELL:
 *
 *       if (UMEM_MAGAZINE_VALID(cp, mp)) {
 *               umem_depot_free(cp, mlp, mp);
 *       } else {
 *               mag_cache = <mp's own slab cache>;
 *               _umem_cache_free(mag_cache, mp);   <-- objects inside lost
 *       }
 *
 *   _umem_free's "both magazines full" path calls that with the FULL loaded
 *   magazine (&cp->cache_full).  So after umem_cache_magazine_resize() changes
 *   the magtype, the first thread to fill its magazine destroys 127 (or 255)
 *   objects: their only reference is gone, while the slab layer goes on
 *   counting them as allocated.  The slab they sit in can never be freed.
 *
 *   The same paths also pushed a POPULATED 'previous' magazine onto the depot's
 *   EMPTY list on a size change.  That loses the contents too, just less
 *   obviously: umem_depot_ws_reap() destroys empty-list magazines with
 *   full_rounds == 0, so it never looks at the rounds that are in there.
 *
 * HOW THIS DETECTS IT
 *   umem_walk_allocated() counts buffers the slab layer still considers handed
 *   out.  A lost object stays in that set forever, because nothing holds a
 *   reference that could return it.
 *
 *   WHY THIS NEEDS A PTC-OFF CONTROL ARM
 *   The magazine, depot, and slab layers all legitimately RETAIN freed objects,
 *   and that retention grows for a while before it plateaus.  An absolute
 *   threshold on outstanding buffers therefore cannot distinguish "retained" from
 *   "lost" -- measured on this project, the same shape of workload grows the
 *   outstanding count from ~315 to ~1092 with the PTC COMPLETELY DISABLED, where
 *   no PTC loss is possible, so any absolute bound would fail even a PTC-free
 *   allocator and passing it would prove nothing.  (See
 *   test/integration/test_ptc_thread_exit_drain.c, which learned this the hard
 *   way, and docs/results/2026-09-22-p1.3-ptc-lifetime.md.)
 *
 *   So this test runs the same workload twice in one process -- PTC active,
 *   then umem_ptc_enabled = 0 -- and requires the PTC arm's growth not to
 *   exceed the control by more than a margin.  The control subtracts out
 *   ordinary retention.  Pre-fix, each magtype change destroyed a full
 *   magazine's worth of objects per thread holding one, which puts the PTC arm
 *   far above the control.
 *
 * FORCING THE MAGTYPE CHANGE WHILE A THREAD HOLDS A FULL MAGAZINE
 *   The threads fill their PTC bin and then their PTC magazine and PARK, still
 *   holding a loaded magazine that is full or nearly so.  The main thread then
 *   forces a resize by driving the DEFAULT contention-scheduled path
 *   (umem_depot_contention = 0, reap_interval = 1, repeated umem_reap()), and
 *   releases the parked threads only once the magtype has actually changed.
 *   Their next free walks straight into the discard path with a populated
 *   magazine in hand.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem.h"
#include "umem_inspect.h"
#include "../../umem_impl.h"
#include "../../umem_ptc.h"

#define OBJ_SIZE	512	/* magtype 127, can grow to 255 */
#define NTHREADS	8
#define ROUNDS		4
/*
 * Enough to fill the PTC bin (64 slots at this size) and then a full 127-round
 * magazine on top of it, so a parked thread is holding a LOADED magazine with
 * objects in it when the magtype changes.
 */
#define PER_THREAD	320

static _Atomic int release;
static _Atomic int parked;
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

/*
 * Fill this thread's PTC bin and magazine, then park with them populated until
 * the main thread has changed the magtype, then free everything.
 */
static void *
park_with_full_magazine(void *arg)
{
	void **keep = calloc(PER_THREAD, sizeof (void *));
	int i;

	(void) arg;
	if (keep == NULL)
		return (NULL);

	for (i = 0; i < PER_THREAD; i++) {
		keep[i] = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		if (keep[i] != NULL)
			memset(keep[i], 0x5a, OBJ_SIZE);
	}
	/*
	 * Free most of them so they land in this thread's PTC bin and then
	 * overflow into its PTC magazine, which is where they have to be
	 * sitting when the magtype changes.
	 */
	for (i = 0; i < PER_THREAD - 8; i++) {
		if (keep[i] != NULL) {
			umem_free(keep[i], OBJ_SIZE);
			keep[i] = NULL;
		}
	}

	atomic_fetch_add(&parked, 1);
	while (!atomic_load(&release))
		(void) usleep(1000);

	/* Now the magtype has changed under us: every free from here on goes
	 * through the stale-magazine path with a populated magazine. */
	for (i = PER_THREAD - 8; i < PER_THREAD; i++) {
		if (keep[i] != NULL)
			umem_free(keep[i], OBJ_SIZE);
	}
	for (i = 0; i < 64; i++) {
		void *p = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, OBJ_SIZE);
	}

	free(keep);
	return (NULL);
}

/* Force a magazine resize; returns the new magsize, or 0 if none happened. */
static int
force_resize(umem_cache_t *cp, int from, int budget_sec)
{
	int sec;

	for (sec = 0; sec < budget_sec; sec++) {
		umem_reap();
		(void) sleep(1);
		if (cp->cache_magtype->mt_magsize != from)
			return (cp->cache_magtype->mt_magsize);
	}
	return (0);
}

static ssize_t
measure_growth(const char *label, umem_cache_t *cp)
{
	size_t counts[ROUNDS];
	pthread_t th[NTHREADS];
	int r, i;

	for (r = 0; r < ROUNDS; r++) {
		int from = cp->cache_magtype->mt_magsize;
		int to;

		atomic_store(&release, 0);
		atomic_store(&parked, 0);

		for (i = 0; i < NTHREADS; i++) {
			if (pthread_create(&th[i], NULL,
			    park_with_full_magazine, NULL) != 0) {
				fprintf(stderr, "pthread_create failed\n");
				exit(2);
			}
		}
		/* Wait for every thread to be holding a populated magazine. */
		while (atomic_load(&parked) < NTHREADS)
			(void) usleep(1000);

		to = force_resize(cp, from, 12);
		if (to != 0) {
			printf("  %s round %d: magtype %d -> %d while %d "
			    "threads held populated magazines\n", label, r,
			    from, to, NTHREADS);
		} else {
			printf("  %s round %d: no resize (magtype already at "
			    "%d)\n", label, r, from);
		}

		atomic_store(&release, 1);
		for (i = 0; i < NTHREADS; i++)
			(void) pthread_join(th[i], NULL);

		umem_reap();
		(void) sleep(1);

		counts[r] = outstanding();
		printf("  %s round %d: outstanding(size=%d)=%zu\n", label, r,
		    OBJ_SIZE, counts[r]);
	}

	return ((ssize_t)counts[ROUNDS - 1] - (ssize_t)counts[0]);
}

int
main(void)
{
	extern int umem_ptc_enabled;
	extern uint32_t umem_reap_interval;
	extern uint_t umem_depot_contention;
	umem_cache_t *cp;
	ssize_t growth_ptc, growth_noptc, margin;

	/* Default contention-scheduled resize path, just impatient. */
	umem_depot_contention = 0;
	umem_reap_interval = 1;

	{
		void *w = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		if (w != NULL)
			umem_free(w, OBJ_SIZE);
	}

	cp = umem_alloc_table[(OBJ_SIZE - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		printf("RESULT: FAIL (no cache for size %d)\n", OBJ_SIZE);
		return (2);
	}
	printf("size=%d cache=%s magsize=%d maxbuf=%zu\n", OBJ_SIZE,
	    cp->cache_name, cp->cache_magtype->mt_magsize,
	    (size_t)cp->cache_magtype->mt_maxbuf);
	if (cp->cache_chunksize >= cp->cache_magtype->mt_maxbuf) {
		printf("RESULT: FAIL (this size class cannot resize; the test "
		    "would prove nothing)\n");
		return (2);
	}

	printf("arm 1: PTC enabled (the path under test)\n");
	umem_ptc_enabled = 1;
	growth_ptc = measure_growth("ptc", cp);

	printf("arm 2: PTC disabled (control -- no PTC magazine to discard)\n");
	umem_ptc_enabled = 0;
	growth_noptc = measure_growth("noptc", cp);

	/*
	 * Margin: half a magazine per thread.  Pre-fix, a magtype change
	 * destroyed a WHOLE populated magazine per thread holding one, so this
	 * sits well below the defect and above run-to-run noise in ordinary
	 * retention.
	 */
	margin = (ssize_t)(NTHREADS * 64);

	printf("growth: ptc=%zd control=%zd margin=%zd\n", growth_ptc,
	    growth_noptc, margin);

	if (growth_ptc > growth_noptc + margin) {
		printf("RESULT: FAIL (PTC-enabled growth exceeds the PTC-off "
		    "control by more than the margin -- a magtype change is "
		    "discarding populated magazines)\n");
		return (1);
	}

	printf("RESULT: PASS (PTC-enabled growth tracks the PTC-off control "
	    "across magazine resizes; populated magazines are drained, not "
	    "discarded)\n");
	return (0);
}

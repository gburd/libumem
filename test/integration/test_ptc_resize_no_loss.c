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
 *   the magtype, the first thread to fill its magazine destroys 127 objects:
 *   their only reference is gone while the slab layer goes on counting them as
 *   allocated, so the slab holding them can never be freed.
 *
 *   The same paths also pushed a POPULATED 'previous' magazine onto the depot's
 *   EMPTY list on a size change.  That loses the contents too, just less
 *   visibly: umem_depot_ws_reap() destroys empty-list magazines with
 *   full_rounds == 0, so it never looks at the rounds that are in there.
 *
 * HOW THIS DETECTS IT
 *   umem_walk_allocated() counts buffers the slab layer still considers handed
 *   out.  A lost object stays in that set forever, because nothing holds a
 *   reference that could return it.
 *
 *   Every allocation is made by a thread that later EXITS, and thread exit
 *   drains the PTC completely (P1.3a, already fixed).  The main thread never
 *   allocates at this size class.  So once the threads are joined, no
 *   per-thread cache holds an object of this size, and the count is then driven
 *   to its FLOOR by reaping until it stops falling: retention is released by
 *   reaping, loss is not.
 *
 * THE CONTROL: AN IDENTICAL ROUND WITH NO RESIZE IN IT
 *   An absolute bound on the floor cannot work.  The magazine, depot, and slab
 *   layers all legitimately retain freed objects, and per-CPU loaded magazines
 *   are not reachable by umem_reap() at all, so a PTC-free allocator leaves
 *   hundreds to thousands of buffers outstanding by design.  This is the same
 *   trap test/integration/test_ptc_thread_exit_drain.c documents: measured
 *   there, the workload grew the outstanding count from ~315 to ~1092 with the
 *   PTC COMPLETELY DISABLED, so any absolute threshold would fail even an
 *   allocator with no PTC in it and passing one would prove nothing.
 *
 *   That test's control is umem_ptc_enabled = 0.  That control is WRONG for this
 *   defect, and an earlier version of this test used it and produced garbage:
 *   turning the PTC off MOVES the retention, it does not just remove the defect.
 *   With the PTC off, freed objects pile into per-CPU magazines that umem_reap()
 *   cannot drain; with it on, they sit in PTC magazines that thread exit drains
 *   completely.  The two arms have different retention structures, so their
 *   floors are not comparable -- measured on a FIXED build across three runs,
 *   the PTC-off control floor came out 64, 64, and 1755, swamping the defect.
 *
 *   So the control here holds the PTC ON in both arms and varies only the one
 *   thing the defect is about: whether a magazine resize happens.  Both rounds
 *   run in ONE process, back to back, same warm caches, same threads, same code
 *   path:
 *
 *     round 1 (control): resize SUPPRESSED (umem_depot_contention = INT_MAX, so
 *                        umem_cache_update never schedules UMU_MAGAZINE_RESIZE)
 *     round 2 (test):    resize FORCED via the ordinary contention-scheduled
 *                        path (umem_depot_contention = 0)
 *
 *   The control round also warms retention to its plateau, so round 2's floor
 *   adds only what the resize itself cost.  The comparison is conservative in
 *   the right direction: umem_cache_magazine_resize() PURGES every per-CPU
 *   magazine, which RELEASES objects the control round is still holding, so a
 *   correct allocator makes round 2's floor come out at or below round 1's.
 *   Any excess is loss.
 *
 * FORCING THE CHANGE WHILE A THREAD HOLDS A FULL MAGAZINE
 *   The threads fill their PTC bin and then their PTC magazine and PARK, still
 *   holding a loaded magazine with objects in it.  The main thread forces the
 *   resize and releases them only once the magtype has actually changed, so
 *   their next free walks straight into the discard path with a populated
 *   magazine in hand.  umem_magazine_tuning is deliberately left off: the defect
 *   is reachable through ordinary depot contention without it.
 *
 *   If the resize does not happen, the test reports INCONCLUSIVE, never PASS.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <limits.h>
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

#define OBJ_SIZE	512	/* magtype 127, one step available: -> 255 */
#define NTHREADS	8
/*
 * Enough to fill the PTC bin (64 slots at this size) and then a full 127-round
 * magazine on top of it, so a parked thread holds a LOADED magazine with
 * objects in it when the magtype changes.
 */
#define PER_THREAD	320
#define HOLD_BACK	8	/* freed only after the magtype has changed */
#define POST_PASSES	3	/* alloc/free passes after the magtype changed */

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
 * Fill this thread's PTC bin and magazine, park with them populated until the
 * main thread is done with the magtype, then free the rest and exit.
 */
static void *
park_with_full_magazine(void *arg)
{
	void **keep = calloc(PER_THREAD, sizeof (void *));
	int i, pass;

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
	for (i = 0; i < PER_THREAD - HOLD_BACK; i++) {
		if (keep[i] != NULL) {
			umem_free(keep[i], OBJ_SIZE);
			keep[i] = NULL;
		}
	}

	atomic_fetch_add(&parked, 1);
	while (!atomic_load(&release))
		(void) usleep(1000);

	/*
	 * In the test round the magtype has changed under us by now, so every
	 * free that fills this thread's loaded magazine reaches the
	 * stale-magazine path with a POPULATED magazine -- the exact call that
	 * pre-fix freed the shell and lost the contents.
	 *
	 * Churn enough to flush the magazine several times: each flush is one
	 * opportunity for the defect, so the signal is proportional to volume
	 * rather than depending on catching a single event.
	 */
	for (i = PER_THREAD - HOLD_BACK; i < PER_THREAD; i++) {
		if (keep[i] != NULL)
			umem_free(keep[i], OBJ_SIZE);
	}
	for (pass = 0; pass < POST_PASSES; pass++) {
		for (i = 0; i < PER_THREAD; i++)
			keep[i] = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		for (i = 0; i < PER_THREAD; i++) {
			if (keep[i] != NULL) {
				umem_free(keep[i], OBJ_SIZE);
				keep[i] = NULL;
			}
		}
	}

	free(keep);
	return (NULL);		/* PTC destructor runs here */
}

/*
 * Reap until the outstanding count stops falling, and return the floor.
 *
 * Retention is released by reaping; loss is not.  Stop after five consecutive
 * non-improving samples, bounded at 30 so a pathological case cannot hang the
 * test.  A single post-reap sample is not usable: the depot's working-set logic
 * needs two update cycles to release a magazine.
 */
static long
reap_to_floor(void)
{
	long best = (long)outstanding();
	int stale = 0, iter;

	for (iter = 0; iter < 30 && stale < 5; iter++) {
		long now;

		umem_reap();
		(void) sleep(1);
		now = (long)outstanding();
		if (now < best) {
			best = now;
			stale = 0;
		} else {
			stale++;
		}
	}
	return (best);
}

/*
 * One round: NTHREADS threads park holding populated PTC magazines; if
 * want_resize, force a magtype change while they are parked; release, join,
 * reap to the floor.
 *
 * Returns the floor.  *observed_to is the new magsize if a resize happened,
 * 0 otherwise.
 *
 * ORDERING MATTERS, AND GETTING IT WRONG MAKES THE TEST USELESS.  An earlier
 * version opened the contention gate (umem_depot_contention = 0) before the
 * threads had parked.  The update thread then often resized DURING the ramp, so
 * the threads ended up parked holding magazines of the NEW magtype and their
 * later frees never reached the discard path at all -- and the test reported
 * PASS on a build with the defect present.  The log signature of that mistake is
 * a test round whose outstanding-while-parked count is LOWER than the control's
 * (1172 vs 2190), because the purge inside the resize already ran.
 *
 * So: keep the resize suppressed until every thread has parked AND the magtype
 * is confirmed unchanged, and only then open the gate.
 */
static long
run_round(const char *label, umem_cache_t *cp, int want_resize,
    int *observed_from, int *observed_to)
{
	extern uint32_t umem_reap_interval;
	extern uint_t umem_depot_contention;
	pthread_t th[NTHREADS];
	long floor;
	size_t during;
	int i, sec;

	/* Suppressed for the whole parking ramp, in BOTH rounds. */
	umem_depot_contention = UINT_MAX;
	umem_reap_interval = 1;

	atomic_store(&release, 0);
	atomic_store(&parked, 0);

	for (i = 0; i < NTHREADS; i++) {
		if (pthread_create(&th[i], NULL, park_with_full_magazine,
		    NULL) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			exit(2);
		}
	}
	/* Wait until every thread is holding a populated magazine. */
	while (atomic_load(&parked) < NTHREADS)
		(void) usleep(1000);

	/*
	 * Sample the magtype only NOW: this is the magtype every parked thread's
	 * magazines were allocated under, which is what the resize has to move
	 * away from for the window to open.
	 */
	*observed_from = cp->cache_magtype->mt_magsize;
	*observed_to = 0;
	during = outstanding();

	if (want_resize) {
		/* Only now open the ordinary contention-scheduled path. */
		umem_depot_contention = 0;
		for (sec = 0; sec < 20; sec++) {
			umem_reap();
			(void) sleep(1);
			if (cp->cache_magtype->mt_magsize != *observed_from) {
				*observed_to = cp->cache_magtype->mt_magsize;
				break;
			}
		}
		umem_depot_contention = UINT_MAX;
	} else {
		/* Same elapsed time and same reap pressure, no resize. */
		for (sec = 0; sec < 5; sec++) {
			umem_reap();
			(void) sleep(1);
		}
		if (cp->cache_magtype->mt_magsize != *observed_from)
			*observed_to = cp->cache_magtype->mt_magsize;
	}

	atomic_store(&release, 1);
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);

	floor = reap_to_floor();
	printf("  %s: magsize %d -> %s, outstanding %zu (threads parked) "
	    "-> %ld (floor, all allocating threads exited)\n", label,
	    *observed_from,
	    *observed_to ? "resized" : "unchanged", during, floor);
	if (*observed_to != 0)
		printf("    resize observed: %d -> %d rounds while %d threads "
		    "held populated magazines\n", *observed_from, *observed_to,
		    NTHREADS);
	(void) fflush(stdout);
	return (floor);
}

int
main(void)
{
	umem_cache_t *cp;
	long floor_control, floor_test, margin;
	int c_from, c_to, t_from, t_to;

	/*
	 * Probe the size class from a thread that exits, so this process's own
	 * PTC never holds an object of this size (it would never be drained,
	 * since main() does not exit until after the measurement).
	 */
	{
		pthread_t pt;
		atomic_store(&release, 1);
		if (pthread_create(&pt, NULL, park_with_full_magazine,
		    NULL) != 0)
			return (2);
		(void) pthread_join(pt, NULL);
		atomic_store(&parked, 0);
	}

	cp = umem_alloc_table[(OBJ_SIZE - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		printf("RESULT: FAIL (no cache for size %d)\n", OBJ_SIZE);
		return (2);
	}
	printf("size=%d cache=%s chunksize=%zu magsize=%d maxbuf=%zu\n",
	    OBJ_SIZE, cp->cache_name, cp->cache_chunksize,
	    cp->cache_magtype->mt_magsize,
	    (size_t)cp->cache_magtype->mt_maxbuf);
	if (cp->cache_chunksize >= cp->cache_magtype->mt_maxbuf) {
		printf("RESULT: FAIL (this size class cannot resize; the test "
		    "would prove nothing)\n");
		return (2);
	}

	printf("round 1 (control): resize suppressed; identical in every other "
	    "way, and warms retention to its plateau\n");
	floor_control = run_round("control", cp, 0, &c_from, &c_to);

	printf("round 2 (test): resize forced while threads hold populated "
	    "magazines\n");
	floor_test = run_round("test", cp, 1, &t_from, &t_to);

	if (c_to != 0) {
		printf("RESULT: INCONCLUSIVE (the control round resized, so it "
		    "is not a control)\n");
		return (3);
	}
	if (t_to == 0) {
		printf("RESULT: INCONCLUSIVE (no magazine resize happened, so "
		    "the window under test was never opened)\n");
		return (3);
	}

	/*
	 * Margin: half a magazine per thread.  Pre-fix, a magtype change
	 * destroyed a WHOLE populated magazine per thread holding one
	 * (8 x 127 = 1016 objects), so this sits well below the defect and
	 * above run-to-run noise in ordinary retention.
	 */
	margin = NTHREADS * 64;

	printf("floor: control(no resize)=%ld test(resize %d->%d)=%ld "
	    "margin=%ld\n", floor_control, t_from, t_to, floor_test, margin);

	if (floor_test > floor_control + margin) {
		printf("RESULT: FAIL (the round with a magazine resize leaves "
		    "%ld more buffers permanently outstanding than the "
		    "otherwise-identical round without one -- populated "
		    "magazines are being discarded)\n",
		    floor_test - floor_control);
		return (1);
	}

	printf("RESULT: PASS (a %d -> %d resize with %d threads holding "
	    "populated magazines leaves no more outstanding than the same "
	    "workload without a resize; populated magazines are drained, not "
	    "discarded)\n", t_from, t_to, NTHREADS);
	return (0);
}

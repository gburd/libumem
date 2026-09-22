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
 *   their only reference is gone, while the slab layer goes on counting them as
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
 *   Every allocation in this test is made by a thread that later EXITS, and
 *   thread exit drains the PTC completely (P1.3a, already fixed).  The main
 *   thread never allocates at this size class, so once all threads are joined
 *   and the allocator has been reaped, NO per-thread cache holds an object of
 *   this size and the residual outstanding count is the measurement: retained
 *   objects have come back, lost ones cannot.
 *
 *   WHY THIS NEEDS A PTC-OFF CONTROL ARM
 *   The magazine, depot, and slab layers all legitimately RETAIN freed objects,
 *   and that retention grows for a while before it plateaus.  An absolute
 *   threshold on outstanding buffers cannot distinguish "retained" from "lost"
 *   -- measured on this project, a workload of this shape grows the outstanding
 *   count from ~315 to ~1092 with the PTC COMPLETELY DISABLED, where no PTC loss
 *   is possible.  Any absolute bound would fail even a PTC-free allocator, so
 *   passing it would prove nothing.  (See
 *   test/integration/test_ptc_thread_exit_drain.c, which learned this the hard
 *   way, and docs/results/2026-09-22-p1.3-ptc-lifetime.md.)
 *
 *   So the same workload runs twice and the residuals are compared: PTC active,
 *   and PTC bypassed (umem_ptc_enabled = 0).  The control subtracts out ordinary
 *   retention.
 *
 *   An earlier version of this test compared the CHANGE in outstanding buffers
 *   across the resize instead of the residual.  That was not a usable signal:
 *   umem_cache_magazine_resize() calls umem_cache_magazine_purge() first, which
 *   destroys every per-CPU magazine, so both arms drop by one to two thousand
 *   buffers and the two arms' baselines differ by more than the defect does
 *   (observed: ptc -1536, control -2120, on a build where the defect was fixed).
 *   The residual after everything has quiesced has no such term in it.
 *
 *   WHY THE TWO ARMS ARE SEPARATE PROCESSES
 *   A cache's magtype only ever GROWS, and at this size class it has exactly one
 *   step to take (127 -> 255 rounds; the next magtype's mt_maxbuf is below this
 *   chunk size).  Both arms need that one step, so they cannot share a process:
 *   whichever ran second would find the magtype already at its maximum and
 *   measure a workload with no resize in it at all.  Each arm therefore runs in
 *   a forked child with its own fresh allocator state.  The fork happens while
 *   this process is still single-threaded, before either arm creates threads.
 *
 * FORCING THE MAGTYPE CHANGE WHILE A THREAD HOLDS A FULL MAGAZINE
 *   The threads fill their PTC bin and then their PTC magazine and PARK, still
 *   holding a loaded magazine with objects in it.  The child then forces a
 *   resize by driving the DEFAULT contention-scheduled path
 *   (umem_depot_contention = 0, reap_interval = 1, repeated umem_reap()) -- the
 *   umem_magazine_tuning option is deliberately left off, because the defect is
 *   reachable without it -- and releases the parked threads only once the
 *   magtype has actually changed.  Their next free then walks straight into the
 *   discard path with a populated magazine in hand.
 *
 *   If no resize happens the child reports that, and the test reports
 *   INCONCLUSIVE rather than passing: this test must never be green without
 *   having opened the window it exists to test.
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
#include <sys/wait.h>
#include <unistd.h>

#include "umem.h"
#include "umem_inspect.h"
#include "../../umem_impl.h"
#include "../../umem_ptc.h"

#define OBJ_SIZE	512	/* magtype 127, one step to 255 */
#define NTHREADS	8
/*
 * Enough to fill the PTC bin (64 slots at this size) and then a full 127-round
 * magazine on top of it, so a parked thread holds a LOADED magazine with
 * objects in it when the magtype changes.
 */
#define PER_THREAD	320
#define HOLD_BACK	8	/* freed only after the magtype has changed */

/* Result the child reports to the parent. */
struct arm_result {
	long	residual;	/* outstanding once everything has quiesced */
	int	from;		/* magsize before */
	int	to;		/* magsize after (0 == no resize happened) */
};

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
 * magtype has changed, then free the rest.
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
	 * The magtype has now changed under us, so every free from here on
	 * reaches the stale-magazine path with a populated magazine.
	 */
	for (i = PER_THREAD - HOLD_BACK; i < PER_THREAD; i++) {
		if (keep[i] != NULL)
			umem_free(keep[i], OBJ_SIZE);
	}
	for (i = 0; i < 128; i++) {
		void *p = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, OBJ_SIZE);
	}

	free(keep);
	return (NULL);
}

/*
 * Warm up in a thread that then EXITS, so ordinary retention is near its
 * plateau before the measurement starts and no PTC is left holding objects of
 * this size afterwards.  Single worker: concurrent warm-up would record depot
 * contention and spend the one available magtype step before the parked
 * threads exist.
 */
static void *
warmup(void *arg)
{
	void **warm = calloc(PER_THREAD * 2, sizeof (void *));
	int pass, i;

	(void) arg;
	if (warm == NULL)
		return (NULL);
	for (pass = 0; pass < 4; pass++) {
		for (i = 0; i < PER_THREAD * 2; i++)
			warm[i] = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		for (i = 0; i < PER_THREAD * 2; i++) {
			if (warm[i] != NULL)
				umem_free(warm[i], OBJ_SIZE);
		}
	}
	free(warm);
	return (NULL);
}

/*
 * Run one arm to completion in this (freshly forked) process and report the
 * outstanding buffers left over once every allocating thread has exited.
 */
static void
run_arm(const char *label, int ptc_on, struct arm_result *out)
{
	extern int umem_ptc_enabled;
	extern uint32_t umem_reap_interval;
	extern uint_t umem_depot_contention;
	umem_cache_t *cp;
	pthread_t th[NTHREADS];
	pthread_t wt;
	size_t before;
	int i, sec;

	umem_ptc_enabled = ptc_on;
	/* The ordinary contention-scheduled resize path, just impatient. */
	umem_depot_contention = 0;
	umem_reap_interval = 1;

	cp = umem_alloc_table[(OBJ_SIZE - 1) >> UMEM_ALIGN_SHIFT];
	out->from = cp->cache_magtype->mt_magsize;
	out->to = 0;
	out->residual = 0;

	if (pthread_create(&wt, NULL, warmup, NULL) != 0)
		exit(2);
	(void) pthread_join(wt, NULL);

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

	before = outstanding();

	for (sec = 0; sec < 20; sec++) {
		umem_reap();
		(void) sleep(1);
		if (cp->cache_magtype->mt_magsize != out->from) {
			out->to = cp->cache_magtype->mt_magsize;
			break;
		}
	}

	atomic_store(&release, 1);
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);

	/*
	 * Every thread that ever allocated at this size has exited, so every
	 * PTC that held one of these objects has been drained.  Reap twice:
	 * the first pass pushes magazine contents down, the second lets the
	 * depot's two-cycle working-set logic release them.
	 */
	umem_reap();
	(void) sleep(2);
	umem_reap();
	(void) sleep(2);

	out->residual = (long)outstanding();
	printf("  %s: magsize %d -> %d, outstanding %zu (during) -> %ld "
	    "(residual, all allocating threads exited)\n", label, out->from,
	    out->to, before, out->residual);
	(void) fflush(stdout);
}

/* Fork a child that runs one arm, and collect its result. */
static int
fork_arm(const char *label, int ptc_on, struct arm_result *out)
{
	int fds[2];
	pid_t pid;
	int status;
	ssize_t n;

	if (pipe(fds) != 0)
		return (-1);

	pid = fork();
	if (pid < 0) {
		(void) close(fds[0]);
		(void) close(fds[1]);
		return (-1);
	}
	if (pid == 0) {
		struct arm_result r;
		(void) close(fds[0]);
		run_arm(label, ptc_on, &r);
		(void) write(fds[1], &r, sizeof (r));
		(void) close(fds[1]);
		_exit(0);
	}

	(void) close(fds[1]);
	n = read(fds[0], out, sizeof (*out));
	(void) close(fds[0]);
	(void) waitpid(pid, &status, 0);

	if (n != (ssize_t)sizeof (*out)) {
		fprintf(stderr, "%s arm did not report a result (status %d)\n",
		    label, status);
		return (-1);
	}
	return (0);
}

int
main(void)
{
	umem_cache_t *cp;
	struct arm_result ptc, control;
	long margin;

	/*
	 * Probe the size class from the still-single-threaded parent.  Done in
	 * a thread that exits, so the parent's own PTC never holds an object of
	 * this size (it would be inherited by both children).
	 */
	{
		pthread_t pt;
		if (pthread_create(&pt, NULL, warmup, NULL) != 0)
			return (2);
		(void) pthread_join(pt, NULL);
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

	/*
	 * Each arm in its own child: the single available magtype step has to
	 * be available to both.  Forked while single-threaded.
	 */
	printf("arm 1: PTC enabled (the path under test)\n");
	if (fork_arm("ptc", 1, &ptc) != 0)
		return (2);

	printf("arm 2: PTC disabled (control -- no PTC magazine to discard)\n");
	if (fork_arm("control", 0, &control) != 0)
		return (2);

	if (ptc.to == 0 || control.to == 0) {
		printf("RESULT: INCONCLUSIVE (no magazine resize happened in "
		    "%s arm, so the window under test was never opened)\n",
		    ptc.to == 0 ? "the PTC" : "the control");
		return (3);
	}

	/*
	 * Margin: half a magazine per thread.  Pre-fix, a magtype change
	 * destroyed a WHOLE populated magazine per thread holding one
	 * (8 x 127 = 1016 objects), so this sits well below the defect and
	 * above run-to-run noise in ordinary retention.
	 */
	margin = NTHREADS * 64;

	printf("residual: ptc=%ld control=%ld margin=%ld\n", ptc.residual,
	    control.residual, margin);

	if (ptc.residual > control.residual + margin) {
		printf("RESULT: FAIL (with the PTC active, more buffers remain "
		    "outstanding after every allocating thread exited than the "
		    "PTC-off control leaves, by more than the margin -- "
		    "populated magazines are being discarded)\n");
		return (1);
	}

	printf("RESULT: PASS (across the %d -> %d resize, the PTC arm leaves no "
	    "more outstanding than the PTC-off control; populated magazines "
	    "are drained, not discarded)\n", ptc.from, ptc.to);
	return (0);
}

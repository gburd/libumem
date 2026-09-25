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
 * P1.3d regression: a fork child drains the PTCs of threads that did not
 * survive the fork.
 *
 * THE DEFECT.  Every thread has a per-thread cache (umem_ptc_t: up to 36
 * bins of cached objects plus two magazines, ~24 KB).  fork() copies them all
 * into the child, where the owning threads do not exist.  Nothing drained
 * them: every object cached in a non-forking thread's PTC at the instant of
 * fork was leaked in the child, permanently, along with the struct -- the
 * P1.3a stranded-object defect with fork as the thread death.  For a process
 * that forks repeatedly from a multithreaded parent that is forks x threads x
 * cached objects (production-readiness review 2026-09-24, 1.6).
 *
 * WHAT THIS TESTS, exactly.  NTHREADS workers each allocate PER_THREAD
 * objects, free them (so they sit in the worker's PTC bins and magazines --
 * not the depot), and park.  The main thread forks.  In the CHILD, the fork
 * handler has already run; the test then exits every worker's PTC path by...
 * nothing: the workers do not exist in the child.  So the child reads the
 * probe build's ledgers:
 *
 *   umem_ptc_probe_fork_drained     PTCs the child handler drained
 *   umem_ptc_probe_fork_busy_leaked PTCs skipped because fork_busy was set
 *   umem_ptc_probe_fork_top_dropped top entries the child deliberately
 *                                   leaked (one per non-empty bin and per
 *                                   loaded/previous magazine: pushes are
 *                                   not fork-ordered, so the top entry may
 *                                   be stale -- umem_ptc.h rule 1)
 *   umem_ptc_probe_exit_stranded    objects still in a bin when a PTC was
 *                                   freed (the P1.3a oracle; the child's
 *                                   drain goes through the same
 *                                   umem_ptc_destroy and is counted the same)
 *
 *   PASS: drained + leaked == NTHREADS, stranded == 0, top_dropped <=
 *         drained x PTC_NBINS, and (workers are parked, so no PTC can be
 *         mid-swap) leaked == 0.
 *   FAIL: drained == 0 -- the handler did not run or found no registry.
 *
 * Pre-fix the registry does not exist and drained is 0; the child leaks
 * NTHREADS x (PER_THREAD objects + 24 KB) and reports nothing, because there
 * was no code to report it.  This test's own pre-fix run therefore fails on
 * "drained == 0", which is the honest statement of the defect: the child had
 * no way to even count what it lost.
 *
 * A second arm forks while the workers are STILL ALLOCATING (not parked), so
 * some PTCs may be snapshotted mid-swap: there leaked may be nonzero and the
 * assertion is drained + leaked == NTHREADS and stranded == 0 -- i.e. every
 * orphan was either drained or deliberately skipped, never drained torn.
 * Under the probe build this second arm also exercises P1.3d rule 1: a torn
 * push that the child drained would show up as a double free or a freed live
 * object, which the concurrency oracle's slab-layer checks would report.
 *
 * Needs the probe build (-DUMEM_PTC_RESIZE_PROBE); the plain build has no
 * ledgers and this test is not built against it.
 */

#include "umem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/wait.h>

#define	NTHREADS	8
#define	PER_THREAD	600	/* > one bin at every class; spills into mags */
#define	OBJ		96

extern volatile long umem_ptc_probe_fork_drained;
extern volatile long umem_ptc_probe_fork_busy_leaked;
extern volatile long umem_ptc_probe_exit_stranded;
extern volatile long umem_ptc_probe_fork_top_dropped;

static atomic_int parked, go_on, keep_running;

static void *
worker_park(void *arg)
{
	void *held[PER_THREAD];
	int i;

	(void) arg;
	for (i = 0; i < PER_THREAD; i++)
		held[i] = umem_alloc(OBJ, UMEM_DEFAULT);
	for (i = 0; i < PER_THREAD; i++)
		if (held[i] != NULL)
			umem_free(held[i], OBJ);
	atomic_fetch_add(&parked, 1);
	while (!atomic_load(&go_on))
		usleep(1000);
	return (NULL);
}

static void *
worker_churn(void *arg)
{
	void *held[64];
	int i;

	(void) arg;
	atomic_fetch_add(&parked, 1);
	while (atomic_load(&keep_running)) {
		for (i = 0; i < 64; i++)
			held[i] = umem_alloc(OBJ + (i % 4) * 16, UMEM_DEFAULT);
		for (i = 0; i < 64; i++)
			if (held[i] != NULL)
				umem_free(held[i], OBJ + (i % 4) * 16);
	}
	return (NULL);
}

static int
run_arm(const char *label, void *(*fn)(void *), int expect_no_leaked)
{
	pthread_t th[NTHREADS];
	pid_t pid;
	int status, i;

	atomic_store(&parked, 0);
	atomic_store(&go_on, 0);
	atomic_store(&keep_running, 1);

	for (i = 0; i < NTHREADS; i++)
		(void) pthread_create(&th[i], NULL, fn, NULL);
	while (atomic_load(&parked) < NTHREADS)
		usleep(1000);
	if (fn == worker_churn)
		usleep(20000);	/* let them get into their loops */

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return (77);
	}
	if (pid == 0) {
		long d = umem_ptc_probe_fork_drained;
		long l = umem_ptc_probe_fork_busy_leaked;
		long s = umem_ptc_probe_exit_stranded;
		long t = umem_ptc_probe_fork_top_dropped;
		int rc = 0;

		printf("  [%s] child: drained=%ld busy_leaked=%ld "
		    "stranded=%ld top_dropped=%ld (threads=%d)\n", label,
		    d, l, s, t, NTHREADS);
		if (t > d * 40 * 3) {	/* <= 36 bins + 2 mags per class per PTC */
			printf("  [%s] FAIL: %ld top slots dropped for %ld "
			    "PTCs -- more than one per bin\n", label, t, d);
			rc = 1;
		}
		if (d + l != NTHREADS) {
			printf("  [%s] FAIL: drained + leaked = %ld, expected "
			    "%d -- %s\n", label, d + l, NTHREADS,
			    d == 0 ? "the child did not drain any orphaned PTC"
			    : "some PTCs were neither drained nor counted");
			rc = 1;
		}
		if (s != 0) {
			printf("  [%s] FAIL: %ld objects stranded in drained "
			    "PTCs\n", label, s);
			rc = 1;
		}
		if (expect_no_leaked && l != 0) {
			printf("  [%s] FAIL: %ld PTCs skipped as busy with "
			    "every worker parked\n", label, l);
			rc = 1;
		}
		/* The child must still have a working allocator. */
		{
			void *p = umem_alloc(OBJ, UMEM_DEFAULT);
			if (p == NULL) {
				printf("  [%s] FAIL: umem_alloc failed in "
				    "child\n", label);
				rc = 1;
			} else {
				umem_free(p, OBJ);
			}
		}
		_exit(rc);
	}

	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
		printf("  [%s] FAIL: child did not exit normally\n", label);
		status = 1;
	} else {
		status = WEXITSTATUS(status);
	}

	atomic_store(&go_on, 1);
	atomic_store(&keep_running, 0);
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);
	return (status);
}

int
main(void)
{
	int rc = 0, r;

	printf("P1.3d: a fork child must drain the PTCs of threads that did "
	    "not survive fork()\n");

	r = run_arm("parked", worker_park, 1);
	if (r == 77)
		return (77);
	rc |= r;

	r = run_arm("churning", worker_churn, 0);
	if (r == 77)
		return (77);
	rc |= r;

	if (rc) {
		printf("RESULT: FAIL\n");
		return (1);
	}
	printf("RESULT: PASS (orphaned PTCs drained in the child; nothing "
	    "stranded; busy ones skipped, not torn)\n");
	return (0);
}

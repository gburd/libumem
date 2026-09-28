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
 * test_findleaks_rseq.c -- O9: findleaks must NOT report buffers that are
 * resident in rseq per-CPU magazines as outstanding.
 *
 * umem_inspect.c's cached_set_build_cache() subtracts, from the outstanding
 * ("leak") count, every buffer it can find still parked in a retention site:
 * depot full lists, per-CPU loaded/previous magazines, and -- the block this
 * test exercises -- each cache's rseq per-CPU loaded/previous magazine
 * (cache_rseq[], guarded by UMEM_RSEQ_AVAILABLE && umem_rseq_enabled).
 *
 * Phase 3 recorded that rseq block as compiled-but-never-exercised because
 * rseq then served zero rounds.  Since P8.5b the PTC magazine refill/flush
 * routes overflow THROUGH the rseq layer, but ONLY under multi-thread CPU
 * contention (single-threaded, rseq_alloc = rseq_free = 0; measured in
 * docs/results/2026-09-26-p85b-arch-rseq-feeds-ptc.md).  So this test drives
 * the same batch-alloc/free churn that probe_rseq_batch.c uses to arm and
 * populate the rseq layer, then QUIESCES the workers (kept alive, not joined:
 * thread exit drains the PTC) and, from the main thread, reads findleaks'
 * JSON cached_skipped while the last-freed rounds are still sitting in
 * cache_rseq[cpu].
 *
 * WHY NOT norseq FOR THE NEGATIVE ARM.  With rseq off, those buffers do not
 * vanish -- they sit in the DEPOT full list instead, which
 * cached_set_build_cache() ALSO enumerates and subtracts.  So norseq barely
 * moves cached_skipped and proves nothing about the rseq block specifically.
 *
 * The clean negative arm is a test-only build knob,
 * -DUMEM_FINDLEAKS_NO_RSEQ_SUBTRACT, that compiles out ONLY the rseq
 * subtraction block and changes nothing else (the default library is
 * byte-identical).  Both builds run this identical program:
 *
 *   default   (rseq subtracted):     rseq-resident rounds are NOT counted
 *   knob build (rseq NOT subtracted): the SAME rounds ARE counted
 *
 * so cached_skipped(default) > cached_skipped(knob) by exactly the rounds
 * this run left in rseq magazines.  The driver
 * (test_findleaks_rseq.sh) runs both and asserts that difference is
 * positive; a single build cannot see both, so each build just prints its
 * own number and the driver compares.
 *
 * If rseq is unavailable/disabled (no UMEM_RSEQ_AVAILABLE, non-x86/arm, or
 * the kernel refused registration) there is nothing to test: the program
 * still runs and prints its count; the driver detects a zero difference and
 * SKIPs.
 */

#define	_GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <umem.h>
#include "umem_inspect.h"

#define	BATCH	1024		/* > any magazine (max magsize 255) */
#define	SZ	64		/* PTC-served (<= umem_ptc_maxsize) */

static atomic_int g_stop = 0;
static atomic_int g_parked = 0;
static int g_nthreads;

/*
 * Churn batches to arm and fill the rseq per-CPU layer, then park (buffers
 * from the LAST freed batch stay in cache_rseq[cpu]).  Do not exit: thread
 * exit drains the PTC.
 */
static void *
worker(void *arg)
{
	(void) arg;
	void **buf = malloc(BATCH * sizeof (*buf));
	unsigned long iters = 0;

	if (buf == NULL)
		return (NULL);
	while (!atomic_load(&g_stop)) {
		int i;
		for (i = 0; i < BATCH; i++)
			buf[i] = umem_alloc(SZ, UMEM_DEFAULT);
		for (i = 0; i < BATCH; i++)
			if (buf[i] != NULL)
				*(char *)buf[i] = (char)i;
		for (i = 0; i < BATCH; i++)
			if (buf[i] != NULL)
				umem_free(buf[i], SZ);
		if ((++iters & 0x3f) == 0)
			sched_yield();
	}
	/* Parked: keep the thread (and its rseq CPU residency) alive. */
	atomic_fetch_add(&g_parked, 1);
	while (atomic_load(&g_parked) != -1)
		usleep(1000);
	free(buf);
	return (NULL);
}

static long
findleaks_cached_skipped(void)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *ms = open_memstream(&buf, &len);
	long v = -1;

	if (ms == NULL)
		return (-1);
	(void) umem_findleaks(ms, UMEM_FMT_JSON, 3);
	(void) fclose(ms);
	if (buf != NULL) {
		const char *p = strstr(buf, "\"cached_skipped\":");
		if (p != NULL)
			v = strtol(p + strlen("\"cached_skipped\":"), NULL, 10);
		free(buf);
	}
	return (v);
}

int
main(int argc, char **argv)
{
	int dur = (argc > 1) ? atoi(argv[1]) : 3;
	pthread_t *t;
	long skipped;
	int i;

	g_nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (g_nthreads < 2)
		g_nthreads = 2;
	t = calloc(g_nthreads, sizeof (*t));
	if (t == NULL)
		return (99);

	for (i = 0; i < g_nthreads; i++) {
		if (pthread_create(&t[i], NULL, worker, NULL) != 0)
			return (99);
	}
	sleep(dur);
	atomic_store(&g_stop, 1);
	/* Wait for every worker to reach the parked state (rseq still live). */
	while (atomic_load(&g_parked) != g_nthreads)
		usleep(1000);

	skipped = findleaks_cached_skipped();

	/* Release the parked workers and join. */
	atomic_store(&g_parked, -1);
	for (i = 0; i < g_nthreads; i++)
		(void) pthread_join(t[i], NULL);
	free(t);

	if (skipped < 0) {
		fprintf(stderr, "harness: findleaks produced no JSON "
		    "cached_skipped field\n");
		return (99);
	}

#ifdef UMEM_FINDLEAKS_NO_RSEQ_SUBTRACT
	printf("cached_skipped=%ld arm=norseqsub\n", skipped);
#else
	printf("cached_skipped=%ld arm=default\n", skipped);
#endif
	return (0);
}

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
 * Regression (P8.3): the malloc interposer's per-call cost relative to the
 * umem_alloc API.
 *
 * After P8.1 removed the global lock, LD_PRELOAD=libumem_malloc.so ran at
 * 0.74-0.91x of the umem_alloc/umem_free API in bench_main (docs/results/
 * 2026-09-24-allocator-comparison.md section 3).  That instrument's own
 * per-op cost hides most of the gap: in a bare loop (this test; bench_pairs)
 * the same build was at 0.41 (c7i.2xlarge) / 0.40 (c7g.2xlarge) at t=8,
 * 418 vs 147 instructions per pair.  The parts, measured one commit at a
 * time on c7i.2xlarge (docs/plans/2026-09-21-production-readiness.md P8.3):
 * is_bootstrap_pointer() out of line three times per free, umem_may_own()
 * out of line twice, __errno_location() through the PLT, and
 * process_free()'s general-purpose frame on the common path.
 *
 * After the fix: 0.61 (c7i.2xlarge) / 0.53 (c7g.2xlarge) at t=8 here;
 * 277 vs 147 instructions per pair.  The rest is what malloc/free must do
 * that umem_alloc/umem_free need not: write, read and validate a header,
 * and go through libumem_malloc.so.  The bar below is the post-fix floor
 * minus the null control's spread, not a target.
 *
 * HOW THIS DETECTS A REGRESSION
 *   Same process, same libumem.so, two loops: umem_alloc/umem_free (the API
 *   arm) and malloc/free (the preload arm, which is the interposer when this
 *   runs under LD_PRELOAD).  Alternating pairs at NTHREADS; the median
 *   per-pair ratio preload/API must be >= RATIO_MIN.
 *
 *   NULL CONTROL: the API loop against itself, same alternation, gives the
 *   rig's own spread on identical code.  If that spread alone would fail the
 *   bar (min null ratio < RATIO_MIN), this run cannot resolve the question
 *   and reports SKIP rather than a verdict either way.
 *
 * ONLY MEANINGFUL UNDER LD_PRELOAD=libumem_malloc.so; SKIP (77) otherwise.
 *
 * RATIO_MIN may be overridden with UMEM_INTERPOSE_RATIO_MIN.
 *
 * THE BAR: 0.48.  Post-fix medians 0.61 (x86) / 0.53 (arm), minimum over
 * 3 runs x 9 pairs 0.499 (arm); null API/API minimum 0.883 (arm), i.e. the
 * rig alone can move a pair by 12 %.  0.53 * 0.88 = 0.47.  The pre-fix
 * medians 0.409 / 0.395 are below it on both boxes, so the test
 * distinguishes the two states; it will not catch a regression smaller
 * than ~10 %, and it says so here rather than pretending otherwise.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "umem.h"

#define NTHREADS	8
#define NOBJ		64
#define ROUNDS		2000
#define PAIRS		9
#define RATIO_MIN	0.48

static int
interposed(void)
{
	extern int umem_malloc_is_interposing __attribute__((weak));
	return (&umem_malloc_is_interposing != NULL &&
	    umem_malloc_is_interposing != 0);
}

static double
now_s(void)
{
	struct timespec ts;
	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

/* arg: 1 = malloc/free, 0 = umem_alloc/umem_free */
static void *
worker(void *arg)
{
	int use_malloc = (int)(uintptr_t)arg & 1;
	unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 7u;
	void *p[NOBJ];
	size_t sz[NOBJ];
	int r, i;

	for (r = 0; r < ROUNDS; r++) {
		for (i = 0; i < NOBJ; i++) {
			sz[i] = 16 + (rand_r(&seed) % 48);	/* 16..63 */
			p[i] = use_malloc ? malloc(sz[i]) :
			    umem_alloc(sz[i], UMEM_DEFAULT);
			if (p[i] == NULL)
				exit(2);
			((char *)p[i])[0] = (char)i;
		}
		if (use_malloc) {
			for (i = 0; i < NOBJ; i++)
				free(p[i]);
		} else {
			for (i = 0; i < NOBJ; i++)
				umem_free(p[i], sz[i]);
		}
	}
	return (NULL);
}

/* Aggregate pairs/sec for NTHREADS threads. */
static double
run(int use_malloc)
{
	pthread_t th[NTHREADS];
	double t0, t1;
	int i;

	t0 = now_s();
	for (i = 0; i < NTHREADS; i++) {
		if (pthread_create(&th[i], NULL, worker,
		    (void *)(uintptr_t)(((i + 1) << 1) | use_malloc)) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			exit(2);
		}
	}
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);
	t1 = now_s();
	return ((double)NTHREADS * ROUNDS * NOBJ / (t1 - t0));
}

static int
cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x < y) ? -1 : (x > y);
}

/* Median of PAIRS alternating (a, b) pairs of b/a; *minp gets the minimum. */
static double
paired_ratio(int arm_a, int arm_b, double *minp)
{
	double r[PAIRS];
	int i;

	for (i = 0; i < PAIRS; i++) {
		double a = run(arm_a);
		double b = run(arm_b);
		r[i] = b / a;
	}
	qsort(r, PAIRS, sizeof (double), cmp_double);
	*minp = r[0];
	return (r[PAIRS / 2]);
}

int
main(void)
{
	double bar = RATIO_MIN, null_med, null_min, med, min;
	const char *env;

	if (!interposed()) {
		printf("SKIP: not running under LD_PRELOAD=libumem_malloc.so "
		    "(the interposer's per-call cost is the subject)\n");
		return (77);
	}
	if ((env = getenv("UMEM_INTERPOSE_RATIO_MIN")) != NULL)
		bar = atof(env);

	/* Warm both paths so the first timed pair is not paying for setup. */
	(void) run(0);
	(void) run(1);

	null_med = paired_ratio(0, 0, &null_min);
	med = paired_ratio(0, 1, &min);

	printf("t=%d %d pairs: null API/API median %.3f min %.3f; "
	    "preload/API median %.3f min %.3f; bar %.2f\n",
	    NTHREADS, PAIRS, null_med, null_min, med, min, bar);

	if (null_min < bar) {
		printf("SKIP: the null control alone falls below the bar "
		    "(%.3f < %.2f); this rig cannot resolve the question\n",
		    null_min, bar);
		return (77);
	}
	if (med < bar) {
		printf("RESULT: FAIL (preload/API %.3f < %.2f at t=%d; the "
		    "interposer's per-call cost is back)\n", med, bar, NTHREADS);
		return (1);
	}
	printf("RESULT: PASS (preload/API %.3f >= %.2f, null min %.3f)\n",
	    med, bar, null_min);
	return (0);
}

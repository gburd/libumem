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
 * THE BAR is architecture-specific (see the #if below): x86_64/aarch64 use
 * 0.48, sparcv9 uses 0.15.  Post-fix medians 0.61 (x86) / 0.53 (arm) / ~0.34
 * (sparcv9); the x86/arm bar is that arch's post-fix floor minus the
 * null-control spread.  The pre-fix medians 0.409 / 0.395 are below the
 * x86/arm bar, so the test distinguishes the two states there; it will not
 * catch a regression smaller than ~10 %, and it says so here rather than
 * pretending otherwise.  On SPARC the ratio is both lower (ISA) and more
 * load-sensitive under `make check`, so its bar (0.15) is a coarse gross-
 * regression backstop only; the deterministic SPARC regression guard is the
 * mechanism check repro_interpose_free_scaling (no global serialization).
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
/*
 * THE BAR is architecture-specific: it is derived per-arch as the post-fix
 * ratio floor minus the null-control spread, not a portable target.  The
 * instruction/cycle cost of the header-validate + PLT overhead the interposer
 * adds, relative to the bare umem_alloc/umem_free API, differs by ISA.
 *   x86_64 / aarch64 (Linux metal): post-fix 0.61 / 0.53, bar 0.48.
 *   sparcv9 (illumos): post-fix median ~0.34, min ~0.24 under steady load, on
 *     UltraSPARC (2026-10-18); null API/API min ~0.85.  SPARC's register-window
 *     calling convention and PLT make the interpose overhead relatively larger,
 *     so the ratio is lower WITHOUT any regression.  It is ALSO more
 *     load-sensitive on this slower hardware: inside `make check` (parallel
 *     tests contending for the same cores) a pair can dip to ~0.20.  Because of
 *     that fragility the sparcv9 bar is set WELL below the post-fix floor, at
 *     0.15 -- its job on SPARC is only to catch a GROSS per-call-cost
 *     regression (a returned global lock drops the t=8 ratio below ~0.10,
 *     half the post-fix value).  The deterministic regression guard on SPARC
 *     is the MECHANISM check, repro_interpose_free_scaling (2.0x t1->t8 = no
 *     global serialization); this ratio test is a coarse backstop there.
 * Override either with UMEM_INTERPOSE_RATIO_MIN.
 */
#if defined(__sparc) || defined(__sparcv9)
#define RATIO_MIN	0.15
#else
#define RATIO_MIN	0.48
#endif
#define QUIET_MIN	0.88	/* the null spread the bar was derived from */
/*
 * Quiet = identical code measures alike in BOTH directions.  The first
 * version tested only the minimum, and r7rel's --disable-rseq make -j check
 * passed it with null min 1.008 but median 1.486 (identical code 49 % apart)
 * and then FAILed the bar at 0.440.  A skew that large up is as disqualifying
 * as one down.
 */
#define QUIET(nmin, nmed)	((nmin) >= QUIET_MIN && (nmed) <= 1.0 / QUIET_MIN)
#define RETRIES		4

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

	/*
	 * QUIET-RIG GATE.  Under `make -j check` on an 8-vCPU box this test's 8
	 * threads share the CPUs with the rest of the suite, and the null
	 * control shows it: the two FAILs seen (0.476, round 7, and 0.419,
	 * r7jet, library code identical to the 0.55-0.63 re-runs) both had
	 * null min 0.63 / 0.81, against 0.95-0.99 on every clean re-run.  A
	 * rig that moves identical code by >10 % cannot judge a bar derived
	 * from a 0.88 null.  So require null min >= QUIET_MIN and retry up to
	 * RETRIES times; if the rig never goes quiet, SKIP (77), not a verdict.
	 */
	for (int attempt = 1; !QUIET(null_min, null_med) && attempt < RETRIES;
	    attempt++) {
		printf("  noisy rig (null min %.3f, median %.3f; need min >= "
		    "%.2f and median <= %.3f); retry %d/%d\n", null_min,
		    null_med, QUIET_MIN, 1.0 / QUIET_MIN, attempt, RETRIES - 1);
		(void) sleep(2);
		null_med = paired_ratio(0, 0, &null_min);
		med = paired_ratio(0, 1, &min);
		printf("t=%d %d pairs: null API/API median %.3f min %.3f; "
		    "preload/API median %.3f min %.3f; bar %.2f\n",
		    NTHREADS, PAIRS, null_med, null_min, med, min, bar);
	}
	if (!QUIET(null_min, null_med)) {
		printf("SKIP: the rig never went quiet (null min %.3f, median "
		    "%.3f after %d attempts); no verdict\n", null_min, null_med,
		    RETRIES);
		return (77);
	}
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

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
 * Regression: the malloc interposer's free() must scale with threads.
 *
 * DEFECT (pre-fix): every free() through libumem_malloc.so called
 * interpose_owner_of(), which called is_libc_pointer(), which took the
 * process-global libc_ptr_lock mutex and scanned all 512 slots of a table
 * that is empty for the entire steady-state life of nearly every process
 * (it is only ever populated by bootstrap-phase memalign()).  Then it ran
 * process_free(ptr, 0, ...) to classify and process_free(ptr, 1, ...) to free:
 * two full header decodes.
 *
 * Measured on c7i.metal-48xl (192 vCPU), multi 16:64, null-controlled:
 * 3.0 Mops/s at t=1 falling to 0.8 Mops/s at t=192 -- NEGATIVE scaling --
 * while the umem_alloc API path on the same build did 398 Mops/s at t=192.
 * Every earlier comparison measured the API path, so a ~500x gap on the
 * actual drop-in path went unseen for months.
 *
 * HOW THIS DETECTS IT
 *   Runs the same alloc/free loop at 1 thread and at N threads and compares
 *   AGGREGATE throughput.  A correct allocator's aggregate throughput does not
 *   COLLAPSE as threads are added; a global lock on free() makes it fall
 *   below the single-thread number.  We assert aggregate(N) >= aggregate(1),
 *   which is a weak bound that any non-pathological allocator clears by a
 *   wide margin, and that the pre-fix interposer fails by ~4x at 8 threads.
 *
 *   This is a scaling-SHAPE test, not a throughput number.  It is
 *   deliberately insensitive to the box's absolute speed, which is what
 *   lets it gate in make check on an 8-vCPU instance.
 *
 * ONLY MEANINGFUL UNDER LD_PRELOAD=libumem_malloc.so; reports SKIP (77)
 * otherwise, since without the interposer there is nothing of ours in front
 * of malloc to measure.
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

#define OPS_PER_THREAD	200000
#define NTHREADS	8

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

static void *
worker(void *arg)
{
	unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 7u;
	long i;

	for (i = 0; i < OPS_PER_THREAD; i++) {
		size_t n = 16 + (rand_r(&seed) % 48);	/* 16..63 */
		void *p = malloc(n);
		if (p != NULL) {
			((char *)p)[0] = (char)i;
			free(p);
		}
	}
	return (NULL);
}

/* Aggregate ops/sec for `nth` threads doing OPS_PER_THREAD each. */
static double
run(int nth)
{
	pthread_t th[NTHREADS];
	double t0, t1;
	int i;

	t0 = now_s();
	for (i = 0; i < nth; i++) {
		if (pthread_create(&th[i], NULL, worker,
		    (void *)(uintptr_t)(i + 1)) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			exit(2);
		}
	}
	for (i = 0; i < nth; i++)
		(void) pthread_join(th[i], NULL);
	t1 = now_s();

	return ((double)nth * OPS_PER_THREAD / (t1 - t0));
}

int
main(void)
{
	double one, many, ratio;
	int i;

	if (!interposed()) {
		printf("SKIP: not running under LD_PRELOAD=libumem_malloc.so "
		    "(the interposer's free() path is the subject)\n");
		return (77);
	}

	/* Warm the allocator and the PTC so the first timed run is not
	 * paying for cache creation. */
	(void) run(1);

	/* Median of 3 to shrug off one unlucky scheduling event. */
	{
		double a[3], b[3];
		for (i = 0; i < 3; i++) {
			a[i] = run(1);
			b[i] = run(NTHREADS);
		}
		for (i = 0; i < 2; i++) {
			int j;
			for (j = 0; j < 2 - i; j++) {
				double t;
				if (a[j] > a[j + 1]) { t = a[j]; a[j] = a[j+1]; a[j+1] = t; }
				if (b[j] > b[j + 1]) { t = b[j]; b[j] = b[j+1]; b[j+1] = t; }
			}
		}
		one = a[1];
		many = b[1];
	}

	ratio = many / one;
	printf("aggregate ops/s: t=1 %.2fM  t=%d %.2fM  ratio=%.2fx\n",
	    one / 1e6, NTHREADS, many / 1e6, ratio);

	/*
	 * The bar: adding threads must not REDUCE aggregate throughput.  The
	 * pre-fix interposer, serialized on libc_ptr_lock, delivered a ratio
	 * well under 1.0 (measured ~0.3-0.7 at 8 threads on 8 vCPU).  A fixed
	 * interposer, and glibc, deliver several x.  1.0 leaves a wide margin
	 * on both sides so this cannot flake on scheduling noise while still
	 * catching a reintroduced global lock.
	 */
	if (ratio < 1.0) {
		printf("RESULT: FAIL (aggregate throughput FELL with more "
		    "threads: the interposer's free() is serialized)\n");
		return (1);
	}
	printf("RESULT: PASS (free() scales; no global serialization on the "
	    "hot path)\n");
	return (0);
}

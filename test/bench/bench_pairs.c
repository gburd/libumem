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
 * bench_pairs: a bare alloc-N-then-free-N loop.  No histogram, no clock per
 * op, no memset.  Exists because bench_main at t=1 spends ~70 % of its cycles
 * in its own t-digest and the vDSO clock, and moved 7 % on a change that made
 * the allocator faster (docs/plans/2026-09-24-team-brief.md).  Use this, or
 * `perf stat` instructions per op, for any t=1 question.
 *
 *   bench_pairs [-s lo[:hi]] [-n N] [-t threads] [-d seconds]
 *
 * Each thread: allocate N objects (size drawn per object from [lo, hi) with
 * rand_r, as bench_main's `multi` does; fixed when hi is absent), free the N
 * in order, repeat until `seconds` elapse.  Prints aggregate Mpairs/s on one
 * line.  N=1 is the plain alloc/free loop; N above a PTC bin's capacity
 * exercises the per-thread magazines behind it (P8.6).
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "umem.h"

static size_t lo = 512, hi = 0;
static int nobj = 64, nthreads = 1;
static double seconds = 1.0;
static volatile int stop;

static double
now(void)
{
	struct timespec ts;
	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec * 1e-9);
}

static void *
worker(void *arg)
{
	uint64_t pairs = 0;
	unsigned seed = 0x9e3779b9u ^ ((unsigned)(uintptr_t)arg * 2654435761u);
	void **p = malloc(sizeof (void *) * (size_t)nobj);
	size_t *sz = malloc(sizeof (size_t) * (size_t)nobj);
	int i;

	if (p == NULL || sz == NULL)
		return ((void *)UINT64_MAX);
	while (!stop) {
		for (i = 0; i < nobj; i++) {
			sz[i] = hi > lo ?
			    lo + (size_t)rand_r(&seed) % (hi - lo) : lo;
			p[i] = umem_alloc(sz[i], UMEM_DEFAULT);
			if (p[i] == NULL)
				return ((void *)UINT64_MAX);
		}
		for (i = 0; i < nobj; i++)
			umem_free(p[i], sz[i]);
		pairs += (uint64_t)nobj;
	}
	free(p);
	free(sz);
	return ((void *)(uintptr_t)pairs);
}

int
main(int argc, char **argv)
{
	pthread_t *th;
	uint64_t total = 0;
	double t0, t1;
	int c, i;

	while ((c = getopt(argc, argv, "s:n:t:d:")) != -1) {
		switch (c) {
		case 's': {
			char *colon = strchr(optarg, ':');
			lo = strtoull(optarg, NULL, 10);
			hi = colon ? strtoull(colon + 1, NULL, 10) : 0;
			break;
		}
		case 'n': nobj = atoi(optarg); break;
		case 't': nthreads = atoi(optarg); break;
		case 'd': seconds = atof(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-s lo[:hi]] [-n N] "
			    "[-t threads] [-d seconds]\n", argv[0]);
			return (2);
		}
	}
	if (nobj < 1 || nthreads < 1 || lo == 0)
		return (2);

	th = malloc(sizeof (pthread_t) * (size_t)nthreads);
	t0 = now();
	for (i = 0; i < nthreads; i++)
		(void) pthread_create(&th[i], NULL, worker,
		    (void *)(uintptr_t)i);
	while (now() - t0 < seconds)
		(void) usleep(1000);
	stop = 1;
	for (i = 0; i < nthreads; i++) {
		void *r;
		(void) pthread_join(th[i], &r);
		if ((uintptr_t)r == UINT64_MAX) {
			printf("alloc failure\n");
			return (1);
		}
		total += (uint64_t)(uintptr_t)r;
	}
	t1 = now();
	printf("size=%zu%s%zu N=%d t=%d %.2f Mpairs/s\n", lo, hi ? ":" : "",
	    hi, nobj, nthreads, (double)total / (t1 - t0) / 1e6);
	return (0);
}

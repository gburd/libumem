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
 * bench_foreign_free: how much does a NON-owned free() cost?
 *
 * Under LD_PRELOAD, free() is handed pointers libumem never issued (another
 * allocator's, or a bug's).  process_free()/umem_malloc_free() classify them
 * with umem_may_own(): the [lo,hi) hull rejects a far-foreign pointer with no
 * span search, but a pointer INSIDE the hull yet in no span (P7.4) pays the
 * ~log2(N) binary search over the span table before it is refused.  This is
 * the microbench for exactly that path: repeatedly free() a pointer that is
 * (a) far outside the heap (hull miss, no search) and (b) inside the heap's
 * hull but owned by no span (hull hit, span search).  The A/B compares the
 * hull-only pre-fix against the span-table post-fix.
 *
 *   bench_foreign_free [-t threads] [-d seconds] [-m mode]
 *     mode far   : free() a static/stack pointer far below the heap (hull miss)
 *     mode inhull : free() a caller mmap the heap grew around (hull hit, search)
 *
 * Prints aggregate Mfrees/s.  Interposer arm: LD_PRELOAD=libumem_malloc.so.
 * umem_abort is 0 under the interposer, so a refused free just returns.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "umem_impl.h"

extern _Atomic uintptr_t vmem_heap_lo, vmem_heap_hi;

#ifndef MAP_FIXED_NOREPLACE
#define	MAP_FIXED_NOREPLACE	0x100000
#endif

static int nthreads = 1;
static double seconds = 1.0;
static volatile int stop;
static void *target;			/* the foreign pointer every thread frees */

typedef struct { uint32_t sz; uint32_t st; } fhdr_t;

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
	uint64_t n = 0;
	(void) arg;
	while (!stop) {
		int i;
		for (i = 0; i < 256; i++)
			free(target);		/* refused; measures the classify path */
		n += 256;
	}
	return ((void *)(uintptr_t)n);
}

/* Build an in-hull, no-span caller region (P7.4 shape): 16 MiB shaping allocs. */
static void *
inhull_region(void)
{
	size_t reg = 64 * 1024, big = 16 * 1024 * 1024, drop = 32 * 1024 * 1024;
	void *keep[512];
	int nk = 0, i;
	uintptr_t lo0, want;
	void *bar;

	for (i = 0; i < 4; i++) {
		keep[nk] = malloc(big);
		if (keep[nk] == NULL) break;
		memset(keep[nk], 1, 64); nk++;
	}
	lo0 = atomic_load(&vmem_heap_lo);
	want = (lo0 - drop) & ~(uintptr_t)(reg - 1);
	bar = mmap((void *)want, reg, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
	if (bar == MAP_FAILED || (uintptr_t)bar != want)
		return (NULL);
	for (i = 0; i < 512 && nk < 512; i++) {
		if (atomic_load(&vmem_heap_lo) < (uintptr_t)bar) break;
		keep[nk] = malloc(big);
		if (keep[nk] == NULL) break;
		memset(keep[nk], 1, 64); nk++;
	}
	if ((uintptr_t)bar < atomic_load(&vmem_heap_lo) ||
	    (uintptr_t)bar + reg > atomic_load(&vmem_heap_hi))
		return (NULL);
	/* forge a MALLOC_MAGIC header so the free path reaches the base gate */
	{
		fhdr_t *h = (fhdr_t *)(((uintptr_t)bar + 15) & ~(uintptr_t)15);
		uint32_t sf = (uint32_t)(32 + sizeof (fhdr_t));
		h->sz = sf;
		h->st = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, sf);
		return ((void *)(h + 1));
	}
}

int
main(int argc, char **argv)
{
	static char farbuf[512] __attribute__((aligned(16)));
	const char *mode = "far";
	pthread_t *th;
	uint64_t total = 0;
	double t0, t1;
	int c, i;

	while ((c = getopt(argc, argv, "t:d:m:")) != -1) {
		switch (c) {
		case 't': nthreads = atoi(optarg); break;
		case 'd': seconds = atof(optarg); break;
		case 'm': mode = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-t threads] [-d sec] "
			    "[-m far|inhull]\n", argv[0]);
			return (2);
		}
	}

	if (strcmp(mode, "inhull") == 0) {
		target = inhull_region();
		if (target == NULL) {
			printf("mode=inhull t=%d could not build region; SKIP\n",
			    nthreads);
			return (0);
		}
	} else {
		/* far foreign: a forged header in static storage, far below heap */
		fhdr_t *h = (fhdr_t *)(((uintptr_t)farbuf + 15) & ~(uintptr_t)15);
		uint32_t sf = (uint32_t)(32 + sizeof (fhdr_t));
		h->sz = sf;
		h->st = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, sf);
		target = (void *)(h + 1);
	}

	th = malloc(sizeof (pthread_t) * (size_t)nthreads);
	t0 = now();
	for (i = 0; i < nthreads; i++)
		(void) pthread_create(&th[i], NULL, worker, NULL);
	while (now() - t0 < seconds)
		(void) usleep(1000);
	stop = 1;
	for (i = 0; i < nthreads; i++) {
		void *r;
		(void) pthread_join(th[i], &r);
		total += (uint64_t)(uintptr_t)r;
	}
	t1 = now();
	printf("mode=%s t=%d %.2f Mfrees/s\n", mode, nthreads,
	    (double)total / (t1 - t0) / 1e6);
	return (0);
}

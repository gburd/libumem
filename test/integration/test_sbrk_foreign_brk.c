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
 * The sbrk heap must not hand out memory that another sbrk() caller owns.
 *
 * THE DEFECT.  vmem_sbrk.c grew the heap with its own copy of illumos libc's
 * _sbrk_grow_aligned(): sbrk(0), round up, brk(), under a lock private to
 * this file.  sbrk() and brk() in libc take libc's __sbrk_lock instead, so an
 * sbrk() in another thread -- libc's own malloc grows the break this way --
 * could run between the copy's sbrk(0) and its brk().  Both callers then
 * believed they owned the same pages.  Measured on OpenIndiana (36 vCPU):
 * 20/20 runs of this test with hundreds of umem buffers inside pages sbrk()
 * had handed the other thread; a variant whose thread also gave its page
 * back with sbrk(-4096) dropped the break below live heap memory and
 * SIGSEGVed 20/20; test_findleaks_rseq, whose workers mix libc malloc with
 * umem_alloc, SIGSEGVed 2/20 under UMEM_OPTIONS=backend=sbrk.
 *
 * THE FIX.  On illumos, call libc's _sbrk_grow_aligned() (SUNWprivate_1.1),
 * which holds __sbrk_lock, as illumos libumem does.
 *
 * WHAT THIS TESTS.  UMEM_OPTIONS=backend=sbrk.  Eight threads each keep
 * NALLOC fresh 8 KiB umem buffers (one heap growth per slab) while one
 * thread takes the break one page at a time with sbrk(4096).  Afterwards no
 * umem buffer may overlap a page sbrk() returned.
 *
 *   PASS: no overlap (and the mover got pages, so the two did interleave).
 *   FAIL: some umem buffer lies in a page sbrk() also handed out.
 *   SKIP: not illumos.  glibc exports no locked grow primitive, so on other
 *         platforms the sbrk backend still assumes nothing else in the
 *         process moves the break; there this layout SIGSEGVs inside vmem
 *         (Linux, 20/20), which is that documented limit, not this defect.
 */

#include "config.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "umem.h"

#define	NGROW	8
#define	NALLOC	4000
#define	OBJ	8192
#define	NPAGE	20000
#define	PAGE	4096

static void *bufs[NGROW][NALLOC];
static char *pages[NPAGE];
static int npages;
static volatile int stop;

static void *
grower(void *arg)
{
	void **b = arg;
	int i;

	for (i = 0; i < NALLOC; i++)
		b[i] = umem_alloc(OBJ, UMEM_DEFAULT);
	return (NULL);
}

/* Take the break a page at a time until the growers finish. */
static void *
mover(void *arg)
{
	(void) arg;
	while (!stop && npages < NPAGE) {
		char *p = sbrk(PAGE);

		if (p == (char *)-1)
			break;
		pages[npages++] = p;
	}
	return (NULL);
}

static int
cmp_ptr(const void *x, const void *y)
{
	uintptr_t a = (uintptr_t)*(char *const *)x;
	uintptr_t b = (uintptr_t)*(char *const *)y;

	return ((a > b) - (a < b));
}

/* Does [b, b + OBJ) overlap any page in the sorted pages[]? */
static int
overlaps_a_page(uintptr_t b)
{
	int lo = 0, hi = npages;

	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;

		if ((uintptr_t)pages[mid] + PAGE <= b)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo < npages && (uintptr_t)pages[lo] < b + OBJ);
}

int
main(void)
{
#if !defined(__sun)
	printf("SKIP: the sbrk backend shares the break safely only on "
	    "illumos\n");
	return (77);
#else
	pthread_t g[NGROW], m;
	long overlap = 0, served = 0;
	int t, i;
	void *warm;

	(void) setenv("UMEM_OPTIONS", "backend=sbrk", 1);
	warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm == NULL) {
		printf("SKIP: umem did not initialize\n");
		return (77);
	}
	umem_free(warm, 64);

	(void) pthread_create(&m, NULL, mover, NULL);
	for (t = 0; t < NGROW; t++)
		(void) pthread_create(&g[t], NULL, grower, bufs[t]);
	for (t = 0; t < NGROW; t++)
		(void) pthread_join(g[t], NULL);
	stop = 1;
	(void) pthread_join(m, NULL);

	qsort(pages, npages, sizeof (pages[0]), cmp_ptr);
	for (t = 0; t < NGROW; t++) {
		for (i = 0; i < NALLOC; i++) {
			if (bufs[t][i] == NULL)
				continue;
			served++;
			if (overlaps_a_page((uintptr_t)bufs[t][i]))
				overlap++;
		}
	}
	printf("umem buffers %ld, pages sbrk() gave the other thread %d, "
	    "overlapping %ld\n", served, npages, overlap);
	if (served == 0 || npages == 0) {
		printf("SKIP: nothing interleaved (served %ld, pages %d)\n",
		    served, npages);
		return (77);
	}
	if (overlap != 0) {
		printf("RESULT: FAIL (%ld umem buffers lie in pages sbrk() also "
		    "handed out: the heap grew without libc's break lock)\n",
		    overlap);
		return (1);
	}
	printf("RESULT: PASS\n");
	return (0);
#endif
}

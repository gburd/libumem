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
 * P8.2 regression: threads must spread across the per-CPU caches.
 *
 * THE DEFECT.  The per-thread CPU hint that selects cache_cpu[] was
 * pthread_self() cast to int: a page-aligned stack address whose low bits are
 * always zero.  `hint & cache_cpu_mask` was therefore 0 for every thread, and
 * the value was cached in TLS once, forever.  So every operation that reached
 * the magazine layer -- every size above umem_ptc_maxsize, every PTC miss
 * below it -- serialised on ONE cc_lock for the whole process.  Measured:
 * 7 of 8 threads on cache_cpu[0]; 1.5 Mops/s at t=8 for 2560-byte objects
 * where 1536-byte (PTC-served) objects did 32; 0.06x glibc at 64 threads.
 *
 * WHAT THIS TESTS, exactly.  N threads each allocate and free a size that
 * bypasses the PTC (2560 B is the first class above umem_ptc_maxsize = 2048).
 * Afterwards, count the per-CPU caches whose cc_alloc is non-zero.
 *
 *   PASS: at least min(N, ncpus) / 2 distinct caches were used.
 *   FAIL: fewer.  Pre-fix: 1 or 2, regardless of N.
 *
 * The threshold is half rather than all because the rseq cpu_id tracks where
 * a thread actually runs, and the scheduler may pack N threads onto fewer
 * than N CPUs for a 200k-op burst.  Half is far above the pre-fix value of
 * one and far below any legitimate spread.  With ncpus == 1 there is nothing
 * to spread over; SKIP.
 *
 * It reads cache internals (umem_base.h) because the property IS internal:
 * the user-visible symptom is throughput, which varies with the box; the
 * cache-slot distribution does not.
 */

#include "umem_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define	OBJ	2560
#define	NTHREADS 8
#define	NOPS	200000

static void *
worker(void *arg)
{
	int i;

	(void) arg;
	for (i = 0; i < NOPS; i++) {
		void *p = umem_alloc(OBJ, UMEM_DEFAULT);
		if (p == NULL)
			return ((void *)1);
		umem_free(p, OBJ);
	}
	return (NULL);
}

int
main(void)
{
	pthread_t th[NTHREADS];
	umem_cache_t *cp;
	unsigned c, used = 0, ncaches;
	long ncpus;
	int i, need;
	void *warm;

	warm = umem_alloc(OBJ, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, OBJ);

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 2) {
		printf("SKIP: %ld CPU; nothing to spread across\n", ncpus);
		return (77);
	}

	for (i = 0; i < NTHREADS; i++)
		(void) pthread_create(&th[i], NULL, worker, NULL);
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);

	cp = umem_alloc_table[(OBJ - 1) >> UMEM_ALIGN_SHIFT];
	ncaches = cp->cache_cpu_mask + 1;
	printf("cache=%s  per-cpu caches=%u  threads=%d  cc_alloc:",
	    cp->cache_name, ncaches, NTHREADS);
	for (c = 0; c < ncaches; c++) {
		umem_cpu_cache_t *cc = (umem_cpu_cache_t *)
		    ((char *)cp + umem_cpus[c].cpu_cache_offset);
		printf(" %lu", (unsigned long)cc->cc_alloc);
		if (cc->cc_alloc != 0)
			used++;
	}
	printf("\n");

	need = (int)((NTHREADS < ncpus ? NTHREADS : ncpus) / 2);
	if (need < 2)
		need = 2;
	printf("distinct per-cpu caches used: %u (need >= %d)\n", used, need);

	if ((int)used < need) {
		printf("RESULT: FAIL (%d threads collapsed onto %u per-CPU "
		    "cache(s): the CPU hint does not spread threads -- every "
		    "magazine-layer operation serialises on one cc_lock)\n",
		    NTHREADS, used);
		return (1);
	}
	printf("RESULT: PASS (%d threads spread over %u per-CPU caches)\n",
	    NTHREADS, used);
	return (0);
}

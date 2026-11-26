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
 * bypasses the PTC.  This was 2560 B, the first class above the then
 * umem_ptc_maxsize of 2048; the PTC now covers through 8192 (P8.2b), so it
 * is 10240 B, the first class above that.  The property is the same: a size
 * whose every operation reaches the per-CPU magazine layer.
 *
 * Each worker first pins itself to one CPU of the process's allowed set
 * (worker i -> allowed[i % n]), THEN makes its first allocation, so the hint
 * it caches is read on that CPU and every later re-read (magazine reload)
 * sees the same CPU.  Afterwards, for each worker, read cc_alloc of the
 * per-CPU cache its CPU maps to (cpu & cache_cpu_mask).
 *
 *   PASS: every pinned worker's cache has cc_alloc != 0 -- the cache
 *         follows the CPU the thread runs on.  With rseq on, the per-CPU
 *         rseq slot serves most operations and cc_alloc counts only its
 *         refills from the magazine layer, so a worker's count can be as low
 *         as 2 (r8hint, 80 runs); it is never 0 because the slot starts
 *         empty and its first refill goes through the hint.
 *   FAIL: some worker's cache is empty.  Pre-fix, pinned: 1-5 caches used,
 *         3-7 of the 8 empty, 80/80 FAIL idle and loaded, both arches.  The
 *         old half bar (>= 4) would have passed the pre-fix runs that used
 *         4 or 5 (4 of 80); an exact bar does not.
 *   SKIP: fewer than 2 allowed CPUs, fewer than 2 distinct caches to map
 *         to, pinning refused (restricted cpuset), or no affinity API.
 *
 * ILLUMOS.  The same test, pinning with processor_bind() and reading the CPU
 * with getcpuid(); the allowed set is every CPU p_online() reports online.
 * Before the illumos hint used getcpuid(), it fell back to thr_self() >> 12,
 * and illumos thread ids are small integers, so the hint was 0 for every
 * thread: all 8 workers on cache_cpu[0], 7 caches empty, every run.
 *
 * WHY PIN.  Unpinned, the rseq cpu_id follows where the scheduler actually
 * runs a thread, and an oversubscribed 8-vCPU box (make -j8, +64 busy
 * loops) packed 8 threads onto 3 CPUs: the old "half of min(N, ncpus)" bar
 * then failed 3/20 on correct code (docs/results/v4gate/, r8hint/).  That
 * measured the scheduler.  Pinning makes the CPU each thread runs on a
 * given, so the only thing left to vary is the hint -- the property under
 * test -- and the bar can be exact instead of half.
 *
 * It reads cache internals (umem_base.h) because the property IS internal:
 * the user-visible symptom is throughput, which varies with the box; the
 * cache-slot distribution does not.
 */

#include "umem_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__sun)
#include <sys/types.h>
#include <sys/processor.h>
#include <sys/procset.h>
#include <errno.h>
#endif

#if defined(__linux__) || defined(__sun)
#define	HAVE_PIN	1
#endif

#define	OBJ	10240
#define	NTHREADS 8
#define	NOPS	200000

struct wk {
	int	cpu;	/* CPU this worker pins itself to */
	int	ran_on;	/* sched_getcpu() after pinning, for the log */
	int	err;	/* pin_self() result */
};

#if defined(__linux__)
#define	MAXCPU	CPU_SETSIZE

/* Pin the calling thread to one CPU; 0 or an errno. */
static int
pin_self(int cpu)
{
	cpu_set_t one;

	CPU_ZERO(&one);
	CPU_SET(cpu, &one);
	return (pthread_setaffinity_np(pthread_self(), sizeof (one), &one));
}

static int
current_cpu(void)
{
	return (sched_getcpu());
}

/* Fill allowed[] with the CPUs this process may run on; the count, or -1. */
static int
allowed_cpus(int *allowed)
{
	cpu_set_t set;
	int i, n = 0;

	if (sched_getaffinity(0, sizeof (set), &set) != 0)
		return (-1);
	for (i = 0; i < CPU_SETSIZE; i++)
		if (CPU_ISSET(i, &set))
			allowed[n++] = i;
	return (n);
}
#elif defined(__sun)
#define	MAXCPU	1024

static int
pin_self(int cpu)
{
	if (processor_bind(P_LWPID, P_MYID, cpu, NULL) != 0)
		return (errno);
	return (0);
}

static int
current_cpu(void)
{
	return ((int)getcpuid());
}

static int
allowed_cpus(int *allowed)
{
	long max = sysconf(_SC_CPUID_MAX);
	int i, n = 0;

	if (max < 0)
		return (-1);
	for (i = 0; i <= max && i < MAXCPU; i++)
		if (p_online(i, P_STATUS) == P_ONLINE)
			allowed[n++] = i;
	return (n);
}
#endif

static void *
worker(void *arg)
{
	struct wk *w = arg;
	int i;
#ifdef HAVE_PIN
	/* Pin BEFORE the first allocation: that is when the hint is cached. */
	w->err = pin_self(w->cpu);
	if (w->err != 0)
		return (NULL);
	w->ran_on = current_cpu();
#endif
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
#ifndef HAVE_PIN
	printf("SKIP: no thread-affinity API on this platform\n");
	return (77);
#else
	pthread_t th[NTHREADS];
	struct wk w[NTHREADS];
	int allowed[MAXCPU];
	umem_cache_t *cp;
	unsigned c, ncaches, used = 0, empty = 0, expect = 0;
	unsigned char want[MAXCPU] = { 0 };
	int i, n;
	void *warm;

	warm = umem_alloc(OBJ, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, OBJ);
	cp = umem_alloc_table[(OBJ - 1) >> UMEM_ALIGN_SHIFT];
	ncaches = cp->cache_cpu_mask + 1;

	n = allowed_cpus(allowed);
	if (n < 0) {
		perror("SKIP: cannot read the allowed CPU set");
		return (77);
	}
	for (i = 0; i < NTHREADS; i++) {
		w[i].cpu = allowed[i % (n > 0 ? n : 1)];
		w[i].ran_on = -1;
		w[i].err = 0;
		if (!want[w[i].cpu & cp->cache_cpu_mask]++)
			expect++;
	}
	if (n < 2 || expect < 2) {
		printf("SKIP: %d allowed CPU(s) mapping to %u per-CPU cache(s); "
		    "nothing to spread across\n", n, expect);
		return (77);
	}

	for (i = 0; i < NTHREADS; i++)
		(void) pthread_create(&th[i], NULL, worker, &w[i]);
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);

	for (i = 0; i < NTHREADS; i++) {
		if (w[i].err != 0) {
			printf("SKIP: cannot pin worker %d to CPU %d (error %d; "
			    "restricted cpuset?)\n", i, w[i].cpu, w[i].err);
			return (77);
		}
	}

	printf("cache=%s  per-cpu caches=%u  threads=%d  allowed cpus=%d\n",
	    cp->cache_name, ncaches, NTHREADS, n);
	printf("pinned (cpu/ran_on):");
	for (i = 0; i < NTHREADS; i++)
		printf(" %d/%d", w[i].cpu, w[i].ran_on);
	printf("\ncc_alloc:");
	for (c = 0; c < ncaches; c++) {
		umem_cpu_cache_t *cc = (umem_cpu_cache_t *)
		    ((char *)cp + umem_cpus[c].cpu_cache_offset);
		printf(" %lu", (unsigned long)cc->cc_alloc);
		if (cc->cc_alloc != 0)
			used++;
		else if (want[c])
			empty++;
	}
	printf("\ndistinct per-cpu caches used: %u; pinned workers map to %u, "
	    "%u of those empty\n", used, expect, empty);

	if (empty != 0) {
		printf("RESULT: FAIL (%u of the %u caches the pinned workers run "
		    "on were never used: the CPU hint does not follow the CPU -- "
		    "magazine-layer operations collapse onto %u cc_lock(s))\n",
		    empty, expect, used);
		return (1);
	}
	printf("RESULT: PASS (every pinned worker used its own CPU's cache; "
	    "%u caches)\n", used);
	return (0);
#endif
}

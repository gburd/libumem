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
 * Regression: threads that make a process's FIRST allocations concurrently
 * must all be served by umem, not by the bootstrap allocator.
 *
 * THE DEFECT (found on OpenIndiana by the solnix alloc1 work, 2026-11-25)
 *   In BOOTSTRAP state the interposer's malloc() ran umem_init() only for
 *   the first thread to arrive, behind a once-only flag.  Any other thread
 *   that allocated while that init was still running skipped umem_init()
 *   and took bootstrap_malloc() -- one mmap and one slot of the 256-entry
 *   bootstrap registry per call -- until init finished.  Frees return
 *   slots, but a live set larger than the registry fills it and
 *   bootstrap_malloc() returns NULL with ENOMEM.  memalign() and the
 *   aligned entry points never ran umem_init() at all before READY; they
 *   used libc memory tracked in a 512-entry table, which fills the same way.
 *   umem_init() already makes a second caller wait on umem_init_cv; the
 *   interposer just never let it call.
 *
 *   It shows on illumos because nothing allocates before main() in a small
 *   C program there, so the first allocations can easily happen in worker
 *   threads.  On glibc Linux the C runtime mallocs on the main thread
 *   before main(), so umem is usually READY before any thread exists and
 *   this test passes with or without the fix.
 *
 * WHAT THIS DOES
 *   main() creates N workers WITHOUT allocating first (no stdio before the
 *   threads either: printf may allocate a buffer).  Each worker waits on a
 *   start barrier, then keeps a live set of LIVE objects larger than both
 *   bootstrap tables and churns it ITERS times.  Any NULL is a failure.
 *   --align uses memalign() for every allocation instead of malloc().
 *
 * ONLY MEANINGFUL UNDER LD_PRELOAD=libumem_malloc.so; without it this is a
 * control run against the platform allocator, which must also pass.
 *
 * Exit: 0 pass, 1 an allocation returned NULL, 99 harness.
 */

#include "config.h"

#include <errno.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>		/* memalign() on glibc */
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	MAX_THREADS	64
#define	LIVE		1024	/* > 256 bootstrap slots, > 512 libc slots */
#define	ITERS		20000

/* Not every libc declares it (it is in <stdlib.h> on illumos). */
extern void *memalign(size_t, size_t);

static pthread_barrier_t start;
static int use_align;
static volatile int failed;

/* Per-thread failure record, written once, printed after the join. */
static struct {
	long	iter;
	size_t	size;
	int	err;
} fail_at[MAX_THREADS];

static void *
worker(void *arg)
{
	long idx = (long)(intptr_t)arg;
	unsigned int seed = (unsigned int)idx * 2654435761u + 1;
	void *live[LIVE];
	long i;
	int k;

	(void) memset(live, 0, sizeof (live));
	(void) pthread_barrier_wait(&start);

	for (i = 0; i < ITERS; i++) {
		size_t sz = 4 + (size_t)(rand_r(&seed) % 32768);

		k = rand_r(&seed) % LIVE;
		free(live[k]);
		errno = 0;
		live[k] = use_align ? memalign(64, sz) : malloc(sz);
		if (live[k] == NULL) {
			fail_at[idx].iter = i;
			fail_at[idx].size = sz;
			fail_at[idx].err = errno;
			failed = 1;
			break;
		}
	}
	for (k = 0; k < LIVE; k++)
		free(live[k]);
	return (NULL);
}

int
main(int argc, char **argv)
{
	pthread_t tid[MAX_THREADS];
	long n = 4, t;
	int i, bad = 0;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--threads=", 10) == 0)
			n = strtol(argv[i] + 10, NULL, 10);
		else if (strcmp(argv[i], "--align") == 0)
			use_align = 1;
		else
			return (99);
	}
	if (n < 1 || n > MAX_THREADS)
		return (99);

	/* Nothing above this line allocates; keep it that way. */
	if (pthread_barrier_init(&start, NULL, (unsigned int)n) != 0)
		return (99);
	for (t = 0; t < n; t++) {
		fail_at[t].iter = -1;
		if (pthread_create(&tid[t], NULL, worker,
		    (void *)(intptr_t)t) != 0)
			return (99);
	}
	for (t = 0; t < n; t++)
		(void) pthread_join(tid[t], NULL);

	for (t = 0; t < n; t++) {
		if (fail_at[t].iter < 0)
			continue;
		bad++;
		(void) printf("  thread %ld: %s(%zu) returned NULL at iteration "
		    "%ld, errno %d\n", t, use_align ? "memalign" : "malloc",
		    fail_at[t].size, fail_at[t].iter, fail_at[t].err);
	}
	(void) printf("first-alloc race: threads=%ld live=%d iters=%d %s: %s\n",
	    n, LIVE, ITERS, use_align ? "memalign" : "malloc",
	    bad ? "FAIL" : "PASS");
	return (bad ? 1 : 0);
}

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
 * free(3) must not modify errno (POSIX.1-2008 TC2 makes this explicit).
 * process_free() saves and restores it around _umem_free(), which can set
 * errno through a vmem/mmap backend on a magazine or slab release.
 *
 * P8.3 changed how the save/restore is done (a cached per-thread pointer
 * to the errno slot; a straight-line fast path in umem_malloc_free()), so
 * this pins the contract for every layout umem_malloc() produces and for
 * both the interposer's free() and umem_malloc_free() directly:
 *
 *   - MALLOC_MAGIC        (request <= 8 bytes; 8-byte tag)
 *   - MALLOC_SECOND_MAGIC (9 .. UMEM_MAXBUF; 16-byte tag, LP64)
 *   - MALLOC_OVERSIZE     (> 4 GiB size field; not allocated here)
 *   - MEMALIGN            (memalign(3) result)
 *
 * errno is set to a sentinel (EDOM) before each free and must still be
 * EDOM after.  Also checks a second thread, since the cached pointer is
 * per-thread and must not be another thread's slot.
 *
 * Exit: 0 pass, 1 fail.
 */

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *umem_malloc(size_t);
extern void umem_malloc_free(void *);

static int failures;

static void
check(const char *what, void (*rel)(void *), void *p)
{
	errno = EDOM;
	rel(p);
	if (errno != EDOM) {
		printf("  FAIL: %s: errno %d after free (expected EDOM=%d "
		    "untouched)\n", what, errno, EDOM);
		failures++;
	} else {
		printf("  PASS: %s\n", what);
	}
}

static void
run_set(const char *tag)
{
	void *p;
	int i;

	/* warm the thread's caches so the frees below hit every path once */
	for (i = 0; i < 4; i++) {
		p = malloc(8); free(p);
		p = malloc(48); free(p);
		p = malloc(4096); free(p);
	}

	p = malloc(8);
	check(tag, free, p);
	p = malloc(48);
	check(tag, free, p);
	p = malloc(4096);
	check(tag, free, p);
	p = malloc(64 * 1024);		/* above UMEM_MAXBUF: oversize arena */
	check(tag, free, p);
	p = memalign(64, 100);
	check(tag, free, p);

	p = umem_malloc(8);
	check(tag, umem_malloc_free, p);
	p = umem_malloc(48);
	check(tag, umem_malloc_free, p);
	p = umem_malloc(64 * 1024);
	check(tag, umem_malloc_free, p);
}

static void *
thread_main(void *arg)
{
	(void) arg;
	run_set("second thread");
	return (NULL);
}

int
main(void)
{
	pthread_t th;

	printf("free() must leave errno unchanged, every layout, both entry "
	    "points\n");
	run_set("main thread");
	if (pthread_create(&th, NULL, thread_main, NULL) != 0) {
		printf("  FAIL: pthread_create\n");
		return (1);
	}
	(void) pthread_join(th, NULL);

	printf("test_free_errno: %s\n", failures ? "FAIL" : "PASS");
	return (failures ? 1 : 0);
}

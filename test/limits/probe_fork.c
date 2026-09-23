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
 * Phase 6 probe: FORK WITH A LARGE HEAP.
 *
 * Build a heap of HEAP_GB (touched) from a mix of small and oversize objects,
 * then fork() K times.  Report parent fork() latency (the atfork prepare +
 * parent handlers: umem_lockup() walks every cache, taking ncpus+2*depot+3
 * mutexes per cache, and vmem_lockup()), and the child's RSS after doing a
 * little allocation work (copy-on-write growth: how many of the parent's pages
 * the child's first allocations dirty).
 *
 * Usage: probe_fork [heap_gb] [forks]
 */
#include "limits.h"
#include <sys/wait.h>
#include <pthread.h>

static void *
churn(void *arg)
{
	/* background allocation load so the fork handlers meet contention */
	volatile int *stop = arg;
	while (!*stop) {
		void *p = ALLOC(128);
		if (p) { *(volatile char *)p = 1; FREE(p, 128); }
	}
	return (NULL);
}

int
main(int argc, char **argv)
{
	double gb = argc > 1 ? atof(argv[1]) : 4.0;
	int forks = argc > 2 ? atoi(argv[2]) : 5;
	uint64_t target = (uint64_t)(gb * (1ULL << 30));
	/* half in 4 KiB slab objects, half in 1 MiB oversize objects */
	uint64_t nsmall = (target / 2) / 4096, nbig = (target / 2) / (1 << 20);
	void **small = malloc(nsmall * sizeof (void *));
	void **big = malloc(nbig * sizeof (void *));
	uint64_t i;
	int k;
	volatile int stop = 0;
	pthread_t bg[2];

	for (i = 0; i < nsmall; i++) {
		small[i] = ALLOC(4096);
		if (!small[i]) { printf("small alloc fail at %llu\n", (unsigned long long)i); return (1); }
		memset(small[i], 1, 4096);
	}
	for (i = 0; i < nbig; i++) {
		big[i] = ALLOC(1 << 20);
		if (!big[i]) { printf("big alloc fail at %llu\n", (unsigned long long)i); return (1); }
		memset(big[i], 1, 1 << 20);
	}
	printf("%s fork heap=%.1fGB (%llu x 4K + %llu x 1M) rss=%.2fGB vmas=%lu\n",
	    LIM_NAME, gb, (unsigned long long)nsmall, (unsigned long long)nbig,
	    rss_bytes() / (double)(1 << 30), count_vmas());
	fflush(stdout);

	for (k = 0; k < forks; k++) {
		int with_load = (k >= forks / 2);
		if (with_load && k == forks / 2) {
			pthread_create(&bg[0], NULL, churn, (void *)&stop);
			pthread_create(&bg[1], NULL, churn, (void *)&stop);
			usleep(100000);
		}
		uint64_t a = now_ns();
		pid_t pid = fork();
		uint64_t d = now_ns() - a;
		if (pid == 0) {
			uint64_t r0 = rss_bytes();
			uint64_t t0 = now_ns();
			/* child: modest allocation work, then report */
			for (i = 0; i < 100000; i++) {
				void *p = ALLOC(64 + (i % 64) * 16);
				if (p) { *(volatile char *)p = 1; FREE(p, 64 + (i % 64) * 16); }
			}
			void *q = ALLOC(1 << 20);
			if (q) { memset(q, 2, 1 << 20); FREE(q, 1 << 20); }
			printf("  child: rss_at_entry=%.1fMB rss_after_work=%.1fMB work=%.1fms\n",
			    MB(r0), MB(rss_bytes()), (now_ns() - t0) / 1e6);
			fflush(stdout);
			_exit(0);
		}
		int st;
		waitpid(pid, &st, 0);
		printf("fork #%d%s: parent fork() latency %.2fms, child rc=%d\n", k,
		    with_load ? " (2 churn threads)" : "", d / 1e6, WEXITSTATUS(st));
		fflush(stdout);
	}
	stop = 1;
	pthread_join(bg[0], NULL); pthread_join(bg[1], NULL);
	return (0);
}

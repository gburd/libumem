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
 * Phase 6 probe: OBJECT SIZE / oversize vmem arena.
 *
 * Sizes > UMEM_MAXBUF (128 KiB) bypass the slab layer and go to
 * umem_oversize_arena (umem.c _umem_alloc), which imports spans from the
 * mmap heap.  Each live oversize object costs vmem segment structures
 * (vmem_seg_t, supplied by vmem_populate() from vmem_seg_arena) and, since
 * the heap is one PROT_NONE reservation re-protected per allocation, a
 * kernel VMA.
 *
 * Cycle: allocate N objects of SZ, free every other one, allocate N/2 more
 * (into the holes if the arena coalesces; new spans if not), repeat R
 * rounds.  Report VMAs, RSS vs live, and the first failure with errno.
 *
 * Usage: probe_objsize <size_bytes> <count> [rounds]
 *   e.g. probe_objsize 1048576 4000 5      (4000 x 1 MB)
 *        probe_objsize 67108864 200 5      (200 x 64 MB, 12.5 GB touched)
 *        probe_objsize 1073741824 12 3     (12 x 1 GB)
 * Touch: one byte per page-cluster (64 KiB) so RSS reflects the objects
 * without needing the whole size in physical memory.
 */
#include "limits.h"

static void
touch(void *p, size_t sz)
{
	char *c = p;
	size_t o;

	for (o = 0; o < sz; o += 65536)
		c[o] = 1;
	c[sz - 1] = 1;
}

int
main(int argc, char **argv)
{
	size_t sz = argc > 1 ? strtoull(argv[1], NULL, 10) : 1048576;
	size_t n = argc > 2 ? strtoul(argv[2], NULL, 10) : 4000;
	int rounds = argc > 3 ? atoi(argv[3]) : 5;
	void **ptrs = calloc(n, sizeof (void *));
	size_t i, live = 0;
	int r;
	uint64_t worst = 0;

	printf("%s objsize size=%zu count=%zu rounds=%d vmas0=%lu rss0=%.1fMB\n",
	    LIM_NAME, sz, n, rounds, count_vmas(), MB(rss_bytes()));
	printf("%5s %6s %8s %10s %10s %10s %12s\n", "round", "phase",
	    "live", "liveGB", "rssGB", "vmas", "maxalloc_us");

	for (r = 0; r < rounds; r++) {
		/* fill every empty slot */
		for (i = 0; i < n; i++) {
			if (ptrs[i] != NULL)
				continue;
			uint64_t a = now_ns();
			ptrs[i] = ALLOC(sz);
			uint64_t d = now_ns() - a;
			if (d > worst) worst = d;
			if (ptrs[i] == NULL) {
				printf("FAIL round %d slot %zu live=%zu (%.2fGB) "
				    "errno=%d (%s) vmas=%lu rss=%.2fGB\n", r, i,
				    live, live * (double)sz / (1 << 30), errno,
				    strerror(errno), count_vmas(),
				    rss_bytes() / (double)(1 << 30));
				return (1);
			}
			touch(ptrs[i], sz);
			live++;
		}
		printf("%5d %6s %8zu %10.2f %10.2f %10lu %12.1f\n", r, "full",
		    live, live * (double)sz / (1 << 30),
		    rss_bytes() / (double)(1 << 30), count_vmas(),
		    worst / 1000.0);
		fflush(stdout);
		/* free every other one */
		for (i = 0; i < n; i += 2) {
			if (ptrs[i] == NULL)
				continue;
			FREE(ptrs[i], sz);
			ptrs[i] = NULL;
			live--;
		}
		printf("%5d %6s %8zu %10.2f %10.2f %10lu %12s\n", r, "half",
		    live, live * (double)sz / (1 << 30),
		    rss_bytes() / (double)(1 << 30), count_vmas(), "-");
		fflush(stdout);
	}
	for (i = 0; i < n; i++)
		if (ptrs[i]) { FREE(ptrs[i], sz); ptrs[i] = NULL; }
	printf("all freed: rss=%.2fGB vmas=%lu\n",
	    rss_bytes() / (double)(1 << 30), count_vmas());
	/* Does the address space get reused?  Allocate once more and compare. */
	void *p = ALLOC(sz);
	printf("post-free alloc: %p vmas=%lu\n", p, count_vmas());
	if (p) FREE(p, sz);
	free(ptrs);
	return (0);
}

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
 * Phase 6 probe: RECLAIM UNDER PRESSURE.
 *
 * Build a heap of HEAP_GB (touched), free ALL of it at t=0, then sample RSS
 * every second for WINDOW s.  libumem's background reclaim
 * (umem_cache_reclaim_pages, run by the update thread every umem_reap_interval
 * = 10 s) advises away a slab's pages only after it has been empty for
 * umem_reclaim_delay = 30 s, and destroys the slab (returning the span) after
 * 60 s.  So the expected shape is: RSS flat for ~30-40 s, then a drop.
 * Oversize objects (> 128 KiB) skip the slab layer and their span is returned
 * to the mmap heap (PROT_NONE remap) on free, so they should drop immediately.
 *
 * glibc: free() of large chunks munmap()s at once (mmap threshold), and
 * top-of-heap trim happens at M_TRIM_THRESHOLD; interior holes stay resident.
 *
 * Usage: probe_reclaim [heap_gb] [window_s] [mix]
 *   mix: small = all 4 KiB objects; big = all 1 MiB; both (default)
 */
#include "limits.h"

int
main(int argc, char **argv)
{
	double gb = argc > 1 ? atof(argv[1]) : 2.0;
	int window = argc > 2 ? atoi(argv[2]) : 90;
	const char *mix = argc > 3 ? argv[3] : "both";
	uint64_t target = (uint64_t)(gb * (1ULL << 30));
	uint64_t nsmall = 0, nbig = 0, i;
	void **small = NULL, **big = NULL;
	int s;

	if (strcmp(mix, "small") == 0) nsmall = target / 4096;
	else if (strcmp(mix, "big") == 0) nbig = target / (1 << 20);
	else { nsmall = (target / 2) / 4096; nbig = (target / 2) / (1 << 20); }
	small = malloc((nsmall + 1) * sizeof (void *));
	big = malloc((nbig + 1) * sizeof (void *));

	uint64_t rss0 = rss_bytes();
	for (i = 0; i < nsmall; i++) {
		small[i] = ALLOC(4096);
		if (!small[i]) { printf("alloc fail\n"); return (1); }
		memset(small[i], 1, 4096);
	}
	for (i = 0; i < nbig; i++) {
		big[i] = ALLOC(1 << 20);
		if (!big[i]) { printf("alloc fail\n"); return (1); }
		memset(big[i], 1, 1 << 20);
	}
	uint64_t rss_peak = rss_bytes();
	printf("%s reclaim mix=%s heap=%.1fGB rss_before=%.0fMB rss_peak=%.0fMB vmas=%lu\n",
	    LIM_NAME, mix, gb, MB(rss0), MB(rss_peak), count_vmas());

	uint64_t t0 = now_ns();
	for (i = 0; i < nsmall; i++) FREE(small[i], 4096);
	for (i = 0; i < nbig; i++) FREE(big[i], 1 << 20);
	printf("freed all in %.2fs; rss now %.0fMB\n", (now_ns() - t0) / 1e9,
	    MB(rss_bytes()));
	printf("%6s %10s %8s\n", "t_s", "rssMB", "vmas");
	fflush(stdout);
	for (s = 0; s <= window; s++) {
		printf("%6d %10.0f %8lu\n", s, MB(rss_bytes()), count_vmas());
		fflush(stdout);
		/* keep the process alive and the allocator ticking with a tiny
		 * amount of unrelated traffic, as a real server would */
		void *p = ALLOC(32); if (p) FREE(p, 32);
		sleep(1);
	}
	uint64_t rss_end = rss_bytes();
	printf("summary: peak=%.0fMB end=%.0fMB returned=%.1f%% of (peak-before)\n",
	    MB(rss_peak), MB(rss_end),
	    100.0 * (double)(rss_peak - rss_end) / (double)(rss_peak - rss0));
	return (0);
}

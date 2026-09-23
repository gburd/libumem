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
 * Phase 6 probe: OBJECT COUNT.  Hold N small objects live, report RSS per
 * object at checkpoints and the worst single-allocation stall.
 *
 * What a cliff would look like: per-object overhead climbing with N (a
 * metadata structure that is not O(1) per object), or max-stall climbing
 * with N (umem_hash_rescale() rehashing under cache_lock, or an O(n) walk
 * on the update thread that blocks allocation).
 *
 * Usage: probe_objcount [count] [size]   (default 100M x 64B)
 */
#include "limits.h"

int
main(int argc, char **argv)
{
	uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 100000000ULL;
	size_t sz = argc > 2 ? strtoul(argv[2], NULL, 10) : 64;
	void **ptrs = malloc(n * sizeof (void *));	/* driver storage */
	uint64_t i, t0, worst = 0, worst_at = 0, sum = 0;
	uint64_t rss0 = rss_bytes();
	uint64_t step = n / 10;
	uint64_t nstall = 0;			/* allocations > 1 ms */

	if (ptrs == NULL) {
		printf("driver could not allocate %llu pointers\n",
		    (unsigned long long)n);
		return (2);
	}
	printf("%s objcount n=%llu size=%zu rss0=%.1fMB\n", LIM_NAME,
	    (unsigned long long)n, sz, MB(rss0));
	printf("%12s %10s %10s %10s %10s %8s\n", "objects", "rssMB",
	    "B/obj", "maxstall_us", "stalls>1ms", "vmas");

	t0 = now_ns();
	for (i = 0; i < n; i++) {
		uint64_t a = now_ns();
		ptrs[i] = ALLOC(sz);
		uint64_t d = now_ns() - a;
		if (ptrs[i] == NULL) {
			printf("FAIL at %llu: errno=%d (%s) rss=%.1fMB\n",
			    (unsigned long long)i, errno, strerror(errno),
			    MB(rss_bytes()));
			return (1);
		}
		*(volatile char *)ptrs[i] = 1;
		sum += d;
		if (d > worst) { worst = d; worst_at = i; }
		if (d > 1000000) nstall++;
		if ((i + 1) % step == 0) {
			uint64_t rss = rss_bytes();
			/* B/obj: (rss - rss0 - driver ptr array) / objects */
			double per = ((double)rss - (double)rss0 -
			    (double)(n * sizeof (void *))) / (double)(i + 1);
			printf("%12llu %10.1f %10.1f %10.1f %10llu %8lu\n",
			    (unsigned long long)(i + 1), MB(rss),
			    per, worst / 1000.0, (unsigned long long)nstall,
			    count_vmas());
			fflush(stdout);
		}
	}
	printf("alloc phase: %.2fs mean %.0fns worst %.1fms at obj %llu\n",
	    (now_ns() - t0) / 1e9, (double)sum / n, worst / 1e6,
	    (unsigned long long)worst_at);

	/*
	 * Now leave the heap parked for two update intervals so the update
	 * thread's periodic walk (umem_cache_update -> hash rescale, reclaim
	 * scan) runs with 100M objects live.  Time a burst of allocations
	 * during that window: a stall here is the walk holding a lock the
	 * allocation path needs.
	 */
	worst = 0; nstall = 0;
	uint64_t park_end = now_ns() + 25ULL * 1000000000ULL;
	uint64_t bursts = 0;
	while (now_ns() < park_end) {
		uint64_t a = now_ns();
		void *p = ALLOC(sz);
		uint64_t d = now_ns() - a;
		if (p) FREE(p, sz);
		if (d > worst) worst = d;
		if (d > 1000000) nstall++;
		bursts++;
		if ((bursts & 0xfff) == 0) usleep(100);
	}
	printf("parked 25s: %llu probe allocs, worst %.1fms, stalls>1ms %llu, rss=%.1fMB\n",
	    (unsigned long long)bursts, worst / 1e6,
	    (unsigned long long)nstall, MB(rss_bytes()));

	/* Free everything; time the free phase and report RSS return. */
	t0 = now_ns(); worst = 0;
	for (i = 0; i < n; i++) {
		uint64_t a = now_ns();
		FREE(ptrs[i], sz);
		uint64_t d = now_ns() - a;
		if (d > worst) worst = d;
	}
	printf("free phase: %.2fs worst %.1fms rss_after_free=%.1fMB\n",
	    (now_ns() - t0) / 1e9, worst / 1e6, MB(rss_bytes()));
	free(ptrs);
	return (0);
}

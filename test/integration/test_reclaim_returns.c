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
 * P6.8 regression: freed slab memory must come back to the OS without the
 * application calling umem_reap().
 *
 * WHAT WAS WRONG.  The periodic update pass ran umem_depot_ws_update() (the
 * working-set BOOKKEEPING) every interval but never the reap that acts on it;
 * only umem_reap() -- called by the application or by a failed backend
 * allocation -- reached umem_depot_ws_reap().  So on a process that freed a
 * heap and never ran out of memory, every freed object sat in a depot
 * magazine forever, slab_refcnt never reached zero, and the slab reclaimer
 * (reclaim=1 by default, reclaim_delay documented) skipped every slab.
 * Measured: 2 GB of freed 4 KiB objects 100 % resident at t = 100 s with the
 * update thread confirmed running.  A second defect capped the reap that
 * umem_reap() DID trigger at 8 magazines per list per pass (5 MB / 100 s).
 *
 * WHAT THIS TEST DOES.  Allocate and touch a heap of 4 KiB objects through
 * umem_alloc, free all of it, then sample RSS.  With reap_interval=1 and
 * reclaim_delay=2 (set via UMEM_OPTIONS by the .sh wrapper -- both are
 * documented tunables) the full cycle -- two ws_update passes, reap,
 * slab_refcnt -> 0, SLAB_DIRTY, delay, madvise -- fits in a few seconds.
 *
 *   PASS: RSS falls below 30 % of the post-free plateau within the window.
 *   FAIL: it does not.  Pre-fix it sits at ~100 % indefinitely.
 *
 * The test does NOT call umem_reap().  That is the point: the application
 * should not have to.
 *
 * SIZING.  128 MB is enough to make the RSS signal unambiguous (the noise
 * floor is a few MB) and small enough for any CI box.  4 KiB objects: the
 * hashed slab path, 16 per 64 KiB slab, so each slab's 16 objects must ALL
 * come back from the depot before it can be reclaimed -- the hard case.
 */

#include "umem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	OBJ		4096
#define	HEAP		(128UL * 1024 * 1024)
#define	NOBJ		(HEAP / OBJ)
#define	WINDOW_S	20
#define	PASS_FRACTION	0.30

static long
rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kb = -1;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof (line), f) != NULL)
		if (sscanf(line, "VmRSS: %ld", &kb) == 1)
			break;
	(void) fclose(f);
	return (kb);
}

int
main(void)
{
	void **held;
	unsigned long i;
	long rss_base, rss_full, rss_freed, rss_now, rss_min;
	int t;

	held = malloc(NOBJ * sizeof (void *));
	if (held == NULL)
		return (77);

	rss_base = rss_kb();
	for (i = 0; i < NOBJ; i++) {
		held[i] = umem_alloc(OBJ, UMEM_DEFAULT);
		if (held[i] == NULL) {
			printf("SKIP: umem_alloc failed at %lu\n", i);
			return (77);
		}
		memset(held[i], 0x5a, OBJ);
	}
	rss_full = rss_kb();

	for (i = 0; i < NOBJ; i++)
		umem_free(held[i], OBJ);
	free(held);
	rss_freed = rss_kb();

	printf("base=%ldMB full=%ldMB after_free=%ldMB (heap %luMB)\n",
	    rss_base / 1024, rss_full / 1024, rss_freed / 1024,
	    HEAP / (1024 * 1024));

	/*
	 * The reclaimable amount is what the heap added.  Pass when at least
	 * (1 - PASS_FRACTION) of THAT is gone, i.e. RSS is within
	 * PASS_FRACTION * heap of the pre-heap baseline.
	 */
	long heap_kb = rss_freed - rss_base;
	long target_kb = rss_base + (long)(heap_kb * PASS_FRACTION);

	rss_min = rss_freed;
	for (t = 1; t <= WINDOW_S; t++) {
		/*
		 * A live process, not a parked one: one small alloc/free per
		 * second so the run resembles an idle server, and to make
		 * sure a busy-enough update thread is not the difference.
		 */
		void *p = umem_alloc(32, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, 32);
		sleep(1);
		rss_now = rss_kb();
		if (rss_now < rss_min)
			rss_min = rss_now;
		printf("  t=%2ds rss=%ldMB\n", t, rss_now / 1024);
		if (rss_now <= target_kb) {
			printf("RESULT: PASS (freed heap returned: RSS %ldMB "
			    "<= %ldMB at t=%ds, no umem_reap() called)\n",
			    rss_now / 1024, target_kb / 1024, t);
			return (0);
		}
	}

	printf("RESULT: FAIL (RSS never fell below %ldMB in %ds; min %ldMB "
	    "of %ldMB after free -- freed slab memory is not being returned; "
	    "the depot is not reaped by the periodic pass)\n",
	    target_kb / 1024, WINDOW_S, rss_min / 1024, rss_freed / 1024);
	return (1);
}

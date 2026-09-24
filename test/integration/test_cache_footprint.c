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
 * P6.4 regression: per-cache footprint, and VMAs left behind by destroy.
 *
 * THE DEFECT.  umem_cache_create() made three page-rounded mmap()s per cache
 * -- two depot arrays and the rseq array -- each holding ncpus x 64 B, i.e.
 * 512 B on an 8-CPU box in a 4 KiB page.  12 KB of an 18.6 KB per-cache
 * footprint was page rounding.  And umem_cache_destroy() munmap()ed three
 * holes into whatever the kernel had merged those mappings into, so 50,000
 * destroyed caches left 29,159 VMAs (44 % of vm.max_map_count) behind for
 * caches that no longer existed.
 *
 * WHAT THIS TESTS, with N = 2,000 caches (the property is per-cache; 2,000 is
 * enough to make both numbers unambiguous and runs in about a second):
 *
 *   footprint:  (RSS after creating N) - (RSS before) / N  <  FOOTPRINT_MAX
 *   VMA leak:   (VMAs after destroying all N) - (VMAs before creating)  <  VMA_LEAK_MAX
 *
 * Pre-fix on 8 CPUs: ~18.6 KB/cache and ~0.6 VMAs left per destroyed cache
 * (5,930 for 10k).  Post-fix: the three arrays share one mapping that is
 * exactly as large as their contents, so footprint is dominated by the
 * descriptor and one live slab, and destroy leaves at most one hole per
 * cache -- and adjacent holes merge back.
 *
 * The thresholds scale with umem_max_ncpus: the descriptor's cache_cpu[] is
 * 128 B x ncpus and is legitimately part of the footprint.  On a 192-CPU box
 * (ncpus rounded to 256) the descriptor alone is 33 KB; that is P6.4b (3),
 * not this test.  FOOTPRINT_MAX is therefore 12 KB + 128 B x ncpus.
 */

#include "umem_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	NCACHES		2000
#define	VMA_LEAK_MAX	200		/* was ~1,200 at this N pre-fix */

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

static long
vma_count(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	long n = 0;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof (line), f) != NULL)
		n++;
	(void) fclose(f);
	return (n);
}

int
main(void)
{
	static umem_cache_t *caches[NCACHES];
	static void *objs[NCACHES];
	char name[64];
	long rss0, rss1, vma0, vma1, vma2;
	long per_cache_bytes, footprint_max, leaked;
	int i, fails = 0;
	void *warm;

	warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, 64);

	footprint_max = 12 * 1024 + 128L * umem_max_ncpus;

	rss0 = rss_kb();
	vma0 = vma_count();

	for (i = 0; i < NCACHES; i++) {
		(void) snprintf(name, sizeof (name), "p64_%d", i);
		caches[i] = umem_cache_create(name, 64, 0, NULL, NULL, NULL,
		    NULL, NULL, 0);
		if (caches[i] == NULL) {
			printf("SKIP: umem_cache_create failed at %d\n", i);
			return (77);
		}
		/* One live object so each cache has a real slab, as in use. */
		objs[i] = umem_cache_alloc(caches[i], UMEM_DEFAULT);
	}
	rss1 = rss_kb();
	vma1 = vma_count();

	for (i = 0; i < NCACHES; i++) {
		if (objs[i] != NULL)
			umem_cache_free(caches[i], objs[i]);
		umem_cache_destroy(caches[i]);
	}
	vma2 = vma_count();

	per_cache_bytes = (rss1 - rss0) * 1024L / NCACHES;
	leaked = vma2 - vma0;

	printf("ncpus=%u caches=%d\n", umem_max_ncpus, NCACHES);
	printf("footprint: %ld bytes/cache (max %ld)\n", per_cache_bytes,
	    footprint_max);
	printf("vmas: before=%ld with_caches=%ld after_destroy=%ld "
	    "leaked=%ld (max %d)\n", vma0, vma1, vma2, leaked, VMA_LEAK_MAX);

	if (per_cache_bytes > footprint_max) {
		printf("FAIL: %ld bytes per cache exceeds %ld -- per-CPU arrays "
		    "are page-rounded mmaps again\n", per_cache_bytes,
		    footprint_max);
		fails++;
	}
	if (leaked > VMA_LEAK_MAX) {
		printf("FAIL: destroying %d caches left %ld VMAs behind -- "
		    "destroy punches holes into merged mappings\n", NCACHES,
		    leaked);
		fails++;
	}
	if (fails) {
		printf("RESULT: FAIL (%d)\n", fails);
		return (1);
	}
	printf("RESULT: PASS (%ld bytes/cache, %ld VMAs left by %d destroys)\n",
	    per_cache_bytes, leaked, NCACHES);
	return (0);
}

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
 * P6.2 regression: freeing oversize objects must not cost one VMA each.
 *
 * THE DEFECT.  Objects above UMEM_MAXBUF (128 KiB) are their own span from
 * the mmap heap.  vmem_mmap_free() remapped a freed span PROT_NONE with
 * MAP_FIXED, which splits the RW mapping it came from into RW / NONE / RW --
 * one new kernel VMA per freed oversize object, permanently, because the
 * kernel does not merge a later MAP_FIXED re-commit back into its
 * neighbours.  Measured: 40,102 VMAs at 40,000 half-freed 136 KiB objects;
 * vm.max_map_count is 65,530.  glibc munmap()s and the VMA disappears.
 *
 * WHAT THIS TESTS.  Allocate N objects of 136 KiB (just over UMEM_MAXBUF, so
 * every one is oversize), touch them, free every other one, allocate N/2
 * more, and count VMAs.  Pre-fix: ~N/2 new VMAs at the half-freed point.
 * Post-fix (MADV_DONTNEED below the guard threshold): a handful.
 *
 * THE CONTROL ARM proves the guard still exists where it is meant to:
 * with UMEM_OPTIONS=mmap_guard=4096 every freed span is PROT_NONE-remapped
 * again, and the VMA count must go back UP.  Without that arm a build that
 * had simply lost the PROT_NONE path (say, a broken madvise fallback) would
 * pass the main arm for the wrong reason.  The .sh wrapper runs both.
 *
 * RSS is reported for both arms: DONTNEED must return the pages as
 * completely as the remap did.  It is asserted loosely (freed half's RSS
 * gone within 25 %), because RSS accounting has page-cluster granularity
 * and the point of this test is the VMA count.
 */

#include "umem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	OBJ		(136 * 1024)
#define	NOBJ		2000
#define	VMA_SLACK	64		/* pre-fix: ~1000 new at half-freed */

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

static void
touch(void *p)
{
	size_t o;

	for (o = 0; o < OBJ; o += 4096)
		((char *)p)[o] = 1;
}

int
main(int argc, char **argv)
{
	static void *held[NOBJ];
	int expect_guard = (argc > 1 && strcmp(argv[1], "guard") == 0);
	long vma0, vma_full, vma_half, rss_full, rss_half;
	int i, freed = 0;

	for (i = 0; i < NOBJ; i++) {
		held[i] = umem_alloc(OBJ, UMEM_DEFAULT);
		if (held[i] == NULL) {
			printf("SKIP: umem_alloc(%d) failed at %d\n", OBJ, i);
			return (77);
		}
		touch(held[i]);
	}
	vma0 = vma_count();
	vma_full = vma0;
	rss_full = rss_kb();

	for (i = 0; i < NOBJ; i += 2) {
		umem_free(held[i], OBJ);
		held[i] = NULL;
		freed++;
	}
	vma_half = vma_count();
	rss_half = rss_kb();

	printf("%s: %d x %d KiB oversize; freed %d\n",
	    expect_guard ? "guard arm (mmap_guard=4096)" : "default arm",
	    NOBJ, OBJ / 1024, freed);
	printf("  vmas: all live=%ld  half freed=%ld  (delta %ld)\n",
	    vma_full, vma_half, vma_half - vma_full);
	printf("  rss:  all live=%ldMB half freed=%ldMB (freed half = %ldMB)\n",
	    rss_full / 1024, rss_half / 1024,
	    (long)freed * OBJ / 1024 / 1024);

	/* RSS must come back either way. */
	{
		long expect_drop_kb = (long)freed * (OBJ / 1024);
		long actual_drop_kb = rss_full - rss_half;
		if (actual_drop_kb < expect_drop_kb * 3 / 4) {
			printf("RESULT: FAIL (RSS dropped %ldMB of the %ldMB "
			    "freed -- freed spans are not being returned)\n",
			    actual_drop_kb / 1024, expect_drop_kb / 1024);
			return (1);
		}
	}

	if (expect_guard) {
		/* PROT_NONE remap: one VMA per freed span is the SIGNATURE. */
		if (vma_half - vma_full < freed / 2) {
			printf("RESULT: FAIL (guard arm: only %ld new VMAs for "
			    "%d PROT_NONE-remapped frees -- the guard path is "
			    "not running; the default arm's pass is vacuous)\n",
			    vma_half - vma_full, freed);
			return (1);
		}
		printf("RESULT: PASS (guard arm: %ld new VMAs for %d frees; the "
		    "PROT_NONE path exists and splits mappings as expected)\n",
		    vma_half - vma_full, freed);
		return (0);
	}

	if (vma_half - vma_full > VMA_SLACK) {
		printf("RESULT: FAIL (%ld new VMAs from freeing %d oversize "
		    "objects -- vmem_mmap_free is punching PROT_NONE holes "
		    "below the guard threshold; see P6.2)\n",
		    vma_half - vma_full, freed);
		return (1);
	}
	printf("RESULT: PASS (%ld new VMAs from %d oversize frees; RSS "
	    "returned)\n", vma_half - vma_full, freed);
	return (0);
}

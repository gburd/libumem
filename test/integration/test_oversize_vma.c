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
 *
 * The default arm also reads the freed spans back through their stale
 * pointers: a span below the guard is discarded, so it must not still hold
 * its old bytes.  illumos's MADV_DONTNEED drops RSS but keeps the data (it only
 * syncs), which left every freed span readable as it was; the discard is
 * MADV_PURGE there (pal_mem.h).
 */

#include "umem.h"
#include "../portable_meminfo.h"
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
	return (pm_vma_count());
}

static long
rss_kb(void)
{
	return (pm_rss_kb());
}

static void
touch(void *p)
{
	size_t o;

	for (o = 0; o < OBJ; o += 4096)
		((char *)p)[o] = 1;
}

/* Pages of a freed span that still read the value touch() wrote. */
static long
stale_pages(const void *p)
{
	const volatile char *c = p;
	size_t o;
	long kept = 0;

	for (o = 0; o < OBJ; o += 4096)
		kept += (c[o] == 1);
	return (kept);
}

int
main(int argc, char **argv)
{
	static void *held[NOBJ];
	static void *gone[NOBJ / 2];	/* stale pointers to the freed half */
	int expect_guard = (argc > 1 && strcmp(argv[1], "guard") == 0);
	long vma0, vma_full, vma_half, rss_full, rss_half, kept = 0;
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
		gone[freed] = held[i];
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
		/*
		 * PROT_NONE remap: on Linux, mprotect() splits the VMA so one
		 * extra /proc/self/maps line appears per freed span -- that is
		 * the observable signature the guard path ran.  illumos does
		 * NOT split the /proc/self/map array on a protection change
		 * (verified: PROT_NONE on 10 full mappings added ~0 entries),
		 * so the VMA-count signature is Linux-only.  Both arms still
		 * assert what IS observable everywhere: RSS came back (above).
		 */
#if defined(__linux__)
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
#else
		printf("RESULT: PASS (guard arm: RSS returned; VMA-split "
		    "signature not checked -- this OS does not expose "
		    "per-protection map splits)\n");
#endif
		return (0);
	}

	/*
	 * Default arm: freeing oversize spans must NOT leave new mappings
	 * behind (vmem_mmap_free must unmap, not PROT_NONE-punch).  The map
	 * count is observable on both Linux (/proc/self/maps) and illumos
	 * (/proc/self/map), so this assertion runs everywhere.
	 */
	if (vma_half - vma_full > VMA_SLACK) {
		printf("RESULT: FAIL (%ld new VMAs from freeing %d oversize "
		    "objects -- vmem_mmap_free is punching PROT_NONE holes "
		    "below the guard threshold; see P6.2)\n",
		    vma_half - vma_full, freed);
		return (1);
	}

	/*
	 * The freed spans' contents are gone.  Nothing has allocated since the
	 * frees, so every one is still free and still mapped read-write (the
	 * guard is not in play below 16 MiB).  Asserted where the decommit
	 * discards (Linux, illumos); FreeBSD's MADV_DONTNEED and MADV_FREE do
	 * not promise to, so there the count is only reported.
	 */
	for (i = 0; i < freed; i++)
		kept += stale_pages(gone[i]);
	printf("  freed spans read back: %ld of %ld pages still hold their "
	    "old data\n", kept, (long)freed * (OBJ / 4096));
#if !defined(__linux__) && !defined(__sun)
	kept = 0;
#endif
	if (kept != 0) {
		printf("RESULT: FAIL (freed spans below the guard were not "
		    "discarded: a stale pointer still reads their contents)\n");
		return (1);
	}
	printf("RESULT: PASS (%ld new VMAs from %d oversize frees; RSS "
	    "returned; freed spans discarded)\n", vma_half - vma_full, freed);
	return (0);
}

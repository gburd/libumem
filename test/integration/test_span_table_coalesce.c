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
 * The exact span table (P7.4) must hold one entry per run of adjacent heap
 * spans, and stay exact.
 *
 * THE COST THIS GUARDS.  free() confirms ownership by finding the pointer in
 * vmem_heap's span table, behind a one-entry per-thread cache of the last
 * span it hit.  The table used to hold one entry per span.  The sbrk heap
 * grows by one span per import, all address-adjacent, so a gcc compile on
 * illumos built a table of ~10,000 entries and 14.3 M of its 21 M frees
 * missed the cache and paid a binary search; the mmap heap (8.7 M misses)
 * behaves the same way, less so.  With adjacent spans merged, the sbrk heap
 * is one entry and the cache hits.
 *
 * WHAT THIS TESTS (UMEM_OPTIONS=backend=sbrk, so growth is adjacent on
 * every platform).
 *   A. Growth: NBIG oversize allocations each import their own heap span.
 *      The table must not grow with them: at most a handful of entries.
 *   B. Exactness inside the run: every one of those allocations is owned
 *      (vmem_span_owns).
 *   C. Exactness after removal: free every other one.  Each fully free
 *      oversize span goes back to sbrk_top (vmem_span_destroy), splitting
 *      the run; the freed ranges must then read as not owned, and the live
 *      ones still as owned.  This is what keeps the forged-header check of
 *      test_forged_span_gap meaningful after a merge.
 *
 *   PASS: A, B, C hold.
 *   FAIL: the table grows per span (no merging), or a freed span still
 *         reads as owned (a merge that does not split), or a live one does
 *         not.
 */

#include "umem_base.h"
#include "vmem_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define	NBIG	64
#define	BIG	(512 * 1024)	/* above UMEM_MAXBUF: one heap span each */
#define	MAXENT	8		/* the run, plus spans made before the test */

int
main(void)
{
	void *big[NBIG];
	uint32_t before, grown, after;
	int i, failures = 0, unowned = 0, live_owned = 0, freed_owned = 0;

	(void) setenv("UMEM_OPTIONS", "backend=sbrk", 1);
	big[0] = umem_alloc(64, UMEM_DEFAULT);
	if (big[0] == NULL) {
		printf("SKIP: umem did not initialize\n");
		return (77);
	}
	umem_free(big[0], 64);
	before = vmem_span_entries();

	for (i = 0; i < NBIG; i++) {
		big[i] = umem_alloc(BIG, UMEM_DEFAULT);
		if (big[i] == NULL) {
			printf("SKIP: umem_alloc(%d) failed at %d\n", BIG, i);
			return (77);
		}
	}
	grown = vmem_span_entries();
	printf("[A] span table: %u entries before, %u after %d adjacent "
	    "%d KiB spans\n", before, grown, NBIG, BIG / 1024);
	if (grown > MAXENT) {
		printf("  FAIL: the table grows per span (one entry each)\n");
		failures++;
	}

	for (i = 0; i < NBIG; i++)
		if (!vmem_span_owns((uintptr_t)big[i], BIG))
			unowned++;
	printf("[B] every live span owned: %s\n",
	    unowned == 0 ? "yes" : "NO");
	failures += unowned;

	for (i = 0; i < NBIG; i += 2)
		umem_free(big[i], BIG);
	after = vmem_span_entries();
	for (i = 0; i < NBIG; i++) {
		int owned = vmem_span_owns((uintptr_t)big[i], BIG);

		if (i % 2 == 0)
			freed_owned += owned;
		else
			live_owned += owned;
	}
	printf("[C] after freeing every other span: %u entries; freed spans "
	    "still owned %d of %d, live spans owned %d of %d\n", after,
	    freed_owned, NBIG / 2, live_owned, NBIG / 2);
	if (freed_owned != 0) {
		printf("  FAIL: freed spans still read as heap (merge without "
		    "split)\n");
		failures++;
	}
	if (live_owned != NBIG / 2) {
		printf("  FAIL: live spans read as not owned\n");
		failures++;
	}
	for (i = 1; i < NBIG; i += 2)
		umem_free(big[i], BIG);

	if (failures != 0) {
		printf("RESULT: FAIL (%d)\n", failures);
		return (1);
	}
	printf("RESULT: PASS\n");
	return (0);
}

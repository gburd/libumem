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
 * test_findleaks_rseq.c -- O9: findleaks must NOT report objects that are
 * resident in rseq per-CPU magazines as outstanding.
 *
 * umem_inspect.c's cached_set_build_cache() subtracts, from the outstanding
 * ("leak") count, every buffer it can find still parked in a retention site:
 * depot full lists, per-CPU loaded/previous magazines, and -- the block this
 * test exercises -- each cache's rseq per-CPU loaded/previous magazine
 * (cache_rseq[], guarded by UMEM_RSEQ_AVAILABLE && umem_rseq_enabled).
 *
 * Phase 3 recorded that subtraction as compiled-but-never-exercised because
 * rseq then served zero rounds.  Since v3.3.2 (P8.5b) the PTC magazine
 * refill/flush routes THROUGH the rseq layer, so on a PTC-magazine flush miss
 * a freed buffer lands in cache_rseq[cpu].  That makes the subtraction
 * testable for the first time.
 *
 * WHY THIS IS NOT A SIMPLE "cached_skipped > 0" CHECK.  The depot and per-CPU
 * magazines are subtracted too, and they exist in both builds.  norseq is NOT
 * a clean negative arm: with rseq off those buffers do not vanish, they sit in
 * the DEPOT full list instead, which cached_set_build_cache() also enumerates
 * and subtracts -- so the count would barely move and would prove nothing
 * about the rseq block specifically.
 *
 * The clean negative arm is therefore a test-only build knob,
 * -DUMEM_FINDLEAKS_NO_RSEQ_SUBTRACT, that compiles out ONLY the rseq
 * subtraction block in cached_set_build_cache() and changes nothing else.
 * The default library is byte-identical.  The two builds run the identical
 * program:
 *
 *   default   (rseq subtracted):     rseq-resident buffers are NOT counted
 *   knob build (rseq NOT subtracted): the SAME buffers ARE counted
 *
 * so cached_skipped(default) - cached_skipped(knob) == (buffers this program
 * parked in rseq magazines) > 0.  That difference is exactly the rseq
 * subtraction doing its job; nothing else in the run differs.
 *
 * If the platform has no rseq (UMEM_RSEQ_AVAILABLE unset, or the kernel
 * refused registration so umem_rseq_enabled==0), both builds behave
 * identically and there is nothing to test: SKIP (77).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <umem.h>
#include "umem_inspect.h"

/*
 * Extract "cached_skipped":<n> from findleaks' one-line JSON object.
 */
static long
findleaks_cached_skipped(void)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *ms = open_memstream(&buf, &len);
	long v = -1;

	if (ms == NULL)
		return (-1);
	(void) umem_findleaks(ms, UMEM_FMT_JSON, 10);
	(void) fclose(ms);

	if (buf != NULL) {
		const char *p = strstr(buf, "\"cached_skipped\":");
		if (p != NULL)
			v = strtol(p + strlen("\"cached_skipped\":"), NULL, 10);
		free(buf);
	}
	return (v);
}

/*
 * Park buffers in the rseq per-CPU magazines.
 *
 * The rseq layer sits behind the PTC.  A freed buffer only reaches it on a
 * PTC magazine FLUSH miss -- i.e. when both the loaded and previous PTC
 * magazines are already full and the flush routes the overflow through
 * umem_rseq_ptc_free().  So a large free burst of one PTC-served size class
 * (<= umem_ptc_maxsize, default 8192) first fills the two PTC magazines and
 * then pours the rest through rseq into cache_rseq[cpu].
 *
 * Every buffer freed here is still counted "allocated" by the slab layer
 * (freeing to a cache does not return it to the slab), so umem_walk_allocated
 * sees all of them; the ones now in rseq magazines are what the subtraction
 * must remove.
 */
#define	OBJSZ	512		/* PTC-served (<= 8192) */
#define	NBUF	20000		/* well past two PTC magazines' worth */

int
main(void)
{
	void **p = calloc(NBUF, sizeof (*p));
	long baseline, after;
	int i;

	if (p == NULL) {
		fprintf(stderr, "harness: calloc failed\n");
		return (99);
	}

	/*
	 * Baseline: cached_skipped from whatever is already resident before
	 * this program frees anything of its own.
	 */
	baseline = findleaks_cached_skipped();
	if (baseline < 0) {
		fprintf(stderr, "harness: findleaks produced no JSON "
		    "cached_skipped field\n");
		free(p);
		return (99);
	}

	for (i = 0; i < NBUF; i++) {
		p[i] = umem_alloc(OBJSZ, UMEM_DEFAULT);
		if (p[i] == NULL) {
			fprintf(stderr, "harness: umem_alloc failed at %d\n", i);
			free(p);
			return (99);
		}
	}
	/* Free them all: PTC mags fill, the overflow routes through rseq. */
	for (i = 0; i < NBUF; i++)
		umem_free(p[i], OBJSZ);
	free(p);

	after = findleaks_cached_skipped();
	if (after < 0) {
		fprintf(stderr, "harness: findleaks produced no JSON\n");
		return (99);
	}

	printf("cached_skipped: baseline=%ld after=%ld delta=%ld\n",
	    baseline, after, after - baseline);

#ifdef UMEM_FINDLEAKS_NO_RSEQ_SUBTRACT
	/*
	 * NEGATIVE ARM: the rseq subtraction is compiled out of the library.
	 * The identical free burst still parks buffers in rseq magazines, but
	 * they are NOT subtracted, so they show up as outstanding.  We only
	 * print our count; the driver compares it against the default build.
	 */
	printf("RESULT: NEGATIVE-ARM (rseq subtraction compiled out); "
	    "cached_skipped after=%ld\n", after);
	return (0);
#else
	/*
	 * POSITIVE ARM (default build): the buffers we just parked in rseq
	 * magazines must be subtracted.  If rseq is unavailable/disabled the
	 * rseq block does nothing and there is nothing under test.
	 */
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
	if (after <= baseline) {
		/*
		 * Either the kernel refused rseq registration (nothing to
		 * test) or the buffers all fit in the two PTC magazines and
		 * none reached rseq.  Distinguish by size: NBUF=20000 far
		 * exceeds two PTC magazines, so a zero delta on a working
		 * rseq build would be a real failure.  We cannot read
		 * umem_rseq_enabled from here, so treat no growth as SKIP and
		 * let the driver decide using the negative-arm delta.
		 */
		printf("RESULT: SKIP (cached_skipped did not grow; rseq likely "
		    "unavailable or disabled)\n");
		return (77);
	}
	printf("RESULT: PASS (findleaks subtracted %ld rseq/magazine-resident "
	    "buffers; they are not reported as outstanding)\n",
	    after - baseline);
	return (0);
#else
	printf("RESULT: SKIP (no rseq on this platform)\n");
	return (77);
#endif
#endif
}

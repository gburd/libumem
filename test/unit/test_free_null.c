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
 * P1.8 regression: umem_free(NULL, size) must be a no-op for every size.
 *
 * THE DEFECT.  _umem_free() indexed umem_alloc_table by size and stored
 * `buf` into the PTC bin (or, with tcache=0, the CPU magazine) with no check
 * that buf != NULL; the only NULL check was on the oversize branch, and only
 * for size == 0.  So umem_free(NULL, 64) put a NULL in the 64 B free list
 * and the NEXT umem_alloc(64) on that thread returned NULL with errno == 0:
 * a spurious allocation failure that reports success, one call removed from
 * its cause.  umem_cache_free(cp, NULL) had the same hole.  Demonstrated in
 * docs/reviews/2026-09-24-production-readiness.md section 2.1.
 *
 * WHAT THIS TESTS, exactly.  For each PTC-served, magazine-served and
 * oversize size, and for a umem_cache_t: free NULL, then allocate.  The
 * allocation must succeed.  Pre-fix the 64 B and 2560 B cases return NULL
 * (the 64 B case with the PTC on, the 2560 B case with tcache=0 as well when
 * run that way); post-fix every allocation is non-NULL.  Then free NULL
 * through both paths a few hundred times and allocate again, so a NULL that
 * merely moved one step down the cache hierarchy still shows up.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "umem.h"

static int fails;

static void
check(const char *what, void *p)
{
	if (p == NULL) {
		printf("FAIL: %s: allocation after freeing NULL returned NULL "
		    "(errno %d)\n", what, errno);
		fails++;
	} else {
		printf("ok:   %s\n", what);
	}
}

int
main(void)
{
	static const size_t sizes[] = { 8, 64, 512, 2048, 2560, 8192, 16384,
	    262144 };
	umem_cache_t *cp;
	void *p;
	size_t i;
	int r;

	for (i = 0; i < sizeof (sizes) / sizeof (sizes[0]); i++) {
		size_t sz = sizes[i];
		char what[64];

		/* Warm the class so the PTC/magazine exists for this thread. */
		p = umem_alloc(sz, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, sz);

		errno = 0;
		umem_free(NULL, sz);
		p = umem_alloc(sz, UMEM_DEFAULT);
		(void) snprintf(what, sizeof (what), "umem_free(NULL, %zu); "
		    "umem_alloc(%zu)", sz, sz);
		check(what, p);
		if (p != NULL)
			umem_free(p, sz);

		/* Many NULLs: a bin holds 16-128, a magazine 63-255. */
		for (r = 0; r < 300; r++)
			umem_free(NULL, sz);
		p = umem_alloc(sz, UMEM_DEFAULT);
		(void) snprintf(what, sizeof (what), "300 x umem_free(NULL, "
		    "%zu); umem_alloc(%zu)", sz, sz);
		check(what, p);
		if (p != NULL)
			umem_free(p, sz);
	}

	cp = umem_cache_create("p18_null_free", 96, 0, NULL, NULL, NULL, NULL,
	    NULL, 0);
	if (cp == NULL) {
		printf("FAIL: umem_cache_create\n");
		return (1);
	}
	p = umem_cache_alloc(cp, UMEM_DEFAULT);
	if (p != NULL)
		umem_cache_free(cp, p);
	for (r = 0; r < 300; r++)
		umem_cache_free(cp, NULL);
	p = umem_cache_alloc(cp, UMEM_DEFAULT);
	check("300 x umem_cache_free(cp, NULL); umem_cache_alloc(cp)", p);
	if (p != NULL)
		umem_cache_free(cp, p);
	umem_cache_destroy(cp);

	umem_free(NULL, 0);
	printf("ok:   umem_free(NULL, 0)\n");

	if (fails == 0)
		printf("RESULT: PASS (freeing NULL is a no-op on every path)\n");
	else
		printf("RESULT: FAIL (%d)\n", fails);
	return (fails ? 1 : 0);
}

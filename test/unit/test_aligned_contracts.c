/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * P1.7(c,e) regression: aligned-allocation contracts and interposer
 * ownership across realloc/free/malloc_usable_size.
 *
 * Meant to run under LD_PRELOAD=libumem_malloc.so (see
 * test/stress/interpose_regress.sh).  Without LD_PRELOAD it checks the
 * platform allocator and acts as a control: every assertion here is a
 * requirement of C11/POSIX, so it must pass either way.
 *
 * DEFECTS REPRODUCED
 *
 *  1. posix_memalign() did not enforce POSIX's "alignment is a multiple of
 *     sizeof(void *)" requirement: posix_memalign(&p, 4, n) on LP64
 *     succeeded (it only rejected non-powers-of-two), and on failure it
 *     returned errno -- which can legitimately be 0 -- rather than the
 *     required error number.
 *
 *  2. aligned_alloc() was not interposed at all, so a program's
 *     aligned_alloc() reached libc malloc while its free() reached umem.
 *     Detected as: the pointer must be recognized by the interposer's own
 *     malloc_usable_size()/realloc(), and must survive free().
 *
 *  3. realloc() of an interposer-tracked libc bootstrap pointer released
 *     the ownership record BEFORE knowing the new allocation succeeded, so
 *     after a failed realloc the original pointer was live but no longer
 *     recognized by free().  Exercised here with a realloc that must fail
 *     (SIZE_MAX-ish) followed by continued legitimate use and free of the
 *     original -- the original must still be intact and freeable.
 */

#define _GNU_SOURCE
#include "config.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

static int failures;

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			(void) printf("FAIL: " __VA_ARGS__);		\
			(void) printf("      (%s:%d: %s)\n",		\
			    __FILE__, __LINE__, #cond);			\
			failures++;					\
		}							\
	} while (0)

/*
 * POSIX: posix_memalign() shall return EINVAL if alignment is not a power of
 * two multiple of sizeof(void *).  It must return the error number, never
 * set errno-as-return.
 */
static void
test_posix_memalign_contract(void)
{
	void *p;
	int rc;
	size_t i;
	/* Not multiples of sizeof(void *) on LP64 (and 4 is invalid there). */
	const size_t bad_align[] = { 1, 2, 4, 3, 5, 6, 12, 0 };

	for (i = 0; i < sizeof (bad_align) / sizeof (bad_align[0]); i++) {
		if (bad_align[i] != 0 && bad_align[i] >= sizeof (void *) &&
		    (bad_align[i] & (bad_align[i] - 1)) == 0)
			continue;	/* legitimately valid on this ABI */
		p = (void *)(uintptr_t)0xdeadbeef;
		errno = 0;
		rc = posix_memalign(&p, bad_align[i], 64);
		CHECK(rc == EINVAL,
		    "posix_memalign(align=%zu) returned %d, want EINVAL(%d)\n",
		    bad_align[i], rc, EINVAL);
		if (rc == 0) {
			CHECK(((uintptr_t)p % bad_align[i]) == 0 ||
			    bad_align[i] == 0,
			    "posix_memalign(align=%zu) returned "
			    "unaligned %p\n", bad_align[i], p);
			free(p);
		}
	}

	/* Valid alignments must work and be honored. */
	for (i = sizeof (void *); i <= 4096; i *= 2) {
		p = NULL;
		rc = posix_memalign(&p, i, 100);
		CHECK(rc == 0,
		    "posix_memalign(align=%zu, 100) failed with %d\n", i, rc);
		if (rc == 0) {
			CHECK(p != NULL,
			    "posix_memalign(align=%zu) returned 0 but NULL\n",
			    i);
			CHECK(((uintptr_t)p % i) == 0,
			    "posix_memalign(align=%zu) gave %p, misaligned\n",
			    i, p);
			(void) memset(p, 0x11, 100);
			free(p);
		}
	}

	/*
	 * A size of 0 may return NULL or a freeable pointer, but must not
	 * report a bogus error.
	 */
	p = NULL;
	rc = posix_memalign(&p, sizeof (void *), 0);
	CHECK(rc == 0 || rc == ENOMEM,
	    "posix_memalign(size=0) returned %d\n", rc);
	if (rc == 0)
		free(p);
}

/*
 * C11 aligned_alloc(): must return storage aligned to `alignment` that
 * free() accepts.  When the interposer is active it must be the
 * interposer's storage, which we probe via realloc() (which only
 * recognizes interposer-owned pointers) and malloc_usable_size().
 */
static void
test_aligned_alloc_contract(void)
{
	size_t align;

	for (align = sizeof (void *); align <= 4096; align *= 2) {
		size_t sz = align * 3;
		unsigned char *p = aligned_alloc(align, sz);
		unsigned char *q;
		size_t i;

		CHECK(p != NULL, "aligned_alloc(%zu, %zu) failed\n", align, sz);
		if (p == NULL)
			continue;
		CHECK(((uintptr_t)p % align) == 0,
		    "aligned_alloc(%zu) returned misaligned %p\n", align,
		    (void *)p);
#ifdef HAVE_MALLOC_USABLE_SIZE
		CHECK(malloc_usable_size(p) >= sz,
		    "malloc_usable_size(aligned_alloc(%zu,%zu)) = %zu < %zu; "
		    "the pointer is not recognized by the active allocator\n",
		    align, sz, malloc_usable_size(p), sz);
#endif
		(void) memset(p, 0x7e, sz);

		/* realloc must recognize it and preserve contents. */
		q = realloc(p, sz * 2);
		CHECK(q != NULL, "realloc(aligned_alloc(%zu,%zu)) failed\n",
		    align, sz);
		if (q == NULL) {
			free(p);
			continue;
		}
		for (i = 0; i < sz; i++) {
			if (q[i] != 0x7e) {
				CHECK(0, "realloc of aligned_alloc storage "
				    "lost byte %zu (0x%02x)\n", i, q[i]);
				break;
			}
		}
		free(q);
	}

	/* Invalid alignment: not a power of two. */
	CHECK(aligned_alloc(3, 16) == NULL,
	    "aligned_alloc(3, 16) succeeded; 3 is not a power of two\n");
	CHECK(aligned_alloc(0, 16) == NULL,
	    "aligned_alloc(0, 16) succeeded\n");
}

/*
 * A realloc that must fail has to leave the original allocation live,
 * intact, and still recognized by free()/realloc().
 */
static void
test_failed_realloc_preserves_ownership(void)
{
	unsigned char *p = malloc(256);
	unsigned char *q;
	size_t i;

	CHECK(p != NULL, "malloc(256) failed\n");
	if (p == NULL)
		return;
	(void) memset(p, 0x6b, 256);

	/* Impossible request: must fail without disturbing p. */
	q = realloc(p, SIZE_MAX - 4096);
	if (q != NULL) {
		/* Astonishing, but not itself the defect: use and release. */
		free(q);
		return;
	}
	CHECK(errno == ENOMEM, "failed realloc set errno=%d, want ENOMEM\n",
	    errno);

	for (i = 0; i < 256; i++) {
		if (p[i] != 0x6b) {
			CHECK(0, "original lost byte %zu after failed "
			    "realloc\n", i);
			break;
		}
	}

	/* Still owned: a legitimate realloc must still work. */
	q = realloc(p, 512);
	CHECK(q != NULL,
	    "realloc(p, 512) failed after an earlier failed realloc; the "
	    "ownership record was consumed by the failed call\n");
	if (q == NULL) {
		free(p);
		return;
	}
	for (i = 0; i < 256; i++) {
		if (q[i] != 0x6b) {
			CHECK(0, "second realloc lost byte %zu\n", i);
			break;
		}
	}
	free(q);
}

/*
 * The interposer's bootstrap-pointer table is finite (512 entries) and used
 * to never reuse cleared slots, so a program doing many memalign/free cycles
 * exhausted it and later memalign results became untracked -- free() then
 * handed a libc pointer to umem's metadata decoder.  Requirement: sustained
 * aligned alloc/free churn must stay correct indefinitely.
 */
static void
test_aligned_churn(void)
{
	int round;

	for (round = 0; round < 4000; round++) {
		void *p = NULL;
		int rc = posix_memalign(&p, 64, 96);

		if (rc != 0 || p == NULL) {
			CHECK(0, "posix_memalign churn failed at round %d "
			    "(rc=%d)\n", round, rc);
			break;
		}
		CHECK(((uintptr_t)p % 64) == 0,
		    "churn round %d: misaligned %p\n", round, p);
		(void) memset(p, round & 0xff, 96);
		free(p);
	}
}

int
main(void)
{
	(void) printf("aligned-contracts: posix_memalign, aligned_alloc, "
	    "realloc ownership, churn\n");

	test_posix_memalign_contract();
	test_aligned_alloc_contract();
	test_failed_realloc_preserves_ownership();
	test_aligned_churn();

	(void) printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
	    failures, failures == 1 ? "" : "s");
	return (failures == 0 ? 0 : 1);
}

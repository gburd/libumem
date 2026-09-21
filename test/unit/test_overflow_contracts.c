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
 * P1.7(a,b) regression: checked arithmetic on size/offset paths.
 *
 * Standalone (not part of test_main) so it can be a TESTS entry without
 * changing the unified runner's test counts.
 *
 * DEFECTS REPRODUCED
 *
 *  1. umem_arena_alloc(): `new_offset = arena->offset + aligned_size` was
 *     unchecked.  After a 32-byte allocation, a request of SIZE_MAX-15
 *     rounded up to 2^64-16, wrapped to new_offset == 16, passed the
 *     `new_offset > capacity` test, returned base+32 and moved the bump
 *     pointer BACKWARD to 16 -- so the next allocation overlapped the
 *     previous one.  Asserted here directly: the impossible request must
 *     fail, and the arena's available count must not grow across it.
 *
 *  2. arena_page_round(): rounding SIZE_MAX up to a page yields 0, so
 *     umem_arena_create(SIZE_MAX) attempted a zero-length mmap.
 *
 *  3. bootstrap_malloc(): `size + sizeof(bootstrap_header_t)` was unchecked,
 *     so bootstrap_malloc(SIZE_MAX) mapped ~15 bytes and returned non-NULL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "umem.h"
#include "umem_arena.h"

/* Bootstrap allocator, exported from malloc.c for the interposer. */
extern void *bootstrap_malloc(size_t);
extern void bootstrap_free(void *);
extern int is_bootstrap_pointer(void *);

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

static void
test_arena_offset_overflow(void)
{
	umem_arena_t *a = umem_arena_create(65536, UMEM_DEFAULT);
	size_t avail_before, avail_after;
	unsigned char *p1, *p2;
	size_t i;
	/*
	 * Requests that individually round up past SIZE_MAX, or that wrap
	 * when added to a nonzero offset.
	 */
	const size_t evil[] = {
		SIZE_MAX,
		SIZE_MAX - 1,
		SIZE_MAX - 15,
		SIZE_MAX - 16,
		SIZE_MAX / 2 + 1,
		SIZE_MAX - 65536,
	};

	CHECK(a != NULL, "umem_arena_create(65536) returned NULL\n");
	if (a == NULL)
		return;

	/* Establish a nonzero offset: this is what made the wrap reachable. */
	p1 = umem_arena_alloc(a, 32);
	CHECK(p1 != NULL, "arena 32-byte allocation failed\n");
	if (p1 == NULL) {
		umem_arena_destroy(a);
		return;
	}
	(void) memset(p1, 0x5a, 32);
	avail_before = umem_arena_available(a);

	for (i = 0; i < sizeof (evil) / sizeof (evil[0]); i++) {
		void *bad = umem_arena_alloc(a, evil[i]);

		CHECK(bad == NULL,
		    "umem_arena_alloc(arena, %zu) returned %p; an allocation "
		    "larger than the arena must fail\n", evil[i], bad);
	}

	avail_after = umem_arena_available(a);
	CHECK(avail_after == avail_before,
	    "arena available changed across failed allocations: %zu -> %zu "
	    "(the bump pointer moved)\n", avail_before, avail_after);

	/*
	 * The decisive check: the next successful allocation must not alias
	 * the still-live first one.
	 */
	p2 = umem_arena_alloc(a, 32);
	CHECK(p2 != NULL, "arena allocation after rejected requests failed\n");
	if (p2 != NULL) {
		CHECK(p2 >= p1 + 32 || p2 + 32 <= p1,
		    "overlapping arena allocations: [%p,+32) and [%p,+32)\n",
		    (void *)p1, (void *)p2);
		(void) memset(p2, 0xa5, 32);
		for (i = 0; i < 32; i++)
			CHECK(p1[i] == 0x5a,
			    "first arena allocation clobbered at byte %zu "
			    "(0x%02x)\n", i, p1[i]);
	}

	umem_arena_destroy(a);
}

static void
test_arena_page_round_overflow(void)
{
	/*
	 * Capacities that round up to 0 (or to less than requested) must be
	 * rejected, not mapped.
	 */
	const size_t evil[] = { SIZE_MAX, SIZE_MAX - 4095, SIZE_MAX - 1 };
	size_t i;

	for (i = 0; i < sizeof (evil) / sizeof (evil[0]); i++) {
		umem_arena_t *a = umem_arena_create(evil[i], UMEM_DEFAULT);

		CHECK(a == NULL,
		    "umem_arena_create(%zu) succeeded (capacity=%zu); the "
		    "page rounding wrapped\n", evil[i],
		    a != NULL ? umem_arena_capacity(a) : (size_t)0);
		if (a != NULL) {
			/* If it "succeeded", it must at least be coherent. */
			CHECK(umem_arena_capacity(a) >= evil[i],
			    "arena capacity %zu < requested %zu\n",
			    umem_arena_capacity(a), evil[i]);
			umem_arena_destroy(a);
		}
	}
}

static void
test_bootstrap_header_overflow(void)
{
	const size_t evil[] = { SIZE_MAX, SIZE_MAX - 1, SIZE_MAX - 8,
		SIZE_MAX - 15, SIZE_MAX - 16 };
	size_t i;
	void *ok;

	for (i = 0; i < sizeof (evil) / sizeof (evil[0]); i++) {
		void *bad = bootstrap_malloc(evil[i]);

		CHECK(bad == NULL,
		    "bootstrap_malloc(%zu) returned %p; the header addition "
		    "wrapped and mapped an undersized region\n",
		    evil[i], bad);
		if (bad != NULL)
			bootstrap_free(bad);
	}

	/* Ordinary bootstrap allocation still works and is recognizable. */
	ok = bootstrap_malloc(128);
	CHECK(ok != NULL, "bootstrap_malloc(128) failed\n");
	if (ok != NULL) {
		CHECK(is_bootstrap_pointer(ok) != 0,
		    "bootstrap_malloc result not recognized as bootstrap\n");
		(void) memset(ok, 0x3c, 128);
		bootstrap_free(ok);
	}
}

int
main(void)
{
	(void) printf("overflow-contracts: arena offset, arena page round, "
	    "bootstrap header\n");

	test_arena_offset_overflow();
	test_arena_page_round_overflow();
	test_bootstrap_header_overflow();

	(void) printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
	    failures, failures == 1 ? "" : "s");
	return (failures == 0 ? 0 : 1);
}

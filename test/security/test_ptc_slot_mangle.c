/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * CDDL HEADER END
 */

/*
 * P5.13 regression: a per-thread-cache (PTC) bin slot must not be usable as
 * a chosen-address primitive.
 *
 * THE EXPOSURE (attacker position D: controls allocation patterns and buffer
 * contents, not the environment and not the code).
 *
 * umem_ptc_t.pool[] holds the addresses of freed objects the thread cached;
 * the next umem_alloc() of that size class hands slots[--count] out with no
 * check at all (umem.c, the inlined PTC fast path).  glibc's tcache has the
 * same structure and has safe-linked its entries since 2.32 (PROTECT_PTR:
 * ptr ^ (&slot >> 12)).  P5.12 moved umem_ptc_t out of the user size-class
 * slabs so an ordinary overrun no longer reaches pool[]; this closes the
 * other half: a write that DOES reach a slot (a stronger primitive, a stale
 * pointer into a recycled PTC, a wild write) must not yield an attacker-chosen
 * allocation.
 *
 * WHAT THIS TEST DOES.  Allocate a live buffer `live`, then allocate and
 * free a second buffer of the same class so it lands in the PTC bin.
 * Overwrite that bin's top slot with `live`'s address -- exactly what an
 * overwrite of the pool does -- and allocate again.  The allocator must NOT
 * return `live` (a double allocation, the exact primitive a heap exploit
 * wants).  Mangled, the stored word is live ^ cookie ^ (&slot >> 12), so
 * writing `live` demangles to garbage; the test only compares the returned
 * pointer, it never dereferences it.
 *
 * The test reads the bin through umem_ptc.h (thread_ptc, bins[], slots) to
 * place the overwrite; this is the same access test_ptc_adjacency uses.
 *
 * Build the library with -DUMEM_NO_LINK_MANGLE to get the pre-fix behaviour
 * in an otherwise identical binary; this test then FAILS (the allocator
 * returns `live`), which is what makes it a regression and not a tautology,
 * and is the control arm the plan requires.
 */

#include "umem_base.h"
#include "umem_ptc.h"
#include "umem_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define	SZ	64

int
main(void)
{
	umem_ptc_t *ptc;
	umem_ptc_bin_t *b;
	void *live, *victim, *got;
	void **slot;
	int bin;

	live = umem_alloc(SZ, UMEM_DEFAULT);
	victim = umem_alloc(SZ, UMEM_DEFAULT);
	if (live == NULL || victim == NULL) {
		printf("SKIP: umem_alloc failed\n");
		return (77);
	}
	ptc = thread_ptc;
	bin = umem_ptc_size_to_bin(SZ);
	if (ptc == NULL || bin < 0) {
		printf("SKIP: PTC disabled or %d B not PTC-eligible\n", SZ);
		return (77);
	}
	b = &ptc->bins[bin];

	/* Free victim into the bin; it must land at the top slot. */
	{
		uint16_t before = b->count;
		umem_free(victim, SZ);
		if (b->count != (uint16_t)(before + 1)) {
			printf("SKIP: free did not land in the PTC bin "
			    "(count %u -> %u)\n", before, b->count);
			return (77);
		}
	}
	slot = &b->slots[b->count - 1];

	/*
	 * The attack: overwrite the top slot with the live neighbour's
	 * address.  Unmangled, the next alloc returns `live` while it is
	 * still allocated.
	 */
	*slot = live;

	got = umem_alloc(SZ, UMEM_DEFAULT);
	printf("  live=%p  victim=%p  slot=%p  got=%p\n",
	    live, victim, (void *)slot, got);

	if (got == live) {
		printf("FAIL: umem_alloc returned the LIVE buffer %p "
		    "(double allocation via a PTC slot overwrite; "
		    "slots are unmangled)\n", live);
		return (1);
	}
	printf("ok: overwritten PTC slot did not yield the chosen address; "
	    "slot mangling defeated it\n");
	/*
	 * `got` is garbage (or, with the cookie, some unrelated address);
	 * do not free it.  The bin is now short one victim, which is a
	 * leak, not a corruption.  Exit without cleanup.
	 */
	return (0);
}

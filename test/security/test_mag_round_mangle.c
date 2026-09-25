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
 * P5.13b regression: a per-thread magazine round must not be usable as a
 * chosen-address primitive.
 *
 * THE EXPOSURE (attacker position D: controls allocation patterns and buffer
 * contents, not the environment and not the code).
 *
 * umem_magazine_t.mag_round[] holds the addresses of freed objects the
 * thread cached in the per-thread magazine layer behind the PTC bins; the
 * next umem_alloc() of that class hands mag_round[--rounds] out with no check
 * (umem.c, the inlined PTC magazine fast path).  P5.13 mangled the bin slots
 * (umem_ptc_t.pool[]); this closes the layer behind them.  A write that
 * reaches a round (a stronger primitive, a stale pointer into a recycled
 * magazine, a wild write) must not yield an attacker-chosen allocation.
 *
 * WHAT THIS TEST DOES.  Allocate a live buffer `live`, then free enough
 * buffers of the same class to fill the PTC bin AND spill into the loaded
 * per-thread magazine.  Overwrite the magazine's top round with `live`'s
 * address -- exactly what an overwrite of the magazine backing store does --
 * then drain the bin and pull from the magazine.  The allocator must NOT
 * return `live` (a double allocation, the exact primitive a heap exploit
 * wants).  Mangled, the stored word is live ^ cookie ^ (&round >> 12), so
 * writing `live` demangles to garbage; the test only compares the returned
 * pointer, it never dereferences it.
 *
 * The test reaches the magazine through umem_ptc.h (thread_ptc, mags[],
 * loaded, rounds) to place the overwrite; this is the same private access
 * test_ptc_adjacency / test_ptc_slot_mangle use.
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
/* Bin capacity + a couple of magazine rounds is enough to spill; be generous. */
#define	NFREE	1024

int
main(void)
{
	umem_ptc_t *ptc;
	umem_ptc_mag_t *mag;
	void *live, *got;
	void **round;
	void *bufs[NFREE];
	int bin, i, n;

	live = umem_alloc(SZ, UMEM_DEFAULT);
	if (live == NULL) {
		printf("SKIP: umem_alloc failed\n");
		return (77);
	}
	ptc = thread_ptc;
	bin = umem_ptc_size_to_bin(SZ);
	if (ptc == NULL || bin < 0) {
		printf("SKIP: PTC disabled or %d B not PTC-eligible\n", SZ);
		return (77);
	}
	mag = &ptc->mags[bin];

	/*
	 * Allocate then free NFREE buffers of the class.  The bin fills first
	 * (ptc_bin_capacity), then further frees spill into the loaded
	 * per-thread magazine, populating mag->loaded->mag_round[].
	 */
	for (i = 0; i < NFREE; i++) {
		bufs[i] = umem_alloc(SZ, UMEM_DEFAULT);
		if (bufs[i] == NULL) {
			printf("SKIP: umem_alloc failed at %d\n", i);
			return (77);
		}
	}
	for (i = 0; i < NFREE; i++)
		umem_free(bufs[i], SZ);

	if (mag->loaded == NULL || mag->rounds <= 0) {
		printf("SKIP: no magazine round populated "
		    "(loaded=%p rounds=%d)\n",
		    (void *)mag->loaded, mag->rounds);
		return (77);
	}

	/*
	 * The attack: overwrite the magazine's top round with the live
	 * neighbour's address.  Unmangled, an alloc that reaches this round
	 * returns `live` while it is still allocated.
	 */
	round = &mag->loaded->mag_round[mag->rounds - 1];
	*round = live;

	/*
	 * Drain the class: the bin is popped first (LIFO), then the magazine.
	 * Any allocation that returns the overwritten round returns `live`
	 * unless the round is mangled.  Bound the loop so a leak cannot hang.
	 */
	for (n = 0; n < NFREE; n++) {
		got = umem_alloc(SZ, UMEM_DEFAULT);
		if (got == NULL)
			break;
		if (got == live) {
			printf("FAIL: umem_alloc returned the LIVE buffer %p "
			    "after %d pops (double allocation via a magazine "
			    "round overwrite; rounds are unmangled)\n",
			    live, n);
			return (1);
		}
		/* Stop once we have drained well past the overwritten round. */
		if (n > 0 && got == NULL)
			break;
	}

	printf("ok: overwritten magazine round did not yield the chosen "
	    "address in %d pops; round mangling defeated it\n", n);
	/*
	 * The overwritten round demangled to garbage and was handed out as
	 * some allocation; do not chase or free it.  Exit without cleanup.
	 */
	return (0);
}

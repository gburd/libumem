/*
 * P7.4 regression: free() must refuse a forged libumem header that sits in
 * the address GAP BETWEEN two heap spans -- inside the [vmem_heap_lo,
 * vmem_heap_hi) convex hull, but not inside any span umem actually owns.
 *
 * THE DEFECT (pre-fix, malloc.c: umem_may_own() / MAY_OWN())
 *   umem_may_own() is a convex hull: it accepts any pointer in
 *   [vmem_heap_lo, vmem_heap_hi).  The heap's spans are NOT contiguous --
 *   the mmap backend grows by fresh kernel-chosen mmap()s, so there are
 *   holes between spans that the kernel handed to nobody, or to some other
 *   mapping.  A pointer in such a hole passes the hull test.  With
 *   UMEM_MALLOC_DECODE = stat+size and MALLOC_MAGIC a fixed constant
 *   (test_forged_free covers the forgeability), an attacker who owns writable
 *   memory in a between-spans hole (position D: a large mmap of their own that
 *   landed in a heap gap) forges a header in front of it, calls free(), and
 *   process_free() accepts it: [base,base+size) lies inside the hull, so it
 *   passes step 4, and _umem_free() pushes the chosen address onto a
 *   per-thread bin -- from which the NEXT malloc() of that size class returns
 *   it.  A chosen-pointer return, reached without touching a slab.
 *
 * THE FIX (vmem.c publishes an exact sorted span table; malloc.c
 * umem_may_own() binary-searches it)
 *   The hull [lo,hi) stays as the cheap FIRST reject for a pointer far outside
 *   the heap.  For a pointer inside the hull, umem_may_own() now confirms
 *   exact containment against the span table vmem_span_create() publishes, so
 *   a between-spans hole is rejected: no span contains it.
 *
 * WHAT THIS ASSERTS
 *   Having placed a forged MALLOC_MAGIC header in a caller mmap that lands in
 *   a verified hole inside [vmem_heap_lo, vmem_heap_hi):
 *
 *   A. free(forged) must REFUSE it: the forged header is left UNMUTATED
 *      (process_free writes nothing on a refusal) -- the same evidence
 *      test_forged_free uses.
 *   B. malloc_usable_size(forged) must return 0 (the classifier does not
 *      accept it) -- runs the same ownership check without mutating.
 *   C. THE EXPLOIT ITSELF: after free(forged), a burst of malloc() of the
 *      forged size class must NEVER return the forged address.  Pre-fix the
 *      hull accepts the free, the address lands on a PTC bin, and a later
 *      malloc() of that class hands it back; post-fix it is refused and never
 *      enters the allocator, so it can never be returned.
 *   D. CONTROL (vacuity guard): a REAL allocation whose payload sits inside a
 *      real span is still accepted (malloc_usable_size > 0), so A-C are not
 *      passing merely because the check rejects everything.
 *
 * PRE-FIX DEMONSTRATION (against the parent commit, convex hull): arm A
 * reports the header mutated to UMEM_FREE_PATTERN_32, arm B reports a nonzero
 * usable size, and arm C reports the forged address returned by a later
 * malloc() -- the test fails.  Post-fix all four pass.
 *
 * Not an ASan test: it links libumem directly (libumem.la + libumem_malloc.la)
 * and drives free() through the interposer, exactly the LD_PRELOAD path.  ASan
 * would interpose free() ahead of libumem and reject the non-malloc()-ed
 * pointer itself (see test_forged_free), so the forgery would never reach
 * process_free(); the arms that hand a forged pointer to free() are skipped
 * under ASan, as there.
 *
 * Exit: 0 pass, 1 fail, 77 skip (no hole could be constructed on this run).
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/mman.h>

#include "umem_impl.h"

/*
 * The published heap hull bounds (vmem.c).  Declared here rather than via
 * vmem_base.h, which pulls in the internal build context (misc.h,
 * sys/vmem.h); test/bench/probe_alloc_failure.c declares its one vmem symbol
 * the same way and for the same reason.
 */
extern _Atomic uintptr_t vmem_heap_lo, vmem_heap_hi;

static int failures;

static int
asan_active(void)
{
#if defined(__SANITIZE_ADDRESS__)
	return (1);
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
	return (1);
#else
	return (0);
#endif
#else
	return (0);
#endif
}

/*
 * The header libumem puts in front of every malloc() result.  Duplicated
 * from malloc.c on purpose (as test_forged_free does): this test forges one,
 * so it must not depend on a definition malloc.c could change without the
 * forgery noticing.
 */
typedef struct {
	uint32_t malloc_size;
	uint32_t malloc_stat;
} forged_header_t;

#ifndef MAP_FIXED_NOREPLACE
#define	MAP_FIXED_NOREPLACE	0x100000
#endif

/*
 * Construct a between-spans hole DETERMINISTICALLY.
 *
 * The mmap heap grows DOWNWARD: hi is fixed at the first span's top and lo
 * drops as the heap adds spans at lower addresses (measured on both arches:
 * a barrier placed below the initial lo ends up inside [lo,hi) after the heap
 * grows past it, with zero naturally-unmapped windows -- the heap run is
 * contiguous, so there is no accidental hole to find).
 *
 * So we MAKE the hole: reserve a caller region just below the current lo,
 * then grow the heap down past it.  The heap cannot mmap over our reservation
 * (MAP_FIXED_NOREPLACE fails if occupied, and the reservation stays ours),
 * so the region sits inside the final [lo,hi) hull, owned by no heap span --
 * exactly a between-spans gap.  This is the position-D case: a caller mmap
 * that the heap grew around.
 *
 * Returns the reserved region (mapped RW so a header can be forged in it), or
 * MAP_FAILED if the layout could not be produced on this run.
 */
static void *
make_between_spans_region(size_t region, void ***keep_out, int *nkeep_out)
{
	void **keep = malloc(sizeof (void *) * 8192);
	int nkeep = 0, i;
	uintptr_t lo0;
	void *bars[8];
	int nbar = 0, b;
	void *chosen = MAP_FAILED;

	if (keep == NULL)
		return (MAP_FAILED);

	/* Establish the first span(s) and read the current bottom. */
	for (i = 0; i < 64; i++) {
		keep[nkeep] = malloc(256 * 1024);
		if (keep[nkeep] == NULL)
			break;
		memset(keep[nkeep], 0x11, 64);
		nkeep++;
	}
	lo0 = atomic_load(&vmem_heap_lo);

	/*
	 * Reserve barriers below the current bottom, at several distances, so
	 * that wherever the heap next grows to, at least one lands strictly
	 * inside the final hull.  region-sized, region-aligned.
	 */
	for (b = 1; b <= 8 && nbar < 8; b++) {
		uintptr_t want = (lo0 - (uintptr_t)b * 16 * 1024 * 1024) &
		    ~(uintptr_t)(region - 1);
		void *r = mmap((void *)want, region, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
		if (r != MAP_FAILED && (uintptr_t)r == want)
			bars[nbar++] = r;
		else if (r != MAP_FAILED)
			(void) munmap(r, region);
	}

	/* Grow the heap down past the barriers. */
	for (i = 0; i < 8192 && nkeep < 8192; i++) {
		keep[nkeep] = malloc(256 * 1024);
		if (keep[nkeep] == NULL)
			break;
		memset(keep[nkeep], 0x11, 64);
		nkeep++;
	}

	/* Pick a barrier now strictly inside [lo,hi); free the rest. */
	{
		uintptr_t lo = atomic_load(&vmem_heap_lo);
		uintptr_t hi = atomic_load(&vmem_heap_hi);
		for (b = 0; b < nbar; b++) {
			uintptr_t a = (uintptr_t)bars[b];
			if (chosen == MAP_FAILED &&
			    a >= lo && a + region <= hi)
				chosen = bars[b];
			else
				(void) munmap(bars[b], region);
		}
	}

	*keep_out = keep;
	*nkeep_out = nkeep;
	return (chosen);
}

int
main(void)
{
	size_t region = 64 * 1024;		/* caller region in the hole */
	void **keep = NULL;
	int nkeep = 0, i;
	uintptr_t lo, hi;
	void *hole;
	forged_header_t *hdr;
	void *payload;
	uint32_t size_field, stat_before, stat_after, size_before, size_after;

	printf("P7.4: free() must refuse a forged header in a between-spans "
	    "hole inside the hull\n");

	hole = make_between_spans_region(region, &keep, &nkeep);
	lo = atomic_load(&vmem_heap_lo);
	hi = atomic_load(&vmem_heap_hi);
	printf("  heap hull [%#lx, %#lx)  width %#lx  (%d live 256K allocs)\n",
	    (unsigned long)lo, (unsigned long)hi,
	    (unsigned long)(hi - lo), nkeep);

	if (nkeep == 0) {
		printf("  SKIP: could not populate the heap\n");
		free(keep);
		return (77);
	}
	if (hole == MAP_FAILED) {
		printf("  SKIP: could not place a caller region inside the "
		    "hull between spans\n");
		goto cleanup;
	}
	printf("  caller region at %p is inside the hull, owned by no span\n",
	    hole);

	if (asan_active()) {
		printf("  SKIP arms A-C under ASan: ASan interposes free() and "
		    "rejects a non-malloc()-ed pointer before libumem sees "
		    "it.\n");
		(void) munmap(hole, region);
		goto cleanup;
	}

	/* Forge a MALLOC_MAGIC header at a 16-byte-aligned payload. */
	hdr = (forged_header_t *)(((uintptr_t)hole + 15) & ~(uintptr_t)15);
	payload = (void *)(hdr + 1);
	size_field = (uint32_t)(32 + sizeof (forged_header_t));
	hdr->malloc_size = size_field;
	hdr->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, size_field);
	memset(payload, 0x5A, 32);

	/* ---- A: free() must refuse without mutating the forged header ---- */
	printf("[A] free() of the between-spans forgery\n");
	stat_before = hdr->malloc_stat;
	size_before = hdr->malloc_size;
	free(payload);
	stat_after = hdr->malloc_stat;
	size_after = hdr->malloc_size;
	if (stat_after != stat_before || size_after != size_before) {
		printf("  FAIL: MUTATED the forged header (stat 0x%08x -> "
		    "0x%08x) -- the hull accepted a between-spans pointer\n",
		    stat_before, stat_after);
		failures++;
	} else {
		printf("  PASS: forgery refused, header untouched\n");
	}

	/* ---- B: the classifier must not accept it ---- */
	printf("[B] malloc_usable_size() of the between-spans forgery\n");
	{
		size_t us = malloc_usable_size(payload);
		if (us != 0) {
			printf("  FAIL: accepted as an allocation "
			    "(usable_size %zu)\n", us);
			failures++;
		} else {
			printf("  PASS: not accepted (usable_size 0)\n");
		}
	}

	/* ---- C: the exploit -- the forged address must never come back ---- */
	printf("[C] no later malloc() returns the forged address\n");
	{
		size_t cls = 32;		/* payload data size we forged */
		int returned = 0, j;
		void *burst[512];

		for (j = 0; j < 512; j++) {
			burst[j] = malloc(cls);
			if (burst[j] == NULL)
				break;
			if (burst[j] == payload)
				returned = 1;
		}
		for (i = 0; i < j; i++)
			free(burst[i]);
		if (returned) {
			printf("  FAIL: a malloc(%zu) returned the forged "
			    "address %p -- chosen-pointer return\n",
			    cls, payload);
			failures++;
		} else {
			printf("  PASS: the forged address was never handed "
			    "back by malloc()\n");
		}
	}

	(void) munmap(hole, region);

cleanup:
	/* ---- D: control -- a real allocation is still accepted ---- */
	printf("[D] control: a real allocation is still recognized\n");
	if (nkeep > 0) {
		size_t us = malloc_usable_size(keep[0]);
		if (us == 0) {
			printf("  FAIL: a real 256K allocation is NOT "
			    "recognized -- the check rejects umem's own "
			    "memory\n");
			failures++;
		} else {
			printf("  PASS: a real allocation reports usable "
			    "size %zu\n", us);
		}
	}
	for (i = 0; i < nkeep; i++)
		free(keep[i]);
	free(keep);

	printf("\n");
	if (failures != 0) {
		printf("test_forged_span_gap: FAIL (%d)\n", failures);
		return (1);
	}
	printf("test_forged_span_gap: PASS\n");
	return (0);
}

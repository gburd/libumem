/*
 * test_slab_member.c -- does UMEM_SLAB_MEMBER describe a slab's buffers?
 *
 * UMEM_SLAB_MEMBER(sp, buf) (umem_impl.h) tests
 *     (buf - sp->slab_base) < sp->slab_cache->cache_slabsize
 * but umem_slab_create() sets slab_base = slab + colour and lays out
 * slab_chunks chunks of cache_chunksize from there.  So the test covers
 *     [slab_base, slab_base + cache_slabsize)
 * while the slab's buffers occupy
 *     [slab_base, slab_base + slab_chunks * cache_chunksize).
 * The same over-wide bound in umemctl's whatis was T6 (fixed 0e395ef).
 *
 * This test measures, for a real non-hash cache, three address classes
 * against the macro, and reports which it accepts:
 *   A. every chunk start                  -- must be accepted
 *   B. an address strictly inside a chunk -- is not a buffer start
 *   C. the first byte past the last chunk -- is not in any buffer
 * It exits 0 only if the macro accepts A and rejects C.  B is reported but
 * not required to be rejected: whether a membership test should also demand
 * a chunk start is a caller contract, recorded separately.
 *
 * _UMEM_INTERNAL build: reads umem_slab_t through umem_impl.h, like
 * test_slab_floor.c.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <umem.h>
#include "umem_base.h"
#include "umem_impl.h"

int
main(void)
{
	/*
	 * 200 B objects: non-hash (below the UMEM_VOID_FRACTION split), one
	 * page per slab, several chunks, a non-zero colour range.
	 */
	umem_cache_t *cp = umem_cache_create("t_slab_member", 200, 0,
	    NULL, NULL, NULL, NULL, NULL, UMC_NODEBUG);
	void *bufs[64];
	int i, n = 0, badA = 0, acceptB = 0, acceptC = 0, checked = 0;

	if (cp == NULL) {
		fprintf(stderr, "harness: umem_cache_create failed\n");
		return (99);
	}
	if (cp->cache_flags & UMF_HASH) {
		fprintf(stderr, "harness: expected a non-hash cache\n");
		return (99);
	}
	for (i = 0; i < 64; i++)
		if ((bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT)) != NULL)
			n++;

	for (i = 0; i < n; i++) {
		umem_slab_t *sp = UMEM_SLAB(cp, bufs[i]);
		uintptr_t base = (uintptr_t)sp->slab_base;
		size_t chunk = cp->cache_chunksize;
		uintptr_t end = base + (uintptr_t)sp->slab_chunks * chunk;
		size_t k;

		if (sp->slab_cache != cp)
			continue;
		checked++;
		for (k = 0; k < sp->slab_chunks; k++)
			if (!UMEM_SLAB_MEMBER(sp, (void *)(base + k * chunk)))
				badA++;
		if (UMEM_SLAB_MEMBER(sp, (void *)(base + chunk / 2)))
			acceptB++;
		if (end < base + cp->cache_slabsize &&
		    UMEM_SLAB_MEMBER(sp, (void *)end))
			acceptC++;
	}

	printf("slabs checked=%d chunksize=%zu slabsize=%zu\n", checked,
	    cp->cache_chunksize, cp->cache_slabsize);
	printf("A chunk starts rejected: %d (must be 0)\n", badA);
	printf("B mid-chunk accepted:    %d (informational)\n", acceptB);
	printf("C past-last-chunk accepted: %d (must be 0)\n", acceptC);

	for (i = 0; i < n; i++)
		umem_cache_free(cp, bufs[i]);
	umem_cache_destroy(cp);

	if (checked == 0) {
		fprintf(stderr, "harness: no slab examined\n");
		return (99);
	}
	if (badA != 0 || acceptC != 0) {
		printf("FAIL: UMEM_SLAB_MEMBER's range does not match the "
		    "slab's buffers\n");
		return (1);
	}
	printf("PASS\n");
	return (0);
}

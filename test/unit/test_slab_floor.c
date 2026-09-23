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
 * Slab-sizing regression: the UMEM_MIN_SLAB_OBJECTS floor.
 *
 * The ~5 GB Linux heap ceiling came from umem_cache_create()'s best-fit loop
 * picking ONE 4 KiB object per 4 KiB slab under a page-sized heap quantum,
 * where the same loop under Solaris's 64 KiB quantum picks sixteen.  The fix is
 * a floor of UMEM_MIN_SLAB_OBJECTS (16) objects per hashed-cache slab, capped
 * at UMEM_MIN_SLAB_CEILING (64 KiB).
 *
 * That floor is deliberately not gated on the platform, on the claim that it is
 * arithmetically a no-op wherever the quantum is already 64 KiB.  This test
 * makes the claim enforceable rather than a comment: it creates caches against
 * caller-supplied arenas with a 4 KiB quantum (the Linux case) and a 64 KiB
 * quantum (the illumos case, simulated on Linux) and checks the resulting slab
 * sizes.
 *
 *   - 4 KiB quantum, 4 KiB chunk:  slab must hold >= 16 objects (the fix).
 *   - 64 KiB quantum, every chunk: slab must equal what unfloored best-fit
 *     gives, i.e. the floor changed nothing (the portability claim).
 *   - any quantum, chunk > 64 KiB/16: the floor must NOT balloon the slab
 *     past the ceiling (large objects stay one-per-slab).
 *
 * The 64 KiB arm is what protects illumos from a Linux-only change being
 * tested only on Linux.  If someone later widens the floor and it starts
 * changing 64 KiB-quantum slab sizes, this fails on x86_64 CI before it ever
 * reaches an illumos box.
 */

#include "umem_base.h"
#include "umem_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		printf("FAIL: " __VA_ARGS__);				\
		printf("\n");						\
		fails++;						\
	}								\
} while (0)

/* Unfloored best-fit, transcribed from umem_cache_create(). */
static size_t
bestfit_unfloored(size_t chunk, size_t quantum)
{
	size_t chunks, best = 0, minwaste = (size_t)-1;

	for (chunks = 1; chunks <= UMEM_VOID_FRACTION; chunks++) {
		size_t slab = P2ROUNDUP(chunk * chunks, quantum);
		size_t c2 = slab / chunk;
		size_t waste = (slab % chunk) / c2;

		if (waste < minwaste) {
			minwaste = waste;
			best = slab;
		}
	}
	return (best);
}

static vmem_t *
arena_with_quantum(const char *name, size_t quantum, void **basep)
{
	size_t span = 64 * quantum;	/* room for a few slabs */
	void *base;

	if (posix_memalign(&base, quantum, span) != 0)
		return (NULL);
	*basep = base;
	return (vmem_create(name, base, span, quantum,
	    NULL, NULL, NULL, 0, VM_NOSLEEP));
}

/*
 * Force the HASH branch: UMC_NOTOUCH takes the cache out of the "small object,
 * embedded bufctl" branch regardless of chunk size, so we exercise the best-fit
 * loop that the floor modifies.
 */
static size_t
slabsize_for(vmem_t *vmp, size_t chunk, const char *tag)
{
	char name[64];
	umem_cache_t *cp;
	size_t sz;

	(void) snprintf(name, sizeof (name), "floor_%s_%zu", tag, chunk);
	cp = umem_cache_create(name, chunk, 0, NULL, NULL, NULL, NULL, vmp,
	    UMC_NOTOUCH | UMC_NOMAGAZINE);
	if (cp == NULL) {
		printf("FAIL: umem_cache_create(%s) failed\n", name);
		fails++;
		return (0);
	}
	sz = cp->cache_slabsize;
	umem_cache_destroy(cp);
	return (sz);
}

int
main(void)
{
	static const size_t chunks[] = {
		64, 256, 1024, 2048, 4096, 8192, 16384, 65536
	};
	void *b4 = NULL, *b64 = NULL;
	vmem_t *q4, *q64;
	size_t i;
	void *warm;

	/*
	 * Initialise the library before touching vmem directly: vmem_create()
	 * on a fresh process reaches vmem_populate() with vmem_seg_arena still
	 * NULL and faults.  The public entry points initialise on first use;
	 * a caller of vmem_create() is expected to have allocated something.
	 */
	warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, 64);

	q4 = arena_with_quantum("floor_q4k", 4096, &b4);
	q64 = arena_with_quantum("floor_q64k", 65536, &b64);
	if (q4 == NULL || q64 == NULL) {
		printf("SKIP: could not create test arenas\n");
		return (77);
	}

	printf("%-8s %-22s %-22s\n", "chunk", "4K-quantum slab/obj",
	    "64K-quantum slab/obj (unfloored)");
	for (i = 0; i < sizeof (chunks) / sizeof (chunks[0]); i++) {
		size_t ch = chunks[i];
		size_t s4 = slabsize_for(q4, ch, "q4");
		size_t s64 = slabsize_for(q64, ch, "q64");
		size_t u64 = bestfit_unfloored(ch, 65536);

		if (s4 == 0 || s64 == 0)
			continue;

		printf("%-8zu %6zu / %-13zu %6zu / %-4zu (%zu)\n", ch,
		    s4, s4 / ch, s64, s64 / ch, u64);

		/*
		 * THE FIX (Linux case): a 4 KiB chunk on a 4 KiB quantum used to
		 * get one object per slab.  Now at least 16, for every chunk the
		 * ceiling allows.
		 */
		if (ch * UMEM_MIN_SLAB_OBJECTS <= UMEM_MIN_SLAB_CEILING) {
			CHECK(s4 / ch >= UMEM_MIN_SLAB_OBJECTS,
			    "4K quantum, chunk %zu: only %zu objects per slab "
			    "(floor of %d not applied)", ch, s4 / ch,
			    UMEM_MIN_SLAB_OBJECTS);
		}

		/*
		 * THE PORTABILITY CLAIM (illumos case): with a 64 KiB quantum
		 * the floor must be a no-op -- the slab size must be exactly
		 * what unfloored best-fit chooses.
		 */
		CHECK(s64 == u64,
		    "64K quantum, chunk %zu: slab %zu differs from unfloored "
		    "best-fit %zu -- the floor is NOT a no-op on illumos",
		    ch, s64, u64);

		/*
		 * THE CEILING: the floor must never push a slab past 64 KiB.
		 * Large objects stay one-per-slab.
		 */
		CHECK(s4 <= UMEM_MIN_SLAB_CEILING || s4 == bestfit_unfloored(ch, 4096),
		    "4K quantum, chunk %zu: slab %zu exceeds the %d ceiling "
		    "and is not best-fit's own choice", ch, s4,
		    UMEM_MIN_SLAB_CEILING);
	}

	vmem_destroy(q4);
	vmem_destroy(q64);
	free(b4);
	free(b64);

	if (fails) {
		printf("RESULT: FAIL (%d)\n", fails);
		return (1);
	}
	printf("RESULT: PASS (floor applies at 4K quantum, is a no-op at 64K)\n");
	return (0);
}

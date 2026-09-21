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
 * P1.5 regression: a slab whose pages were reclaimed must still be usable.
 *
 * Three cases, selected by argv[1].  hash_guards and big_quantum run the
 * same shape: fill a slab, free it all, drive the reclaim state machine to
 * CLEAN, then allocate through the whole slab again and check the result.
 * A fresh process is required because UMEM_DEBUG / UMEM_OPTIONS are read
 * once at init; the parent side lives in test/unit/test_reclaim_reuse.c.
 *
 *   hash_guards  (P1.5a)
 *     UMEM_DEBUG=guards + a 4072-byte object at align 8 with UMC_NOMAGAZINE:
 *     the 24-byte buftag pushes chunksize to 4096, so the cache is UMF_HASH
 *     with a 4096-byte (one page) slab and color 0.  For a hash cache the
 *     madvise range was the WHOLE slab, so MADV_DONTNEED zero-filled the
 *     in-buffer buftag (bt_bufctl / bt_bxstat / bt_redzone) and the
 *     UMEM_FREE_PATTERN -- both written only by umem_slab_create() and
 *     never rebuilt on reactivation.  The next legitimate allocation then
 *     failed umem_cache_alloc_debug()'s first check,
 *     bt_bxstat != (bufctl ^ UMEM_BUFTAG_FREE), and reported heap
 *     corruption on correct usage.
 *     PRE-FIX: "umem allocator: boundary tag corrupted" + abort.
 *
 *   big_quantum  (P1.5b)
 *     A caller-supplied arena with quantum 16 KiB (> PAGESIZE) makes
 *     cache_slabsize 16 KiB for a non-hash cache, i.e. a multi-page slab
 *     whose embedded bufctls (and their bc_next freelist links) live in the
 *     interior pages that madvise did discard -- only the metadata page at
 *     the end was excluded.  umem_slab_alloc() then followed bc_next into
 *     zeroed memory, truncating the freelist.
 *     PRE-FIX: the second pass allocates fewer objects than the first, or
 *     returns a duplicate/zero pointer.
 *
 *   race  (P1.5c)
 *     Reclaim passes concurrent with allocation/free on one cache, so the
 *     unsynchronized slab_state publication has a real racing reader.
 *
 * Exit: 0 pass, 1 fail, 2 usage, 77 skip (preconditions not met).
 */

#include "umem_base.h"		/* umem_cache_t, tunables, PAGESIZE */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Count this cache's slabs and how many have reached SLAB_CLEAN.
 *
 * Reads slab_state under cache_lock, the lock that protects it.
 */
static void
slab_census(umem_cache_t *cp, int *nslabs, int *nclean)
{
	umem_slab_t *nullsp = &cp->cache_nullslab;
	umem_slab_t *sp;

	*nslabs = 0;
	*nclean = 0;

	(void) mutex_lock(&cp->cache_lock);
	for (sp = nullsp->slab_next; sp != nullsp; sp = sp->slab_next) {
		(*nslabs)++;
		if (sp->slab_state == SLAB_CLEAN)
			(*nclean)++;
	}
	(void) mutex_unlock(&cp->cache_lock);
}

/*
 * Drive the idle slab from DIRTY to CLEAN and stop exactly there.
 *
 * Each pass through umem_cache_update() -- the same entry point the update
 * thread uses -- adds umem_reap_interval to every idle slab's
 * slab_idle_time and acts once it reaches umem_reclaim_delay.  Calling it
 * directly skips umem_reap()'s rate limiter, so the test need not sleep out
 * umem_reap_interval.
 *
 * Stopping at the first CLEAN slab matters: one further pass past
 * umem_reclaim_delay * 2 DESTROYS the slab, and a test that then allocates
 * from a brand-new slab proves nothing about reuse -- it would pass even
 * with the bug.  Returns 1 if a slab reached CLEAN and still exists.
 */
static int
reclaim_until_clean(umem_cache_t *cp)
{
	int i, nslabs, nclean;

	for (i = 0; i < 64; i++) {
		umem_cache_applyall(umem_cache_update);
		slab_census(cp, &nslabs, &nclean);
		if (nclean > 0)
			return (1);
		if (nslabs == 0) {
			printf("FAIL: slab destroyed before reaching CLEAN\n");
			return (0);
		}
	}
	printf("FAIL: no slab reached SLAB_CLEAN in 64 update passes "
	    "(reclaim_delay=%u reap_interval=%u)\n",
	    umem_reclaim_delay, umem_reap_interval);
	return (0);
}

/*
 * Allocate until the cache has to create a second slab, so the run stays
 * within one slab: stop as soon as an address falls outside the first slab's
 * span.  Returns the number of buffers held in bufs[].
 */
static int
fill_one_slab(umem_cache_t *cp, void **bufs, int max)
{
	uintptr_t slab_base = 0;
	int n = 0;

	while (n < max) {
		void *buf = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (buf == NULL)
			break;
		if (n == 0)
			slab_base = P2ALIGN((uintptr_t)buf, cp->cache_slabsize);
		else if (P2ALIGN((uintptr_t)buf, cp->cache_slabsize) !=
		    slab_base) {
			umem_cache_free(cp, buf);
			break;
		}
		bufs[n++] = buf;
	}
	return (n);
}

static int
run_hash_guards(void)
{
	umem_cache_t *cp;
	void *bufs[64];
	int first, second, i;

	if (!(umem_flags & UMF_BUFTAG)) {
		printf("SKIP: UMF_BUFTAG not set (UMEM_DEBUG=guards missing)\n");
		return (77);
	}
	if (pagesize != 4096) {
		printf("SKIP: pagesize %lu != 4096\n", (unsigned long)pagesize);
		return (77);
	}

	cp = umem_cache_create("reclaim_hash_guards", 4072, 8,
	    NULL, NULL, NULL, NULL, NULL, UMC_NOMAGAZINE);
	if (cp == NULL) {
		printf("FAIL: umem_cache_create failed\n");
		return (1);
	}
	if (!(cp->cache_flags & UMF_HASH) ||
	    !(cp->cache_flags & UMF_BUFTAG)) {
		printf("SKIP: cache_flags=0x%x, wanted UMF_HASH|UMF_BUFTAG\n",
		    cp->cache_flags);
		umem_cache_destroy(cp);
		return (77);
	}

	first = fill_one_slab(cp, bufs, 64);
	if (first < 1) {
		printf("FAIL: no allocation from a fresh cache\n");
		umem_cache_destroy(cp);
		return (1);
	}
	for (i = 0; i < first; i++)
		umem_cache_free(cp, bufs[i]);

	if (!reclaim_until_clean(cp)) {
		umem_cache_destroy(cp);
		return (1);
	}

	/*
	 * The abort, if it happens, happens inside here: allocation validates
	 * the buftag and the free pattern before any constructor runs.
	 */
	second = fill_one_slab(cp, bufs, 64);
	for (i = 0; i < second; i++)
		umem_cache_free(cp, bufs[i]);

	umem_cache_destroy(cp);

	if (second != first) {
		printf("FAIL: reused slab yielded %d buffers, first pass %d\n",
		    second, first);
		return (1);
	}
	printf("ok: hash+guards slab reused, %d buffers both passes\n", first);
	return (0);
}

static int
run_big_quantum(void)
{
	size_t quantum = 16384;
	size_t span = 4 * 1024 * 1024;
	void *base;
	vmem_t *vmp;
	umem_cache_t *cp;
	void *bufs[512];
	int first, second, i, j;

	if (pagesize >= quantum) {
		printf("SKIP: pagesize %lu >= quantum %lu\n",
		    (unsigned long)pagesize, (unsigned long)quantum);
		return (77);
	}

	if (posix_memalign(&base, quantum, span) != 0) {
		printf("FAIL: posix_memalign failed\n");
		return (1);
	}
	vmp = vmem_create("reclaim_bigq", base, span, quantum,
	    NULL, NULL, NULL, 0, VM_NOSLEEP);
	if (vmp == NULL) {
		printf("FAIL: vmem_create failed\n");
		free(base);
		return (1);
	}

	/* Small object in a 16 KiB-quantum arena: non-hash, multi-page slab. */
	cp = umem_cache_create("reclaim_bigq_cache", 64, 0,
	    NULL, NULL, NULL, NULL, vmp, UMC_NOMAGAZINE);
	if (cp == NULL) {
		printf("FAIL: umem_cache_create failed\n");
		vmem_destroy(vmp);
		free(base);
		return (1);
	}
	if ((cp->cache_flags & UMF_HASH) || cp->cache_slabsize <= pagesize) {
		printf("SKIP: cache_flags=0x%x slabsize=%lu, wanted "
		    "multi-page non-hash\n", cp->cache_flags,
		    (unsigned long)cp->cache_slabsize);
		umem_cache_destroy(cp);
		vmem_destroy(vmp);
		free(base);
		return (77);
	}

	first = fill_one_slab(cp, bufs, 512);
	if (first < 2) {
		printf("FAIL: only %d buffers from a fresh slab\n", first);
		umem_cache_destroy(cp);
		vmem_destroy(vmp);
		free(base);
		return (1);
	}
	for (i = 0; i < first; i++)
		umem_cache_free(cp, bufs[i]);

	if (!reclaim_until_clean(cp)) {
		umem_cache_destroy(cp);
		vmem_destroy(vmp);
		free(base);
		return (1);
	}

	/*
	 * A truncated freelist shows up as a short second pass; a corrupted
	 * one can also hand out the same address twice, so check both.
	 */
	second = fill_one_slab(cp, bufs, 512);
	for (i = 0; i < second; i++) {
		if (bufs[i] == NULL) {
			printf("FAIL: NULL buffer at index %d\n", i);
			second = -1;
			break;
		}
		for (j = 0; j < i; j++) {
			if (bufs[i] == bufs[j]) {
				printf("FAIL: duplicate buffer %p at %d "
				    "and %d\n", bufs[i], j, i);
				second = -1;
				break;
			}
		}
		if (second < 0)
			break;
	}
	for (i = 0; i < second; i++)
		umem_cache_free(cp, bufs[i]);

	umem_cache_destroy(cp);
	vmem_destroy(vmp);
	free(base);

	if (second != first) {
		printf("FAIL: reused slab yielded %d buffers, first pass %d "
		    "(freelist truncated)\n", second, first);
		return (1);
	}
	printf("ok: multi-page non-hash slab reused, %d buffers both passes "
	    "(slabsize %lu, pagesize %lu)\n", first,
	    (unsigned long)quantum, (unsigned long)pagesize);
	return (0);
}

/*
 * P1.5c: run the reclaim path concurrently with allocation and free on the
 * same cache, so slab_state has a real reader (umem_slab_alloc /
 * umem_slab_free under cache_lock) racing a real writer (the reclaim pass).
 *
 * Two kinds of check:
 *  - TSAN, when the library is built with --enable-tsan, reports the
 *    unsynchronized slab_state store directly.
 *  - Without TSAN this still exercises the state machine hard, and the
 *    ASSERTs added in umem_cache_reclaim_pages() (state is RECLAIMING and
 *    refcnt 0 when CLEAN is published under the lock) plus the allocator's
 *    own invariants turn a mis-sequenced transition into a failure.  A
 *    plain pass here is NOT by itself proof the race is gone; it is a
 *    liveness/consistency check.  See the run report for the TSAN result.
 */
struct race_ctx {
	umem_cache_t	*cp;
	volatile int	stop;
	int		alloc_fail;
};

static void *
race_churn(void *arg)
{
	struct race_ctx *ctx = arg;
	void *bufs[32];

	while (!ctx->stop) {
		int n, i;

		for (n = 0; n < 32; n++) {
			bufs[n] = umem_cache_alloc(ctx->cp, UMEM_DEFAULT);
			if (bufs[n] == NULL) {
				ctx->alloc_fail++;
				break;
			}
			/* Touch it: a discarded page would fault or differ. */
			memset(bufs[n], 0x5a, 64);
		}
		for (i = 0; i < n; i++)
			umem_cache_free(ctx->cp, bufs[i]);
	}
	return (NULL);
}

static void *
race_reclaim(void *arg)
{
	struct race_ctx *ctx = arg;

	while (!ctx->stop)
		umem_cache_applyall(umem_cache_update);
	return (NULL);
}

static int
run_race(void)
{
	enum { NCHURN = 4 };
	struct race_ctx ctx;
	pthread_t churn[NCHURN], reclaimer;
	int i;

	ctx.cp = umem_cache_create("reclaim_race", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, UMC_NOMAGAZINE);
	if (ctx.cp == NULL) {
		printf("FAIL: umem_cache_create failed\n");
		return (1);
	}
	ctx.stop = 0;
	ctx.alloc_fail = 0;

	for (i = 0; i < NCHURN; i++) {
		if (pthread_create(&churn[i], NULL, race_churn, &ctx) != 0) {
			printf("FAIL: pthread_create failed\n");
			ctx.stop = 1;
			while (--i >= 0)
				(void) pthread_join(churn[i], NULL);
			umem_cache_destroy(ctx.cp);
			return (1);
		}
	}
	if (pthread_create(&reclaimer, NULL, race_reclaim, &ctx) != 0) {
		printf("FAIL: pthread_create failed\n");
		ctx.stop = 1;
		for (i = 0; i < NCHURN; i++)
			(void) pthread_join(churn[i], NULL);
		umem_cache_destroy(ctx.cp);
		return (1);
	}

	sleep(3);
	ctx.stop = 1;

	for (i = 0; i < NCHURN; i++)
		(void) pthread_join(churn[i], NULL);
	(void) pthread_join(reclaimer, NULL);

	umem_cache_destroy(ctx.cp);

	printf("ok: reclaim raced %d churn threads for 3s "
	    "(%d allocation failures)\n", NCHURN, ctx.alloc_fail);
	return (0);
}

int
main(int argc, char **argv)
{
	void *warm;

	if (argc < 2) {
		printf("usage: %s <hash_guards|big_quantum|race>\n", argv[0]);
		return (2);
	}

	/* Force init so the env-derived flags/tunables are in effect. */
	warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, 64);

	if (!umem_reclaim_enabled) {
		printf("SKIP: reclamation disabled\n");
		return (77);
	}

	if (strcmp(argv[1], "hash_guards") == 0)
		return (run_hash_guards());
	if (strcmp(argv[1], "big_quantum") == 0)
		return (run_big_quantum());
	if (strcmp(argv[1], "race") == 0)
		return (run_race());

	printf("usage: %s <hash_guards|big_quantum|race>\n", argv[0]);
	return (2);
}

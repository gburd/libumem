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
 * P1.4 regression: umem_cache_destroy() must release the empty slabs that
 * background reclamation retains.
 *
 * With umem_reclaim_enabled (the default), umem_slab_free()'s last-object
 * path marks the now-empty slab SLAB_DIRTY and leaves it linked for the
 * update thread instead of destroying it.  umem_cache_destroy() used to
 * only log a nonzero cache_buftotal and then free the cache descriptor, so
 * the retained slab's backing span (and, for a UMF_HASH cache, its external
 * slab/bufctl metadata) stayed allocated forever, with slab_cache pointing
 * at freed memory.
 *
 * The leak is measured, not inferred: each cache gets its own vmem arena,
 * so vmem_size(arena, VMEM_ALLOC) is exactly the backing this cache holds.
 * The sequence is the minimal trigger from the plan -- create
 * UMC_NOMAGAZINE cache, allocate one object, free it, destroy the cache --
 * with no reclaim delay and no madvise involved.
 *
 * PRE-FIX: "leaked N bytes after destroy" for both the non-hash and the
 * hash cache (and vmem_destroy() additionally reports the same leak).
 * POST-FIX: both arenas are empty after destroy.
 */

#include "umem_base.h"		/* umem_cache_t, umem_slab_t, tunables */

#include <stdio.h>
#include <stdlib.h>

#define	ARENA_SPAN	(1024 * 1024)

static int failures;

static void
fail(const char *what, const char *detail, size_t value)
{
	printf("FAIL: %s: %s (%lu)\n", what, detail, (unsigned long)value);
	failures++;
}

/*
 * create -> alloc one -> free it -> destroy, on a private arena.
 *
 * Between the free and the destroy the slab must still be held (that is the
 * retention this regression is about); after the destroy the arena must be
 * empty.
 */
static void
check_cache(const char *what, size_t bufsize, int expect_hash)
{
	void *base;
	vmem_t *vmp;
	umem_cache_t *cp;
	void *buf;
	size_t held_after_free, leaked;

	if (posix_memalign(&base, pagesize, ARENA_SPAN) != 0) {
		fail(what, "posix_memalign failed", 0);
		return;
	}

	vmp = vmem_create(what, base, ARENA_SPAN, pagesize,
	    NULL, NULL, NULL, 0, VM_NOSLEEP);
	if (vmp == NULL) {
		fail(what, "vmem_create failed", 0);
		free(base);
		return;
	}

	cp = umem_cache_create((char *)what, bufsize, 0,
	    NULL, NULL, NULL, NULL, vmp, UMC_NOMAGAZINE);
	if (cp == NULL) {
		fail(what, "umem_cache_create failed", 0);
		vmem_destroy(vmp);
		free(base);
		return;
	}

	if (((cp->cache_flags & UMF_HASH) != 0) != (expect_hash != 0)) {
		printf("SKIP: %s: cache_flags=0x%x, wanted %s cache\n",
		    what, cp->cache_flags, expect_hash ? "hash" : "non-hash");
		umem_cache_destroy(cp);
		vmem_destroy(vmp);
		free(base);
		return;
	}

	buf = umem_cache_alloc(cp, UMEM_DEFAULT);
	if (buf == NULL) {
		fail(what, "umem_cache_alloc failed", 0);
		umem_cache_destroy(cp);
		vmem_destroy(vmp);
		free(base);
		return;
	}
	umem_cache_free(cp, buf);

	held_after_free = vmem_size(vmp, VMEM_ALLOC);
	if (held_after_free == 0) {
		/*
		 * Nothing retained means the slab was destroyed on the free
		 * path, so this run cannot say anything about destroy-time
		 * draining.  Report it rather than passing vacuously.
		 */
		printf("SKIP: %s: no slab retained after free "
		    "(reclaim disabled?)\n", what);
		umem_cache_destroy(cp);
		vmem_destroy(vmp);
		free(base);
		return;
	}

	umem_cache_destroy(cp);

	leaked = vmem_size(vmp, VMEM_ALLOC);
	if (leaked != 0)
		fail(what, "arena still holds bytes after destroy", leaked);
	else
		printf("ok: %s: %lu bytes retained after free, 0 after "
		    "destroy\n", what, (unsigned long)held_after_free);

	vmem_destroy(vmp);
	free(base);
}

int
main(void)
{
	/* Force umem initialization before touching its globals. */
	void *warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, 64);

	if (!umem_reclaim_enabled) {
		printf("SKIP: reclamation disabled (umem_reclaim_enabled=0)\n");
		return (77);
	}

	/* bufsize < quantum/UMEM_VOID_FRACTION -> embedded slab metadata */
	check_cache("reclaim_destroy_nohash", 64, 0);
	/* bufsize >= quantum/UMEM_VOID_FRACTION -> UMF_HASH, external metadata */
	check_cache("reclaim_destroy_hash", 1024, 1);

	if (failures != 0) {
		printf("FAILED (%d)\n", failures);
		return (1);
	}
	printf("PASS\n");
	return (0);
}

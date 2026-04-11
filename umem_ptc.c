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

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>

#include "umem_ptc.h"
#include "umem_base.h"
#include "umem_impl.h"

/*
 * External reference to umem_alloc_table
 */
extern umem_cache_t *umem_alloc_table[];

/*
 * Global configuration (can be tuned via UMEM_OPTIONS)
 */
size_t umem_ptc_maxsize = 448;       /* max cached size */
int umem_ptc_enabled = 1;            /* enabled by default for performance */

/*
 * Thread-local storage for ptc
 * Using __thread with initial-exec TLS model for fast access
 */
static __thread umem_ptc_t *thread_ptc
    __attribute__((tls_model("initial-exec"))) = NULL;

/*
 * pthread key for cleanup
 */
static pthread_key_t ptc_key;
static int ptc_key_initialized = 0;

/*
 * Size class table for quick bin lookup
 * Maps allocation sizes to bin indices.
 * Zero-initialized by C static storage rules; umem_ptc_init() fills it
 * with valid bin indices (and -1 for unmapped entries).  The
 * ptc_table_ready flag guards against use before initialization,
 * since a zero entry would silently alias every size to bin 0.
 */
static int8_t size_to_bin_table[PTC_NBINS * 32];
static int ptc_table_ready;

/*
 * Size classes we cache (matching umem's small size classes)
 * These correspond to the first PTC_NBINS entries in umem_alloc_sizes
 */
static const size_t ptc_size_classes[PTC_NBINS] = {
#ifdef _LP64
	8, 16, 32, 48,          /* 1*8, 1*16, 2*16, 3*16 */
	64, 80, 96, 112,        /* 4*16, 5*16, 6*16, 7*16 */
	128, 160, 192, 224,     /* 4*32, 5*32, 6*32, 7*32 */
	256, 320, 384, 448      /* 4*64, 5*64, 6*64, 7*64 */
#else
	8, 16, 24, 32,          /* 1*8, 2*8, 3*8, 4*8 */
	40, 48, 56, 64,         /* 5*8, 6*8, 7*8, 4*16 */
	80, 96, 112, 128,       /* 5*16, 6*16, 7*16, 4*32 */
	160, 192, 224, 256      /* 5*32, 6*32, 7*32, 4*64 */
#endif
};

/*
 * Cleanup callback for pthread_key
 */
static void
umem_ptc_cleanup(void *arg)
{
	umem_ptc_t *ptc = (umem_ptc_t *)arg;

	if (ptc != NULL) {
		umem_ptc_destroy(ptc);
		thread_ptc = NULL;
	}
}

/*
 * Initialize ptc subsystem
 */
void
umem_ptc_init(void)
{
	int i, bin;
	size_t size;

	if (!umem_ptc_enabled) {
		return;
	}

	/* Initialize pthread key for cleanup */
	if (!ptc_key_initialized) {
		if (pthread_key_create(&ptc_key, umem_ptc_cleanup) != 0) {
			umem_ptc_enabled = 0;
			return;
		}
		ptc_key_initialized = 1;
	}

	/* Build size-to-bin lookup table */
	for (i = 0; i < (int)(sizeof(size_to_bin_table)); i++) {
		size_to_bin_table[i] = -1;
	}

	for (bin = 0; bin < PTC_NBINS; bin++) {
		size = ptc_size_classes[bin];
		if (size / 8 < sizeof(size_to_bin_table)) {
			size_to_bin_table[size / 8] = bin;
		}
	}

	/*
	 * Disable PTC for size classes whose backing cache has debug
	 * flags enabled. PTC bypasses the magazine layer and would
	 * skip UMF_AUDIT/UMF_DEADBEEF/UMF_REDZONE checking.
	 */
	for (bin = 0; bin < PTC_NBINS; bin++) {
		umem_cache_t *cp;
		size = ptc_size_classes[bin];
		if (size == 0)
			continue;
		cp = umem_alloc_table[(size - 1) >> UMEM_ALIGN_SHIFT];
		if (cp != NULL &&
		    (cp->cache_flags &
		    (UMF_AUDIT | UMF_DEADBEEF | UMF_REDZONE))) {
			if (size / 8 < sizeof(size_to_bin_table)) {
				size_to_bin_table[size / 8] = -1;
			}
		}
	}

	ptc_table_ready = 1;
}

/*
 * Map allocation size to bin index
 * Returns -1 if size is not eligible for per-thread caching
 */
int
umem_ptc_size_to_bin(size_t size)
{
	size_t index;

	if (!umem_ptc_enabled || !ptc_table_ready || size > umem_ptc_maxsize) {
		return (-1);
	}

	/* Round up to 8-byte alignment */
	size = (size + 7) & ~7;

	index = size / 8;
	if (index >= sizeof(size_to_bin_table)) {
		return (-1);
	}

	return (size_to_bin_table[index]);
}

/*
 * Get the actual size for a bin
 */
static size_t
umem_ptc_bin_size(int bin)
{
	if (bin < 0 || bin >= PTC_NBINS) {
		return (0);
	}
	return (ptc_size_classes[bin]);
}

/*
 * Get or create the current thread's cache
 */
umem_ptc_t *
umem_ptc_get(void)
{
	umem_ptc_t *ptc;

	if (!umem_ptc_enabled) {
		return (NULL);
	}

	ptc = thread_ptc;
	if (ptc != NULL) {
		return (ptc);
	}

	/* Allocate new ptc using umem_alloc to avoid recursion */
	ptc = (umem_ptc_t *)umem_alloc(sizeof(umem_ptc_t),
	    UMEM_DEFAULT);
	if (ptc == NULL) {
		return (NULL);
	}

	(void) memset(ptc, 0, sizeof(umem_ptc_t));

	/* Store in TLS and pthread-specific data */
	thread_ptc = ptc;
	if (ptc_key_initialized) {
		(void) pthread_setspecific(ptc_key, ptc);
	}

	return (ptc);
}

/*
 * Allocate from thread cache
 */
void *
umem_ptc_alloc(size_t size)
{
	umem_ptc_t *ptc;
	umem_ptc_bin_t *bin;
	void *ptr;
	int bin_idx;

	if (!umem_ptc_enabled) {
		return (NULL);
	}

	bin_idx = umem_ptc_size_to_bin(size);
	if (bin_idx < 0) {
		return (NULL);
	}

	ptc = umem_ptc_get();
	if (ptc == NULL) {
		return (NULL);
	}

	bin = &ptc->bins[bin_idx];

	/* Fast path: take from cache */
	if (bin->count > 0) {
		ptr = bin->slots[--bin->count];
		ptc->alloc_count++;
		ptc->hits++;
		return (ptr);
	}

	/* Cache miss - try to refill from magazine layer */
	if (umem_ptc_bin_refill(bin, umem_ptc_bin_size(bin_idx)) == 0) {
		if (bin->count > 0) {
			ptr = bin->slots[--bin->count];
			ptc->alloc_count++;
			ptc->hits++;
			return (ptr);
		}
	}

	ptc->misses++;
	return (NULL);
}

/*
 * Free to thread cache
 */
int
umem_ptc_free(void *ptr, size_t size)
{
	umem_ptc_t *ptc;
	umem_ptc_bin_t *bin;
	int bin_idx;

	if (!umem_ptc_enabled || ptr == NULL) {
		return (-1);
	}

	bin_idx = umem_ptc_size_to_bin(size);
	if (bin_idx < 0) {
		return (-1);
	}

	ptc = umem_ptc_get();
	if (ptc == NULL) {
		return (-1);
	}

	bin = &ptc->bins[bin_idx];

	/* Fast path: cache it */
	if (bin->count < PTC_NSLOTS) {
		bin->slots[bin->count++] = ptr;
		ptc->free_count++;
		return (0);
	}

	/* Cache full - flush half to magazine layer */
	umem_ptc_bin_flush(bin, umem_ptc_bin_size(bin_idx));

	/* Try again after flush */
	if (bin->count < PTC_NSLOTS) {
		bin->slots[bin->count++] = ptr;
		ptc->free_count++;
		return (0);
	}

	return (-1);
}

/*
 * Flush cached objects from a bin to the magazine layer.
 * Uses batch free to take cc_lock once instead of per-object.
 *
 * Objects flushed here enter the per-CPU magazine via umem_cache_free_batch(),
 * which is intentional: the magazine layer is the correct next level in
 * the caching hierarchy (PTC -> magazine -> depot -> slab). This means
 * flushed objects may be re-cached in magazines rather than immediately
 * reaching the depot, but that is the desired behavior for warm reuse.
 */
void
umem_ptc_bin_flush(umem_ptc_bin_t *bin, size_t size)
{
	int i;
	int flush_count;
	void *ptr;
	umem_cache_t *cp;

	if (bin->count == 0) {
		return;
	}

	/* Flush half the bin */
	flush_count = bin->count / 2;
	if (flush_count == 0) {
		flush_count = bin->count;
	}

	/* Look up the appropriate cache */
	cp = umem_alloc_table[(size - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		/* No cache - free directly */
		for (i = 0; i < flush_count; i++) {
			ptr = bin->slots[--bin->count];
			umem_free(ptr, size);
		}
		return;
	}

	/* Free to magazine layer */
	for (i = 0; i < flush_count; i++) {
		ptr = bin->slots[--bin->count];
		_umem_cache_free(cp, ptr);
	}
}

/*
 * Refill a bin from the magazine layer
 */
int
umem_ptc_bin_refill(umem_ptc_bin_t *bin, size_t size)
{
	int i;
	int refill_count;
	void *ptr;
	umem_cache_t *cp;

	/* Refill half the bin */
	refill_count = PTC_NSLOTS / 2;

	/* Look up the appropriate cache */
	cp = umem_alloc_table[(size - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		return (-1);
	}

	/* Allocate from magazine layer */
	for (i = 0; i < refill_count; i++) {
		ptr = _umem_cache_alloc(cp, UMEM_DEFAULT);
		if (ptr == NULL) {
			break;
		}
		bin->slots[bin->count++] = ptr;
	}

	return (i > 0 ? 0 : -1);
}

/*
 * Destroy thread cache
 */
void
umem_ptc_destroy(umem_ptc_t *ptc)
{
	int bin_idx;
	umem_ptc_bin_t *bin;

	if (ptc == NULL) {
		return;
	}

	/* Flush all bins back to magazine layer incrementally,
	 * yielding between bins to reduce thread-exit latency spike */
	for (bin_idx = 0; bin_idx < PTC_NBINS; bin_idx++) {
		bin = &ptc->bins[bin_idx];
		if (bin->count > 0) {
			umem_ptc_bin_flush(bin,
			    umem_ptc_bin_size(bin_idx));
			sched_yield();
		}
	}

	/* Free the ptc structure itself */
	umem_free(ptc, sizeof(umem_ptc_t));
}

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

#include "umem_tcache.h"
#include "umem_base.h"
#include "umem_impl.h"

/*
 * External reference to umem_alloc_table
 */
extern umem_cache_t *umem_alloc_table[];

/*
 * Global configuration (can be tuned via UMEM_OPTIONS)
 */
size_t umem_tcache_maxsize = 448;       /* max cached size */
int umem_tcache_enabled = 0;            /* disabled by default for now */

/*
 * Thread-local storage for tcache
 * Using __thread with initial-exec TLS model for fast access
 */
static __thread umem_tcache_t *thread_tcache
    __attribute__((tls_model("initial-exec"))) = NULL;

/*
 * pthread key for cleanup
 */
static pthread_key_t tcache_key;
static int tcache_key_initialized = 0;

/*
 * Size class table for quick bin lookup
 * Maps allocation sizes to bin indices
 */
static int8_t size_to_bin_table[TCACHE_NBINS * 32];

/*
 * Size classes we cache (matching umem's small size classes)
 * These correspond to the first TCACHE_NBINS entries in umem_alloc_sizes
 */
static const size_t tcache_size_classes[TCACHE_NBINS] = {
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
umem_tcache_cleanup(void *arg)
{
	umem_tcache_t *tcache = (umem_tcache_t *)arg;

	if (tcache != NULL) {
		umem_tcache_destroy(tcache);
		thread_tcache = NULL;
	}
}

/*
 * Initialize tcache subsystem
 */
void
umem_tcache_init(void)
{
	int i, bin;
	size_t size;

	if (!umem_tcache_enabled) {
		return;
	}

	/* Initialize pthread key for cleanup */
	if (!tcache_key_initialized) {
		if (pthread_key_create(&tcache_key, umem_tcache_cleanup) != 0) {
			umem_tcache_enabled = 0;
			return;
		}
		tcache_key_initialized = 1;
	}

	/* Build size-to-bin lookup table */
	for (i = 0; i < (int)(sizeof(size_to_bin_table)); i++) {
		size_to_bin_table[i] = -1;
	}

	for (bin = 0; bin < TCACHE_NBINS; bin++) {
		size = tcache_size_classes[bin];
		if (size / 8 < sizeof(size_to_bin_table)) {
			size_to_bin_table[size / 8] = bin;
		}
	}
}

/*
 * Map allocation size to bin index
 * Returns -1 if size is not eligible for tcaching
 */
int
umem_tcache_size_to_bin(size_t size)
{
	size_t index;

	if (!umem_tcache_enabled || size > umem_tcache_maxsize) {
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
umem_tcache_bin_size(int bin)
{
	if (bin < 0 || bin >= TCACHE_NBINS) {
		return (0);
	}
	return (tcache_size_classes[bin]);
}

/*
 * Get or create the current thread's cache
 */
umem_tcache_t *
umem_tcache_get(void)
{
	umem_tcache_t *tcache;

	if (!umem_tcache_enabled) {
		return (NULL);
	}

	tcache = thread_tcache;
	if (tcache != NULL) {
		return (tcache);
	}

	/* Allocate new tcache using umem_alloc to avoid recursion */
	tcache = (umem_tcache_t *)umem_alloc(sizeof(umem_tcache_t),
	    UMEM_DEFAULT);
	if (tcache == NULL) {
		return (NULL);
	}

	(void) memset(tcache, 0, sizeof(umem_tcache_t));

	/* Store in TLS and pthread-specific data */
	thread_tcache = tcache;
	if (tcache_key_initialized) {
		(void) pthread_setspecific(tcache_key, tcache);
	}

	return (tcache);
}

/*
 * Allocate from thread cache
 */
void *
umem_tcache_alloc(size_t size)
{
	umem_tcache_t *tcache;
	umem_tcache_bin_t *bin;
	void *ptr;
	int bin_idx;

	if (!umem_tcache_enabled) {
		return (NULL);
	}

	bin_idx = umem_tcache_size_to_bin(size);
	if (bin_idx < 0) {
		return (NULL);
	}

	tcache = umem_tcache_get();
	if (tcache == NULL) {
		return (NULL);
	}

	bin = &tcache->bins[bin_idx];

	/* Fast path: take from cache */
	if (bin->count > 0) {
		ptr = bin->slots[--bin->count];
		tcache->alloc_count++;
		tcache->hits++;
		return (ptr);
	}

	/* Cache miss - try to refill from magazine layer */
	if (umem_tcache_bin_refill(bin, umem_tcache_bin_size(bin_idx)) == 0) {
		if (bin->count > 0) {
			ptr = bin->slots[--bin->count];
			tcache->alloc_count++;
			tcache->hits++;
			return (ptr);
		}
	}

	tcache->misses++;
	return (NULL);
}

/*
 * Free to thread cache
 */
int
umem_tcache_free(void *ptr, size_t size)
{
	umem_tcache_t *tcache;
	umem_tcache_bin_t *bin;
	int bin_idx;

	if (!umem_tcache_enabled || ptr == NULL) {
		return (-1);
	}

	bin_idx = umem_tcache_size_to_bin(size);
	if (bin_idx < 0) {
		return (-1);
	}

	tcache = umem_tcache_get();
	if (tcache == NULL) {
		return (-1);
	}

	bin = &tcache->bins[bin_idx];

	/* Fast path: cache it */
	if (bin->count < TCACHE_NSLOTS) {
		bin->slots[bin->count++] = ptr;
		tcache->free_count++;
		return (0);
	}

	/* Cache full - flush half to magazine layer */
	umem_tcache_bin_flush(bin, umem_tcache_bin_size(bin_idx));

	/* Try again after flush */
	if (bin->count < TCACHE_NSLOTS) {
		bin->slots[bin->count++] = ptr;
		tcache->free_count++;
		return (0);
	}

	return (-1);
}

/*
 * Flush cached objects from a bin to the magazine layer
 */
void
umem_tcache_bin_flush(umem_tcache_bin_t *bin, size_t size)
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
umem_tcache_bin_refill(umem_tcache_bin_t *bin, size_t size)
{
	int i;
	int refill_count;
	void *ptr;
	umem_cache_t *cp;

	/* Refill half the bin */
	refill_count = TCACHE_NSLOTS / 2;

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
umem_tcache_destroy(umem_tcache_t *tcache)
{
	int bin_idx;
	umem_tcache_bin_t *bin;

	if (tcache == NULL) {
		return;
	}

	/* Flush all bins back to magazine layer */
	for (bin_idx = 0; bin_idx < TCACHE_NBINS; bin_idx++) {
		bin = &tcache->bins[bin_idx];
		if (bin->count > 0) {
			umem_tcache_bin_flush(bin,
			    umem_tcache_bin_size(bin_idx));
		}
	}

	/* Free the tcache structure itself */
	umem_free(tcache, sizeof(umem_tcache_t));
}

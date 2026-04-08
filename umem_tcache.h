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

#ifndef _UMEM_TCACHE_H
#define _UMEM_TCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-thread cache for small allocations (similar to jemalloc tcache).
 * Provides a zero-synchronization fast path for small allocations by
 * maintaining thread-local bins of recently freed objects.
 *
 * Design:
 * - Thread-local cache for allocations <= tcache_maxsize (default 448 bytes)
 * - Array of bins, one per size class
 * - Each bin holds up to TCACHE_NSLOTS pointers
 * - Zero synchronization for cache hit
 * - Fallback to magazine layer when full/empty
 */

#define TCACHE_NSLOTS 32        /* slots per bin */
#define TCACHE_NBINS 16         /* number of small size classes */

/*
 * Per-bin structure holding cached objects
 */
typedef struct umem_tcache_bin {
	void *slots[TCACHE_NSLOTS];
	uint16_t count;         /* current number of cached objects */
	uint16_t low_water;     /* for auto-tuning (future) */
} umem_tcache_bin_t;

/*
 * Per-thread cache structure
 */
typedef struct umem_tcache {
	umem_tcache_bin_t bins[TCACHE_NBINS];
	uint64_t alloc_count;   /* statistics */
	uint64_t free_count;
	uint64_t hits;
	uint64_t misses;
} umem_tcache_t;

/*
 * Global configuration
 */
extern size_t umem_tcache_maxsize;      /* max size cached (default 448) */
extern int umem_tcache_enabled;         /* tcache globally enabled */

/*
 * Size class to bin index mapping
 * Returns -1 if size is not eligible for tcaching
 */
int umem_tcache_size_to_bin(size_t size);

/*
 * Get the current thread's cache, creating it if necessary
 * Returns NULL if tcache is disabled or creation fails
 */
umem_tcache_t *umem_tcache_get(void);

/*
 * Allocate from thread cache
 * Returns NULL if not found in cache (caller should use slow path)
 */
void *umem_tcache_alloc(size_t size);

/*
 * Free to thread cache
 * Returns 0 if cached, -1 if cache full (caller should use slow path)
 */
int umem_tcache_free(void *ptr, size_t size);

/*
 * Destroy thread cache (called at thread exit)
 */
void umem_tcache_destroy(umem_tcache_t *tcache);

/*
 * Initialize tcache subsystem (called during umem initialization)
 */
void umem_tcache_init(void);

/*
 * Flush a bin to the magazine layer
 */
void umem_tcache_bin_flush(umem_tcache_bin_t *bin, size_t size);

/*
 * Refill a bin from the magazine layer
 */
int umem_tcache_bin_refill(umem_tcache_bin_t *bin, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_TCACHE_H */

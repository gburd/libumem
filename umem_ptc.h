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

#ifndef _UMEM_PTC_H
#define _UMEM_PTC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-Thread Cache (PTC) for small allocations (similar to jemalloc ptc).
 * Provides a zero-synchronization fast path for small allocations by
 * maintaining thread-local bins of recently freed objects.
 *
 * Design:
 * - Thread-local cache for allocations <= ptc_maxsize (default 448 bytes)
 * - Array of bins, one per size class
 * - Each bin holds up to PTC_NSLOTS pointers
 * - Zero synchronization for cache hit
 * - Fallback to magazine layer when full/empty
 */

#define PTC_NSLOTS 32        /* slots per bin */
#define PTC_NBINS 16         /* number of small size classes */

/*
 * Per-bin structure holding cached objects
 */
typedef struct umem_ptc_bin {
	void *slots[PTC_NSLOTS];
	uint16_t count;         /* current number of cached objects */
	uint16_t low_water;     /* for auto-tuning (future) */
} __attribute__((aligned(64))) umem_ptc_bin_t;

/*
 * Per-thread cache structure
 */
typedef struct umem_ptc {
	umem_ptc_bin_t bins[PTC_NBINS];
	uint64_t alloc_count;   /* statistics */
	uint64_t free_count;
	uint64_t hits;
	uint64_t misses;
} umem_ptc_t;

/*
 * Global configuration
 */
extern size_t umem_ptc_maxsize;      /* max size cached (default 448) */
extern int umem_ptc_enabled;         /* ptc globally enabled */

/*
 * Size class to bin index mapping
 * Returns -1 if size is not eligible for per-thread caching
 */
int umem_ptc_size_to_bin(size_t size);

/*
 * Get the current thread's cache, creating it if necessary
 * Returns NULL if ptc is disabled or creation fails
 */
umem_ptc_t *umem_ptc_get(void);

/*
 * Allocate from thread cache
 * Returns NULL if not found in cache (caller should use slow path)
 */
void *umem_ptc_alloc(size_t size);

/*
 * Free to thread cache
 * Returns 0 if cached, -1 if cache full (caller should use slow path)
 */
int umem_ptc_free(void *ptr, size_t size);

/*
 * Destroy thread cache (called at thread exit)
 */
void umem_ptc_destroy(umem_ptc_t *ptc);

/*
 * Initialize ptc subsystem (called during umem initialization)
 */
void umem_ptc_init(void);

/*
 * Flush a bin to the magazine layer
 */
void umem_ptc_bin_flush(umem_ptc_bin_t *bin, size_t size);

/*
 * Refill a bin from the magazine layer
 */
int umem_ptc_bin_refill(umem_ptc_bin_t *bin, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_PTC_H */

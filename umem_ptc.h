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
 * - Thread-local cache for allocations <= ptc_maxsize (default 2048 bytes)
 * - Array of bins, one per size class
 * - Each bin holds up to PTC_NSLOTS pointers
 * - Zero synchronization for cache hit
 * - Fallback to magazine layer when full/empty
 */

#define PTC_NSLOTS 32        /* slots per bin */
#define PTC_NBINS 28         /* number of size classes (up to 2048B) */

/* Forward declaration */
struct umem_magazine;
struct umem_cache;

/*
 * Per-thread magazine: thread-local loaded/previous magazine pair.
 * Sits between PTC bins and the depot, eliminating cc_lock contention.
 * When the PTC bin is empty/full, the thread magazine provides/accepts
 * objects without taking any lock. Only depot refill/flush takes a lock.
 */
typedef struct umem_ptc_mag {
	struct umem_magazine *loaded;   /* currently loaded magazine */
	struct umem_magazine *previous; /* previously loaded magazine */
	int rounds;                     /* rounds remaining in loaded */
	int prounds;                    /* rounds remaining in previous */
	int magsize;                    /* capacity of magazine */
	struct umem_cache *cache;       /* owning cache (set on first use) */
} umem_ptc_mag_t;

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
	umem_ptc_mag_t mags[PTC_NBINS]; /* per-thread magazines */
	uint64_t alloc_count;   /* statistics */
	uint64_t free_count;
	uint64_t hits;
	uint64_t misses;
} umem_ptc_t;

/*
 * Global configuration
 */
extern size_t umem_ptc_maxsize;      /* max size cached (default 2048) */
extern int umem_ptc_enabled;         /* ptc globally enabled */

/*
 * Pre-computed bin table indexed by umem_alloc_table index.
 * Maps (size - 1) >> UMEM_ALIGN_SHIFT to PTC bin index (-1 if not eligible).
 * Populated during umem_ptc_init().
 */
extern int8_t umem_ptc_bin_table[];

/*
 * Thread-local PTC pointer, accessible from umem.c for inlined fast path.
 */
extern __thread umem_ptc_t *thread_ptc;

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
 * Flush all per-thread magazines back to depot (called at thread exit)
 */
void umem_ptc_mag_flush_all(umem_ptc_t *ptc);

/*
 * Flush a bin to the magazine layer
 */
void umem_ptc_bin_flush(umem_ptc_bin_t *bin, size_t size);

/*
 * Refill a bin from the magazine layer
 */
int umem_ptc_bin_refill(umem_ptc_bin_t *bin, size_t size);

/*
 * Small-Buffer Optimization (SBO)
 *
 * Thread-local bump allocator for tiny allocations (<= 128 bytes).
 * Serves from a pre-allocated 4KB thread-local buffer with no metadata
 * overhead and no locking (~3-5ns).
 *
 * Constraints:
 * - When the buffer is full, it resets and all outstanding pointers from
 *   the previous generation become invalid. Callers must not hold SBO
 *   pointers across a reset boundary.
 * - Free is a no-op for SBO pointers; memory is reclaimed only on reset.
 * - Disabled when debug flags (UMF_AUDIT, UMF_DEADBEEF, UMF_REDZONE)
 *   are active, falling back to umem_alloc().
 */
#define UMEM_SBO_BUFSZ		4096	/* per-thread SBO buffer size */
#define UMEM_SBO_MAXALLOC	128	/* max allocation served by SBO */
#define UMEM_SBO_ALIGN		16	/* bump pointer alignment */

/*
 * Allocate from the thread-local SBO buffer.
 * Returns NULL if SBO is disabled, size > UMEM_SBO_MAXALLOC, or
 * the buffer is full (caller should fall back to umem_alloc).
 */
void *umem_sbo_alloc(size_t size, int umflags);

/*
 * Free an SBO pointer. This is a no-op if ptr came from the SBO buffer.
 * Returns 1 if ptr was an SBO pointer (caller should not free elsewhere),
 * returns 0 if ptr is not from SBO (caller must free normally).
 */
int umem_sbo_free(void *ptr, size_t size);

/*
 * Reset the SBO buffer (all outstanding SBO pointers become invalid).
 */
void umem_sbo_reset(void);

/*
 * Check if SBO is enabled for the current thread.
 */
int umem_sbo_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_PTC_H */

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
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-Thread Cache (PTC) for small allocations (similar to jemalloc ptc).
 * Provides a zero-synchronization fast path for small allocations by
 * maintaining thread-local bins of recently freed objects.
 *
 * Design:
 * - Thread-local cache for allocations <= ptc_maxsize (default 8192 bytes)
 * - Array of bins, one per size class
 * - Each bin holds up to ptc_bin_capacity(bin) pointers
 * - Zero synchronization for cache hit
 * - Fallback to magazine layer when full/empty
 *
 * Footprint (P6.3).  The slot arrays are packed at their real capacities
 * in one pool inside umem_ptc_t (below), not padded to a common maximum:
 * the previous layout carried 28 x 128 slots for every thread, 31 KB, of
 * which 20 KB was never indexed.
 *
 * THE CAPACITIES ARE NOT A FREE VARIABLE.  The first version of this
 * change also halved them (64/32/16) on the reasoning that the bins are
 * only the L1 of a two-level per-thread cache and the per-thread magazines
 * (umem_ptc_mag_t) are a lock-free L2, so a bin's size merely sets where
 * an object crosses from one to the other.  At the time, that L2 did not
 * exist in practice: nothing primed it.  It took magazines from the depot
 * by trylock only and never allocated one, and the CPU layer publishes an
 * empty magazine to the depot only when its own loaded magazine drains --
 * which a steady alloc/free workload never does.  So an object past the
 * bin cost up to UMEM_DEPOT_STEAL_MAX failed trylock/unlock pairs on
 * empty stripes and then cc_lock, every time.  Measured, single thread,
 * 512 B, alloc-N-then-free-N: 157 Mpairs/s at N = 32, 63 at N = 33, 5.5
 * at N = 64 -- a 28x cliff exactly at the bin boundary, and halving the
 * medium bin moved that cliff from N = 64 to N = 32 (bench `single`
 * 512 B: 5.72 -> 5.31 Mops).  The capacities stay at 128/64/32; the
 * footprint win is the packing (31 -> 12 KB), not the halving.  The
 * unprimed L2 was P8.6, fixed in a2177b9 (umem_ptc_mag_prime in umem.c):
 * the same loop then runs 137 Mpairs/s at N = 128 against 160 at N = 64.
 * The capacities were not revisited after that fix; a smaller bin is now
 * a real trade against the L2, not a cliff, but it has not been measured.
 */

#define PTC_NSLOTS_SMALL  128  /* bins 0-(PTC_BIN_MEDIUM-1): sizes <=256B */
#define PTC_NSLOTS_MEDIUM  64  /* bins PTC_BIN_MEDIUM-(PTC_BIN_LARGE-1) */
#define PTC_NSLOTS_LARGE   32  /* bins PTC_BIN_LARGE-(PTC_BIN_XLARGE-1) */
#define PTC_NSLOTS_XLARGE  16  /* bins PTC_BIN_XLARGE-(PTC_NBINS-1) */
#define PTC_NBINS 36           /* number of size classes (up to 8192B) */
#define PTC_BIN_MEDIUM    13   /* first bin for medium sizes (257-1024B) */
#define PTC_BIN_LARGE     21   /* first bin for large sizes (1025-2048B) */
#define PTC_BIN_XLARGE    28   /* first bin for xlarge sizes (2049-8192B) */

/*
 * PTC_NBINS bounds size_to_bin_table (indexed by size / 8, so it must reach
 * the largest class / 8) and umem_ptc_maxsize's default, both in umem_ptc.c.
 * The bins through 8192 exist for P8.2b: above the old 2048 ceiling every
 * operation took cc_lock and, every 31 ops per CPU, a blocking depot trip
 * into a stripe other CPUs had emptied; at 128+ CPUs the depot convoyed
 * (x86 metal 105 -> 91 Mops from t=64 to 128, arm 277 -> 77, both nulls
 * falling with them).  Footprint: 8 x 16 slots is 1 KB of pool, and the
 * struct stays under test_ptc_footprint's 24 KB (24,000 B on LP64).
 */

/*
 * Total slots across all bins.  Must equal the sum of ptc_bin_capacity()
 * over [0, PTC_NBINS); umem_ptc_get() asserts that when it lays the pool out.
 */
#define PTC_TOTAL_SLOTS \
	(PTC_BIN_MEDIUM * PTC_NSLOTS_SMALL + \
	(PTC_BIN_LARGE - PTC_BIN_MEDIUM) * PTC_NSLOTS_MEDIUM + \
	(PTC_BIN_XLARGE - PTC_BIN_LARGE) * PTC_NSLOTS_LARGE + \
	(PTC_NBINS - PTC_BIN_XLARGE) * PTC_NSLOTS_XLARGE)

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
	/*
	 * Capacity of each magazine, in rounds.
	 *
	 * These MUST be derived from the magazine itself (umem_mag_capacity()
	 * in umem.c), never from cp->cache_magtype->mt_magsize.  The update
	 * thread can run umem_cache_magazine_resize() at any point, including
	 * between obtaining a magazine and reading the cache's magtype, and an
	 * old 127-round magazine indexed with the new 255-round capacity walks
	 * off the end of its allocation (P1.3b).
	 *
	 * loaded and previous can legitimately be of DIFFERENT magtypes -- a
	 * resize only has to land between two refills -- so each needs its own
	 * capacity, and the two travel together with their magazine across the
	 * loaded/previous swap.
	 */
	int magsize;                    /* capacity of loaded */
	int pmagsize;                   /* capacity of previous */
	struct umem_cache *cache;       /* owning cache (set on first use) */
} umem_ptc_mag_t;

/*
 * Set once umem_ptc_mag_t carries a separate capacity for `previous`.
 * Regressions that must build against both the pre- and post-P1.3b struct
 * (to show they discriminate) test for this.
 */
#define	UMEM_PTC_MAG_HAS_PMAGSIZE	1

/*
 * Default cache line size for alignment. Must match UMEM_CACHE_LINE_SIZE
 * in umem_impl.h (which may be overridden by configure --with-cache-line-size).
 */
#ifndef _UMEM_PTC_CACHE_LINE
#define _UMEM_PTC_CACHE_LINE 64
#endif

/*
 * Per-bin header.  `slots` points into umem_ptc_t.pool and is set ONCE by
 * umem_ptc_get(); it never changes for the life of the PTC.  The fast paths
 * in umem.c do `b->slots[b->count++] = buf` with b = &ptc->bins[bin]: the
 * same shape as when slots was an inline array, one more independent load
 * (the base pointer) and no multiply by a 1 KB stride.
 *
 * Only slots[0 .. count) are meaningful; the rest of a bin's array is never
 * read and is not zeroed at creation.
 */
typedef struct umem_ptc_bin {
	void **slots;           /* -> umem_ptc_t.pool, ptc_bin_capacity() long */
	uint16_t count;         /* current number of cached objects */
	uint16_t low_water;     /* for auto-tuning (future) */
	/*
	 * One record per cache line.  Packing 28 of these 16-byte records into
	 * 7 lines measured 2 % slower than giving each its own (bench `multi`
	 * 16:1024, t=1: 5.76 vs 5.88 Mops, median of 9, alternating builds);
	 * PTC_NBINS lines is 2.25 KB of the 24 KB struct.
	 */
} __attribute__((aligned(_UMEM_PTC_CACHE_LINE))) umem_ptc_bin_t;

/*
 * Per-thread cache structure.  Everything before `pool` is zeroed at
 * creation; `pool` is written only below each bin's count.
 */
typedef struct umem_ptc {
	umem_ptc_bin_t bins[PTC_NBINS];
	umem_ptc_mag_t mags[PTC_NBINS]; /* per-thread magazines */
	uint64_t alloc_count;   /* statistics */
	uint64_t free_count;
	uint64_t hits;
	uint64_t misses;
	/*
	 * FORK CONSISTENCY (P1.3d).  A PTC is thread-private and lock-free,
	 * so fork() can snapshot it mid-update; the child then holds a copy
	 * whose owning thread does not exist.  Two rules make that copy
	 * usable by the child's fork handler instead of leaked:
	 *
	 *  1. MAGAZINE pushes store the round first and the count second with
	 *     a release store (umem.c _umem_free PTC paths; measured free at
	 *     t=8).  A snapshot between the two shows the old count and the
	 *     in-flight object is the parent's.  Pops are a single count
	 *     store and are already tear-free.
	 *
	 *     BIN pushes are NOT ordered: every ordered form measured -4..-5 %
	 *     at t=8 (c7i.2xlarge, 9 alternating pairs, null +-1 %; release
	 *     store, signal fence and plain slot-then-count all the same,
	 *     with identical instruction streams -- the cost is in the
	 *     store-to-store ordering itself and was not explained further).
	 *     So a bin snapshot may show count = k+1 over a stale slots[k].
	 *     Since a PTC has one owner, at most ONE push is in flight, and
	 *     only the top slot can be torn.  The child therefore drains each
	 *     bin's slots [0, count-1) and LEAKS slots[count-1]: at most one
	 *     object per non-empty bin per orphaned PTC, instead of the whole
	 *     PTC.  A stale top slot is thereby never freed; a live one is
	 *     leaked, which is the pre-P1.3d outcome for that object.
	 *
	 *  2. Every MULTI-STORE block (the loaded/previous swaps on both
	 *     sides, the depot refill, the retire) sets fork_busy = 1 before
	 *     and 0 (release) after.  A snapshot with fork_busy set is
	 *     mid-swap -- loaded may equal previous -- and the child's
	 *     handler SKIPS that PTC (leaks it, which is the pre-P1.3d
	 *     behaviour for every PTC) rather than drain one magazine twice.
	 *
	 * fork_busy is written only by the owning thread and read only by a
	 * fork child (a different process); no atomics beyond the release
	 * store are needed, and the stores cost nothing on the hit paths,
	 * which touch neither rule -- the bin push is rule 1 and is one
	 * store either way.
	 */
	volatile int fork_busy;
	/*
	 * Registry linkage (P1.3d part 2).  Every live PTC is on a doubly
	 * linked list under umem_ptc_list_lock, linked in umem_ptc_get() after
	 * the struct is fully built and unlinked in umem_ptc_cleanup() before
	 * it is freed.  The list exists for exactly one reader: the fork
	 * child, which walks it to drain the PTCs of threads that did not
	 * survive fork().  owner is the creating thread, so the child can
	 * tell its own (surviving) PTC from the orphans.
	 */
	struct umem_ptc *reg_next;
	struct umem_ptc *reg_prev;
	pthread_t owner;
	void *pool[PTC_TOTAL_SLOTS];    /* bins' slot arrays, packed */
} umem_ptc_t;

/* Fork handlers for the PTC registry (umem_fork.c calls these). */
void umem_ptc_fork_lockup(void);
void umem_ptc_fork_release(void);
void umem_ptc_fork_release_child(void);

/*
 * Global configuration
 */
extern size_t umem_ptc_maxsize;      /* max size cached (default 8192) */
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
 * Return the slot capacity for a given bin index.
 * Small bins (<=256B) get 128 slots, medium (<=1024B) 64, large (<=2048B)
 * 32, xlarge (<=8192B) 16.
 */
static inline int
ptc_bin_capacity(int bin)
{
	if (bin < PTC_BIN_MEDIUM) return (PTC_NSLOTS_SMALL);
	if (bin < PTC_BIN_LARGE) return (PTC_NSLOTS_MEDIUM);
	if (bin < PTC_BIN_XLARGE) return (PTC_NSLOTS_LARGE);
	return (PTC_NSLOTS_XLARGE);
}

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
 * Drain a bin completely.  Only correct at thread exit, where the bin is about
 * to be freed and anything left behind would lose its only reference while the
 * slab layer still counted it as allocated.
 */
void umem_ptc_bin_flush_all(umem_ptc_bin_t *bin, size_t size);

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

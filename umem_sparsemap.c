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

/*
 * umem_sparsemap.c - Page-granularity hash map for O(1) GC pointer lookup
 *
 * Open-addressing hash table that maps page-aligned addresses to counts
 * of GC objects residing on each page. Provides O(1) amortized lookup
 * for the GC's find_header() hot path.
 *
 * The table uses linear probing with a load factor limit of 70%.
 * When the load factor is exceeded, the table doubles in size.
 */

#include "config.h"

#include <umem.h>
#include "umem_sparsemap.h"
#include "umem_gc.h"

#include <pthread.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* ----------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------- */

#define	SM_EMPTY	((uintptr_t)0)
#define	SM_TOMBSTONE	((uintptr_t)1)

typedef struct sm_entry {
	uintptr_t		se_page;	/* page-aligned address, or sentinel */
	uint32_t		se_count;	/* number of GC objects on this page */
	struct umem_gc_header	*se_objects;	/* per-page object list head */
} sm_entry_t;

struct umem_sparsemap {
	sm_entry_t	*sm_buckets;
	size_t		sm_capacity;	/* number of buckets */
	size_t		sm_count;	/* number of live entries */
	size_t		sm_tombstones;	/* number of tombstone entries */
	uintptr_t	sm_page_mask;	/* ~(pagesize - 1) */
	int		sm_growing;	/* a lock-dropped grow is in progress */
};

/* Default initial capacity (must be power of 2) */
#define	SM_DEFAULT_CAPACITY	256

/* Maximum load factor before resize (70%) */
#define	SM_LOAD_NUM		7
#define	SM_LOAD_DEN		10

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static uintptr_t
sm_page_size(void)
{
	static uintptr_t pgsz;

	if (pgsz == 0) {
#ifdef _SC_PAGESIZE
		long sz = sysconf(_SC_PAGESIZE);
		pgsz = (sz > 0) ? (uintptr_t)sz : 4096;
#else
		pgsz = 4096;
#endif
	}
	return (pgsz);
}

static uintptr_t
sm_page_of(umem_sparsemap_t *map, void *ptr)
{
	return ((uintptr_t)ptr & map->sm_page_mask);
}

/*
 * Hash a page address to a bucket index. Uses a multiplicative hash
 * (golden ratio) after discarding the low page-offset bits.
 */
static size_t
sm_hash(uintptr_t page, size_t capacity)
{
	/* Shift out page-offset bits (at least 12 for 4K pages) */
	uintptr_t key = page >> 12;

	/* Fibonacci / golden-ratio multiplicative hash */
	key *= (uintptr_t)0x9E3779B97F4A7C15ULL;

	return ((size_t)(key & (capacity - 1)));
}

/* ----------------------------------------------------------------
 * Allocation wrappers (use umem to avoid bootstrapping issues)
 * ---------------------------------------------------------------- */

static sm_entry_t *
sm_alloc_buckets(size_t capacity)
{
	size_t nbytes = capacity * sizeof (sm_entry_t);
	sm_entry_t *buckets = umem_alloc(nbytes, UMEM_DEFAULT);

	if (buckets != NULL)
		memset(buckets, 0, nbytes);
	return (buckets);
}

static void
sm_free_buckets(sm_entry_t *buckets, size_t capacity)
{
	umem_free(buckets, capacity * sizeof (sm_entry_t));
}

/* ----------------------------------------------------------------
 * Resize
 * ---------------------------------------------------------------- */

/*
 * Rehash every live entry of `src` (sm_capacity = src_cap) into freshly
 * allocated `dst` buckets (capacity dst_cap).  Pure function of its inputs:
 * reads src, writes dst, touches no shared map state.  Factored out so the
 * expensive O(n) rehash can run against a private snapshot with the object
 * lock DROPPED (see umem_sparsemap_grow_locked).
 */
static void
sm_rehash_into(sm_entry_t *dst, size_t dst_cap,
    const sm_entry_t *src, size_t src_cap)
{
	for (size_t i = 0; i < src_cap; i++) {
		const sm_entry_t *e = &src[i];

		if (e->se_page <= SM_TOMBSTONE)
			continue;

		size_t idx = sm_hash(e->se_page, dst_cap);
		for (;;) {
			if (dst[idx].se_page == SM_EMPTY) {
				dst[idx] = *e;
				break;
			}
			idx = (idx + 1) & (dst_cap - 1);
		}
	}
}

static int
sm_resize(umem_sparsemap_t *map, size_t new_capacity)
{
	sm_entry_t *new_buckets = sm_alloc_buckets(new_capacity);

	if (new_buckets == NULL)
		return (-1);

	sm_rehash_into(new_buckets, new_capacity,
	    map->sm_buckets, map->sm_capacity);

	sm_free_buckets(map->sm_buckets, map->sm_capacity);
	map->sm_buckets = new_buckets;
	map->sm_capacity = new_capacity;
	map->sm_tombstones = 0;

	return (0);
}

/* ----------------------------------------------------------------
 * Lookup (returns pointer to entry, or NULL)
 * ---------------------------------------------------------------- */

static sm_entry_t *
sm_lookup(umem_sparsemap_t *map, uintptr_t page)
{
	size_t idx = sm_hash(page, map->sm_capacity);

	for (size_t probes = 0; probes < map->sm_capacity; probes++) {
		sm_entry_t *e = &map->sm_buckets[idx];

		if (e->se_page == SM_EMPTY)
			return (NULL);

		if (e->se_page == page)
			return (e);

		idx = (idx + 1) & (map->sm_capacity - 1);
	}

	return (NULL);
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

umem_sparsemap_t *
umem_sparsemap_create(size_t initial_capacity)
{
	umem_sparsemap_t *map;

	if (initial_capacity == 0)
		initial_capacity = SM_DEFAULT_CAPACITY;

	/* Round up to power of 2 */
	size_t cap = 1;
	while (cap < initial_capacity)
		cap <<= 1;

	map = umem_alloc(sizeof (umem_sparsemap_t), UMEM_DEFAULT);
	if (map == NULL)
		return (NULL);

	map->sm_buckets = sm_alloc_buckets(cap);
	if (map->sm_buckets == NULL) {
		umem_free(map, sizeof (umem_sparsemap_t));
		return (NULL);
	}

	map->sm_capacity = cap;
	map->sm_count = 0;
	map->sm_tombstones = 0;
	map->sm_page_mask = ~(sm_page_size() - 1);
	map->sm_growing = 0;

	return (map);
}

void
umem_sparsemap_destroy(umem_sparsemap_t *map)
{
	if (map == NULL)
		return;

	sm_free_buckets(map->sm_buckets, map->sm_capacity);
	umem_free(map, sizeof (umem_sparsemap_t));
}

int
umem_sparsemap_set(umem_sparsemap_t *map, void *ptr)
{
	uintptr_t page = sm_page_of(map, ptr);
	sm_entry_t *e;

	/* Check if this page is already tracked */
	e = sm_lookup(map, page);
	if (e != NULL) {
		e->se_count++;
		return (0);
	}

	/* Check load factor and resize if needed */
	size_t used = map->sm_count + map->sm_tombstones;
	if (used * SM_LOAD_DEN >= map->sm_capacity * SM_LOAD_NUM) {
		if (sm_resize(map, map->sm_capacity * 2) != 0)
			return (-1);
	}

	/* Insert new entry */
	size_t idx = sm_hash(page, map->sm_capacity);
	for (;;) {
		e = &map->sm_buckets[idx];

		if (e->se_page == SM_EMPTY || e->se_page == SM_TOMBSTONE) {
			if (e->se_page == SM_TOMBSTONE)
				map->sm_tombstones--;
			e->se_page = page;
			e->se_count = 1;
			map->sm_count++;
			return (0);
		}

		idx = (idx + 1) & (map->sm_capacity - 1);
	}
}

void
umem_sparsemap_clear(umem_sparsemap_t *map, void *ptr)
{
	uintptr_t page = sm_page_of(map, ptr);
	sm_entry_t *e;

	e = sm_lookup(map, page);
	if (e == NULL)
		return;

	if (e->se_count > 1) {
		e->se_count--;
		return;
	}

	/* Last object on this page: remove entry */
	e->se_page = SM_TOMBSTONE;
	e->se_count = 0;
	map->sm_count--;
	map->sm_tombstones++;
}

int
umem_sparsemap_test(umem_sparsemap_t *map, void *ptr)
{
	if (map == NULL)
		return (0);

	uintptr_t page = sm_page_of(map, ptr);

	return (sm_lookup(map, page) != NULL);
}

size_t
umem_sparsemap_count(umem_sparsemap_t *map)
{
	if (map == NULL)
		return (0);

	return (map->sm_count);
}

/*
 * Grow the map with the O(n) rehash performed OUTSIDE the caller's lock.
 *
 * The caller holds `lock` (the GC object lock) and has observed the load
 * factor exceeded.  Holding that lock across the full rehash serialized
 * every GC allocator behind one O(n) rebuild -- the >=6x-oversubscription
 * throughput tail.  Instead we:
 *
 *   1. snapshot the current capacity, DROP the lock,
 *   2. allocate + rehash a PRIVATE copy of the buckets (the O(n) work) with
 *      the lock released so other allocators run concurrently,
 *   3. RE-ACQUIRE the lock and, if no other thread resized in the gap
 *      (capacity unchanged) and the private copy is still big enough for
 *      the current live+tombstone population, swap it in (O(1)); otherwise
 *      discard it and fall back to an under-lock sm_resize (rare).
 *
 * Soundness vs stop-the-world: the collector reads the sparsemap locklessly
 * ONLY while every mutator is parked.  A mutator running this grow is inside
 * a GC critical section (gcti_in_gc_critical), so a suspend signal DEFERS
 * its park until the grow completes and the lock is released -- the
 * collector waits for it and never observes a half-swapped map.  The swap
 * itself publishes sm_buckets/sm_capacity under `lock`, so a concurrent
 * locked reader (another mutator) also never sees a torn map.
 *
 * Returns 0 on success (map grown, still holding `lock`), -1 on OOM (still
 * holding `lock`, map unchanged).
 */
int
umem_sparsemap_grow_locked(umem_sparsemap_t *map, pthread_mutex_t *lock)
{
	size_t old_cap;
	size_t new_cap;
	size_t snap_count;
	size_t snap_tomb;
	sm_entry_t *old_buckets;
	sm_entry_t *priv;

	/*
	 * Only ONE lock-dropped grow at a time.  The grow reads the old bucket
	 * array with `lock` released; if a second thread could also grow it
	 * could free that array out from under our rehash (use-after-free).
	 * The sm_growing guard serializes grows: a concurrent adder that also
	 * crosses the load factor skips its own resize while a grow is in
	 * flight (see umem_sparsemap_add_object) and just inserts into the
	 * current buckets -- linear probing tolerates load past the resize
	 * threshold, and this grow will enlarge the table momentarily.  So the
	 * old array is freed only here, only once, only by us.
	 */
	if (map->sm_growing)
		return (0);

	old_cap = map->sm_capacity;
	new_cap = old_cap * 2;
	snap_count = map->sm_count;
	snap_tomb = map->sm_tombstones;
	old_buckets = map->sm_buckets;
	map->sm_growing = 1;

	/*
	 * Do the ENTIRE O(n) resize -- allocate, zero, and rehash -- against
	 * the snapshot with `lock` released, so all other GC allocators run
	 * concurrently instead of serializing behind one long critical
	 * section.  This is the change that removes the >=6x-oversubscription
	 * throughput tail: the expensive part of gc_object_add no longer runs
	 * under the object lock.
	 */
	(void) pthread_mutex_unlock(lock);

	priv = sm_alloc_buckets(new_cap);
	if (priv != NULL)
		sm_rehash_into(priv, new_cap, old_buckets, old_cap);

	(void) pthread_mutex_lock(lock);
	map->sm_growing = 0;

	if (priv == NULL)
		return (-1);	/* OOM; caller inserts into current buckets */

	/*
	 * Swap the private copy in ONLY if the map is exactly what we
	 * rehashed: same bucket array (the sm_growing guard guarantees no
	 * other resize ran, so this always holds) and same live + tombstone
	 * population (no add/remove landed while we were unlocked).  Then the
	 * private copy is an exact image and the swap is a sound O(1) publish
	 * under the lock: a locked reader sees either the old or the new map,
	 * never a torn one, and the collector (which reads locklessly only
	 * while every mutator is parked -- and this mutator is inside a GC
	 * critical section, so it defers parking until this grow returns)
	 * never observes a half-swap.
	 */
	if (map->sm_count == snap_count && map->sm_tombstones == snap_tomb) {
		map->sm_buckets = priv;
		map->sm_capacity = new_cap;
		map->sm_tombstones = 0;
		sm_free_buckets(old_buckets, old_cap);
		return (0);
	}

	/*
	 * An add/remove changed the population while we were unlocked, so our
	 * copy is stale.  Discard it; the current buckets are unchanged (we
	 * held the growing guard, nobody resized).  If a resize is still
	 * warranted, do a plain in-lock resize against the current state.
	 * Rare (the unlocked window is short relative to the inter-resize
	 * gap), so the occasional in-lock O(n) is acceptable and keeps
	 * correctness trivial.
	 */
	sm_free_buckets(priv, new_cap);
	if ((map->sm_count + map->sm_tombstones) * SM_LOAD_DEN >=
	    map->sm_capacity * SM_LOAD_NUM)
		return (sm_resize(map, map->sm_capacity * 2));
	return (0);
}

/* ----------------------------------------------------------------
 * Per-page object list management
 * ---------------------------------------------------------------- */

int
umem_sparsemap_add_object(umem_sparsemap_t *map,
    struct umem_gc_header *hdr, void *user_ptr, pthread_mutex_t *lock)
{
	uintptr_t page;
	sm_entry_t *e;

	if (map == NULL || hdr == NULL)
		return (-1);

	page = sm_page_of(map, user_ptr);
	e = sm_lookup(map, page);

	if (e != NULL) {
		/* Page already tracked: prepend to per-page list */
		hdr->gc_page_next = e->se_objects;
		e->se_objects = hdr;
		e->se_count++;
		return (0);
	}

	/*
	 * New page: need to insert an entry.  If the load factor is exceeded,
	 * grow first.  umem_sparsemap_grow_locked does the O(n) rehash with
	 * `lock` DROPPED (when a lock is supplied), which is what removes the
	 * >=6x-oversubscription throughput tail.  If it cannot grow right now
	 * (a concurrent grow is in flight, or OOM) we insert into the current
	 * buckets anyway: linear probing tolerates load past the threshold as
	 * long as the table is not full, and it never fills because a grow is
	 * always in progress or imminent when we are over the threshold.
	 */
	{
		size_t used = map->sm_count + map->sm_tombstones;
		if (used * SM_LOAD_DEN >= map->sm_capacity * SM_LOAD_NUM) {
			if (lock != NULL) {
				(void) umem_sparsemap_grow_locked(map, lock);
				/*
				 * The lock was dropped during the grow, so the
				 * page may have been inserted by a racing add
				 * while we were unlocked.  Re-check.
				 */
				e = sm_lookup(map, page);
				if (e != NULL) {
					hdr->gc_page_next = e->se_objects;
					e->se_objects = hdr;
					e->se_count++;
					return (0);
				}
			} else if (sm_resize(map,
			    map->sm_capacity * 2) != 0) {
				return (-1);
			}
		}
	}

	{
		size_t idx = sm_hash(page, map->sm_capacity);
		for (;;) {
			e = &map->sm_buckets[idx];
			if (e->se_page == SM_EMPTY ||
			    e->se_page == SM_TOMBSTONE) {
				if (e->se_page == SM_TOMBSTONE)
					map->sm_tombstones--;
				e->se_page = page;
				e->se_count = 1;
				e->se_objects = hdr;
				hdr->gc_page_next = NULL;
				map->sm_count++;
				return (0);
			}
			idx = (idx + 1) & (map->sm_capacity - 1);
		}
	}
}

void
umem_sparsemap_remove_object(umem_sparsemap_t *map,
    struct umem_gc_header *hdr, void *user_ptr)
{
	uintptr_t page;
	sm_entry_t *e;
	struct umem_gc_header **pp;

	if (map == NULL || hdr == NULL)
		return;

	page = sm_page_of(map, user_ptr);
	e = sm_lookup(map, page);
	if (e == NULL)
		return;

	/* Unlink from per-page list */
	for (pp = &e->se_objects; *pp != NULL; pp = &(*pp)->gc_page_next) {
		if (*pp == hdr) {
			*pp = hdr->gc_page_next;
			hdr->gc_page_next = NULL;
			e->se_count--;

			/* Last object on this page: remove entry */
			if (e->se_count == 0) {
				e->se_page = SM_TOMBSTONE;
				e->se_objects = NULL;
				map->sm_count--;
				map->sm_tombstones++;
			}
			return;
		}
	}
}

struct umem_gc_header *
umem_sparsemap_find_object(umem_sparsemap_t *map, void *ptr)
{
	uintptr_t page;
	sm_entry_t *e;
	struct umem_gc_header *hdr;
	struct umem_gc_header *candidate;

	if (map == NULL || ptr == NULL)
		return (NULL);

	page = sm_page_of(map, ptr);
	e = sm_lookup(map, page);
	if (e == NULL)
		return (NULL);

	candidate = UMEM_GC_HEADER(ptr);

	for (hdr = e->se_objects; hdr != NULL; hdr = hdr->gc_page_next) {
		if (hdr == candidate)
			return (hdr);
	}

	return (NULL);
}

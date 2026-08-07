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
};

/* Default initial capacity (must be power of 2) */
#define	SM_DEFAULT_CAPACITY	4096

/* Maximum load factor before resize (70%) */
#define	SM_LOAD_NUM		7
#define	SM_LOAD_DEN		10

/*
 * Growth factor on resize.  The rehash in sm_resize() is O(capacity) and
 * runs while the caller holds gc_objects_lock, so under high allocator
 * concurrency (the GC's >=6x-oversubscription tail) it serializes peers.
 * Growing 4x per resize (rather than 2x) reaches steady-state capacity in
 * half as many resizes and halves the number of O(n) lock holds, cutting
 * the tail, at the cost of a modestly larger table.  Sound and risk-free:
 * resize never runs concurrently with the collector (STW marks only while
 * every mutator is parked; a resizing mutator is in a GC critical section
 * that defers parking), so this only affects mutator-vs-mutator contention.
 */
#define	SM_GROWTH_SHIFT		2	/* new_cap = cap << 2  (4x) */

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

static int
sm_resize(umem_sparsemap_t *map, size_t new_capacity)
{
	sm_entry_t *new_buckets = sm_alloc_buckets(new_capacity);

	if (new_buckets == NULL)
		return (-1);

	/* Rehash all live entries */
	for (size_t i = 0; i < map->sm_capacity; i++) {
		sm_entry_t *e = &map->sm_buckets[i];

		if (e->se_page <= SM_TOMBSTONE)
			continue;

		size_t idx = sm_hash(e->se_page, new_capacity);
		for (;;) {
			if (new_buckets[idx].se_page == SM_EMPTY) {
				new_buckets[idx] = *e;
				break;
			}
			idx = (idx + 1) & (new_capacity - 1);
		}
	}

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
		if (sm_resize(map, map->sm_capacity << SM_GROWTH_SHIFT) != 0)
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

/* ----------------------------------------------------------------
 * Per-page object list management
 * ---------------------------------------------------------------- */

int
umem_sparsemap_add_object(umem_sparsemap_t *map,
    struct umem_gc_header *hdr, void *user_ptr)
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

	/* New page: need to insert entry */
	{
		size_t used = map->sm_count + map->sm_tombstones;
		if (used * SM_LOAD_DEN >= map->sm_capacity * SM_LOAD_NUM) {
			if (sm_resize(map, map->sm_capacity << SM_GROWTH_SHIFT) != 0)
				return (-1);
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

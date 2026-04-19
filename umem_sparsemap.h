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

#ifndef _UMEM_SPARSEMAP_H
#define	_UMEM_SPARSEMAP_H

/*
 * umem_sparsemap.h - Page-granularity hash map for O(1) GC pointer lookup
 *
 * Maps page-aligned addresses to reference counts of GC objects residing
 * on that page. Used by the GC's find_header() to quickly reject pointers
 * that cannot be GC-managed (because no GC objects live on their page).
 *
 * Thread safety: callers must hold the GC objects lock when mutating.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct umem_sparsemap umem_sparsemap_t;

/*
 * Create a new sparsemap with an initial capacity (number of buckets).
 * Returns NULL on allocation failure.
 */
umem_sparsemap_t *umem_sparsemap_create(size_t initial_capacity);

/*
 * Destroy a sparsemap and free all associated memory.
 */
void umem_sparsemap_destroy(umem_sparsemap_t *map);

/*
 * Increment the reference count for the page containing ptr.
 * Called when a GC object is allocated.
 * Returns 0 on success, -1 on failure (out of memory).
 */
int umem_sparsemap_set(umem_sparsemap_t *map, void *ptr);

/*
 * Decrement the reference count for the page containing ptr.
 * Called when a GC object is freed. Removes the entry when count
 * reaches zero.
 */
void umem_sparsemap_clear(umem_sparsemap_t *map, void *ptr);

/*
 * Test whether the page containing ptr has any GC objects.
 * Returns non-zero if the page is tracked (has GC objects), 0 otherwise.
 * This is the fast-path check used by find_header().
 */
int umem_sparsemap_test(umem_sparsemap_t *map, void *ptr);

/*
 * Return the number of pages currently tracked.
 */
size_t umem_sparsemap_count(umem_sparsemap_t *map);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_SPARSEMAP_H */

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

#ifndef _UMEM_ARENA_H
#define _UMEM_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Scoped Arena Allocator
 *
 * Bump-pointer allocator backed by a single mmap region.
 * Designed for single-threaded use within a scope where many small
 * allocations are made and then freed in bulk.
 *
 * Thread safety: An arena is NOT thread-safe. Each arena should be
 * used by exactly one thread. If shared access is needed, the caller
 * must provide external synchronization.
 *
 * Typical usage:
 *     umem_arena_t *a = umem_arena_create(65536, 0);
 *     void *p1 = umem_arena_alloc(a, 100);
 *     void *p2 = umem_arena_alloc(a, 200);
 *     umem_arena_reset(a);   // instant bulk free
 *     void *p3 = umem_arena_alloc(a, 50);  // reuses space
 *     umem_arena_destroy(a); // munmap backing memory
 */

typedef struct umem_arena umem_arena_t;

/*
 * Create an arena with the given capacity (bytes).
 * Capacity is rounded up to page size. Backing memory is obtained
 * via mmap. Returns NULL on failure.
 * Flags: UMEM_DEFAULT or UMEM_NOFAIL (calls exit on failure).
 */
umem_arena_t *umem_arena_create(size_t capacity, int flags);

/*
 * Allocate size bytes from the arena.
 * Returns a 16-byte-aligned pointer, or NULL if the arena is exhausted.
 */
void *umem_arena_alloc(umem_arena_t *arena, size_t size);

/*
 * Reset the arena: set offset back to 0.
 * All previously returned pointers become invalid.
 */
void umem_arena_reset(umem_arena_t *arena);

/*
 * Destroy the arena and release all backing memory.
 */
void umem_arena_destroy(umem_arena_t *arena);

/*
 * Query how many bytes remain available in the arena.
 */
size_t umem_arena_available(const umem_arena_t *arena);

/*
 * Query the total capacity of the arena.
 */
size_t umem_arena_capacity(const umem_arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_ARENA_H */

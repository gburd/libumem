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
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "umem_arena.h"
#include "umem.h"

#define ARENA_ALIGN	16	/* allocation alignment within the arena */

struct umem_arena {
	char	*base;		/* mmap'd region base */
	size_t	capacity;	/* total usable bytes */
	size_t	offset;		/* current bump pointer offset */
};

/*
 * Round up to the system page size.
 *
 * Returns 0 if the rounding would wrap past SIZE_MAX -- callers MUST treat 0
 * as failure.  Rounding SIZE_MAX up to a page used to yield 0, so
 * umem_arena_create(SIZE_MAX) attempted a zero-length mmap.
 */
static size_t
arena_page_round(size_t sz)
{
	long pgsz = sysconf(_SC_PAGESIZE);
	size_t p;

	if (pgsz <= 0) {
		pgsz = 4096;
	}
	p = (size_t)pgsz;

	if (sz > SIZE_MAX - (p - 1)) {
		return (0);
	}
	return ((sz + p - 1) & ~(p - 1));
}

umem_arena_t *
umem_arena_create(size_t capacity, int flags)
{
	umem_arena_t *arena;
	size_t map_size;
	void *mem;

	if (capacity == 0) {
		return (NULL);
	}

	/*
	 * Allocate the arena header via umem so it participates in
	 * the normal allocation hierarchy.
	 */
	arena = (umem_arena_t *)umem_alloc(sizeof(umem_arena_t), flags);
	if (arena == NULL) {
		return (NULL);
	}

	map_size = arena_page_round(capacity);
	if (map_size == 0) {
		/* capacity so large that page rounding would wrap */
		umem_free(arena, sizeof(umem_arena_t));
		if (flags & UMEM_NOFAIL) {
			exit(1);
		}
		return (NULL);
	}
	mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		umem_free(arena, sizeof(umem_arena_t));
		if (flags & UMEM_NOFAIL) {
			exit(1);
		}
		return (NULL);
	}

	arena->base = (char *)mem;
	arena->capacity = map_size;
	arena->offset = 0;

	return (arena);
}

void *
umem_arena_alloc(umem_arena_t *arena, size_t size)
{
	size_t aligned_size;
	size_t new_offset;
	void *ptr;

	if (arena == NULL || size == 0) {
		return (NULL);
	}

	if (size > SIZE_MAX - (ARENA_ALIGN - 1))
		return (NULL);
	aligned_size = (size + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);

	/*
	 * The addition must be CHECKED, not merely its result compared with
	 * capacity: offset + aligned_size used to wrap, so a request of
	 * SIZE_MAX-15 after a 32-byte allocation produced new_offset == 16,
	 * passed the capacity test, returned base+32 and moved the bump
	 * pointer BACKWARD -- handing the same bytes out twice.
	 *
	 * Equivalent to (offset + aligned_size > capacity) in exact
	 * arithmetic, arranged so no intermediate can wrap.  offset <=
	 * capacity is an invariant of this structure.
	 */
	if (aligned_size > arena->capacity - arena->offset) {
		return (NULL);
	}
	new_offset = arena->offset + aligned_size;

	if (new_offset > arena->capacity) {
		return (NULL);
	}

	ptr = arena->base + arena->offset;
	arena->offset = new_offset;
	return (ptr);
}

void
umem_arena_reset(umem_arena_t *arena)
{
	if (arena != NULL) {
		arena->offset = 0;
	}
}

void
umem_arena_destroy(umem_arena_t *arena)
{
	if (arena == NULL) {
		return;
	}

	if (arena->base != NULL) {
		(void)munmap(arena->base, arena->capacity);
	}

	umem_free(arena, sizeof(umem_arena_t));
}

size_t
umem_arena_available(const umem_arena_t *arena)
{
	if (arena == NULL) {
		return (0);
	}
	return (arena->capacity - arena->offset);
}

size_t
umem_arena_capacity(const umem_arena_t *arena)
{
	if (arena == NULL) {
		return (0);
	}
	return (arena->capacity);
}

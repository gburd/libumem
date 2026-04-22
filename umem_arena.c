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
 */
static size_t
arena_page_round(size_t sz)
{
	long pgsz = sysconf(_SC_PAGESIZE);

	if (pgsz <= 0) {
		pgsz = 4096;
	}
	return ((sz + (size_t)pgsz - 1) & ~((size_t)pgsz - 1));
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

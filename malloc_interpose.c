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
 * malloc interposition via dlsym(RTLD_NEXT)
 *
 * This file implements lazy initialization of libumem to avoid the
 * pthread_create/malloc circular dependency deadlock. The approach is
 * based on patterns used by profiling tools (valgrind, ASan) and
 * recommended in docs/PTHREAD_RESEARCH.md section 3.
 *
 * Key design:
 * 1. Use dlsym(RTLD_NEXT) to get real libc malloc
 * 2. State machine: UNINIT → BOOTSTRAP → READY
 * 3. Route early calls to bootstrap allocator (mmap-based)
 * 4. Track pointer ownership to know which free path to use
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"
#include <dlfcn.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <pthread.h>
#include <stdatomic.h>

#include "umem_impl.h"
#include "malloc_guard.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* External: umem readiness state and functions */
#define	UMEM_READY_STARTUP	1
#define	UMEM_READY		3
extern int umem_ready;
extern int umem_init(void);
extern void *umem_malloc(size_t);
extern void umem_malloc_free(void *);

/* Bootstrap allocator functions from malloc.c */
extern void *bootstrap_malloc(size_t);
extern void bootstrap_free(void *);
extern int is_bootstrap_pointer(void *);

/* Bootstrap header structure (from malloc.c) */
#define BOOTSTRAP_MAGIC 0xB007B007B007B007ULL
typedef struct bootstrap_header {
	uint64_t magic;
	size_t size;
} bootstrap_header_t;

/* State machine for interposition */
typedef enum {
	INTERPOSE_UNINIT,	/* Before first malloc call */
	INTERPOSE_BOOTSTRAP,	/* Resolving libc or initializing umem */
	INTERPOSE_READY		/* Fully initialized, use umem */
} interpose_state_t;

/* Direct initialization - ATOMIC_VAR_INIT deprecated in C17 */
static atomic_int interpose_state = INTERPOSE_UNINIT;

/* Pointers to real libc functions */
static void *(*libc_malloc)(size_t) = NULL;
static void (*libc_free)(void *) = NULL;
static void *(*libc_calloc)(size_t, size_t) = NULL;
static void *(*libc_realloc)(void *, size_t) = NULL;
static void *(*libc_memalign)(size_t, size_t) = NULL;

/*
 * Pointer tracking: track which allocations came from libc
 * during bootstrap phase so we can free them correctly.
 *
 * THREAD SAFETY: These functions are protected by a mutex to prevent
 * races when tracking/checking libc pointers from multiple threads.
 */
#define MAX_BOOTSTRAP_PTRS 512
static void *bootstrap_ptrs[MAX_BOOTSTRAP_PTRS];
static size_t bootstrap_ptr_count = 0;
static pthread_mutex_t bootstrap_ptr_lock = PTHREAD_MUTEX_INITIALIZER;

static void
track_bootstrap_ptr(void *ptr)
{
	if (ptr == NULL)
		return;

	(void) pthread_mutex_lock(&bootstrap_ptr_lock);
	if (bootstrap_ptr_count < MAX_BOOTSTRAP_PTRS) {
		bootstrap_ptrs[bootstrap_ptr_count++] = ptr;
	}
	(void) pthread_mutex_unlock(&bootstrap_ptr_lock);
}

static int
is_libc_pointer(void *ptr)
{
	size_t i;
	int found = 0;

	if (ptr == NULL)
		return (0);

	(void) pthread_mutex_lock(&bootstrap_ptr_lock);
	for (i = 0; i < bootstrap_ptr_count; i++) {
		if (bootstrap_ptrs[i] == ptr) {
			bootstrap_ptrs[i] = NULL;  /* Clear slot */
			found = 1;
			break;
		}
	}
	(void) pthread_mutex_unlock(&bootstrap_ptr_lock);

	return (found);
}

/*
 * Get the usable size of a bootstrap allocation.
 * Bootstrap allocations have a header before the returned pointer.
 */
static size_t
get_bootstrap_size(void *ptr)
{
	bootstrap_header_t *hdr;

	if (ptr == NULL)
		return (0);

	hdr = (bootstrap_header_t *)ptr - 1;
	if (hdr->magic != BOOTSTRAP_MAGIC)
		return (0);

	/* Return data size (total size minus header) */
	return (hdr->size - sizeof(bootstrap_header_t));
}

/*
 * Static buffer for dlsym allocations.
 * dlsym may call calloc internally, which creates a circular dependency.
 * We use a static buffer to handle these allocations.
 */
#define DLSYM_BUFFER_SIZE 1024
static char dlsym_buffer[DLSYM_BUFFER_SIZE];
static size_t dlsym_buffer_used = 0;
static int in_dlsym = 0;

/*
 * Static buffer for calloc recursion.
 * During pthread_create, allocate_dtv() calls calloc() for TLS initialization.
 * Our calloc() calls malloc(), which may trigger another calloc() through
 * memset() or TLS operations, creating infinite recursion.
 * We use a separate static buffer to break this cycle.
 *
 * NOTE: in_calloc is NOT __thread because we use it to detect TLS initialization.
 * Using __thread here would create a chicken-and-egg problem: we'd need TLS
 * to be initialized to access in_calloc, but we're using in_calloc to handle
 * the malloc calls that happen during TLS initialization.
 */
#define CALLOC_BUFFER_SIZE 2048
static char calloc_buffer[CALLOC_BUFFER_SIZE];
static size_t calloc_buffer_used = 0;
static volatile int in_calloc = 0;

/*
 * Resolve libc malloc functions using dlsym(RTLD_NEXT)
 */
static void
resolve_libc_functions(void)
{
	in_dlsym = 1;
	libc_malloc = dlsym(RTLD_NEXT, "malloc");
	libc_free = dlsym(RTLD_NEXT, "free");
	libc_calloc = dlsym(RTLD_NEXT, "calloc");
	libc_realloc = dlsym(RTLD_NEXT, "realloc");
	libc_memalign = dlsym(RTLD_NEXT, "memalign");
	in_dlsym = 0;
}

/*
 * Constructor: Resolve libc functions before any malloc calls
 * This is called automatically when the library is loaded via LD_PRELOAD.
 *
 * IMPORTANT: This constructor MUST run before libumem's __umem_init constructor,
 * because __umem_init may call pthread functions which call malloc. We use
 * priority 101 to ensure this runs first (lower priority numbers run first).
 *
 * Cross-.so note: numbered constructor priorities only order constructors
 * WITHIN a single shared object; they do not order across the .so boundary.
 * malloc_interpose lives in libumem_malloc.so and __umem_init in libumem.so,
 * so ordering is actually governed by load order (the LD_PRELOAD'd
 * libumem_malloc.so loads before libumem.so is pulled in as its NEEDED
 * dependency).  The numeric priority is therefore a no-op across the
 * boundary -- and the Solaris/illumos ld has no numbered .init_array, so we
 * use the plain constructor form there.
 *
 * NOTE: We do NOT call umem_init() here because that would trigger
 * pthread_create which calls malloc, creating a deadlock. Instead,
 * we let malloc calls during the bootstrap phase use the bootstrap
 * allocator, and umem will be initialized lazily on first use.
 */
#if defined(__sun) || defined(__SVR4)
__attribute__((constructor))
#else
__attribute__((constructor(101)))
#endif
static void
umem_interpose_init(void)
{
	/* Tell libumem we're acting as the process malloc.  This
	 * disables backtrace(3)-based stack capture in the slab
	 * paths, which would otherwise dlopen libgcc_s through us
	 * and recurse fatally. */
	extern int umem_malloc_is_interposing;
	umem_malloc_is_interposing = 1;

	/* Resolve libc functions */
	atomic_store(&interpose_state, INTERPOSE_BOOTSTRAP);
	resolve_libc_functions();

	/*
	 * Disable abort on recoverable errors in interpose mode.
	 * When LD_PRELOAD is used, we may encounter pointers from:
	 * - libc malloc (allocated before libumem loaded)
	 * - Shared libraries with their own allocators
	 * - System libraries (zlib, etc.)
	 *
	 * Rather than crashing on invalid frees, log errors and continue.
	 * This matches glibc malloc's behavior and is safer for LD_PRELOAD.
	 */
	extern uint_t umem_abort;
	umem_abort = 0;

	/*
	 * Don't call umem_init() here - let it initialize naturally
	 * through the first umem API call. The bootstrap allocator will
	 * handle any malloc calls that happen during initialization.
	 */
}

/*
 * malloc - main interposition point
 */
void *
malloc(size_t size)
{
	void *ret;

	/*
	 * Handle dlsym's malloc calls with static buffer.
	 * dlsym may call malloc/calloc internally, so we provide
	 * a temporary buffer to avoid infinite recursion.
	 */
	if (in_dlsym) {
		size_t aligned_size = (size + 15) & ~15;  /* 16-byte align */
		if (dlsym_buffer_used + aligned_size <= DLSYM_BUFFER_SIZE) {
			ret = &dlsym_buffer[dlsym_buffer_used];
			dlsym_buffer_used += aligned_size;
			return (ret);
		}
		/* Buffer exhausted - this shouldn't happen */
		return (NULL);
	}

	/*
	 * Trigger umem initialization if startup is complete but
	 * umem_init() hasn't been called yet. This breaks the
	 * chicken-and-egg problem: umem_init() is normally called
	 * by umem_alloc(), but malloc interposition never calls
	 * umem_alloc() while in BOOTSTRAP state, so umem never
	 * initializes.
	 *
	 * During umem_init(), recursive malloc() calls will still
	 * use bootstrap_malloc() because interpose_state remains
	 * BOOTSTRAP until init completes.
	 */
	if (atomic_load(&interpose_state) == INTERPOSE_BOOTSTRAP) {
		static volatile int umem_init_attempted = 0;

		if (umem_ready == UMEM_READY_STARTUP &&
		    !umem_init_attempted) {
			umem_init_attempted = 1;
			(void) umem_init();
		}

		if (umem_ready == UMEM_READY) {
			int expected = INTERPOSE_BOOTSTRAP;
			atomic_compare_exchange_strong(&interpose_state,
			    &expected, INTERPOSE_READY);
		}
	}

	/* Fast path: fully initialized */
	if (__builtin_expect(atomic_load(&interpose_state) == INTERPOSE_READY, 1)) {
		return (umem_malloc(size));
	}

	/*
	 * Pre-constructor or during umem_init(): use bootstrap allocator.
	 * This handles recursive malloc calls during initialization.
	 */
	return (bootstrap_malloc(size));
}

/*
 * free - must handle mixed allocation sources
 */
void
free(void *ptr)
{
	if (ptr == NULL)
		return;

	/*
	 * Check if this is from the dlsym static buffer.
	 * These allocations cannot be freed.
	 */
	if (ptr >= (void *)dlsym_buffer &&
	    ptr < (void *)(dlsym_buffer + DLSYM_BUFFER_SIZE)) {
		/* Ignore frees of dlsym buffer allocations */
		return;
	}

	/*
	 * Check if this is from the calloc recursion buffer.
	 * These allocations cannot be freed.
	 */
	if (ptr >= (void *)calloc_buffer &&
	    ptr < (void *)(calloc_buffer + CALLOC_BUFFER_SIZE)) {
		/* Ignore frees of calloc buffer allocations */
		return;
	}

	/*
	 * Check if this is a bootstrap allocation.
	 * Bootstrap allocations use mmap with a magic header.
	 */
	if (is_bootstrap_pointer(ptr)) {
		bootstrap_free(ptr);
		return;
	}

	/*
	 * Check if this came from libc during bootstrap phase
	 */
	if (is_libc_pointer(ptr)) {
		if (libc_free != NULL)
			libc_free(ptr);
		return;
	}

	/*
	 * If we're not ready yet and it's not a tracked pointer,
	 * be defensive and try libc_free
	 */
	if (atomic_load(&interpose_state) != INTERPOSE_READY) {
		if (libc_free != NULL)
			libc_free(ptr);
		return;
	}

	/*
	 * Normal umem free.
	 * If this is an invalid pointer (from libc, zlib, etc.),
	 * umem_err_recoverable() will log an error but won't crash
	 * because we set umem_abort = 0 in umem_interpose_init().
	 */
	umem_malloc_free(ptr);
}

/*
 * calloc - allocate and zero
 */
void *
calloc(size_t nelem, size_t elsize)
{
	size_t size;
	void *ret;

	/* Check for overflow BEFORE any size calculations */
	if (nelem > 0 && elsize > 0) {
		if (SIZE_MAX / elsize < nelem) {
			errno = ENOMEM;
			return (NULL);
		}
	}

	size = nelem * elsize;

	/*
	 * Handle dlsym's calloc calls with static buffer.
	 * dlsym may call calloc internally on some systems.
	 */
	if (in_dlsym) {
		size_t aligned_size = (size + 15) & ~15;  /* 16-byte align */
		/* Also check that alignment doesn't overflow */
		if (aligned_size < size) {
			errno = ENOMEM;
			return (NULL);
		}
		if (dlsym_buffer_used + aligned_size <= DLSYM_BUFFER_SIZE) {
			ret = &dlsym_buffer[dlsym_buffer_used];
			dlsym_buffer_used += aligned_size;
			(void) memset(ret, 0, size);
			return (ret);
		}
		/* Buffer exhausted */
		return (NULL);
	}

	/*
	 * Handle recursive calloc during pthread TLS initialization.
	 * pthread_create -> allocate_dtv() -> calloc() -> malloc() ->
	 * memset/TLS ops -> calloc() creates infinite recursion.
	 * Use static buffer to break the cycle.
	 */
	if (in_calloc > 0) {
		size_t aligned_size = (size + 15) & ~15;  /* 16-byte align */
		/* Also check that alignment doesn't overflow */
		if (aligned_size < size) {
			errno = ENOMEM;
			return (NULL);
		}
		if (calloc_buffer_used + aligned_size <= CALLOC_BUFFER_SIZE) {
			ret = &calloc_buffer[calloc_buffer_used];
			calloc_buffer_used += aligned_size;
			(void) memset(ret, 0, size);
			return (ret);
		}
		/* Buffer exhausted */
		return (NULL);
	}

	/*
	 * Set recursion guard before calling malloc() and memset().
	 * This prevents infinite recursion during pthread TLS initialization.
	 */
	in_calloc = 1;
	ret = malloc(size);
	if (ret != NULL)
		(void) memset(ret, 0, size);
	in_calloc = 0;

	/*
	 * Reset calloc buffer after TLS initialization completes.
	 * Allocations from calloc_buffer can never be freed (static buffer),
	 * and are only used during pthread TLS setup. Once in_calloc returns
	 * to 0, TLS init is complete and we can reuse the buffer for the
	 * next thread. This fixes the bug where creating 8+ threads would
	 * exhaust the 2KB buffer and cause pthread_create to fail.
	 */
	if (calloc_buffer_used > 0 && in_calloc == 0) {
		calloc_buffer_used = 0;
	}

	return (ret);
}

/*
 * realloc - resize allocation
 */
void *
realloc(void *ptr, size_t size)
{
	void *new_ptr;
	size_t old_size;

	if (ptr == NULL)
		return (malloc(size));

	if (size == 0) {
		free(ptr);
		return (NULL);
	}

	/*
	 * Check for bootstrap pointers first.
	 * These can exist even in READY state if they were allocated
	 * during bootstrap phase and never freed.
	 */
	if (is_bootstrap_pointer(ptr)) {
		old_size = get_bootstrap_size(ptr);
		if (old_size == 0) {
			/* Header corrupted or invalid */
			errno = EINVAL;
			return (NULL);
		}

		new_ptr = malloc(size);
		if (new_ptr == NULL)
			return (NULL);

		/*
		 * Copy old data to new buffer.
		 * Use the smaller of old_size or new size to avoid overruns.
		 */
		(void) memcpy(new_ptr, ptr, MIN(old_size, size));

		/*
		 * Free the old buffer AFTER copying is complete.
		 * The order is critical to ensure data integrity.
		 */
		free(ptr);
		return (new_ptr);
	}

	if (is_libc_pointer(ptr)) {
		new_ptr = malloc(size);
		if (new_ptr == NULL)
			return (NULL);

#ifdef HAVE_MALLOC_USABLE_SIZE
		old_size = malloc_usable_size(ptr);
		(void) memcpy(new_ptr, ptr, MIN(old_size, size));
#else
		/*
		 * Without malloc_usable_size (e.g. Solaris), we cannot
		 * determine the old allocation size.  Copy up to `size`
		 * bytes, which is safe: the new buffer is at least `size`
		 * bytes, and libc pointers only appear during early
		 * bootstrap when allocations are small.
		 */
		(void) memcpy(new_ptr, ptr, size);
#endif
		free(ptr);
		return (new_ptr);
	}

	/*
	 * In READY state, try umem's process_free to get the size.
	 * If that fails, the pointer is invalid.
	 */
	if (__builtin_expect(atomic_load(&interpose_state) == INTERPOSE_READY, 1)) {
		extern int process_free(void *, int, size_t *);

		if (process_free(ptr, 0, &old_size) == 0) {
			/*
			 * Pointer is invalid or corrupted.
			 * Don't fall through to malloc_usable_size as that
			 * reads libc malloc metadata, not umem metadata.
			 */
			errno = EINVAL;
			return (NULL);
		}

		/* Valid umem pointer */
		if (size == old_size)
			return (ptr);

		new_ptr = malloc(size);
		if (new_ptr == NULL)
			return (NULL);

		(void) memcpy(new_ptr, ptr, MIN(old_size, size));
		free(ptr);
		return (new_ptr);
	}

	/*
	 * Bootstrap phase fallback.
	 * This path should only be reached during bootstrap phase.
	 */
	new_ptr = malloc(size);
	if (new_ptr == NULL)
		return (NULL);

#ifdef HAVE_MALLOC_USABLE_SIZE
	old_size = malloc_usable_size(ptr);
	(void) memcpy(new_ptr, ptr, MIN(old_size, size));
#else
	(void) memcpy(new_ptr, ptr, size);
#endif
	free(ptr);
	return (new_ptr);
}

/*
 * memalign - allocate aligned memory
 */
void *
memalign(size_t align, size_t size)
{
	void *ret;

	/* Validate alignment */
	if (align == 0 || (align & (align - 1)) != 0) {
		errno = EINVAL;
		return (NULL);
	}

	/* Fast path: fully initialized */
	if (__builtin_expect(atomic_load(&interpose_state) == INTERPOSE_READY, 1)) {
		extern void *umem_memalign(size_t, size_t);
		return (umem_memalign(align, size));
	}

	/*
	 * Bootstrap phase: use libc_memalign if available,
	 * otherwise use regular malloc (may not be aligned as requested)
	 */
	if (atomic_load(&interpose_state) == INTERPOSE_BOOTSTRAP && libc_memalign != NULL) {
		ret = libc_memalign(align, size);
		if (ret != NULL)
			track_bootstrap_ptr(ret);
		return (ret);
	}

	/* Fall back to malloc */
	return (malloc(size));
}

/*
 * posix_memalign - POSIX aligned allocation
 */
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *ptr;

	ptr = memalign(alignment, size);
	if (ptr != NULL) {
		*memptr = ptr;
		return (0);
	}

	return (errno);
}

/*
 * valloc - page-aligned allocation
 */
void *
valloc(size_t size)
{
	extern size_t pagesize;
	return (memalign(pagesize, size));
}

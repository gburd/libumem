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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/* #pragma ident	"@(#)malloc.c	1.5	05/06/08 SMI" */

#include "config.h"
#include <unistd.h>

#include <errno.h>

#include <string.h>

#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#include "umem_base.h"
#include "umem_impl.h"

#include "misc.h"
#include "malloc_guard.h"

/* External: umem readiness state */
extern int umem_ready;

#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

/*
 * Bootstrap allocator: Used during early initialization to avoid deadlock
 * when pthread_create/pthread_setspecific call malloc before umem is ready.
 *
 * Based on jemalloc's arena 0 and tcmalloc's Arena pattern:
 * - Uses direct mmap (no umem dependency)
 * - No TLS access required
 * - Marked allocations for detection during free
 *
 * This breaks the circular dependency:
 *   pthread_create -> malloc -> umem_init -> pthread_once -> malloc
 */
#define BOOTSTRAP_MAGIC 0xB007B007B007B007ULL

typedef struct bootstrap_header {
	uint64_t magic;
	size_t size;
} bootstrap_header_t;


/*
 * Exposed for malloc_interpose.c
 * These functions are used during bootstrap phase when umem is not yet ready.
 */
void *
bootstrap_malloc(size_t size)
{
	bootstrap_header_t *hdr;
	size_t total_size = size + sizeof(bootstrap_header_t);

#ifdef _WIN32
	hdr = (bootstrap_header_t *)VirtualAlloc(NULL, total_size,
	    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (hdr == NULL)
		return (NULL);
#else
	hdr = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (hdr == MAP_FAILED)
		return (NULL);
#endif

	hdr->magic = BOOTSTRAP_MAGIC;
	hdr->size = total_size;
	return (void *)(hdr + 1);
}

int
is_bootstrap_pointer(void *buf)
{
	bootstrap_header_t *hdr;

	if (buf == NULL)
		return (0);

	hdr = (bootstrap_header_t *)buf - 1;
	return (hdr->magic == BOOTSTRAP_MAGIC);
}

void
bootstrap_free(void *buf)
{
	bootstrap_header_t *hdr;

	if (buf == NULL)
		return;

	hdr = (bootstrap_header_t *)buf - 1;
	if (hdr->magic != BOOTSTRAP_MAGIC)
		return;

#ifdef _WIN32
	(void) VirtualFree(hdr, 0, MEM_RELEASE);
#else
	(void) munmap(hdr, hdr->size);
#endif
}

/*
 * malloc_data_t is an 8-byte structure which is located "before" the pointer
 * returned from {m,c,re}alloc and memalign.  The first four bytes give
 * information about the buffer, and the second four bytes are a status byte.
 *
 * See umem_impl.h for the various magic numbers used, and the size
 * encode/decode macros.
 *
 * The 'size' of the buffer includes the tags.  That is, we encode the
 * argument to umem_alloc(), not the argument to malloc().
 */

typedef struct malloc_data {
	uint32_t malloc_size;
	uint32_t malloc_stat; /* = UMEM_MALLOC_ENCODE(state, malloc_size) */
} malloc_data_t;

/*
 * NOTE: malloc/free interposition is now handled by malloc_interpose.c
 * which uses dlsym(RTLD_NEXT) to avoid pthread_create deadlocks.
 * The weak symbol pragmas have been removed.
 */

/*
 * On Linux, the PTC genasm code sets these function pointers after
 * generating the per-thread-cache assembly.  malloc() and free() check
 * these and dispatch through them when set.
 */

/*
 * umem_malloc: the real malloc implementation.
 *
 * This is the function that PTC falls back to for allocations it
 * cannot handle (overflow, oversized, etc.).
 * On non-x86, malloc is a weak alias to this function.
 */
void *
umem_malloc(size_t size_arg)
{
#ifdef _LP64
	uint32_t high_size = 0;
#endif
	size_t size;
	malloc_data_t *ret;

	/*
	 * Use bootstrap allocator if umem is not fully initialized.
	 * Based on jemalloc's arena 0 pattern - provides emergency allocation
	 * during initialization without requiring TLS or umem infrastructure.
	 */
	if (umem_ready != UMEM_READY)
		return (bootstrap_malloc(size_arg));

	/*
	 * Check for recursive malloc call (e.g., pthread_create -> malloc ->
	 * pthread_getspecific -> malloc). Use bootstrap allocator to break
	 * the cycle. This uses initial-exec TLS for single-instruction access.
	 */
	if (umem_enter_malloc() > 0) {
		umem_exit_malloc();
		return (bootstrap_malloc(size_arg));
	}

	size = size_arg + sizeof (malloc_data_t);

#ifdef _LP64
	if (size > UMEM_SECOND_ALIGN) {
		size += sizeof (malloc_data_t);
		high_size = (size >> 32);
	}
#endif
	if (size < size_arg) {
		umem_exit_malloc();
		errno = ENOMEM;			/* overflow */
		return (NULL);
	}
	ret = (malloc_data_t *)_umem_alloc(size, UMEM_DEFAULT);
	if (ret == NULL) {
		umem_exit_malloc();
		if (size <= UMEM_MAXBUF)
			errno = EAGAIN;
		else
			errno = ENOMEM;
		return (NULL);
#ifdef _LP64
	} else if (high_size > 0) {
		uint32_t low_size = (uint32_t)size;

		/*
		 * uses different magic numbers to make it harder to
		 * undetectably corrupt
		 */
		ret->malloc_size = high_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, high_size);
		ret++;

		ret->malloc_size = low_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_OVERSIZE_MAGIC,
		    low_size);
		ret++;
	} else if (size > UMEM_SECOND_ALIGN) {
		uint32_t low_size = (uint32_t)size;

		ret++; /* leave the first 8 bytes alone */

		ret->malloc_size = low_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_SECOND_MAGIC,
		    low_size);
		ret++;
#endif
	} else {
		ret->malloc_size = size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, size);
		ret++;
	}

	umem_exit_malloc();
	return ((void *)ret);
}

/*
 * NOTE: calloc() is now in malloc_interpose.c
 */

/*
 * umem_memalign: internal memalign implementation
 * Used by malloc_interpose.c when umem is fully initialized
 */
void *
umem_memalign(size_t align, size_t size_arg)
{
	size_t size;
	uintptr_t phase;

	void *buf;
	malloc_data_t *ret;

	size_t overhead;

	if (size_arg == 0 || align == 0 || (align & (align - 1)) != 0) {
		errno = EINVAL;
		return (NULL);
	}

	/*
	 * if malloc provides the required alignment, use it.
	 */
	if (align <= UMEM_ALIGN ||
	    (align <= UMEM_SECOND_ALIGN && size_arg >= UMEM_SECOND_ALIGN))
		return (umem_malloc(size_arg));

#ifdef _LP64
	overhead = 2 * sizeof (malloc_data_t);
#else
	overhead = sizeof (malloc_data_t);
#endif

	ASSERT(overhead <= align);

	size = size_arg + overhead;
	phase = align - overhead;

	if (umem_memalign_arena == NULL && umem_init() == 0) {
		errno = ENOMEM;
		return (NULL);
	}

	if (size < size_arg) {
		errno = ENOMEM;			/* overflow */
		return (NULL);
	}

	buf = vmem_xalloc(umem_memalign_arena, size, align, phase,
	    0, NULL, NULL, VM_NOSLEEP);

	if (buf == NULL) {
		if ((size_arg + align) <= UMEM_MAXBUF)
			errno = EAGAIN;
		else
			errno = ENOMEM;

		return (NULL);
	}

	ret = (malloc_data_t *)buf;
	{
		uint32_t low_size = (uint32_t)size;

#ifdef _LP64
		uint32_t high_size = (uint32_t)(size >> 32);

		ret->malloc_size = high_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MEMALIGN_MAGIC,
		    high_size);
		ret++;
#endif

		ret->malloc_size = low_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MEMALIGN_MAGIC, low_size);
		ret++;
	}

	ASSERT(P2PHASE((uintptr_t)ret, align) == 0);
	ASSERT((void *)((uintptr_t)ret - overhead) == buf);

	return ((void *)ret);
}

/*
 * NOTE: memalign, posix_memalign, and valloc are now in malloc_interpose.c
 */

/*
 * process_free:
 *
 * Pulls information out of a buffer pointer, and optionally free it.
 * This is used by free() and realloc() to process buffers.
 *
 * On failure, calls umem_err_recoverable() with an appropriate message
 * On success, returns the data size through *data_size_arg, if (!is_free).
 *
 * Preserves errno, since free()'s semantics require it.
 *
 * Exposed for malloc_interpose.c
 */

int
process_free(void *buf_arg,
    int do_free,		/* free the buffer, or just get its size? */
    size_t *data_size_arg)	/* output: bytes of data in buf_arg */
{
	malloc_data_t *buf;

	/*
	 * Check for bootstrap pointers first.
	 * These have a different header format (bootstrap_header_t) and must
	 * be handled separately to avoid misinterpreting their headers.
	 */
	if (is_bootstrap_pointer(buf_arg)) {
		if (data_size_arg != NULL) {
			bootstrap_header_t *hdr = (bootstrap_header_t *)buf_arg - 1;
			*data_size_arg = hdr->size - sizeof(bootstrap_header_t);
		}
		/* For do_free=1, bootstrap_free should be called instead */
		return (1);
	}

	void *base;
	size_t size;
	size_t data_size;

	const char *message;
	int old_errno = errno;

	buf = (malloc_data_t *)buf_arg;

	buf--;
	size = buf->malloc_size;

	switch (UMEM_MALLOC_DECODE(buf->malloc_stat, size)) {

	case MALLOC_MAGIC:
		base = (void *)buf;
		data_size = size - sizeof (malloc_data_t);

		if (do_free)
			buf->malloc_stat = UMEM_FREE_PATTERN_32;

		goto process_malloc;

#ifdef _LP64
	case MALLOC_SECOND_MAGIC:
		base = (void *)(buf - 1);
		data_size = size - 2 * sizeof (malloc_data_t);

		if (do_free)
			buf->malloc_stat = UMEM_FREE_PATTERN_32;

		goto process_malloc;

	case MALLOC_OVERSIZE_MAGIC: {
		size_t high_size;

		buf--;
		high_size = buf->malloc_size;

		if (UMEM_MALLOC_DECODE(buf->malloc_stat, high_size) !=
		    MALLOC_MAGIC) {
			message = "invalid or corrupted buffer";
			break;
		}

		size += high_size << 32;

		base = (void *)buf;
		data_size = size - 2 * sizeof (malloc_data_t);

		if (do_free) {
			buf->malloc_stat = UMEM_FREE_PATTERN_32;
			(buf + 1)->malloc_stat = UMEM_FREE_PATTERN_32;
		}

		goto process_malloc;
	}
#endif

	case MEMALIGN_MAGIC: {
		size_t overhead = sizeof (malloc_data_t);

#ifdef _LP64
		size_t high_size;

		overhead += sizeof (malloc_data_t);

		buf--;
		high_size = buf->malloc_size;

		if (UMEM_MALLOC_DECODE(buf->malloc_stat, high_size) !=
		    MEMALIGN_MAGIC) {
			message = "invalid or corrupted buffer";
			break;
		}
		size += high_size << 32;

		/*
		 * destroy the main tag's malloc_stat
		 */
		if (do_free)
			(buf + 1)->malloc_stat = UMEM_FREE_PATTERN_32;
#endif

		base = (void *)buf;
		data_size = size - overhead;

		if (do_free)
			buf->malloc_stat = UMEM_FREE_PATTERN_32;

		goto process_memalign;
	}
	default:
		if (buf->malloc_stat == UMEM_FREE_PATTERN_32)
			message = "double-free or invalid buffer";
		else
			message = "invalid or corrupted buffer";
		break;
	}

	umem_err_recoverable("%s(%p): %s\n",
	    do_free? "free" : "realloc", buf_arg, message);

	errno = old_errno;
	return (0);

process_malloc:
	if (do_free)
		_umem_free(base, size);
	else
		*data_size_arg = data_size;

	errno = old_errno;
	return (1);

process_memalign:
	if (do_free)
		vmem_xfree(umem_memalign_arena, base, size);
	else
		*data_size_arg = data_size;

	errno = old_errno;
	return (1);
}

/*
 * umem_malloc_free: the real free implementation.
 *
 * This is the function that PTC generated code falls back to for
 * free operations it cannot handle.
 * On non-x86, free is a weak alias to this function.
 */
void
umem_malloc_free(void *buf)
{
	if (buf == NULL)
		return;

	/*
	 * Check if this is a bootstrap allocation (from before umem was ready).
	 * These use direct mmap and must be freed with munmap, not umem.
	 */
	if (is_bootstrap_pointer(buf)) {
		bootstrap_free(buf);
		return;
	}

	/*
	 * Process buf, freeing it if it is not corrupt.
	 */
	(void) process_free(buf, 1, NULL);
}

/*
 * NOTE: malloc(), free(), calloc(), realloc(), memalign(), posix_memalign(),
 * and valloc() are now in malloc_interpose.c which handles both the bootstrap
 * phase and dispatching to umem once initialized.
 */

/*
 * _malloc and _free are the PTC (per-thread cache) trampoline entry points.
 * On Solaris/Illumos, libc provides these in writable+executable text segments
 * so that umem_genasm() can overwrite them with generated assembly.
 * On Linux, these are simple trampolines -- after PTC genasm activates,
 * malloc()/free() call through the generated code directly via function
 * pointers, so these functions are only used for symbol resolution by the
 * genasm initialization code.
 */
#ifndef __sun
void *
_malloc(size_t size)
{
	return (umem_malloc(size));
}

void
_free(void *buf)
{
	umem_malloc_free(buf);
}
#endif

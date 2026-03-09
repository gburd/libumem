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

#include "misc.h"

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
 * On non-x86, we use weak aliases so malloc/free resolve to umem_malloc
 * and umem_malloc_free.  On x86, malloc and free are defined as thin
 * dispatchers that call through the PTC generated code (if enabled)
 * or fall through to umem_malloc/umem_malloc_free.
 */
#if !defined(__amd64__) && !defined(__x86_64__)
#pragma weak malloc = umem_malloc
#pragma weak free = umem_malloc_free
#endif /* !_x86 */

/*
 * On Linux, the PTC genasm code sets these function pointers after
 * generating the per-thread-cache assembly.  malloc() and free() check
 * these and dispatch through them when set.
 */
#ifndef __sun
extern void *(*umem_genasm_malloc_ptr)(size_t);
extern void (*umem_genasm_free_ptr)(void *);
#endif

/*
 * umem_malloc: the real malloc implementation.
 *
 * This is the function that PTC generated code falls back to for
 * allocations it cannot handle (overflow, oversized, etc.).
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
	size = size_arg + sizeof (malloc_data_t);

#ifdef _LP64
	if (size > UMEM_SECOND_ALIGN) {
		size += sizeof (malloc_data_t);
		high_size = (size >> 32);
	}
#endif
	if (size < size_arg) {
		errno = ENOMEM;			/* overflow */
		return (NULL);
	}
	ret = (malloc_data_t *)_umem_alloc(size, UMEM_DEFAULT);
	if (ret == NULL) {
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
	return ((void *)ret);
}

void *
calloc(size_t nelem, size_t elsize)
{
	size_t size = nelem * elsize;
	void *retval;

	if (nelem > 0 && elsize > 0 && size/nelem != elsize) {
		errno = ENOMEM;				/* overflow */
		return (NULL);
	}

	retval = malloc(size);
	if (retval == NULL)
		return (NULL);

	(void) memset(retval, 0, size);
	return (retval);
}

/*
 * memalign uses vmem_xalloc to do its work.
 *
 * in 64-bit, the memaligned buffer always has two tags.  This simplifies the
 * code.
 */

void *
memalign(size_t align, size_t size_arg)
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
		return (malloc(size_arg));

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

int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	*memptr = memalign(alignment, size);
	if (*memptr) {
		return 0;
	}
	return errno;
}

void *
valloc(size_t size)
{
	return (memalign(pagesize, size));
}

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
 */

static int
process_free(void *buf_arg,
    int do_free,		/* free the buffer, or just get its size? */
    size_t *data_size_arg)	/* output: bytes of data in buf_arg */
{
	malloc_data_t *buf;

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
	 * Process buf, freeing it if it is not corrupt.
	 */
	(void) process_free(buf, 1, NULL);
}

/*
 * On x86 platforms, malloc() and free() are dispatchers that check
 * whether PTC genasm has been enabled.  If so, they call through the
 * generated assembly code (which handles small allocations via per-thread
 * caches and falls back to umem_malloc/umem_malloc_free for everything else).
 * If PTC is not enabled, they call the real implementations directly.
 */
#if defined(__amd64__) || defined(__x86_64__)

#ifdef __sun
/*
 * On Solaris, _malloc/_free are in writable text segments and the PLT
 * handles dispatching via weak aliases.  malloc/free are defined as
 * weak aliases to _malloc/_free in the mapfile.
 */
#else
void *
malloc(size_t size_arg)
{
	if (__builtin_expect(umem_genasm_malloc_ptr != NULL, 1))
		return (umem_genasm_malloc_ptr(size_arg));
	return (umem_malloc(size_arg));
}

void
free(void *buf)
{
	if (__builtin_expect(umem_genasm_free_ptr != NULL, 1))
		return (umem_genasm_free_ptr(buf));
	umem_malloc_free(buf);
}
#endif /* __sun */

#endif /* __amd64__ || __x86_64__ */

void *
realloc(void *buf_arg, size_t newsize)
{
	size_t oldsize;
	void *buf;

	if (buf_arg == NULL)
		return (malloc(newsize));

	if (newsize == 0) {
		free(buf_arg);
		return (NULL);
	}

	/*
	 * get the old data size without freeing the buffer
	 */
	if (process_free(buf_arg, 0, &oldsize) == 0) {
		errno = EINVAL;
		return (NULL);
	}

	if (newsize == oldsize)		/* size didn't change */
		return (buf_arg);

	buf = malloc(newsize);
	if (buf == NULL)
		return (NULL);

	(void) memcpy(buf, buf_arg, MIN(newsize, oldsize));
	free(buf_arg);
	return (buf);
}

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

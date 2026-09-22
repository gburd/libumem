/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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
 *
 * Portions Copyright 2006-2008 Message Systems, Inc. All rights reserved.
 */

/* #pragma ident	"@(#)vmem_mmap.c	1.2	05/06/08 SMI" */

#include "config.h"
#include <errno.h>
#include <stdint.h>		/* uintptr_t, for CHUNKSIZE alignment below */
#include <pthread.h>		/* guards the shared heap reservation */

#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "vmem_base.h"
#include "umem_base.h"	/* umem_mmap_chunksize (UMEM_OPTIONS=chunksize) */

/*
 * No PROT_EXEC: libumem allocates data memory, not executable code.
 * The original Solaris code had PROT_EXEC for genasm (runtime code
 * generation in the heap), which has been removed.
 * W^X enforcement on modern OSes (FreeBSD, OpenBSD) rejects RWX mappings.
 */
#ifndef _WIN32
#define	ALLOC_PROT	PROT_READ | PROT_WRITE
#define	FREE_PROT	PROT_NONE
#endif

/*
 * MAP_FAILED portability.
 * On Windows without sys/mman.h, define MAP_FAILED for the fallback
 * code path in vmem_mmap_top_alloc.
 */
#ifndef MAP_FAILED
#define	MAP_FAILED	((void *)-1)
#endif

#ifndef _WIN32
/*
 * MAP_ANON / MAP_ANONYMOUS portability.
 * POSIX does not define either; MAP_ANONYMOUS is the Linux/glibc name,
 * MAP_ANON is the BSD name.  Most systems define both as aliases.
 */
#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#define	MAP_ANON	MAP_ANONYMOUS
#endif

#define	ALLOC_FLAGS	MAP_PRIVATE | MAP_ANON
/*
 * MAP_NORESERVE is not available on all platforms (e.g., FreeBSD).
 * Define it to 0 if missing so it has no effect in the flags bitmask.
 */
#ifndef MAP_NORESERVE
#define	MAP_NORESERVE	0
#endif
#define	FREE_FLAGS	MAP_PRIVATE | MAP_ANON | MAP_NORESERVE
#endif /* !_WIN32 */

#ifdef MAP_ALIGN
#define	CHUNKSIZE	(64*1024)	/* 64 kilobytes */
#else
static size_t CHUNKSIZE;

/*
 * Default quantum for the "mmap_top" parent arena on platforms without
 * MAP_ALIGN (i.e. everything except Solaris/illumos).
 *
 * This used to be the page size, which is a correctness-neutral but
 * operationally severe choice on Linux: the parent arena hands the heap
 * address space in quantum-sized units, so a page-sized quantum makes the heap
 * accumulate roughly one kernel VMA per ~76 KiB of mapped space.  The kernel
 * caps VMAs per process at vm.max_map_count (default 65530), so libumem hit a
 * hard ceiling at about 5 GB and then returned NULL -- measured: 65,532 VMAs
 * at failure (100%% of the limit) and ~39%% of allocations failing at 192
 * threads, while glibc on the same box reached 96 GB without a single failure.
 * See docs/results/2026-09-22-umem-heap-ceiling-vma.md.
 *
 * 64 KiB matches the value Solaris has always used here (the MAP_ALIGN branch
 * above), which is the configuration this allocator was designed and tuned
 * for, and it raises the VMA-bound ceiling by ~16x.  It costs address space,
 * not memory: the span is reserved PROT_NONE/MAP_NORESERVE and only the pages
 * actually allocated are ever faulted in.
 *
 * Override at runtime if a deployment needs something else:
 *   UMEM_OPTIONS=chunksize=<bytes>   (rounded up to a page multiple)
 */
#define	UMEM_CHUNKSIZE_DEFAULT	(64*1024)

/*
 * How much address space to reserve per heap growth.
 *
 * Spans carved from one reservation are contiguous, so after
 * vmem_mmap_alloc() mprotect()s them they merge into a single VMA.  Bigger
 * reservations therefore mean fewer VMAs; the cost is reserved (not committed)
 * address space, which on 64-bit is effectively free.  256MB keeps the VMA
 * count for a multi-GB heap in the dozens rather than the tens of thousands.
 */
#define	UMEM_HEAP_RESERVE	(256ULL * 1024 * 1024)
#endif

static vmem_t *mmap_heap;

static void *
vmem_mmap_alloc(vmem_t *src, size_t size, int vmflags)
{
	void *ret;
	int old_errno = errno;

	ret = vmem_alloc(src, size, vmflags);
#ifndef _WIN32
	/*
	 * Make the span usable.  The parent reservation is PROT_NONE so that
	 * address space vmem has not handed out cannot be touched; a span
	 * becomes readable/writable only here.
	 *
	 * mprotect(), not a fresh MAP_FIXED mmap().  Both make the range RW, but
	 * the mmap() replaces the mapping, and a distinct mapping cannot be
	 * merged with its neighbours by the kernel -- so every span permanently
	 * cost one VMA.  That is the real driver of vm.max_map_count exhaustion:
	 * measured at 4013 mappings in the 64K-256K range for 500MB of 4K
	 * allocations, i.e. one VMA per span regardless of span size.  Raising
	 * CHUNKSIZE alone did not help, because the span COUNT, not their
	 * alignment, is what consumes VMAs.
	 *
	 * mprotect() keeps the single underlying mapping, so adjacent RW spans
	 * coalesce back into one VMA instead of accumulating.
	 * See docs/results/2026-09-22-umem-heap-ceiling-vma.md.
	 */
	if (ret != NULL && mprotect(ret, size, ALLOC_PROT) != 0) {
		vmem_free(src, ret, size);
		vmem_reap();
		/*
		 * Leave errno alone: mprotect() has just said why (ENOMEM when
		 * the VMA limit is reached), and erasing it is what made this
		 * ceiling look like a performance problem for years.
		 */
		return (NULL);
	}
#endif

	errno = old_errno;
	return (ret);
}

static void
vmem_mmap_free(vmem_t *src, void *addr, size_t size)
{
	int old_errno = errno;
#ifdef _WIN32
	VirtualFree(addr, size, MEM_RELEASE);
#else
	/*
	 * Release the pages.  MADV_DONTNEED drops the physical memory while
	 * leaving the mapping in place, so this is not a leak.
	 */
	(void) madvise(addr, size, MADV_DONTNEED);

	/*
	 * Whether to also restore PROT_NONE is a real trade-off, measured:
	 *
	 *   64 contiguous 128K spans, mprotect'd RW  -> 25 VMAs (they merge)
	 *   ...then MADV_DONTNEED on alternating ones -> 25 VMAs (no change)
	 *   ...then mprotect(PROT_NONE) on those too  -> 88 VMAs
	 *
	 * The protection change is what splits the mapping, and splits are what
	 * exhaust vm.max_map_count -- that is the whole ~5GB ceiling, not the
	 * span size or the heap quantum (both of which were tried first and did
	 * not help).  See docs/results/2026-09-22-umem-heap-ceiling-vma.md.
	 *
	 * What PROT_NONE buys is that touching a span vmem has reclaimed faults
	 * immediately instead of silently reading stale or reused memory.  That
	 * is worth a VMA when someone is hunting a bug, and not worth a hard
	 * multi-gigabyte heap ceiling in production.
	 *
	 * So: trap under UMF_DEADBEEF (UMEM_DEBUG=default, which already exists
	 * to catch exactly this class of error), and keep the address space
	 * mergeable otherwise.  MADV_DONTNEED means a stray read in the
	 * non-debug case sees a zero page rather than another allocation's data.
	 */
	if (unlikely(umem_flags & UMF_DEADBEEF))
		(void) mprotect(addr, size, FREE_PROT);
#endif
	vmem_free(src, addr, size);
	errno = old_errno;
}

static void *
vmem_mmap_top_alloc(vmem_t *src, size_t size, int vmflags)
{
	void *ret;
	void *buf;
	int old_errno = errno;

	ret = vmem_alloc(src, size, VM_NOSLEEP);

	if (ret) {
		errno = old_errno;
		return (ret);
	}
	/*
	 * Need to grow the heap
	 */
#ifdef _WIN32
	buf = VirtualAlloc(NULL, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
	if (buf == NULL) buf = MAP_FAILED;
#elif defined(MAP_ALIGN)
	buf = mmap((void*)CHUNKSIZE, size, FREE_PROT, FREE_FLAGS | MAP_ALIGN,
			-1, 0);
#else
	/*
	 * Grow in large CONTIGUOUS reservations.
	 *
	 * Why not simply mmap(size): the heap then gets one separate mapping per
	 * growth, and separate mappings never merge, so every span costs a VMA
	 * permanently.  That is what exhausted vm.max_map_count at ~5GB.
	 *
	 * Why not over-map and trim for CHUNKSIZE alignment (the previous attempt
	 * here): trimming leaves an unmapped HOLE between reservations, which is
	 * even worse -- measured 4593 rw mappings each separated by a 64KB gap,
	 * unable to merge for exactly that reason.
	 *
	 * So reserve a large aligned region at once and satisfy many growths from
	 * it.  Spans handed out of one reservation are contiguous, so once
	 * vmem_mmap_alloc() mprotect()s them RW they coalesce into a single VMA
	 * instead of accumulating one per span.  Reservations are
	 * PROT_NONE/MAP_NORESERVE, so an unused tail costs address space only.
	 *
	 * See docs/results/2026-09-22-umem-heap-ceiling-vma.md.
	 */
	static char *resv_base;		/* current reservation */
	static size_t resv_left;
	static pthread_mutex_t resv_lock = PTHREAD_MUTEX_INITIALIZER;

	(void) pthread_mutex_lock(&resv_lock);
	if (resv_left < size) {
		size_t want = UMEM_HEAP_RESERVE;

		while (want < size)
			want *= 2;
		/*
		 * Over-map by one quantum and trim only the HEAD, so the
		 * reservation is CHUNKSIZE-aligned (_vmem_extend_alloc asserts
		 * this) without leaving a hole after it: the tail stays part of
		 * the reservation and is handed out by later growths.
		 */
		char *raw = mmap(0, want + CHUNKSIZE, FREE_PROT, FREE_FLAGS,
		    -1, 0);

		if (raw == MAP_FAILED) {
			(void) pthread_mutex_unlock(&resv_lock);
			buf = MAP_FAILED;
			goto grown;
		}
		char *aligned = (char *)P2ROUNDUP((uintptr_t)raw, CHUNKSIZE);
		size_t head = (size_t)(aligned - raw);

		if (head != 0)
			(void) munmap(raw, head);
		resv_base = aligned;
		resv_left = want + CHUNKSIZE - head;
	}
	buf = resv_base;
	resv_base += size;
	resv_left -= size;
	(void) pthread_mutex_unlock(&resv_lock);
grown:
#endif

	if (buf != MAP_FAILED) {
		ret = _vmem_extend_alloc(src, buf, size, size, vmflags);
		if (ret != NULL)
			return (ret);
		else {
#ifdef _WIN32
			(void) VirtualFree(buf, 0, MEM_RELEASE);
#else
			(void) munmap(buf, size);
#endif
			errno = old_errno;
			return (NULL);
		}
	} else {
		/*
		 * Growing the heap failed.  The allocation above will
		 * already have called umem_reap().
		 *
		 * Do NOT restore errno here.  mmap() has just told us WHY it
		 * failed, and that is the single most useful fact available:
		 * ENOMEM from vm.max_map_count exhaustion is indistinguishable
		 * from ordinary out-of-memory unless the caller can see it.
		 * This function used to overwrite it with the value on entry, so
		 * a caller got NULL with a stale, unrelated errno -- which is
		 * why libumem hitting its ~5GB address-space ceiling presented
		 * for years as "libumem is slower on this workload" rather than
		 * "libumem could not get memory"
		 * (docs/results/2026-09-22-umem-heap-ceiling-vma.md).
		 *
		 * The success paths still restore errno: a successful allocation
		 * must not perturb it.
		 */
		return (NULL);
	}
}

vmem_t *
vmem_mmap_arena(vmem_alloc_t **a_out, vmem_free_t **f_out)
{
#ifdef _WIN32
	SYSTEM_INFO info;
	size_t pagesize;
#else
	size_t pagesize = _sysconf(_SC_PAGESIZE);
#endif
	
#ifdef _WIN32
	GetSystemInfo(&info);
	pagesize = info.dwPageSize;
	CHUNKSIZE = info.dwAllocationGranularity;
#elif !defined(MAP_ALIGN)
	/*
	 * 64 KiB by default rather than the page size -- see
	 * UMEM_CHUNKSIZE_DEFAULT above for why (vm.max_map_count exhaustion at
	 * ~5 GB).  umem_mmap_chunksize is settable via
	 * UMEM_OPTIONS=chunksize=<bytes>; 0 means "use the default".
	 */
	if (umem_mmap_chunksize != 0) {
		CHUNKSIZE = P2ROUNDUP(umem_mmap_chunksize, pagesize);
	} else {
		CHUNKSIZE = UMEM_CHUNKSIZE_DEFAULT;
	}
	if (CHUNKSIZE < pagesize)
		CHUNKSIZE = pagesize;
#endif
	
	if (mmap_heap == NULL) {
		mmap_heap = vmem_init("mmap_top", CHUNKSIZE,
		    vmem_mmap_top_alloc, vmem_free,
		    "mmap_heap", NULL, 0, pagesize,
		    vmem_mmap_alloc, vmem_mmap_free);
	}

	if (a_out != NULL)
		*a_out = vmem_mmap_alloc;
	if (f_out != NULL)
		*f_out = vmem_mmap_free;

	return (mmap_heap);
}

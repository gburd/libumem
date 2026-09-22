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
#endif

static vmem_t *mmap_heap;

static void *
vmem_mmap_alloc(vmem_t *src, size_t size, int vmflags)
{
	void *ret;
	int old_errno = errno;

	ret = vmem_alloc(src, size, vmflags);
#ifndef _WIN32
	if (ret != NULL &&
	    mmap(ret, size, ALLOC_PROT, ALLOC_FLAGS | MAP_FIXED, -1, 0) ==
	    MAP_FAILED) {
		vmem_free(src, ret, size);
		vmem_reap();

		ASSERT((vmflags & VM_NOSLEEP) == VM_NOSLEEP);
		errno = old_errno;
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
	(void) mmap(addr, size, FREE_PROT, FREE_FLAGS | MAP_FIXED, -1, 0);
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
	buf = mmap(0, size, FREE_PROT, FREE_FLAGS, -1, 0);
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

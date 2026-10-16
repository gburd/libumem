/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * See usr/src/OPENSOLARIS.LICENSE or http://www.opensolaris.org/os/licensing.
 *
 * CDDL HEADER END
 */

/*
 * pal_mem.h -- the PAL memory contract (reserve/commit/decommit/release).
 *
 * BEHAVIOUR-PRESERVING: each function here emits the *exact* syscall and
 * flag set the scattered call sites emitted before the carve-out
 * (vmem_mmap.c's reserve/commit/free, umem.c's slab-page reclaim and the
 * CPU->node table map).  It relocates the syscall primitives, not the arena
 * policy: the guard-vs-dontneed decision, the errno discipline, and the vmem
 * bookkeeping all stay with their callers.  So the generated code at each
 * site is identical and the fast path (which touches NONE of these -- the
 * PTC-hit path makes no syscall) is unchanged (validation B).
 *
 * Everything is a `static inline` so the carve-out adds no indirection a
 * hotpath A/B could catch: where a PAL memory op were ever to reach the fast
 * path it must stay inline, and these are (docs/design/PAL.md, pal.h).
 *
 * Per-OS differences (MAP_NORESERVE absent on FreeBSD, MAP_ALIGN on illumos,
 * MADV_FREE vs MADV_DONTNEED for lazy reclaim, VirtualAlloc/VirtualFree on
 * Windows) live behind the same #if blocks the call sites used, so a port
 * overrides nothing it did not already have to.
 *
 * Design: docs/design/PAL.md ("reserve/commit/decommit/release" memory
 * contract); contract enumerated in pal_concept.h.
 */

#ifndef _UMEM_PAL_MEM_H
#define _UMEM_PAL_MEM_H

#include "config.h"
#include <stddef.h>

#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Portability shims, moved verbatim from vmem_mmap.c so every site that maps
 * or frees sees the identical flag set.
 */
#ifndef MAP_FAILED
#define	MAP_FAILED	((void *)-1)
#endif

#ifndef _WIN32
#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#define	MAP_ANON	MAP_ANONYMOUS
#endif
/*
 * MAP_NORESERVE is not available on all platforms (e.g. FreeBSD); define it to
 * 0 so it has no effect in the flags bitmask where it is absent.
 */
#ifndef MAP_NORESERVE
#define	MAP_NORESERVE	0
#endif

/*
 * No PROT_EXEC: libumem allocates data memory, not executable code.  The
 * original Solaris code had PROT_EXEC for genasm (runtime code generation in
 * the heap), which has been removed.  W^X enforcement on modern OSes
 * (FreeBSD, OpenBSD) rejects RWX mappings.
 */
#define	UMEM_PAL_ALLOC_PROT	(PROT_READ | PROT_WRITE)
#define	UMEM_PAL_FREE_PROT	PROT_NONE
#define	UMEM_PAL_ALLOC_FLAGS	(MAP_PRIVATE | MAP_ANON)
#define	UMEM_PAL_FREE_FLAGS	(MAP_PRIVATE | MAP_ANON | MAP_NORESERVE)
#endif /* !_WIN32 */

/*
 * Decommit modes -- which byte-for-byte syscall a given caller used to emit:
 *   GUARD    : remap PROT_NONE (fault-on-use-after-free); vmem_mmap_free's
 *              large-span guard branch.
 *   DONTNEED : madvise(MADV_DONTNEED); vmem_mmap_free's small-span branch.
 *   LAZY     : madvise(MADV_FREE) where the OS has it (FreeBSD), else
 *              MADV_DONTNEED; umem_slab_reclaim's page reclaim.  On Windows,
 *              MEM_RESET.  Kept distinct from DONTNEED because the FreeBSD
 *              reclaim path deliberately used MADV_FREE and must stay so.
 */
#define	UMEM_PAL_DECOMMIT_GUARD		0
#define	UMEM_PAL_DECOMMIT_DONTNEED	1
#define	UMEM_PAL_DECOMMIT_LAZY		2

/*
 * umem_pal_reserve() -- reserve address space, not yet usable (PROT_NONE,
 * MAP_NORESERVE).  The heap-growth path in vmem_mmap_top_alloc().  Returns
 * MAP_FAILED on failure, leaving errno as the OS set it (the caller depends
 * on that, see vmem_mmap.c).
 */
static inline void *
umem_pal_reserve(size_t size)
{
#ifdef _WIN32
	void *buf = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT,
	    PAGE_READWRITE);
	return (buf == NULL ? MAP_FAILED : buf);
#else
	return (mmap(0, size, UMEM_PAL_FREE_PROT, UMEM_PAL_FREE_FLAGS, -1, 0));
#endif
}

/*
 * umem_pal_reserve_aligned() -- reserve with OS-provided alignment where the
 * platform can (illumos MAP_ALIGN: the alignment is passed as the hint
 * address, exactly as vmem_mmap_top_alloc() did).  Platforms without strong
 * alignment do not define UMEM_PAL_STRONG_ALIGN and the caller uses
 * umem_pal_reserve() + trim instead; this entry exists only on MAP_ALIGN
 * platforms.
 */
#if defined(MAP_ALIGN) && !defined(_WIN32)
static inline void *
umem_pal_reserve_aligned(size_t size, size_t align)
{
	return (mmap((void *)align, size, UMEM_PAL_FREE_PROT,
	    UMEM_PAL_FREE_FLAGS | MAP_ALIGN, -1, 0));
}
#endif

/*
 * umem_pal_commit() -- make a reserved span usable (RW) in place (MAP_FIXED
 * over the reservation).  vmem_mmap_alloc().  Returns 0 on success, -1 on
 * failure (errno left as mmap set it).
 */
static inline int
umem_pal_commit(void *addr, size_t size)
{
#ifdef _WIN32
	/*
	 * Win32 reserves+commits in one VirtualAlloc in umem_pal_reserve, so
	 * there is nothing to commit in place here -- matching the original
	 * vmem_mmap_alloc, whose commit mmap was #ifndef _WIN32.
	 */
	(void) addr;
	(void) size;
	return (0);
#else
	return (mmap(addr, size, UMEM_PAL_ALLOC_PROT,
	    UMEM_PAL_ALLOC_FLAGS | MAP_FIXED, -1, 0) == MAP_FAILED ? -1 : 0);
#endif
}

/*
 * umem_pal_map_rw() -- one-shot reserve+commit of fresh anonymous RW memory
 * (no MAP_FIXED, no NORESERVE); the CPU->node table in umem.c.  Returns
 * MAP_FAILED on failure.  Distinct from reserve()+commit() because it is a
 * single plain anonymous map, not a reservation that is later committed in
 * place.
 */
static inline void *
umem_pal_map_rw(size_t size)
{
#ifdef _WIN32
	void *buf = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT,
	    PAGE_READWRITE);
	return (buf == NULL ? MAP_FAILED : buf);
#else
	return (mmap(NULL, size, UMEM_PAL_ALLOC_PROT, UMEM_PAL_ALLOC_FLAGS,
	    -1, 0));
#endif
}

/*
 * umem_pal_decommit() -- return physical pages to the OS by the exact method
 * the caller chose (see UMEM_PAL_DECOMMIT_*).  Advises/remaps only; the
 * virtual range stays valid.  errno-neutral is the caller's job (it saves and
 * restores around the call, as the original sites did).
 */
static inline void
umem_pal_decommit(void *addr, size_t size, int mode)
{
#ifdef _WIN32
	/*
	 * Match the two original Win32 sites byte-for-byte: vmem_mmap_free
	 * released the span (MEM_RELEASE) for both GUARD and DONTNEED;
	 * umem_slab_reclaim's LAZY path reset it (MEM_RESET).  (Windows is not
	 * a certified target -- pal_concept.h #errors on it -- so this is kept
	 * faithful, not exercised.)
	 */
	if (mode == UMEM_PAL_DECOMMIT_LAZY)
		(void) VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
	else
		(void) VirtualFree(addr, size, MEM_RELEASE);
#else
	if (mode == UMEM_PAL_DECOMMIT_GUARD) {
		(void) mmap(addr, size, UMEM_PAL_FREE_PROT,
		    UMEM_PAL_FREE_FLAGS | MAP_FIXED, -1, 0);
	} else if (mode == UMEM_PAL_DECOMMIT_LAZY) {
#if defined(__FreeBSD__) && defined(MADV_FREE)
		(void) madvise(addr, size, MADV_FREE);
#else
		(void) madvise(addr, size, MADV_DONTNEED);
#endif
	} else {
		(void) madvise(addr, size, MADV_DONTNEED);
	}
#endif
}

/*
 * umem_pal_release() -- unmap a span entirely (munmap / VirtualFree).  The
 * heap-growth rollback path in vmem_mmap_top_alloc().
 */
static inline void
umem_pal_release(void *addr, size_t size)
{
#ifdef _WIN32
	(void) VirtualFree(addr, 0, MEM_RELEASE);
#else
	(void) munmap(addr, size);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_PAL_MEM_H */

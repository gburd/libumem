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
 * pal.h -- the Platform Abstraction Layer contract.
 *
 * Selects the active PAL by platform macro and declares the umem_pal_*
 * surface every platform must provide.  This is a BEHAVIOUR-PRESERVING
 * organizing layer over code that already existed scattered across
 * vmem_mmap.c, init_lib.c, umem.c (entropy), umem_fail.c (abort),
 * umem_fork.c, and freebsd_pthread_hooks.c.  It does NOT change the allocator.
 *
 * The memory operations are the core of the allocator's demand on the OS, but
 * none of them is on the malloc/free FAST path (the PTC-hit path touches no
 * syscall), so routing them through this contract costs the fast path nothing
 * (validation B).  Where a PAL op were ever to reach the fast path it must be
 * a static inline here, not an out-of-line call.
 *
 * The required-symbol contract and the #error cascade live in pal_concept.h,
 * included at the bottom.
 *
 * Design: docs/design/PAL.md.
 */

#ifndef _UMEM_PAL_H
#define _UMEM_PAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform identity.
 */
#if defined(__linux__)
#define	UMEM_PAL_LINUX		1
#define	UMEM_PAL_NAME		"linux"
#define	UMEM_PAL_POSIX		1
#elif defined(__FreeBSD__)
#define	UMEM_PAL_FREEBSD	1
#define	UMEM_PAL_NAME		"freebsd"
#define	UMEM_PAL_POSIX		1
#elif defined(__sun) || defined(__illumos__)
#define	UMEM_PAL_ILLUMOS	1
#define	UMEM_PAL_NAME		"illumos"
#define	UMEM_PAL_POSIX		1
#elif defined(__APPLE__)
#define	UMEM_PAL_MACOS		1
#define	UMEM_PAL_NAME		"macos"
#define	UMEM_PAL_POSIX		1
#elif defined(__NetBSD__)
#define	UMEM_PAL_NETBSD		1
#define	UMEM_PAL_NAME		"netbsd"
#define	UMEM_PAL_POSIX		1
#elif defined(__OpenBSD__)
#define	UMEM_PAL_OPENBSD	1
#define	UMEM_PAL_NAME		"openbsd"
#define	UMEM_PAL_POSIX		1
#elif defined(_WIN32)
#define	UMEM_PAL_WINDOWS	1
#define	UMEM_PAL_NAME		"windows"
#else
#define	UMEM_PAL_UNKNOWN	1
#define	UMEM_PAL_NAME		"unknown"
#endif

/*
 * pal_features bitmask -- so the core can #if on capability, not on OS name
 * (docs/design/PAL.md).  These mirror the capability tests already compiled
 * into the tree; the carve-out names them.
 */
#define	UMEM_PAL_HAS_RSEQ	0x0001u	/* restartable sequences available */
#define	UMEM_PAL_HAS_MADV_FREE	0x0002u	/* madvise(MADV_FREE) lazy decommit */
#define	UMEM_PAL_STRONG_ALIGN	0x0004u	/* OS can map at a requested align */
#define	UMEM_PAL_AT_SECURE_GATE	0x0008u	/* getauxval(AT_SECURE) gate present */
#define	UMEM_PAL_HAS_AUXV	0x0010u	/* AT_RANDOM entropy via auxv */

/* Return the compile-time capability bitmask for this platform. */
unsigned int umem_pal_features(void);

/*
 * Memory (the core demand on the OS).  The reserve/commit/decommit/release
 * state machine is the PAL memory contract, implemented as static inlines in
 * pal/pal_mem.h so the carve-out adds no indirection a hotpath A/B could
 * catch (none of these is on the malloc/free fast path -- the PTC-hit path
 * makes no syscall -- but they stay inline so that if one ever reached the
 * fast path it would cost nothing).  vmem_mmap.c reaches the heap through
 * these; umem.c's slab-page reclaim and CPU->node table map do too.
 *
 *   void *umem_pal_reserve(size_t);              -- PROT_NONE reservation
 *   void *umem_pal_reserve_aligned(size_t, size_t); -- MAP_ALIGN platforms
 *   int   umem_pal_commit(void *, size_t);       -- MAP_FIXED RW commit
 *   void  umem_pal_decommit(void *, size_t, int);-- GUARD/DONTNEED/LAZY
 *   void  umem_pal_release(void *, size_t);      -- munmap
 *   void *umem_pal_map_rw(size_t);               -- one-shot anon RW map
 */
size_t umem_pal_page_size(void);

/*
 * Entropy / identity / scheduling.
 */
int  umem_pal_getentropy(void *buf, size_t len);
long umem_pal_ncpus(void);
void umem_pal_yield(void);
int  umem_pal_cpu_hint(void);

/*
 * Diagnostics / lifecycle.
 */
void umem_pal_abort(void) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif

#include "pal/pal_mem.h"
#include "pal/pal_concept.h"

#endif /* _UMEM_PAL_H */

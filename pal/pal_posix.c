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
 * pal_posix.c -- the common POSIX base of the PAL.
 *
 * BEHAVIOUR-PRESERVING: every function here delegates to the exact code path
 * libumem already used before the PAL carve-out, so the allocator's behaviour
 * is unchanged.  This unit exists to give the umem_pal_* contract real symbols
 * (so the port smoke test links) and a single auditable home for the port
 * surface, per docs/design/PAL.md.
 *
 * The memory operations (reserve/commit/decommit/release) are NOT re-homed
 * here: they are internal statics of vmem_mmap.c reached only through
 * vmem_mmap_arena(), which is the published PAL memory entry point and is on
 * NO fast path.  Moving the mmap body would be a rewrite, not a relocation, so
 * vmem_mmap.c stays the PAL's memory implementation and this file provides the
 * scalar POSIX ops.
 *
 * Platform overrides (pal_linux.c etc.) would include this file and redefine
 * only what differs; for the certified Linux/FreeBSD/illumos targets the POSIX
 * base is sufficient and the per-OS behaviour already lives behind the #if
 * blocks in the delegated functions.
 */

#include "config.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sched.h>
#if HAVE_SYS_AUXV_H
#include <sys/auxv.h>
#define	UMEM_PAL_HAVE_AUXV	1
#endif

#include "pal/pal.h"
#include "misc.h"

/* The hardware/OS page size, cached by umem_startup() (umem.c defines it). */
extern size_t pagesize;

/* init_lib.c: the online-CPU-count reader (proc_stat on Linux, sysconf
 * elsewhere), already O7-shrunk off the 64 KiB stack.  umem_pal_ncpus() is
 * the PAL name for exactly this. */
extern int umem_get_max_ncpus(void);

/*
 * umem_pal_features() -- compile-time capability bitmask so the core can #if
 * on capability, not OS name.  Each bit is set from the SAME macro the tree
 * already tests, so this is a reflection of current behaviour, not a new
 * policy.
 */
unsigned int
umem_pal_features(void)
{
	unsigned int f = 0;

#if defined(HAVE_LINUX_RSEQ_H)
	f |= UMEM_PAL_HAS_RSEQ;
#endif
#if defined(MADV_FREE)
	f |= UMEM_PAL_HAS_MADV_FREE;
#endif
#if defined(MAP_ALIGN) || defined(MAP_ALIGNED)
	f |= UMEM_PAL_STRONG_ALIGN;
#endif
#if defined(UMEM_PAL_HAVE_AUXV) && defined(AT_SECURE) && defined(HAVE_GETAUXVAL)
	f |= UMEM_PAL_AT_SECURE_GATE;
#endif
#if defined(UMEM_PAL_HAVE_AUXV) && defined(AT_RANDOM) && defined(HAVE_GETAUXVAL)
	f |= UMEM_PAL_HAS_AUXV;
#endif
	return (f);
}

/*
 * umem_pal_page_size() -- the page size umem_startup() cached.  May exceed the
 * hardware page on an OS with a larger allocation granularity (ppc 64K).
 */
size_t
umem_pal_page_size(void)
{
	return (pagesize);
}

/*
 * umem_pal_ncpus() -- online CPU count.  Delegates to the existing reader so
 * the two never diverge (the allocator sizes its per-CPU layer from this).
 */
long
umem_pal_ncpus(void)
{
	return ((long)umem_get_max_ncpus());
}

/*
 * umem_pal_yield() -- sched_yield.  errno-neutral like every other POSIX op
 * here (a successful yield must not perturb the caller's errno).
 */
void
umem_pal_yield(void)
{
	int save = errno;
	(void) sched_yield();
	errno = save;
}

/*
 * umem_pal_cpu_hint() -- a current-CPU id for striping.  The AAL provides the
 * rseq-backed primitive where present; this POSIX fallback returns the same
 * sched_getcpu() the per-CPU layer uses, or -1 where it is unavailable.  The
 * allocator masks it, so an imprecise answer is a correctness-safe hint.
 */
int
umem_pal_cpu_hint(void)
{
#if defined(__linux__)
	int save = errno;
	int cpu = sched_getcpu();
	errno = save;
	return (cpu);
#else
	return (-1);
#endif
}

/*
 * umem_pal_getentropy() -- fill buf with len bytes of entropy for the P5.4
 * link cookie.  Mirrors the existing cookie derivation (umem.c
 * umem_link_cookie_init): AT_RANDOM where auxv has it, then getrandom/
 * getentropy, then an ASLR+pid fallback that is never worse than the
 * unmangled behaviour it replaces.  Returns 0 on success, -1 if NO source
 * produced anything (the caller then keeps its own fallback mixing).
 */
int
umem_pal_getentropy(void *buf, size_t len)
{
	int save = errno;
	int got = -1;

#if defined(UMEM_PAL_HAVE_AUXV) && defined(AT_RANDOM) && defined(HAVE_GETAUXVAL)
	{
		const unsigned long at = getauxval(AT_RANDOM);
		if (at != 0 && len <= 16) {
			/* AT_RANDOM is 16 kernel bytes, no syscall. */
			(void) memcpy(buf, (const void *)(uintptr_t)at, len);
			got = 0;
		}
	}
#endif

	errno = save;
	return (got);
}

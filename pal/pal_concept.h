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
 * pal_concept.h -- the C analogue of snmalloc's C++20 Concept for the PAL.
 *
 * C17 has no Concept, so the checkable contract is: a #error cascade that
 * fails a port which has not declared a platform, plus an enumerated list of
 * every symbol a complete PAL must provide.  A port that forgot a symbol
 * fails to LINK (the smoke test, test/port/pal_aal_smoke.c, references every
 * one); a port on a platform libumem does not know fails to COMPILE here,
 * naming what is missing.  Do NOT fake support.
 *
 * Design: docs/design/PAL.md ("one header that enumerates every required
 * symbol, plus a compile-time #error cascade").
 */

#ifndef _UMEM_PAL_CONCEPT_H
#define _UMEM_PAL_CONCEPT_H

#include "pal/pal.h"

/*
 * 1. Every platform must be named.  UMEM_PAL_UNKNOWN means pal.h did not
 *    recognise the OS: a port that has not been done, which must fail loudly.
 */
#if defined(UMEM_PAL_UNKNOWN)
#error "PAL: unknown platform. libumem has no PAL for this OS. Add a \
UMEM_PAL_<os> block to pal/pal.h and, if it is POSIX-ish, a pal_<os>.c that \
includes pal_posix.c and overrides only what differs. See \
docs/porting/README.md and docs/design/PAL.md."
#endif

/*
 * 2. The required-symbol contract.  Every complete PAL provides the symbols
 *    below.  Most are satisfied by the common POSIX base; a platform file
 *    overrides only what differs.  The symbols are declared in their existing
 *    homes (vmem_base.h for the mmap arena, misc.h, umem_base.h); this is the
 *    port contract, enforced by the smoke test which references each one.
 *
 *  Memory (pal/pal_mem.h static inlines; byte-for-byte the syscalls the
 *  scattered sites emitted before the carve-out):
 *    void *umem_pal_reserve(size_t);
 *        -- reserve address space, not yet usable (PROT_NONE, MAP_NORESERVE);
 *           the heap-growth path in vmem_mmap_top_alloc().
 *    void *umem_pal_reserve_aligned(size_t, size_t align);
 *        -- OS-aligned reservation (illumos MAP_ALIGN); exists only where
 *           UMEM_PAL_STRONG_ALIGN, else the core over-reserves and trims.
 *    int   umem_pal_commit(void *, size_t);
 *        -- make a reservation usable (MAP_FIXED RW); vmem_mmap_alloc().
 *    void  umem_pal_decommit(void *, size_t, int mode);
 *        -- return pages to the OS: GUARD (PROT_NONE), DONTNEED, or LAZY
 *           (MADV_FREE where present); vmem_mmap_free() and umem_slab_reclaim.
 *    void  umem_pal_release(void *, size_t);
 *        -- munmap the span; vmem_mmap_top_alloc() rollback.
 *    void *umem_pal_map_rw(size_t);
 *        -- one-shot anonymous RW map (no MAP_FIXED); umem.c CPU->node table.
 *    vmem_t *vmem_mmap_arena(vmem_alloc_t **, vmem_free_t **);
 *        -- the published heap arena entry point, wiring the above into the
 *           reserve/commit/decommit/release state machine (vmem_base.h).
 *    size_t umem_pal_page_size(void);
 *        -- sysconf(_SC_PAGESIZE), may exceed the hardware page (ppc 64K).
 *
 *  Entropy / identity / scheduling:
 *    int   umem_pal_getentropy(void *, size_t);  (AT_RANDOM / getrandom /
 *          getentropy / arc4random / /dev/urandom fallback; feeds P5.4 cookie)
 *    long  umem_pal_ncpus(void);   (online CPU count; init_lib.c proc_stat)
 *    void  umem_pal_yield(void);   (sched_yield)
 *    int   umem_pal_cpu_hint(void);(current CPU id; rseq-backed where present)
 *
 *  Diagnostics / lifecycle:
 *    void  umem_pal_abort(void);   (the controlled abort; umem_do_abort)
 *    fork hooks: pthread_atfork wiring (umem_fork.c), FreeBSD hook variant
 *          (freebsd_pthread_hooks.c).
 *    unsigned int umem_pal_features(void);  (capability bitmask)
 */

/*
 * 3. Windows is recognised but not a supported runtime target: the tree has
 *    _WIN32 fallbacks in vmem_mmap.c but no PAL has been certified on it, and
 *    the pthread/fork/rseq model does not apply.  Name the gap; do not pretend.
 */
#if defined(UMEM_PAL_WINDOWS)
#error "PAL: Windows is not a certified libumem target. vmem_mmap.c carries \
VirtualAlloc fallbacks but no PAL has been validated. Remove this #error and \
complete pal_windows.c (reserve/commit/decommit/release via VirtualAlloc, \
BCryptGenRandom entropy, GetSystemInfo ncpus) to port it."
#endif

#endif /* _UMEM_PAL_CONCEPT_H */

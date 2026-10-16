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
 * aal_concept.h -- the C analogue of snmalloc's C++20 Concept for the AAL.
 *
 * C17 has no Concept, so the checkable contract is this: a #error cascade
 * that fails a port which has not declared an architecture, plus an
 * enumerated list of every symbol a complete AAL must provide.  A port that
 * forgot a symbol fails to LINK (the PAL/AAL smoke test references every one);
 * a port on an architecture libumem does not yet know fails to COMPILE here,
 * naming what is missing.  Do NOT fake support by stubbing these out silently.
 *
 * Design: docs/design/PAL.md ("The C equivalent of a checkable contract").
 */

#ifndef _UMEM_AAL_CONCEPT_H
#define _UMEM_AAL_CONCEPT_H

#include "aal/aal.h"

/*
 * 1. Every architecture must be named.  UMEM_AAL_UNKNOWN means aal.h did not
 *    recognise the compiler's arch macros: that is a port that has not been
 *    done, and it must fail loudly rather than build a nameless binary.
 */
#if defined(UMEM_AAL_UNKNOWN)
#error "AAL: unknown architecture. libumem has no AAL for this target. \
Add a UMEM_AAL_<arch> block to aal/aal.h (name, UMEM_AAL_PTR_BITS, \
UMEM_AAL_WORD_BITS) and an aal_<arch> implementation unit providing getfp, \
getpcstack, and (if rseq-capable) the rseq critical section. See \
docs/porting/README.md and docs/design/PAL.md."
#endif

/*
 * 2. The required-symbol contract.  Every complete AAL provides:
 *
 *   void *getfp(void);
 *       -- the current frame pointer (misc.h).  x86_64 may inline it from
 *          __builtin_frame_address; SPARC/illumos uses sparc_subr_sol.S; x86
 *          illumos uses x86_subr_compat.c.
 *   int   getpcstack(uintptr_t *pcstack, int limit, int check_sighandler);
 *       -- the frame walk (misc.h, getpcstack.c), per-arch and honest about
 *          frame-pointer dependence on x86_64.
 *   void  umem_aal_pause(void);
 *       -- cpu-relax (this header's companion aal.h, inline).
 *   UMEM_PREFETCH_READ / UMEM_PREFETCH_WRITE (umem_simd.h)
 *       -- prefetch hints.
 *   The atomic surface (atomic.h): atomic_cas_ptr, atomic_add_64,
 *       membar_producer/consumer, etc.
 *
 * rseq-capable architectures additionally provide the rseq critical section
 * (umem_rseq_<arch>.S + umem_rseq.c), gated at build time by ENABLE_RSEQ and
 * at run time by UMEM_RSEQ_AVAILABLE.  An architecture WITHOUT rseq is still a
 * complete AAL -- the core falls back to the classic per-CPU cache -- so the
 * rseq asm is NOT in this required set; it is an optional capability the PAL
 * advertises via UMEM_PAL_HAS_RSEQ.
 *
 * These symbols are declared in their existing headers (misc.h, umem_simd.h,
 * atomic.h); this list is the port contract, enforced by the smoke test
 * (test/port/pal_aal_smoke.c) which references each one.
 */

/*
 * 3. New architectures carved out in this round (docs/design/PAL.md "Required
 *    targets"): arm32 (armhf), riscv64, sparcv9.  Each has a getpcstack()
 *    arm in getpcstack.c; sparcv9 additionally has sparc_subr_sol.S for
 *    getfp/flush_windows on illumos.  Where an arch cannot yet provide a
 *    required symbol, the gap is named here, not stubbed:
 */
#if defined(UMEM_AAL_SPARCV8)
#error "AAL: 32-bit SPARC (sparcv8) is not a supported libumem target. \
The historic SPARC asm (sparc_subr_sol.S) is UltraSPARCv9 (STACK_BIAS, \
64-bit windows). Build for sparcv9 (-m64) or add a sparcv8 AAL."
#endif

#endif /* _UMEM_AAL_CONCEPT_H */

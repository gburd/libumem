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
 * aal.h -- the Architecture Abstraction Layer contract.
 *
 * This header selects the active AAL by architecture macro and declares the
 * umem_aal_* surface every architecture must provide.  It is a
 * BEHAVIOUR-PRESERVING organizing layer: it names and routes the per-arch
 * code (frame walk, cpu-relax, prefetch, rseq critical section, atomics) that
 * already existed scattered across the tree.  It does NOT change behaviour.
 *
 * The required-symbol contract and the #error cascade that fails a port which
 * forgot one live in aal_concept.h, included at the bottom.
 *
 * Design: docs/design/PAL.md.
 */

#ifndef _UMEM_AAL_H
#define _UMEM_AAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Architecture identity.  UMEM_AAL_NAME is a short string; the width
 * constants size address-dependent structures (P7.4: the span table must
 * size from UMEM_AAL_PTR_BITS, never a hard-coded 48-bit VA).
 */
#if defined(__x86_64__) || defined(__amd64)
#define	UMEM_AAL_X86_64		1
#define	UMEM_AAL_NAME		"X86_64"
#define	UMEM_AAL_PTR_BITS	64
#define	UMEM_AAL_WORD_BITS	64
#elif defined(__i386__) || defined(__i386)
#define	UMEM_AAL_I386		1
#define	UMEM_AAL_NAME		"I386"
#define	UMEM_AAL_PTR_BITS	32
#define	UMEM_AAL_WORD_BITS	32
#elif defined(__aarch64__)
#define	UMEM_AAL_AARCH64	1
#define	UMEM_AAL_NAME		"AARCH64"
#define	UMEM_AAL_PTR_BITS	64
#define	UMEM_AAL_WORD_BITS	64
#elif defined(__arm__)
#define	UMEM_AAL_ARM32		1
#define	UMEM_AAL_NAME		"ARM32"
#define	UMEM_AAL_PTR_BITS	32
#define	UMEM_AAL_WORD_BITS	32
#elif defined(__riscv) && (__riscv_xlen == 64)
#define	UMEM_AAL_RISCV64	1
#define	UMEM_AAL_NAME		"RISCV64"
#define	UMEM_AAL_PTR_BITS	64
#define	UMEM_AAL_WORD_BITS	64
#elif defined(__riscv) && (__riscv_xlen == 32)
#define	UMEM_AAL_RISCV32	1
#define	UMEM_AAL_NAME		"RISCV32"
#define	UMEM_AAL_PTR_BITS	32
#define	UMEM_AAL_WORD_BITS	32
#elif defined(__sparcv9) || defined(__sparc_v9__) || \
    (defined(__sparc__) && defined(__arch64__)) || \
    (defined(__sparc) && defined(__sparcv9))
#define	UMEM_AAL_SPARCV9	1
#define	UMEM_AAL_NAME		"SPARCV9"
#define	UMEM_AAL_PTR_BITS	64
#define	UMEM_AAL_WORD_BITS	64
#elif defined(__sparc__) || defined(__sparc)
#define	UMEM_AAL_SPARCV8	1
#define	UMEM_AAL_NAME		"SPARCV8"
#define	UMEM_AAL_PTR_BITS	32
#define	UMEM_AAL_WORD_BITS	32
#elif defined(__powerpc64__) || defined(__ppc64__)
#define	UMEM_AAL_PPC64		1
#define	UMEM_AAL_NAME		"PPC64"
#define	UMEM_AAL_PTR_BITS	64
#define	UMEM_AAL_WORD_BITS	64
#else
#define	UMEM_AAL_UNKNOWN	1
#define	UMEM_AAL_NAME		"UNKNOWN"
#endif

/*
 * umem_aal_pause() -- cpu-relax inside a CAS retry spin (PAUSE/YIELD/WFE).
 * This is the same instruction UMEM_SPIN_HINT() emits; the AAL names it as
 * the contract surface.  Kept as an inline so the spin loop pays no call.
 */
static inline void
umem_aal_pause(void)
{
#if defined(UMEM_AAL_X86_64) || defined(UMEM_AAL_I386)
	__asm__ volatile("pause");
#elif defined(UMEM_AAL_AARCH64) || defined(UMEM_AAL_ARM32)
	__asm__ volatile("yield");
#elif defined(UMEM_AAL_RISCV64) || defined(UMEM_AAL_RISCV32)
	/* Zihintpause 'pause'; encoded so an old assembler accepts it. */
	__asm__ volatile(".insn i 0x0f, 0, x0, x0, 0x010");
#elif defined(UMEM_AAL_SPARCV9) || defined(UMEM_AAL_SPARCV8)
	__asm__ volatile("rd %%ccr, %%g0" ::: "memory");
#else
	((void)0);
#endif
}

/*
 * The architecture-specific declarations that already existed (frame walk,
 * breakpoint, prefetch) are declared in misc.h / umem_simd.h and implemented
 * in the aal_<arch> units.  The AAL does not re-declare them to avoid a double
 * declaration; aal_concept.h records them as required symbols for the port
 * contract and the #error cascade.
 */

#ifdef __cplusplus
}
#endif

#include "aal/aal_concept.h"

#endif /* _UMEM_AAL_H */

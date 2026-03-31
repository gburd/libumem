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
 * Copyright (c) 2025 Greg Burd. All rights reserved.
 * Adapted from amd64 implementation Copyright (c) 2013 Joyent, Inc.
 */

/*
 * aarch64 (ARM64) implementation of Per-Thread Cache (PTC) assembly generation
 *
 * This file implements runtime generation of optimized malloc/free functions
 * for aarch64 architecture. See section 8 of umem.c for the PTC theory.
 *
 * ARM64 Register Conventions (AAPCS64):
 *   x0-x7:    Argument/result registers (x0 = first arg, return value)
 *   x8:       Indirect result location
 *   x9-x15:   Temporary registers (caller-saved)
 *   x16-x17:  Intra-procedure-call temporary (IP0, IP1)
 *   x18:      Platform register (reserved on some platforms)
 *   x19-x28:  Callee-saved registers
 *   x29:      Frame pointer (FP)
 *   x30:      Link register (LR)
 *   sp:       Stack pointer
 *
 * Malloc register usage:
 *   x0: Original size to malloc (preserved as argument)
 *   x1: Adjusted malloc size for malloc_data_tag(s)
 *   x9: Pointer to the tmem_t structure
 *   x10: Pointer to the tmem_t array of roots
 *   x11: Size of the cache
 *   x12: Scratch register
 *
 * Free register usage:
 *   x0: Original buffer to free (preserved as argument)
 *   x1: The actual buffer, adjusted for the hidden malloc_data_t(s)
 *   x9: Pointer to the tmem_t structure
 *   x10: Pointer to the tmem_t array of roots
 *   x11: Size of the cache
 *   x12: Scratch register
 *
 * TLS Access on aarch64:
 *   Thread-local storage is accessed via TPIDR_EL0 register.
 *   The tmem structure offset from thread pointer is computed at runtime
 *   by _tmem_get_base() in tmem_stubs.c.
 *
 * Instruction Encoding:
 *   All aarch64 instructions are fixed 32-bit (4-byte) words.
 *   Branches use PC-relative offsets measured in instructions (not bytes).
 *
 * TODO: This is a placeholder implementation. Full assembly generation
 * requires implementing:
 *   1. Complete instruction encoding functions for aarch64
 *   2. TLS access sequence (MRS + ADD with offset)
 *   3. Cache size checks and branching logic
 *   4. Buffer allocation/deallocation from cache lists
 *   5. Memory protection (mmap/mprotect) on non-Solaris systems
 */

#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include "../umem_impl.h"
#include "../umem_base.h"

#ifndef __sun
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../atomic.h"

/*
 * IMPORTANT: Currently disabled until full implementation is complete.
 * Set to 1 when the instruction encoding and code generation is implemented.
 */
const int umem_genasm_supported = 0;

/*
 * On Solaris, _malloc/_free would be in writable text segments.
 * On Linux, we need to allocate RWX memory via mmap, generate code into it,
 * then mprotect to RX. The function pointers point to these buffers.
 *
 * umem_genasm_omptr/ofptr are the addresses of the fallback malloc/free
 * implementations that the generated code jumps to for cases it can't handle.
 */
#ifdef __sun
static uintptr_t umem_genasm_mptr = (uintptr_t)&_malloc;
static size_t umem_genasm_msize = 1024;  /* Larger than amd64 due to fixed-size instructions */
static uintptr_t umem_genasm_fptr = (uintptr_t)&_free;
static size_t umem_genasm_fsize = 1024;
#else
static uintptr_t umem_genasm_mptr;
static size_t umem_genasm_msize = 1024;
static uintptr_t umem_genasm_fptr;
static size_t umem_genasm_fsize = 1024;
static void *umem_genasm_mmap_base;
static size_t umem_genasm_mmap_size;
#endif
static uintptr_t umem_genasm_omptr = (uintptr_t)umem_malloc;
static uintptr_t umem_genasm_ofptr = (uintptr_t)umem_malloc_free;

/*
 * On Linux, after genasm succeeds, we update these function pointers so that
 * malloc()/free() in malloc.c call through to the generated code.
 */
#ifndef __sun
void *(*umem_genasm_malloc_ptr)(size_t) = NULL;
void (*umem_genasm_free_ptr)(void *) = NULL;
#endif

#define	UMEM_GENASM_MAX64	(UINT32_MAX / sizeof (uintptr_t))
#define	PTC_ROOT_SIZE	sizeof (uintptr_t)

/*
 * aarch64 instruction encoding helpers
 * All instructions are 32-bit fixed width.
 *
 * TODO: Implement these helper functions for generating aarch64 instructions:
 *
 * - Branch instructions: B (unconditional), B.cond (conditional), CBZ/CBNZ
 * - Load/Store: LDR, STR (various forms and addressing modes)
 * - Arithmetic: ADD, SUB, CMP
 * - Logical: AND, ORR, EOR
 * - Move: MOV, MOVK (move wide immediate)
 * - TLS access: MRS (for reading TPIDR_EL0)
 * - Return: RET
 *
 * Example (not yet implemented):
 *
 * static inline uint32_t
 * arm64_mov_imm(int rd, uint64_t imm)
 * {
 *     // MOVZ: Move wide with zero (clears other bits)
 *     // encoding: sf=1, opc=10, hw=00, imm16, Rd
 *     return 0xd2800000 | (imm << 5) | rd;
 * }
 *
 * static inline uint32_t
 * arm64_ldr_reg_offset(int rt, int rn, int offset)
 * {
 *     // LDR (immediate, unsigned offset)
 *     // LDR Xt, [Xn, #imm]
 *     uint32_t imm12 = (offset / 8) & 0xfff;  // offset must be 8-byte aligned
 *     return 0xf9400000 | (imm12 << 10) | (rn << 5) | rt;
 * }
 *
 * static inline uint32_t
 * arm64_add_reg_reg_imm(int rd, int rn, uint64_t imm)
 * {
 *     // ADD Xd, Xn, #imm
 *     return 0x91000000 | ((imm & 0xfff) << 10) | (rn << 5) | rd;
 * }
 *
 * static inline uint32_t
 * arm64_cbz(int rt, int32_t offset)
 * {
 *     // CBZ Xt, label  (Compare and Branch on Zero)
 *     // offset is in instructions (4-byte units), signed 19-bit
 *     return 0xb4000000 | ((offset & 0x7ffff) << 5) | rt;
 * }
 *
 * static inline uint32_t
 * arm64_b(int32_t offset)
 * {
 *     // B label (unconditional branch)
 *     // offset is in instructions (4-byte units), signed 26-bit
 *     return 0x14000000 | (offset & 0x3ffffff);
 * }
 *
 * static inline uint32_t
 * arm64_mrs_tpidr(int rd)
 * {
 *     // MRS Xd, TPIDR_EL0
 *     // Read thread pointer register
 *     return 0xd53bd040 | rd;
 * }
 *
 * static inline uint32_t
 * arm64_ret(void)
 * {
 *     // RET (return, defaults to X30/LR)
 *     return 0xd65f03c0;
 * }
 */

/*
 * Pseudocode for malloc prologue (equivalent to amd64 malinit):
 *
 * void *ptcmalloc(size_t orig_size);   // x0 = orig_size
 *
 * size_t size = orig_size + 8;         // x1 = adjusted size
 * if (size > UMEM_SECOND_ALIGN)
 *     size += 8;
 *
 * if (size < orig_size)                // overflow check
 *     goto tomalloc;
 *
 * if (size > cache_max)
 *     goto tomalloc;
 *
 * // Get thread-local tmem structure
 * uintptr_t tp;
 * __asm__("mrs %0, tpidr_el0" : "=r" (tp));
 * tmem_t *t = (tmem_t *)(tp + umem_tmem_off);
 * void **roots = &t->tm_roots[0];
 *
 * TODO: Generate aarch64 instructions:
 * - ADD x1, x0, #8              // size = orig_size + 8
 * - CMP x1, #0x10               // if (size > UMEM_SECOND_ALIGN)
 * - B.LS skip
 * - ADD x1, x0, #0x10           // size += 8
 * skip:
 * - CMP x1, x0                  // if (size < orig_size) overflow
 * - B.LO errout
 * - MOV x12, #cache_max
 * - CMP x1, x12                 // if (size > cache_max)
 * - B.HI errout
 * - MRS x9, tpidr_el0           // Get thread pointer
 * - MOV x12, #umem_tmem_off
 * - ADD x9, x9, x12             // x9 = tmem pointer
 * - ADD x10, x9, #8             // x10 = &tmem->tm_roots[0]
 */

/*
 * Pseudocode for free prologue (equivalent to amd64 freeinit):
 *
 * void ptcfree(void *buf);      // x0 = buf
 *
 * if (buf == NULL)
 *     return;
 *
 * malloc_data_t *tag = (malloc_data_t *)buf - 1;
 * int size = tag->malloc_size;
 * int tagval = UMEM_MALLOC_DECODE(tag->malloc_stat, size);
 *
 * if (tagval == MALLOC_SECOND_MAGIC) {
 *     tag--;
 * } else if (tagval != MALLOC_MAGIC) {
 *     goto tofree;
 * }
 *
 * if (size > cache_max)
 *     goto tofree;
 *
 * // Get thread-local tmem structure (same as malloc)
 * uintptr_t tp;
 * __asm__("mrs %0, tpidr_el0" : "=r" (tp));
 * tmem_t *t = (tmem_t *)(tp + umem_tmem_off);
 * void **roots = &t->tm_roots[0];
 *
 * TODO: Generate aarch64 instructions for tag validation and TLS access
 */

/*
 * Pseudocode for cache size check (equivalent to amd64 inicache/gencache):
 *
 * if (size <= CACHE_SIZE) {
 *     csize = CACHE_SIZE;
 *     roots += CACHE_NUM * sizeof(void*);  // advance to correct cache
 *     goto allocbuf;
 * }
 * // else try next cache
 *
 * TODO: Generate aarch64 instructions:
 * - MOV x12, #CACHE_SIZE
 * - CMP x1, x12                 // if (size <= CACHE_SIZE)
 * - B.HI next_cache
 * - MOV x11, #CACHE_SIZE        // x11 = csize
 * - ADD x10, x10, #(CACHE_NUM*8) // advance roots pointer
 * - B allocbuf
 * next_cache:
 */

/*
 * Pseudocode for malloc buffer allocation (equivalent to amd64 malfini):
 *
 * allocbuf:
 * if (*roots == NULL)           // no cached buffers
 *     goto tomalloc;
 *
 * malloc_data_t *ret = (malloc_data_t *)*roots;
 * *roots = *(void **)ret;       // pop from list
 * t->tm_size -= csize;          // update cached size
 *
 * if (size > UMEM_SECOND_ALIGN) {
 *     ret->malloc_size = size;
 *     ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_SECOND_MAGIC, size);
 *     ret += 2;
 * } else {
 *     ret->malloc_size = size;
 *     ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, size);
 *     ret += 1;
 * }
 * return (void *)ret;
 *
 * tomalloc:
 *     return umem_malloc(orig_size);  // tail call to fallback
 *
 * TODO: Generate aarch64 instructions for list manipulation and tag encoding
 */

/*
 * Pseudocode for free buffer return (equivalent to amd64 freefini):
 *
 * rbuf:
 * if (t->tm_size + csize > umem_ptc_size)  // cache full
 *     goto tofree;
 *
 * t->tm_size += csize;          // update cached size
 * *(void **)tag = *roots;       // push onto list
 * *roots = (void *)tag;
 * return;
 *
 * tofree:
 *     umem_malloc_free(buf);    // tail call to fallback
 *     return;
 *
 * TODO: Generate aarch64 instructions for list insertion and size tracking
 */

/*
 * Placeholder functions - not yet implemented
 */
static int __attribute__((unused))
genasm_malinit(uint8_t *bp, uint32_t off, uint32_t ep, uint32_t csize)
{
	(void)bp;
	(void)off;
	(void)ep;
	(void)csize;
	/* TODO: Implement aarch64 malloc initialization code generation */
	return 0;
}

static int __attribute__((unused))
genasm_frinit(uint8_t *bp, uint32_t off, uint32_t dp, uint32_t ep, uint32_t mcs)
{
	(void)bp;
	(void)off;
	(void)dp;
	(void)ep;
	(void)mcs;
	/* TODO: Implement aarch64 free initialization code generation */
	return 0;
}

static int __attribute__((unused))
genasm_firstcache(uint8_t *bp, uint32_t csize, uint32_t ap)
{
	(void)bp;
	(void)csize;
	(void)ap;
	/* TODO: Implement aarch64 first cache check code generation */
	return 0;
}

static int __attribute__((unused))
genasm_gencache(uint8_t *bp, int num, uint32_t csize, uint32_t ap)
{
	(void)bp;
	(void)num;
	(void)csize;
	(void)ap;
	/* TODO: Implement aarch64 generic cache check code generation */
	return 0;
}

static int __attribute__((unused))
genasm_lastcache(uint8_t *bp, int num, uint32_t csize, uint32_t ep)
{
	(void)bp;
	(void)num;
	(void)csize;
	(void)ep;
	/* TODO: Implement aarch64 last cache check code generation */
	return 0;
}

static int __attribute__((unused))
genasm_malfini(uint8_t *bp, uintptr_t mptr)
{
	(void)bp;
	(void)mptr;
	/* TODO: Implement aarch64 malloc finalization code generation */
	return 0;
}

static int __attribute__((unused))
genasm_frfini(uint8_t *bp, uint32_t maxthr, uintptr_t fptr)
{
	(void)bp;
	(void)maxthr;
	(void)fptr;
	/* TODO: Implement aarch64 free finalization code generation */
	return 0;
}

static int __attribute__((unused))
genasm_malloc(void *base, size_t len, int nents, int *umem_alloc_sizes)
{
	(void)base;
	(void)len;
	(void)nents;
	(void)umem_alloc_sizes;
	/* TODO: Implement aarch64 malloc code generation */
	return 1;  /* Return error until implemented */
}

static int __attribute__((unused))
genasm_free(void *base, size_t len, int nents, int *umem_alloc_sizes)
{
	(void)base;
	(void)len;
	(void)nents;
	(void)umem_alloc_sizes;
	/* TODO: Implement aarch64 free code generation */
	return 1;  /* Return error until implemented */
}

#ifndef __sun
/*
 * On Linux, allocate RW memory for the generated PTC code buffers.
 * We try to allocate near the library's text to ensure branch instructions
 * can reach the fallback malloc/free implementations.
 *
 * The buffers are laid out as:
 *   [branch to fallback][space for generated code]
 *
 * Returns 0 on success, 1 on failure.
 *
 * TODO: Implement proper allocation and branch encoding for aarch64
 * Branch range on aarch64:
 *   - B instruction: +/- 128MB (26-bit signed offset * 4)
 *   - BL instruction: +/- 128MB (26-bit signed offset * 4)
 * This is much larger than x86_64's +/- 2GB, so range checking is easier.
 */
static int __attribute__((unused))
genasm_alloc_buffers(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t alloc_size;
	void *hint;
	uint8_t *base;

	if (page_size <= 0)
		page_size = 4096;

	/*
	 * We need space for both malloc and free buffers.
	 * Round up to page size.
	 */
	alloc_size = umem_genasm_msize + umem_genasm_fsize;
	alloc_size = (alloc_size + page_size - 1) & ~(page_size - 1);

	/*
	 * Try to mmap near umem_malloc so branch instructions can reach it.
	 */
	hint = (void *)((uintptr_t)umem_malloc & ~(page_size - 1));
	base = (uint8_t *)mmap(hint, alloc_size,
	    PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (base == MAP_FAILED) {
		/* Try without a hint */
		base = (uint8_t *)mmap(NULL, alloc_size,
		    PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED)
			return (1);
	}

	/*
	 * Verify that the fallback targets are reachable via branch
	 * instructions (must be within +/- 128MB).
	 * TODO: Implement range check for aarch64 branches
	 */
	int64_t malloc_offset = (int64_t)umem_genasm_omptr -
	    (int64_t)(uintptr_t)base;
	int64_t free_offset = (int64_t)umem_genasm_ofptr -
	    (int64_t)(uintptr_t)(base + umem_genasm_msize);

	/* aarch64 B/BL range is +/- 128MB (2^27 bytes) */
	if (malloc_offset < -0x8000000LL || malloc_offset > 0x7ffffffLL ||
	    free_offset < -0x8000000LL || free_offset > 0x7ffffffLL) {
		munmap(base, alloc_size);
		return (1);
	}

	umem_genasm_mmap_base = base;
	umem_genasm_mmap_size = alloc_size;

	/*
	 * Set up malloc buffer: branch to umem_malloc, then space for generated code
	 * TODO: Encode aarch64 B instruction properly
	 * For now, just fill with NOP instructions (0xd503201f)
	 */
	umem_genasm_mptr = (uintptr_t)base;
	uint32_t *instr = (uint32_t *)base;
	for (size_t i = 0; i < umem_genasm_msize / 4; i++) {
		instr[i] = 0xd503201f;  /* NOP instruction */
	}

	/*
	 * Set up free buffer: branch to umem_malloc_free, then space for generated code
	 */
	base += umem_genasm_msize;
	umem_genasm_fptr = (uintptr_t)base;
	instr = (uint32_t *)base;
	for (size_t i = 0; i < umem_genasm_fsize / 4; i++) {
		instr[i] = 0xd503201f;  /* NOP instruction */
	}

	return (0);
}
#endif /* !__sun */

/*ARGSUSED*/
int
umem_genasm(int *cp, umem_cache_t **caches, int nc)
{
	(void)cp;
	(void)caches;
	(void)nc;

	/*
	 * TODO: Remove this early return once implementation is complete.
	 * For now, PTC is disabled on aarch64.
	 */
	return (1);

#if 0  /* Disabled until implementation is complete */
	int nents, i;
	uint8_t *mptr;
	uint8_t *fptr;
	uint64_t v, *vptr;

#ifndef __sun
	/*
	 * On Linux, allocate mmap'd RWX buffers for the generated code.
	 */
	if (genasm_alloc_buffers() != 0)
		return (1);
#endif

	mptr = (void *)((uintptr_t)umem_genasm_mptr + 4);  /* Skip initial branch */
	fptr = (void *)((uintptr_t)umem_genasm_fptr + 4);
	if (umem_genasm_mptr == 0 || umem_genasm_msize == 0 ||
	    umem_genasm_fptr == 0 || umem_genasm_fsize == 0)
		return (1);

	/*
	 * The total number of caches that we can service is the minimum of:
	 *  o the amount supported by libc (_tmem_get_nentries)
	 *  o the total number of umem caches
	 *  o we use pointer arithmetic, so it's UINT32_MAX / sizeof (uintptr_t)
	 *    For 64-bit, this is UINT32_MAX >> 3, plenty.
	 */
	nents = _tmem_get_nentries();

	if (UMEM_GENASM_MAX64 < (unsigned)nents)
		nents = UMEM_GENASM_MAX64;

	if (nc < nents)
		nents = nc;

	/* Based on our constraints, this is not an error */
	if (nents == 0 || umem_ptc_size == 0)
		return (0);

	/* Generate malloc and free code */
	if (genasm_malloc(mptr, umem_genasm_msize - 4, nents, cp) != 0)
		return (1);

	if (genasm_free(fptr, umem_genasm_fsize - 4, nents, cp) != 0)
		return (1);

	/* TODO: Replace initial branch with actual generated code entry point */

#ifndef __sun
	/*
	 * On Linux, make the generated code read-only + executable.
	 * Also set up function pointers so malloc.c can redirect through them.
	 */
	if (umem_genasm_mmap_base != NULL) {
		mprotect(umem_genasm_mmap_base, umem_genasm_mmap_size,
		    PROT_READ | PROT_EXEC);
	}
	umem_genasm_malloc_ptr =
	    (void *(*)(size_t))(uintptr_t)umem_genasm_mptr;
	umem_genasm_free_ptr =
	    (void (*)(void *))(uintptr_t)umem_genasm_fptr;
#endif

	for (i = 0; i < nents; i++)
		caches[i]->cache_flags |= UMF_PTC;

	return (0);
#endif /* 0 */
}

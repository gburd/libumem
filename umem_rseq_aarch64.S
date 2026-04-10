/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * aarch64 (ARM64) assembly implementations of rseq critical sections for umem
 *
 * This file provides true restartable sequences for lock-free per-CPU
 * magazine caching. The kernel automatically restarts operations if
 * CPU migration occurs.
 *
 * Requires: Linux 4.18+, aarch64
 */

#if defined(__aarch64__) && defined(__linux__)

.text

/*
 * rseq signature - unique value to identify rseq abort points
 */
#define RSEQ_SIG 0x53053053

/*
 * Offsets into umem_rseq_cache_t structure
 */
#define CACHE_LOADED_MAG_OFFSET 0
#define CACHE_PREVIOUS_MAG_OFFSET 8
#define CACHE_ROUNDS_OFFSET 16
#define CACHE_PROUNDS_OFFSET 20
#define CACHE_ALLOC_COUNT_OFFSET 24
#define CACHE_RESTART_COUNT_OFFSET 40

/*
 * umem_rseq_alloc_fastpath - Allocate from per-CPU magazine using rseq
 *
 * C prototype:
 *   void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache, int cpu_id);
 *
 * Arguments:
 *   x0 = cache (umem_rseq_cache_t *)
 *   w1 = cpu_id (current CPU from rseq area)
 *
 * Returns:
 *   x0 = allocated object pointer, or NULL if magazine empty
 */
.globl umem_rseq_alloc_fastpath
.type umem_rseq_alloc_fastpath, %function
.align 4
umem_rseq_alloc_fastpath:
	.cfi_startproc

	/* Save frame pointer and link register */
	stp	x29, x30, [sp, #-96]!
	.cfi_adjust_cfa_offset 96
	.cfi_offset 29, -96
	.cfi_offset 30, -88
	mov	x29, sp

	/* Save callee-saved registers we'll use */
	stp	x19, x20, [sp, #16]
	.cfi_offset 19, -80
	.cfi_offset 20, -72
	stp	x21, x22, [sp, #32]
	.cfi_offset 21, -64
	.cfi_offset 22, -56

	/* Save arguments */
	mov	x19, x0		/* cache */
	mov	w20, w1		/* cpu_id */

	/* Set up rseq_cs structure on stack (32-byte aligned) */
	/* Stack already aligned by frame setup */
	add	x21, sp, #48	/* x21 = &rseq_cs */

	/* Fill in rseq_cs structure */
	/* version = 0, flags = 0 */
	stp	wzr, wzr, [x21]
	/* start_ip */
	adrp	x0, .Lrseq_alloc_start
	add	x0, x0, :lo12:.Lrseq_alloc_start
	str	x0, [x21, #8]
	/* post_commit_offset */
	adrp	x1, .Lrseq_alloc_post_commit
	add	x1, x1, :lo12:.Lrseq_alloc_post_commit
	sub	x1, x1, x0
	str	x1, [x21, #16]
	/* abort_ip */
	adrp	x2, .Lrseq_alloc_abort
	add	x2, x2, :lo12:.Lrseq_alloc_abort
	str	x2, [x21, #24]

	/* Get TLS base via TPIDR_EL0 */
	mrs	x22, tpidr_el0

	/* Register critical section */
	/* umem_rseq_area is at fixed TLS offset - get from GOT */
	adrp	x0, :gottprel:umem_rseq_area
	ldr	x0, [x0, #:gottprel_lo12:umem_rseq_area]
	add	x0, x22, x0	/* x0 = &umem_rseq_area */
	str	x21, [x0, #8]	/* umem_rseq_area.rseq_cs = &rseq_cs */

.Lrseq_alloc_start:
	/* CRITICAL SECTION START */

	/* Verify CPU hasn't changed */
	ldr	w1, [x0, #4]	/* Load current cpu_id */
	cmp	w1, w20
	b.ne	.Lrseq_alloc_abort

	/* Load rounds */
	ldr	w2, [x19, #CACHE_ROUNDS_OFFSET]
	cmp	w2, #0
	b.le	.Lrseq_alloc_empty

	/* Load loaded_mag pointer */
	ldr	x3, [x19, #CACHE_LOADED_MAG_OFFSET]
	cbz	x3, .Lrseq_alloc_empty

	/* Decrement rounds */
	sub	w4, w2, #1
	str	w4, [x19, #CACHE_ROUNDS_OFFSET]

	/* Get object from magazine: mag->mag_round[rounds-1] */
	/* Magazine: void *mag_next [offset 0], void *mag_round[] [offset 8] */
	lsl	x2, x2, #3	/* rounds * 8 */
	add	x3, x3, #8	/* &mag_round[0] */
	ldr	x0, [x3, x2]	/* Load mag_round[rounds] */

	/* Increment alloc counter */
	ldr	x4, [x19, #CACHE_ALLOC_COUNT_OFFSET]
	add	x4, x4, #1
	str	x4, [x19, #CACHE_ALLOC_COUNT_OFFSET]

	/* COMMIT POINT */
.Lrseq_alloc_post_commit:
	/* Clear rseq_cs pointer */
	adrp	x1, :gottprel:umem_rseq_area
	ldr	x1, [x1, #:gottprel_lo12:umem_rseq_area]
	add	x1, x22, x1
	str	xzr, [x1, #8]

	/* Restore registers and return */
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	.cfi_adjust_cfa_offset -96
	ret

	/* Magazine empty - return NULL */
.Lrseq_alloc_empty:
	mov	x0, #0
	b	.Lrseq_alloc_post_commit

	/* ABORT HANDLER */
	.align	5
.Lrseq_alloc_abort:
	.inst	RSEQ_SIG

	/* Increment restart counter */
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	add	x1, x1, #1
	str	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]

	/* Clear rseq_cs pointer */
	adrp	x1, :gottprel:umem_rseq_area
	ldr	x1, [x1, #:gottprel_lo12:umem_rseq_area]
	add	x1, x22, x1
	str	xzr, [x1, #8]

	/* Return NULL */
	mov	x0, #0
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	ret

	.cfi_endproc
.size umem_rseq_alloc_fastpath, .-umem_rseq_alloc_fastpath


/*
 * umem_rseq_free_fastpath - Free to per-CPU magazine using rseq
 *
 * C prototype:
 *   int umem_rseq_free_fastpath(umem_rseq_cache_t *cache, void *buf, int cpu_id);
 *
 * Arguments:
 *   x0 = cache (umem_rseq_cache_t *)
 *   x1 = buf (object to free)
 *   w2 = cpu_id (current CPU from rseq area)
 *
 * Returns:
 *   x0 = 0 on success, -1 if magazine full
 */
.globl umem_rseq_free_fastpath
.type umem_rseq_free_fastpath, %function
.align 4
umem_rseq_free_fastpath:
	.cfi_startproc

	/* Save frame pointer and link register */
	stp	x29, x30, [sp, #-96]!
	.cfi_adjust_cfa_offset 96
	.cfi_offset 29, -96
	.cfi_offset 30, -88
	mov	x29, sp

	/* Save callee-saved registers */
	stp	x19, x20, [sp, #16]
	.cfi_offset 19, -80
	.cfi_offset 20, -72
	stp	x21, x22, [sp, #32]
	.cfi_offset 21, -64
	.cfi_offset 22, -56

	/* Save arguments */
	mov	x19, x0		/* cache */
	mov	x20, x1		/* buf */
	mov	w21, w2		/* cpu_id */

	/* Set up rseq_cs structure */
	add	x22, sp, #48

	/* Fill in rseq_cs structure */
	stp	wzr, wzr, [x22]
	adrp	x0, .Lrseq_free_start
	add	x0, x0, :lo12:.Lrseq_free_start
	str	x0, [x22, #8]
	adrp	x1, .Lrseq_free_post_commit
	add	x1, x1, :lo12:.Lrseq_free_post_commit
	sub	x1, x1, x0
	str	x1, [x22, #16]
	adrp	x2, .Lrseq_free_abort
	add	x2, x2, :lo12:.Lrseq_free_abort
	str	x2, [x22, #24]

	/* Get TLS base */
	mrs	x3, tpidr_el0

	/* Register critical section */
	adrp	x0, :gottprel:umem_rseq_area
	ldr	x0, [x0, #:gottprel_lo12:umem_rseq_area]
	add	x0, x3, x0
	str	x22, [x0, #8]

.Lrseq_free_start:
	/* CRITICAL SECTION START */

	/* Verify CPU */
	ldr	w1, [x0, #4]
	cmp	w1, w21
	b.ne	.Lrseq_free_abort

	/* Load loaded_mag pointer */
	ldr	x4, [x19, #CACHE_LOADED_MAG_OFFSET]
	cbz	x4, .Lrseq_free_full

	/* Load rounds */
	ldr	w5, [x19, #CACHE_ROUNDS_OFFSET]

	/* Check if magazine full (assume max 63 rounds) */
	cmp	w5, #63
	b.ge	.Lrseq_free_full

	/* Store object in magazine */
	lsl	x6, x5, #3
	add	x4, x4, #8
	str	x20, [x4, x6]

	/* Increment rounds */
	add	w5, w5, #1
	str	w5, [x19, #CACHE_ROUNDS_OFFSET]

	/* Increment free counter (at offset 32) */
	ldr	x6, [x19, #32]
	add	x6, x6, #1
	str	x6, [x19, #32]

	/* COMMIT POINT */
.Lrseq_free_post_commit:
	/* Clear rseq_cs pointer */
	adrp	x1, :gottprel:umem_rseq_area
	ldr	x1, [x1, #:gottprel_lo12:umem_rseq_area]
	add	x1, x3, x1
	str	xzr, [x1, #8]

	/* Success - return 0 */
	mov	x0, #0
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	.cfi_adjust_cfa_offset -96
	ret

	/* Magazine full */
.Lrseq_free_full:
	mov	x0, #-1
	b	.Lrseq_free_post_commit

	/* ABORT HANDLER */
	.align	5
.Lrseq_free_abort:
	.inst	RSEQ_SIG

	/* Increment restart counter */
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	add	x1, x1, #1
	str	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]

	/* Clear rseq_cs pointer */
	adrp	x1, :gottprel:umem_rseq_area
	ldr	x1, [x1, #:gottprel_lo12:umem_rseq_area]
	add	x1, x3, x1
	str	xzr, [x1, #8]

	/* Return -1 */
	mov	x0, #-1
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	ret

	.cfi_endproc
.size umem_rseq_free_fastpath, .-umem_rseq_free_fastpath

#endif /* __aarch64__ && __linux__ */

/* Mark stack as non-executable (outside #if to always emit) */
#if defined(__linux__)
.section .note.GNU-stack, "", %progbits
#endif

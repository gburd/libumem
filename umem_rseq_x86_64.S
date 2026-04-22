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
 * x86_64 rseq critical sections for lock-free per-CPU magazine caching.
 * Uses rbp-based frames so abort handlers can safely unwind the stack
 * even after rsp alignment (andq $-32, %rsp).
 *
 * The rseq area offset is read from the global umem_rseq_fs_offset,
 * which is set at init time to either glibc's __rseq_offset (when
 * glibc 2.35+ manages rseq) or to umem_rseq_area's TLS offset.
 * Access goes through the GOT because this is a shared library.
 */

#if defined(__x86_64__) && defined(__linux__)

.text

#define RSEQ_SIG 0x53053053

#define RSEQ_CPU_ID_OFFSET 4
#define RSEQ_CS_OFFSET 8

#define CACHE_LOADED_MAG_OFFSET 0
#define CACHE_ROUNDS_OFFSET 16
#define CACHE_ALLOC_COUNT_OFFSET 24
#define CACHE_FREE_COUNT_OFFSET 32
#define CACHE_RESTART_COUNT_OFFSET 40
#define CACHE_MAGSIZE_OFFSET 56

/*
 * Load umem_rseq_fs_offset into the given 64-bit register.
 * Two-step GOT access: load pointer from GOT, then dereference.
 */
.macro LOAD_FS_OFFSET reg
	movq	umem_rseq_fs_offset@GOTPCREL(%rip), \reg
	movslq	(\reg), \reg
.endm

/*
 * void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache, int cpu_id)
 */
.globl umem_rseq_alloc_fastpath
.type umem_rseq_alloc_fastpath, @function
.align 16
umem_rseq_alloc_fastpath:
	.cfi_startproc
	pushq	%rbp
	.cfi_adjust_cfa_offset 8
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	pushq	%rbx
	.cfi_offset %rbx, -24

	movq	%rdi, %rbx

	subq	$64, %rsp
	andq	$-32, %rsp

	/* Build rseq_cs descriptor on stack (32-byte aligned) */
	movl	$0, 0(%rsp)
	movl	$0, 4(%rsp)
	leaq	.Lrseq_alloc_start(%rip), %rax
	movq	%rax, 8(%rsp)
	leaq	.Lrseq_alloc_post_commit(%rip), %rax
	leaq	.Lrseq_alloc_start(%rip), %rcx
	subq	%rcx, %rax
	movq	%rax, 16(%rsp)
	leaq	.Lrseq_alloc_abort(%rip), %rax
	movq	%rax, 24(%rsp)

	/* Point rseq_cs to our descriptor using runtime offset */
	LOAD_FS_OFFSET %rax
	movq	%rsp, %fs:RSEQ_CS_OFFSET(%rax)

.Lrseq_alloc_start:
	/* Read cpu_id from rseq area, compare with expected */
	LOAD_FS_OFFSET %rax
	movl	%fs:RSEQ_CPU_ID_OFFSET(%rax), %ecx
	cmpl	%esi, %ecx
	jne	.Lrseq_alloc_abort

	movl	CACHE_ROUNDS_OFFSET(%rbx), %edx
	testl	%edx, %edx
	jle	.Lrseq_alloc_empty

	movq	CACHE_LOADED_MAG_OFFSET(%rbx), %rcx
	testq	%rcx, %rcx
	jz	.Lrseq_alloc_empty

	leal	-1(%rdx), %eax
	movl	%eax, CACHE_ROUNDS_OFFSET(%rbx)
	movq	8(%rcx,%rdx,8), %rax
	incq	CACHE_ALLOC_COUNT_OFFSET(%rbx)

.Lrseq_alloc_post_commit:
	/* Clear rseq_cs */
	LOAD_FS_OFFSET %rcx
	movq	$0, %fs:RSEQ_CS_OFFSET(%rcx)
	leaq	-8(%rbp), %rsp
	popq	%rbx
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	ret

.Lrseq_alloc_empty:
	xorq	%rax, %rax
	jmp	.Lrseq_alloc_post_commit

	.align	32
.Lrseq_alloc_abort:
	.byte	RSEQ_SIG >> 24, RSEQ_SIG >> 16, RSEQ_SIG >> 8, RSEQ_SIG
	incq	CACHE_RESTART_COUNT_OFFSET(%rbx)
	LOAD_FS_OFFSET %rax
	movq	$0, %fs:RSEQ_CS_OFFSET(%rax)
	xorq	%rax, %rax
	leaq	-8(%rbp), %rsp
	popq	%rbx
	popq	%rbp
	ret
	.cfi_endproc
.size umem_rseq_alloc_fastpath, .-umem_rseq_alloc_fastpath

/*
 * int umem_rseq_free_fastpath(umem_rseq_cache_t *cache, void *buf, int cpu_id)
 */
.globl umem_rseq_free_fastpath
.type umem_rseq_free_fastpath, @function
.align 16
umem_rseq_free_fastpath:
	.cfi_startproc
	pushq	%rbp
	.cfi_adjust_cfa_offset 8
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	pushq	%rbx
	.cfi_offset %rbx, -24
	pushq	%r12
	.cfi_offset %r12, -32
	pushq	%r13
	.cfi_offset %r13, -40

	movq	%rdi, %rbx
	movq	%rsi, %r13
	movl	%edx, %r12d

	subq	$64, %rsp
	andq	$-32, %rsp

	/* Build rseq_cs descriptor on stack (32-byte aligned) */
	movl	$0, 0(%rsp)
	movl	$0, 4(%rsp)
	leaq	.Lrseq_free_start(%rip), %rax
	movq	%rax, 8(%rsp)
	leaq	.Lrseq_free_post_commit(%rip), %rax
	leaq	.Lrseq_free_start(%rip), %rcx
	subq	%rcx, %rax
	movq	%rax, 16(%rsp)
	leaq	.Lrseq_free_abort(%rip), %rax
	movq	%rax, 24(%rsp)

	/* Point rseq_cs to our descriptor using runtime offset */
	LOAD_FS_OFFSET %rax
	movq	%rsp, %fs:RSEQ_CS_OFFSET(%rax)

.Lrseq_free_start:
	/* Read cpu_id from rseq area, compare with expected */
	LOAD_FS_OFFSET %rax
	movl	%fs:RSEQ_CPU_ID_OFFSET(%rax), %ecx
	cmpl	%r12d, %ecx
	jne	.Lrseq_free_abort

	movq	CACHE_LOADED_MAG_OFFSET(%rbx), %rcx
	testq	%rcx, %rcx
	jz	.Lrseq_free_full

	movl	CACHE_ROUNDS_OFFSET(%rbx), %edx
	cmpl	CACHE_MAGSIZE_OFFSET(%rbx), %edx
	jge	.Lrseq_free_full

	movq	%r13, 8(%rcx,%rdx,8)
	incl	%edx
	movl	%edx, CACHE_ROUNDS_OFFSET(%rbx)
	incq	CACHE_FREE_COUNT_OFFSET(%rbx)

.Lrseq_free_post_commit:
	/* Clear rseq_cs */
	LOAD_FS_OFFSET %rax
	movq	$0, %fs:RSEQ_CS_OFFSET(%rax)
	xorq	%rax, %rax
	leaq	-24(%rbp), %rsp
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	ret

.Lrseq_free_full:
	LOAD_FS_OFFSET %rax
	movq	$0, %fs:RSEQ_CS_OFFSET(%rax)
	movq	$-1, %rax
	leaq	-24(%rbp), %rsp
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret

	.align	32
.Lrseq_free_abort:
	.byte	RSEQ_SIG >> 24, RSEQ_SIG >> 16, RSEQ_SIG >> 8, RSEQ_SIG
	incq	CACHE_RESTART_COUNT_OFFSET(%rbx)
	LOAD_FS_OFFSET %rax
	movq	$0, %fs:RSEQ_CS_OFFSET(%rax)
	movq	$-1, %rax
	leaq	-24(%rbp), %rsp
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
	.cfi_endproc
.size umem_rseq_free_fastpath, .-umem_rseq_free_fastpath

.section .note.GNU-stack, "", @progbits

#endif /* __x86_64__ && __linux__ */

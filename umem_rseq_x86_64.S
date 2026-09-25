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
 * P5.13b: mag_round[] slots are stored XOR-mangled
 *   stored = ptr ^ umem_link_cookie ^ (&slot >> 12)
 * exactly as UMEM_SLOT_MANGLE in umem_impl.h does for the cc_lock path.
 * The reload arms cache_rseq[cpu].loaded_mag with a depot magazine whose
 * rounds are mangled, so this asm fast path -- which shares that magazine
 * with the C cc_lock path -- MUST demangle on pop and mangle on push, or it
 * hands the encoded word out as a pointer.  XOR is an involution: one macro
 * both encodes and decodes.  umem_link_cookie is a hidden-visibility
 * non-TLS global (rip-relative, no GOT).  slot_reg holds the ABSOLUTE
 * address of the slot; it is clobbered (shifted) by this macro.  cookie_reg
 * and slot_reg must differ from val_reg.
 */
.macro SLOT_XOR val_reg cookie_reg slot_reg
	movq	umem_link_cookie(%rip), \cookie_reg
	shrq	$12, \slot_reg
	xorq	\cookie_reg, \val_reg
	xorq	\slot_reg, \val_reg
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

	/*
	 * Index with the POST-decrement round count (the slot being
	 * popped), matching the C fast path's
	 * `cc_rounds = rounds - 1; buf = cc_loaded->mag_round[rounds - 1]`.
	 * decl zero-extends %edx into %rdx, so %rdx is safe to use as a
	 * 64-bit array index below. Off-by-one fix: this used to index
	 * with the PRE-decrement %rdx (mag_round[rounds]), which double-
	 * hands-out the top round and reads past the magazine on the
	 * very first pop from a full one. Was undetected because the
	 * reload path that ever produces rounds>0 was dead code.
	 *
	 * ORDERING (rseq ABI requirement, not just a style choice): the
	 * kernel aborts (jumps to abort_ip, discarding all register state
	 * including %rax) if this thread is preempted/migrated/signaled
	 * at ANY point in [start_ip, post_commit_offset) -- i.e. anywhere
	 * from .Lrseq_alloc_start up to (but not including) the
	 * .Lrseq_alloc_post_commit label. That means EVERY store in this
	 * window except the very LAST one is at risk of being "orphaned":
	 * if it retires but a later instruction in the same window is then
	 * interrupted, the abort handler still runs and returns NULL,
	 * discarding the return value -- even though an earlier store
	 * inside the window already changed memory. Concretely: with the
	 * rounds-- store BEFORE the mag_round[] load into %rax, a signal
	 * landing between them commits "one fewer round" to memory while
	 * the popped pointer is never delivered to any caller -- a real,
	 * reproducible LEAK (see test/stress/repro_rseq_trailing_store.c,
	 * confirmed on both x86_64 and aarch64 hardware via a SIGALRM
	 * storm, no migration needed -- ordinary preemption is enough).
	 * Fix: compute %rax (the full return value) and bump the stats
	 * counter (advisory; harmless if "wasted" by an abort that
	 * discards it) FIRST, and make the rounds store the unconditional
	 * LAST instruction before .Lrseq_alloc_post_commit. Then: abort
	 * before the rounds store retires => memory still shows the OLD
	 * rounds count => the object was never actually removed => NULL
	 * is a consistent answer (no leak, safely retryable). Preemption
	 * at/after the rounds store retires is OUTSIDE the checked window
	 * (ordinary preemption, no abort) => execution resumes normally
	 * and returns the already-computed %rax.
	 */
	decl	%edx
	leaq	8(%rcx,%rdx,8), %rsi	/* &mag_round[rounds-1] (absolute) */
	movq	(%rsi), %rax		/* mangled slot value */
	/* P5.13b: demangle.  %rax=stored, %rsi=&slot, %r8=cookie scratch. */
	SLOT_XOR %rax, %r8, %rsi
	incq	CACHE_ALLOC_COUNT_OFFSET(%rbx)
	movl	%edx, CACHE_ROUNDS_OFFSET(%rbx)	/* COMMIT: last store */

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
	.long	RSEQ_SIG	/* signature BEFORE label — kernel reads *(abort_ip-4) */
.Lrseq_alloc_abort:
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

	/*
	 * ORDERING (rseq ABI requirement -- see the matching comment in
	 * umem_rseq_alloc_fastpath above): the rounds++ store below MUST be
	 * the unconditional LAST store before .Lrseq_free_post_commit.
	 * Previously the array write (buf into mag_round[]), rounds++, AND
	 * the stats counter were three separate stores with rounds++ in the
	 * MIDDLE -- a signal landing between rounds++ and the stats store
	 * left rounds already bumped (buf now logically live and poppable
	 * by a future alloc) while the caller was told -1 (failed) and
	 * would free the SAME buffer again via the fallback path: a real
	 * double-presence/double-free hazard (confirmed via signal-storm
	 * repro, test/stress/repro_rseq_trailing_store.c, no migration
	 * needed). The array write happening before the final commit is
	 * safe even if "wasted" by an abort: if rounds is NOT bumped, the
	 * alloc fast path never reads that slot (it only reads indices <
	 * rounds), so the write is silently overwritten by the next
	 * successful free into the same slot.
	 */
	leaq	8(%rcx,%rdx,8), %rsi	/* &mag_round[rounds] (absolute) */
	movq	%r13, %rax		/* buf */
	/* P5.13b: mangle before store.  %rax=buf, %rsi=&slot, %r8=cookie. */
	SLOT_XOR %rax, %r8, %rsi
	leaq	8(%rcx,%rdx,8), %rsi	/* recompute &slot (SLOT_XOR shifted %rsi) */
	movq	%rax, (%rsi)
	incq	CACHE_FREE_COUNT_OFFSET(%rbx)
	incl	%edx
	movl	%edx, CACHE_ROUNDS_OFFSET(%rbx)	/* COMMIT: last store */

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
	.long	RSEQ_SIG	/* signature BEFORE label — kernel reads *(abort_ip-4) */
.Lrseq_free_abort:
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

/*
 * int umem_rseq_reload_alloc_commit(umem_rseq_cache_t *cache, int cpu_id,
 *     umem_magazine_t *new_mag, int new_rounds, umem_magazine_t **old_mag_out,
 *     int *old_rounds_out)
 *
 * Phase 2 of the armed rseq reload (design:
 * docs/results/2026-09-09-rseq-reload-asm-design.md).  Phase 1 (a depot
 * pull, in C) is done by the caller and does NOT touch cache_rseq[cpu];
 * this function is the ONLY thing that publishes the swap, and it does so
 * inside its OWN rseq critical section so it is atomic w.r.t. this thread's
 * occupancy of `cpu` -- the same kernel guarantee the fast path has.  This
 * closes the migration-safety gap the v2 analysis proved a C-only or
 * lock-based reload cannot (at most one writer per slot at any instant).
 *
 * Returns 1 if committed (caller reclassifies *old_mag_out via
 * umem_ptc_mag_return using *old_rounds_out), 0 if aborted (thread migrated
 * between the caller reading cpu_id and this commit; caller retries against
 * the new cpu -- *old_mag_out / *old_rounds_out untouched).
 *
 * old_rounds_out: the migration path can strand a NON-empty old magazine
 * here (thread migrated onto a cpu whose loaded_mag still had rounds), and
 * the depot's ml_full/ml_empty lists have exact-count contracts, so the C
 * caller must know the old rounds to classify it (umem_ptc_mag_return).
 * Both *old_mag_out and *old_rounds_out are read/written BEFORE the
 * three-step commit, so the SINGLE unconditional last store is still the
 * `rounds = new_rounds` store below (bug-6 discipline preserved): the two
 * caller-pointer writes are ordinary safe-to-waste pre-commit stores, not
 * part of the committed cache_rseq[cpu] state.
 *
 * SysV AMD64: %rdi=cache %esi=cpu_id %rdx=new_mag %ecx=new_rounds
 *             %r8=old_mag_out %r9=old_rounds_out.
 */
.globl umem_rseq_reload_alloc_commit
.type umem_rseq_reload_alloc_commit, @function
.align 16
umem_rseq_reload_alloc_commit:
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
	pushq	%r14
	.cfi_offset %r14, -48
	pushq	%r15
	.cfi_offset %r15, -56

	movq	%rdi, %rbx		/* cache */
	movq	%rdx, %r12		/* new_mag */
	movl	%ecx, %r13d		/* new_rounds */
	movq	%r8, %r14		/* old_mag_out */
	movq	%r9, %r15		/* old_rounds_out */
	/* %esi (cpu_id) used directly in the critical section below */

	subq	$64, %rsp
	andq	$-32, %rsp

	/* Build rseq_cs descriptor on stack (32-byte aligned) */
	movl	$0, 0(%rsp)
	movl	$0, 4(%rsp)
	leaq	.Lrseq_reload_alloc_start(%rip), %rax
	movq	%rax, 8(%rsp)
	leaq	.Lrseq_reload_alloc_post_commit(%rip), %rax
	leaq	.Lrseq_reload_alloc_start(%rip), %rcx
	subq	%rcx, %rax
	movq	%rax, 16(%rsp)
	leaq	.Lrseq_reload_alloc_abort(%rip), %rax
	movq	%rax, 24(%rsp)

	LOAD_FS_OFFSET %rax
	movq	%rsp, %fs:RSEQ_CS_OFFSET(%rax)

.Lrseq_reload_alloc_start:
	LOAD_FS_OFFSET %rax
	movl	%fs:RSEQ_CPU_ID_OFFSET(%rax), %ecx
	cmpl	%esi, %ecx
	jne	.Lrseq_reload_alloc_abort

	/* Read OLD loaded_mag AND rounds BEFORE any write (design section 6
	 * + old-magazine classification fix).  Both go to caller pointers;
	 * neither is part of the committed cache_rseq[cpu] state, so the
	 * single unconditional last store below is unaffected. */
	movq	CACHE_LOADED_MAG_OFFSET(%rbx), %rax
	movq	%rax, (%r14)		/* *old_mag_out = OLD loaded_mag */
	movl	CACHE_ROUNDS_OFFSET(%rbx), %eax
	movl	%eax, (%r15)		/* *old_rounds_out = OLD rounds */

	/*
	 * Resolution A (design section 4): rounds=0 (transient safe-empty),
	 * then loaded_mag=new_mag, then rounds=new_rounds as the single
	 * unconditional last store.  An abort after any prefix leaves a
	 * consistent view: rounds=0 => fast path treats the slot as empty
	 * (falls back to cc_lock, always correct); the real rounds is
	 * exposed only once loaded_mag already points at new_mag.
	 */
	movl	$0, CACHE_ROUNDS_OFFSET(%rbx)
	movq	%r12, CACHE_LOADED_MAG_OFFSET(%rbx)
	movl	%r13d, CACHE_ROUNDS_OFFSET(%rbx)	/* COMMIT: last store */

	movl	$1, %eax		/* committed */

.Lrseq_reload_alloc_post_commit:
	LOAD_FS_OFFSET %rcx
	movq	$0, %fs:RSEQ_CS_OFFSET(%rcx)
	leaq	-40(%rbp), %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	ret

	.align	32
	.long	RSEQ_SIG	/* signature BEFORE label */
.Lrseq_reload_alloc_abort:
	incq	CACHE_RESTART_COUNT_OFFSET(%rbx)
	LOAD_FS_OFFSET %rax
	movq	$0, %fs:RSEQ_CS_OFFSET(%rax)
	xorl	%eax, %eax		/* aborted; caller retries */
	leaq	-40(%rbp), %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
	.cfi_endproc
.size umem_rseq_reload_alloc_commit, .-umem_rseq_reload_alloc_commit

/*
 * int umem_rseq_reload_free_commit(umem_rseq_cache_t *cache, int cpu_id,
 *     umem_magazine_t *new_mag, umem_magazine_t **old_mag_out,
 *     int *old_rounds_out)
 *
 * Free-side phase 2: swap in a fresh EMPTY magazine (new_rounds is always
 * 0), simpler than the alloc side (no section 4 transient-zero subtlety --
 * the final rounds value IS zero regardless of the old value).  Write
 * loaded_mag first, rounds=0 last (unconditional commit store).
 *
 * old_rounds_out: as on the alloc side, the migration path can strand a
 * partial old magazine here; the caller needs its rounds to classify it
 * for the depot.  Written BEFORE the commit stores, so the single last
 * store (rounds=0) discipline is preserved.
 *
 * SysV AMD64: %rdi=cache %esi=cpu_id %rdx=new_mag %rcx=old_mag_out
 *             %r8=old_rounds_out.
 */
.globl umem_rseq_reload_free_commit
.type umem_rseq_reload_free_commit, @function
.align 16
umem_rseq_reload_free_commit:
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
	pushq	%r14
	.cfi_offset %r14, -48

	movq	%rdi, %rbx		/* cache */
	movq	%rdx, %r12		/* new_mag (empty) */
	movq	%rcx, %r13		/* old_mag_out */
	movq	%r8, %r14		/* old_rounds_out */
	/* %esi (cpu_id) used directly below */

	subq	$64, %rsp
	andq	$-32, %rsp

	movl	$0, 0(%rsp)
	movl	$0, 4(%rsp)
	leaq	.Lrseq_reload_free_start(%rip), %rax
	movq	%rax, 8(%rsp)
	leaq	.Lrseq_reload_free_post_commit(%rip), %rax
	leaq	.Lrseq_reload_free_start(%rip), %rcx
	subq	%rcx, %rax
	movq	%rax, 16(%rsp)
	leaq	.Lrseq_reload_free_abort(%rip), %rax
	movq	%rax, 24(%rsp)

	LOAD_FS_OFFSET %rax
	movq	%rsp, %fs:RSEQ_CS_OFFSET(%rax)

.Lrseq_reload_free_start:
	LOAD_FS_OFFSET %rax
	movl	%fs:RSEQ_CPU_ID_OFFSET(%rax), %ecx
	cmpl	%esi, %ecx
	jne	.Lrseq_reload_free_abort

	/* Read OLD loaded_mag AND rounds before overwrite (classification). */
	movq	CACHE_LOADED_MAG_OFFSET(%rbx), %rax
	movq	%rax, (%r13)		/* *old_mag_out = OLD loaded_mag */
	movl	CACHE_ROUNDS_OFFSET(%rbx), %eax
	movl	%eax, (%r14)		/* *old_rounds_out = OLD rounds */

	movq	%r12, CACHE_LOADED_MAG_OFFSET(%rbx)	/* new empty mag */
	movl	$0, CACHE_ROUNDS_OFFSET(%rbx)		/* COMMIT: last store */

	movl	$1, %eax

.Lrseq_reload_free_post_commit:
	LOAD_FS_OFFSET %rcx
	movq	$0, %fs:RSEQ_CS_OFFSET(%rcx)
	leaq	-32(%rbp), %rsp
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	ret

	.align	32
	.long	RSEQ_SIG
.Lrseq_reload_free_abort:
	incq	CACHE_RESTART_COUNT_OFFSET(%rbx)
	LOAD_FS_OFFSET %rax
	movq	$0, %fs:RSEQ_CS_OFFSET(%rax)
	xorl	%eax, %eax
	leaq	-32(%rbp), %rsp
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
	.cfi_endproc
.size umem_rseq_reload_free_commit, .-umem_rseq_reload_free_commit

.section .note.GNU-stack, "", @progbits

#endif /* __x86_64__ && __linux__ */

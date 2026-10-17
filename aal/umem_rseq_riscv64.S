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
 * riscv64 (RV64) assembly implementations of rseq critical sections for umem.
 *
 * A direct port of aal/umem_rseq_aarch64.S -- same four entry points, same
 * rseq_cs descriptor layout, the same two correctness properties (the
 * trailing-store ORDERING that makes an abort/preemption anywhere in the
 * window leave memory consistent, and the P5.13b slot mangling), rendered in
 * the RV64 ABI.  Read the aarch64 file's block comments for the WHY; this
 * file only notes the RISC-V-specific details.
 *
 * Requires: Linux (rseq >= 4.18, RV64 rseq support >= 5.x), rv64.
 *
 * RISC-V specifics vs aarch64:
 *   - TLS base is the thread pointer tp (x4); aarch64 reads TPIDR_EL0.
 *   - args a0..a7; callee-saved s0..s11; ra = x1; sp = x2.
 *   - PC-relative label addresses via `lla` (load local address).
 *   - umem_rseq_fs_offset / umem_link_cookie are read GOT-indirect
 *     (-fPIC shared lib) via `la` (which uses the GOT for a global).
 *   - RSEQ signature is 0x53053053 on RV64 (kernel
 *     tools/testing/selftests/rseq/rseq-riscv.h; the C side's
 *     UMEM_RSEQ_SIG #else branch already selects this -- NO aarch64-style
 *     signature mismatch here, because RV64 and the kernel agree on the
 *     x86-style filler value).  Emitted as a .word immediately before each
 *     abort label; the kernel reads *(abort_ip-4) and SIGSEGVs on mismatch.
 *   - No branch-delay slots (unlike sparc); straightforward.
 *
 * rseq_cs descriptor (as the kernel defines it): u32 version, u32 flags,
 * u64 start_ip, u64 post_commit_offset, u64 abort_ip = 32 bytes, 32-aligned.
 * rseq_area layout we touch: +4 = cpu_id (u32), +8 = rseq_cs pointer (u64).
 */

#if defined(__riscv) && (__riscv_xlen == 64) && defined(__linux__)

.text

#define RSEQ_SIG 0x53053053

/* Offsets into umem_rseq_cache_t (must match umem_rseq_aarch64.S / the C struct). */
#define CACHE_LOADED_MAG_OFFSET   0
#define CACHE_PREVIOUS_MAG_OFFSET 8
#define CACHE_ROUNDS_OFFSET       16
#define CACHE_PROUNDS_OFFSET      20
#define CACHE_ALLOC_COUNT_OFFSET  24
#define CACHE_FREE_COUNT_OFFSET   32
#define CACHE_RESTART_COUNT_OFFSET 40
#define CACHE_MAGSIZE_OFFSET      56

/*
 * LOAD_RSEQ_OFFSET reg -- reg = (sign-extended) umem_rseq_fs_offset, the
 * offset from tp to the ACTIVE rseq area (glibc's when glibc owns rseq,
 * else umem's).  umem_rseq_fs_offset is a plain non-TLS global int, read
 * GOT-indirect in a PIC shared object.  Mirrors the aarch64 macro.
 */
.macro LOAD_RSEQ_OFFSET reg
	la	\reg, umem_rseq_fs_offset
	lw	\reg, 0(\reg)		/* lw sign-extends into the 64-bit reg on RV64 */
.endm

/*
 * SLOT_XOR val, cookie, shift, slot -- P5.13b mangle/demangle (an
 * involution; same macro encodes and decodes):
 *   stored = ptr ^ umem_link_cookie ^ (&slot >> 12)
 * exactly as UMEM_SLOT_MANGLE in umem_impl.h.  val is transformed in place;
 * cookie, shift are scratch; slot holds the absolute slot address on entry.
 * umem_link_cookie is a hidden-visibility non-TLS global.
 */
.macro SLOT_XOR val cookie shift slot
	la	\cookie, umem_link_cookie
	ld	\cookie, 0(\cookie)
	srli	\shift, \slot, 12
	xor	\val, \val, \cookie
	xor	\val, \val, \shift
.endm

/*
 * void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache, int cpu_id);
 *   a0 = cache, a1 = cpu_id; returns a0 = object or NULL.
 */
.globl umem_rseq_alloc_fastpath
.type umem_rseq_alloc_fastpath, @function
.align 2
umem_rseq_alloc_fastpath:
	.cfi_startproc
	addi	sp, sp, -96
	.cfi_adjust_cfa_offset 96
	sd	ra, 88(sp)
	.cfi_offset 1, -8
	sd	s0, 80(sp)
	.cfi_offset 8, -16
	sd	s1, 72(sp)
	.cfi_offset 9, -24
	sd	s2, 64(sp)
	.cfi_offset 18, -32
	sd	s3, 56(sp)
	.cfi_offset 19, -40

	mv	s0, a0		/* cache */
	mv	s1, a1		/* cpu_id (32-bit value in a reg) */

	/* rseq_cs on the stack at sp+0 .. sp+32 (32-byte aligned: sp is
	 * 16-aligned on entry, 96 keeps it 16-aligned; sp+0 is fine for the
	 * kernel which only needs 32-bit alignment of the struct). */
	mv	s2, sp		/* s2 = &rseq_cs */

	/* version=0 flags=0 */
	sw	zero, 0(s2)
	sw	zero, 4(s2)
	/* start_ip */
	lla	t0, .Lrv_alloc_start
	sd	t0, 8(s2)
	/* post_commit_offset = post_commit - start */
	lla	t1, .Lrv_alloc_post_commit
	sub	t1, t1, t0
	sd	t1, 16(s2)
	/* abort_ip */
	lla	t2, .Lrv_alloc_abort
	sd	t2, 24(s2)

	/* s3 = &active rseq area = tp + umem_rseq_fs_offset */
	LOAD_RSEQ_OFFSET t3
	add	s3, tp, t3
	sd	s2, 8(s3)	/* rseq_area.rseq_cs = &rseq_cs */

.Lrv_alloc_start:
	/* CRITICAL SECTION START */
	lw	t0, 4(s3)	/* current cpu_id */
	bne	t0, s1, .Lrv_alloc_abort

	lw	t1, CACHE_ROUNDS_OFFSET(s0)	/* rounds */
	blez	t1, .Lrv_alloc_empty

	ld	t2, CACHE_LOADED_MAG_OFFSET(s0)	/* loaded_mag */
	beqz	t2, .Lrv_alloc_empty

	addi	t3, t1, -1	/* t3 = rounds-1 = slot to pop (post-decrement) */
	slli	t4, t3, 3	/* (rounds-1)*8 */
	addi	t2, t2, 8	/* &mag_round[0] */
	add	t5, t2, t4	/* &mag_round[rounds-1] (absolute) */
	ld	a0, 0(t5)	/* mangled slot -> return value */
	/* demangle: a0=stored, t6=cookie, a2=shift, t5=&slot */
	SLOT_XOR a0, t6, a2, t5

	/* advisory alloc counter (safe to waste on abort) */
	ld	a3, CACHE_ALLOC_COUNT_OFFSET(s0)
	addi	a3, a3, 1
	sd	a3, CACHE_ALLOC_COUNT_OFFSET(s0)

	/*
	 * ORDERING (rseq ABI): the rounds store is the unconditional LAST
	 * store before post_commit.  Abort before it retires => memory
	 * still shows the old rounds => object never removed => NULL is a
	 * consistent, retryable answer (no leak).  See the aarch64 file's
	 * ORDERING note (repro_rseq_trailing_store.c).
	 */
	sw	t3, CACHE_ROUNDS_OFFSET(s0)	/* COMMIT: last store */

.Lrv_alloc_post_commit:
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)	/* clear rseq_cs */

	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	addi	sp, sp, 96
	.cfi_adjust_cfa_offset -96
	ret

.Lrv_alloc_empty:
	li	a0, 0
	j	.Lrv_alloc_post_commit

	/* signature IMMEDIATELY before abort_ip */
	.align	4
	.word	RSEQ_SIG
.Lrv_alloc_abort:
	ld	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	addi	t0, t0, 1
	sd	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	li	a0, 0
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	addi	sp, sp, 96
	ret
	.cfi_endproc
.size umem_rseq_alloc_fastpath, .-umem_rseq_alloc_fastpath

/*
 * int umem_rseq_free_fastpath(umem_rseq_cache_t *cache, void *buf, int cpu_id);
 *   a0=cache a1=buf a2=cpu_id; returns 0 ok / -1 full.
 */
.globl umem_rseq_free_fastpath
.type umem_rseq_free_fastpath, @function
.align 2
umem_rseq_free_fastpath:
	.cfi_startproc
	addi	sp, sp, -96
	.cfi_adjust_cfa_offset 96
	sd	ra, 88(sp)
	sd	s0, 80(sp)
	sd	s1, 72(sp)
	sd	s2, 64(sp)
	sd	s3, 56(sp)
	sd	s4, 48(sp)

	mv	s0, a0		/* cache */
	mv	s1, a1		/* buf */
	mv	s2, a2		/* cpu_id */

	mv	s3, sp		/* &rseq_cs */
	sw	zero, 0(s3)
	sw	zero, 4(s3)
	lla	t0, .Lrv_free_start
	sd	t0, 8(s3)
	lla	t1, .Lrv_free_post_commit
	sub	t1, t1, t0
	sd	t1, 16(s3)
	lla	t2, .Lrv_free_abort
	sd	t2, 24(s3)

	LOAD_RSEQ_OFFSET t3
	add	s4, tp, t3	/* s4 = &active rseq area */
	sd	s3, 8(s4)

.Lrv_free_start:
	lw	t0, 4(s4)
	bne	t0, s2, .Lrv_free_abort

	ld	t1, CACHE_LOADED_MAG_OFFSET(s0)	/* loaded_mag */
	beqz	t1, .Lrv_free_full

	lw	t2, CACHE_ROUNDS_OFFSET(s0)	/* rounds */
	/* full? compare against the cache's actual magsize, NOT a constant */
	lw	t3, CACHE_MAGSIZE_OFFSET(s0)
	bge	t2, t3, .Lrv_free_full

	slli	t4, t2, 3	/* rounds*8 */
	addi	t1, t1, 8	/* &mag_round[0] */
	add	a0, t1, t4	/* &mag_round[rounds] (absolute) */
	mv	a1, s1		/* buf */
	/* mangle before store: a1=buf, t5=cookie, t6=shift, a0=&slot */
	SLOT_XOR a1, t5, t6, a0
	sd	a1, 0(a0)

	/* advisory free counter */
	ld	t5, CACHE_FREE_COUNT_OFFSET(s0)
	addi	t5, t5, 1
	sd	t5, CACHE_FREE_COUNT_OFFSET(s0)

	/*
	 * ORDERING: rounds++ is the unconditional LAST store before
	 * post_commit (see the aarch64 ORDERING note -- a signal between a
	 * mid-window rounds++ and a later store is a double-presence
	 * hazard).  Array write and counter are "wasteable" on abort: the
	 * alloc path only reads indices < rounds, so an un-bumped slot is
	 * never seen.
	 */
	addi	t2, t2, 1
	sw	t2, CACHE_ROUNDS_OFFSET(s0)	/* COMMIT: last store */

.Lrv_free_post_commit:
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	li	a0, 0		/* success */
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	ld	s4, 48(sp)
	addi	sp, sp, 96
	.cfi_adjust_cfa_offset -96
	ret

	/* magazine full: own epilogue, must NOT fall into the success path */
.Lrv_free_full:
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	li	a0, -1
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	ld	s4, 48(sp)
	addi	sp, sp, 96
	ret

	.align	4
	.word	RSEQ_SIG
.Lrv_free_abort:
	ld	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	addi	t0, t0, 1
	sd	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	li	a0, -1
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	ld	s4, 48(sp)
	addi	sp, sp, 96
	ret
	.cfi_endproc
.size umem_rseq_free_fastpath, .-umem_rseq_free_fastpath

/*
 * int umem_rseq_reload_detach(umem_rseq_cache_t *cache, int cpu_id,
 *     umem_magazine_t **old_mag_out, int *old_rounds_out);
 *   a0=cache a1=cpu_id a2=old_mag_out a3=old_rounds_out; 1=detached 0=aborted.
 * Exactly ONE slot store (loaded_mag = NULL) as the last window instruction;
 * result set before it.
 */
.globl umem_rseq_reload_detach
.type umem_rseq_reload_detach, @function
.align 2
umem_rseq_reload_detach:
	.cfi_startproc
	addi	sp, sp, -96
	.cfi_adjust_cfa_offset 96
	sd	ra, 88(sp)
	sd	s0, 80(sp)
	sd	s1, 72(sp)
	sd	s2, 64(sp)
	sd	s3, 56(sp)
	sd	s4, 48(sp)

	mv	s0, a0		/* cache */
	mv	s1, a1		/* cpu_id */
	mv	s2, a2		/* old_mag_out */
	mv	s3, a3		/* old_rounds_out */

	mv	s4, sp		/* &rseq_cs */
	sw	zero, 0(s4)
	sw	zero, 4(s4)
	lla	t0, .Lrv_detach_start
	sd	t0, 8(s4)
	lla	t1, .Lrv_detach_post_commit
	sub	t1, t1, t0
	sd	t1, 16(s4)
	lla	t2, .Lrv_detach_abort
	sd	t2, 24(s4)

	LOAD_RSEQ_OFFSET t3
	add	t3, tp, t3
	sd	s4, 8(t3)	/* register (reuse tp+off below to clear) */

.Lrv_detach_start:
	lw	t0, 4(t3)
	bne	t0, s1, .Lrv_detach_abort

	ld	t1, CACHE_LOADED_MAG_OFFSET(s0)
	sd	t1, 0(s2)	/* *old_mag_out */
	lw	t2, CACHE_ROUNDS_OFFSET(s0)
	sw	t2, 0(s3)	/* *old_rounds_out */
	li	a0, 1		/* result BEFORE commit */
	sd	zero, CACHE_LOADED_MAG_OFFSET(s0)	/* COMMIT: only slot store */

.Lrv_detach_post_commit:
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	ld	s4, 48(sp)
	addi	sp, sp, 96
	.cfi_adjust_cfa_offset -96
	ret

	.align	4
	.word	RSEQ_SIG
.Lrv_detach_abort:
	ld	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	addi	t0, t0, 1
	sd	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	li	a0, 0
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	ld	s4, 48(sp)
	addi	sp, sp, 96
	ret
	.cfi_endproc
.size umem_rseq_reload_detach, .-umem_rseq_reload_detach

/*
 * int umem_rseq_reload_attach(umem_rseq_cache_t *cache, int cpu_id,
 *     umem_magazine_t *new_mag, int rounds, int magsize);
 *   a0=cache a1=cpu_id a2=new_mag a3=rounds a4=magsize; 1=attached 0=occupied/abort.
 */
.globl umem_rseq_reload_attach
.type umem_rseq_reload_attach, @function
.align 2
umem_rseq_reload_attach:
	.cfi_startproc
	addi	sp, sp, -96
	.cfi_adjust_cfa_offset 96
	sd	ra, 88(sp)
	sd	s0, 80(sp)
	sd	s1, 72(sp)
	sd	s2, 64(sp)
	sd	s3, 56(sp)
	sd	s4, 48(sp)
	sd	s5, 40(sp)

	mv	s0, a0		/* cache */
	mv	s1, a1		/* cpu_id */
	mv	s2, a2		/* new_mag */
	mv	s3, a3		/* rounds */
	mv	s4, a4		/* magsize */

	mv	s5, sp		/* &rseq_cs */
	sw	zero, 0(s5)
	sw	zero, 4(s5)
	lla	t0, .Lrv_attach_start
	sd	t0, 8(s5)
	lla	t1, .Lrv_attach_post_commit
	sub	t1, t1, t0
	sd	t1, 16(s5)
	lla	t2, .Lrv_attach_abort
	sd	t2, 24(s5)

	LOAD_RSEQ_OFFSET t3
	add	t3, tp, t3
	sd	s5, 8(t3)

.Lrv_attach_start:
	lw	t0, 4(t3)
	bne	t0, s1, .Lrv_attach_abort

	li	a0, 0		/* 0 = slot occupied, nothing written */
	ld	t1, CACHE_LOADED_MAG_OFFSET(s0)
	bnez	t1, .Lrv_attach_post_commit

	/* don't-care while loaded_mag is NULL: wasteable on abort */
	sw	s3, CACHE_ROUNDS_OFFSET(s0)
	sw	s4, CACHE_MAGSIZE_OFFSET(s0)
	li	a0, 1		/* result BEFORE commit */
	sd	s2, CACHE_LOADED_MAG_OFFSET(s0)	/* COMMIT: last store */

.Lrv_attach_post_commit:
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	ld	s4, 48(sp)
	ld	s5, 40(sp)
	addi	sp, sp, 96
	.cfi_adjust_cfa_offset -96
	ret

	.align	4
	.word	RSEQ_SIG
.Lrv_attach_abort:
	ld	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	addi	t0, t0, 1
	sd	t0, CACHE_RESTART_COUNT_OFFSET(s0)
	LOAD_RSEQ_OFFSET t0
	add	t0, tp, t0
	sd	zero, 8(t0)
	li	a0, 0
	ld	ra, 88(sp)
	ld	s0, 80(sp)
	ld	s1, 72(sp)
	ld	s2, 64(sp)
	ld	s3, 56(sp)
	ld	s4, 48(sp)
	ld	s5, 40(sp)
	addi	sp, sp, 96
	ret
	.cfi_endproc
.size umem_rseq_reload_attach, .-umem_rseq_reload_attach

#endif /* __riscv && xlen==64 && __linux__ */

/* Non-executable stack marker (always emit on Linux). */
#if defined(__linux__)
.section .note.GNU-stack, "", @progbits
#endif

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
 * rseq signature - unique value to identify rseq abort points.
 *
 * MUST match the architecture-specific signature glibc/the kernel use for
 * aarch64: the encoding of "BRK #0x45e0" (see glibc's
 * sysdeps/unix/sysv/linux/aarch64/bits/rseq.h RSEQ_SIG_CODE, and the
 * kernel's tools/testing/selftests/rseq/rseq-arm64.h). This is NOT the
 * same value as x86_64's RSEQ_SIG (0x53053053, an arbitrary filler value
 * with no architectural meaning on x86) -- aarch64's must be a real BRK
 * encoding because the kernel validates *(abort_ip-4) against the
 * signature the thread registered via rseq(2)'s 4th argument, and
 * glibc >= 2.35 (which owns rseq registration whenever it is present,
 * including on this target) registers with ITS OWN 0xd428bc00, not
 * umem's previous copy-pasted 0x53053053. A mismatch here is not a
 * silent no-op: the kernel treats it as a "possible attack attempt"
 * and SIGSEGVs the thread the first time a real migration lands inside
 * the critical section -- verified by reproducing exactly this crash
 * on Graviton (c8g.metal) hardware before this fix.
 */
#define RSEQ_SIG 0xd428bc00

/*
 * Offsets into umem_rseq_cache_t structure
 */
#define CACHE_LOADED_MAG_OFFSET 0
#define CACHE_PREVIOUS_MAG_OFFSET 8
#define CACHE_ROUNDS_OFFSET 16
#define CACHE_PROUNDS_OFFSET 20
#define CACHE_ALLOC_COUNT_OFFSET 24
#define CACHE_RESTART_COUNT_OFFSET 40
#define CACHE_MAGSIZE_OFFSET 56

/*
 * Load umem_rseq_fs_offset (a plain, non-TLS global int; GOT-indirect
 * access since this is a shared library) sign-extended into a 64-bit
 * register. This is the offset from the thread pointer (TPIDR_EL0) to
 * the CURRENTLY ACTIVE rseq area: glibc's own struct when glibc manages
 * rseq (glibc >= 2.35, umem_rseq_use_glibc=1, offset = __rseq_offset),
 * or umem's private umem_rseq_area otherwise. Mirrors umem_rseq_x86_64.S's
 * LOAD_FS_OFFSET macro -- see umem_rseq_init() in umem_rseq.c for how the
 * two cases set umem_rseq_fs_offset. A previous version of this file used
 * a fixed :gottprel:umem_rseq_area offset here, which only ever pointed at
 * umem's own (unregistered, when glibc owns rseq) struct -- the cpu_id
 * check below then always mismatched and every call fell through to
 * .Lrseq_*_abort, silently disabling the fast path on any glibc >= 2.35
 * aarch64 target.
 */
.macro LOAD_RSEQ_OFFSET reg
	adrp	\reg, :got:umem_rseq_fs_offset
	ldr	\reg, [\reg, #:got_lo12:umem_rseq_fs_offset]
	ldrsw	\reg, [\reg]
.endm

/*
 * P5.13b: mag_round[] slots are stored XOR-mangled
 *   stored = ptr ^ umem_link_cookie ^ (&slot >> 12)
 * exactly as UMEM_SLOT_MANGLE in umem_impl.h.  The armed reload puts a
 * depot magazine (mangled rounds) into cache_rseq[cpu].loaded_mag, shared
 * with the C cc_lock path, so this fast path MUST demangle on pop and
 * mangle on push.  XOR is an involution: one macro encodes and decodes.
 * umem_link_cookie is a hidden-visibility non-TLS global (no GOT).
 * val_reg is transformed in place; cookie_reg, shift_reg, slot_reg are
 * scratch and must all differ from val_reg.  slot_reg holds the absolute
 * slot address on entry.
 */
.macro SLOT_XOR val_reg cookie_reg shift_reg slot_reg
	adrp	\cookie_reg, umem_link_cookie
	ldr	\cookie_reg, [\cookie_reg, #:lo12:umem_link_cookie]
	lsr	\shift_reg, \slot_reg, #12
	eor	\val_reg, \val_reg, \cookie_reg
	eor	\val_reg, \val_reg, \shift_reg
.endm

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

	/* Register critical section: x0 = &active_rseq_area, using the
	 * runtime fs-relative offset (handles both glibc-managed and
	 * umem-managed rseq -- see LOAD_RSEQ_OFFSET above). */
	LOAD_RSEQ_OFFSET x0
	add	x0, x22, x0	/* x0 = &active rseq area */
	str	x21, [x0, #8]	/* rseq_area.rseq_cs = &rseq_cs */

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

	/* Decrement rounds. w4 (zero-extended into x4) holds the
	 * POST-decrement count == the slot being popped. Do NOT store it
	 * yet -- see the ORDERING note below. */
	sub	w4, w2, #1

	/* Get object from magazine: mag->mag_round[rounds-1] */
	/* Magazine: void *mag_next [offset 0], void *mag_round[] [offset 8] */
	/* Off-by-one fix: index with x4 (post-decrement), not x2
	 * (pre-decrement) -- indexing with x2 double-hands-out the top
	 * round and reads past the magazine on the very first pop from a
	 * full one (matches the fix in umem_rseq_x86_64.S). */
	lsl	x2, x4, #3	/* (rounds-1) * 8 */
	add	x3, x3, #8	/* &mag_round[0] */
	add	x6, x3, x2	/* &mag_round[rounds-1] (absolute) */
	ldr	x0, [x6]	/* Load mangled slot -> return value */
	/* P5.13b: demangle.  x0=stored, x7=cookie, x9=shift, x6=&slot. */
	SLOT_XOR x0, x7, x9, x6

	/* Increment alloc counter (advisory; safe to "waste" on abort --
	 * see ORDERING note below). */
	ldr	x5, [x19, #CACHE_ALLOC_COUNT_OFFSET]
	add	x5, x5, #1
	str	x5, [x19, #CACHE_ALLOC_COUNT_OFFSET]

	/*
	 * ORDERING (rseq ABI requirement, not just a style choice): the
	 * kernel aborts (jumps to abort_ip, discarding all register state
	 * including x0) if this thread is preempted/migrated/signaled at
	 * ANY point in [start_ip, post_commit_offset) -- i.e. anywhere from
	 * .Lrseq_alloc_start up to (but not including)
	 * .Lrseq_alloc_post_commit. Every store in this window except the
	 * unconditional LAST one is at risk of being "orphaned": if it
	 * retires but a LATER instruction in the same window is then
	 * interrupted, the abort handler still runs and returns NULL,
	 * discarding the return value -- even though an earlier store
	 * inside the window already changed memory. Previously the rounds
	 * store came BEFORE the mag_round[] load; a signal landing between
	 * them committed "one fewer round" to memory while the popped
	 * pointer was never delivered to any caller -- a real, reproducible
	 * LEAK (see test/stress/repro_rseq_trailing_store.c, confirmed on
	 * this hardware via a SIGALRM storm, no migration needed --
	 * ordinary preemption is enough). Fix: the rounds store below is
	 * now the unconditional LAST store before .Lrseq_alloc_post_commit.
	 * Abort before it retires => memory still shows the OLD rounds
	 * count => the object was never actually removed => NULL is a
	 * consistent answer (no leak, safely retryable). Preemption at/
	 * after this store retires is OUTSIDE the checked window (ordinary
	 * preemption, no abort) => execution resumes normally and returns
	 * the already-computed x0.
	 */
	str	w4, [x19, #CACHE_ROUNDS_OFFSET]	/* COMMIT: last store */

	/* COMMIT POINT */
.Lrseq_alloc_post_commit:
	/* Clear rseq_cs pointer */
	LOAD_RSEQ_OFFSET x1
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
	/*
	 * rseq ABI: the 4-byte signature must sit IMMEDIATELY BEFORE
	 * abort_ip. The kernel reads *(abort_ip-4) on restart and SIGSEGVs
	 * if it does not match RSEQ_SIG. abort_ip points at the label, so
	 * the signature goes before it (matches umem_rseq_x86_64.S).
	 */
	.align	5
	.inst	RSEQ_SIG
.Lrseq_alloc_abort:
	/* Increment restart counter */
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	add	x1, x1, #1
	str	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]

	/* Clear rseq_cs pointer */
	LOAD_RSEQ_OFFSET x1
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

	/* Register critical section: x0 = &active_rseq_area (see
	 * LOAD_RSEQ_OFFSET above). */
	LOAD_RSEQ_OFFSET x0
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

	/* Check if magazine full: compare against the cache's ACTUAL
	 * magsize field, not a hardcoded constant. A previous version of
	 * this file hardcoded "cmp w5, #63", which is a heap-buffer-
	 * overflow landmine for any magtype with magsize < 63 (e.g. 1, 3,
	 * 7, 15, 31 -- see umem_magtype[] in umem.c): it would keep
	 * storing past the end of a magazine allocated for only
	 * (magsize+1) pointers. Matches x86_64's
	 * "cmpl CACHE_MAGSIZE_OFFSET(%rbx), %edx". */
	ldr	w6, [x19, #CACHE_MAGSIZE_OFFSET]
	cmp	w5, w6
	b.ge	.Lrseq_free_full

	/* Store object in magazine (safe to "waste" on abort: if rounds
	 * is not bumped, the alloc fast path never reads this slot -- it
	 * only reads indices < rounds). */
	lsl	x6, x5, #3
	add	x4, x4, #8
	add	x0, x4, x6	/* &mag_round[rounds] (absolute) */
	mov	x1, x20		/* buf */
	/* P5.13b: mangle before store.  x1=buf, x2=cookie, x7=shift, x0=&slot. */
	SLOT_XOR x1, x2, x7, x0
	str	x1, [x0]

	/* Increment free counter (advisory; safe to "waste" on abort --
	 * see the ORDERING note below). */
	ldr	x6, [x19, #32]
	add	x6, x6, #1
	str	x6, [x19, #32]

	/*
	 * ORDERING (rseq ABI requirement -- see the matching comment in
	 * umem_rseq_alloc_fastpath above): the rounds++ store below MUST be
	 * the unconditional LAST store before .Lrseq_free_post_commit.
	 * Previously the array write, THEN rounds++, THEN the stats counter
	 * were three separate stores with rounds++ in the middle -- a
	 * signal landing between rounds++ and the stats store left rounds
	 * already bumped (buf now logically live and poppable by a future
	 * alloc) while the caller was told -1 (failed, via the abort
	 * handler) and would free the SAME buffer again via the fallback
	 * path: a real double-presence/double-free hazard (confirmed via
	 * signal-storm repro, test/stress/repro_rseq_trailing_store.c, no
	 * migration needed).
	 */
	add	w5, w5, #1
	str	w5, [x19, #CACHE_ROUNDS_OFFSET]	/* COMMIT: last store */

	/* COMMIT POINT */
.Lrseq_free_post_commit:
	/* Clear rseq_cs pointer */
	LOAD_RSEQ_OFFSET x1
	add	x1, x3, x1
	str	xzr, [x1, #8]

	/* Success - return 0 */
	mov	x0, #0
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	.cfi_adjust_cfa_offset -96
	ret

	/* Magazine full: own epilogue (do NOT fall through to
	 * .Lrseq_free_post_commit's "mov x0, #0" -- that would clobber
	 * the -1 failure return with success, silently reporting the push
	 * as accepted while the buffer was never stored anywhere. Matches
	 * x86_64's .Lrseq_free_full, which likewise never falls through to
	 * the success path's xorq %rax,%rax). */
.Lrseq_free_full:
	LOAD_RSEQ_OFFSET x1
	add	x1, x3, x1
	str	xzr, [x1, #8]
	mov	x0, #-1
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	ret

	/* ABORT HANDLER */
	/* signature BEFORE label — kernel reads *(abort_ip-4) */
	.align	5
	.inst	RSEQ_SIG
.Lrseq_free_abort:
	/* Increment restart counter */
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	add	x1, x1, #1
	str	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]

	/* Clear rseq_cs pointer */
	LOAD_RSEQ_OFFSET x1
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

/*
 * int umem_rseq_reload_alloc_commit(umem_rseq_cache_t *cache, int cpu_id,
 *     umem_magazine_t *new_mag, int new_rounds, umem_magazine_t **old_mag_out)
 *
 * Phase 2 of the armed rseq reload (design:
 * docs/results/2026-09-09-rseq-reload-asm-design.md).  Publishes the swap
 * of a depot magazine into cache_rseq[cpu] inside its OWN rseq critical
 * section, atomic w.r.t. this thread's occupancy of `cpu`.  Returns w0=1
 * if committed (caller frees *old_mag_out), 0 if aborted (thread migrated;
 * caller retries; *old_mag_out untouched).
 *
 * AAPCS64: x0=cache w1=cpu_id x2=new_mag w3=new_rounds x4=old_mag_out.
 */
.globl umem_rseq_reload_alloc_commit
.type umem_rseq_reload_alloc_commit, %function
.align 4
umem_rseq_reload_alloc_commit:
	.cfi_startproc
	stp	x29, x30, [sp, #-128]!
	.cfi_adjust_cfa_offset 128
	.cfi_offset 29, -128
	.cfi_offset 30, -120
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	.cfi_offset 19, -112
	.cfi_offset 20, -104
	stp	x21, x22, [sp, #32]
	.cfi_offset 21, -96
	.cfi_offset 22, -88
	stp	x23, x24, [sp, #48]
	.cfi_offset 23, -80
	.cfi_offset 24, -72
	str	x25, [sp, #64]
	.cfi_offset 25, -64

	mov	x19, x0		/* cache */
	mov	w20, w1		/* cpu_id */
	mov	x21, x2		/* new_mag */
	mov	w22, w3		/* new_rounds */
	mov	x23, x4		/* old_mag_out */

	add	x24, sp, #80	/* &rseq_cs */

	stp	wzr, wzr, [x24]
	adrp	x0, .Lrseq_reload_alloc_start
	add	x0, x0, :lo12:.Lrseq_reload_alloc_start
	str	x0, [x24, #8]
	adrp	x1, .Lrseq_reload_alloc_post_commit
	add	x1, x1, :lo12:.Lrseq_reload_alloc_post_commit
	sub	x1, x1, x0
	str	x1, [x24, #16]
	adrp	x2, .Lrseq_reload_alloc_abort
	add	x2, x2, :lo12:.Lrseq_reload_alloc_abort
	str	x2, [x24, #24]

	mrs	x25, tpidr_el0
	LOAD_RSEQ_OFFSET x0
	add	x0, x25, x0
	str	x24, [x0, #8]

.Lrseq_reload_alloc_start:
	ldr	w1, [x0, #4]	/* current cpu_id */
	cmp	w1, w20
	b.ne	.Lrseq_reload_alloc_abort

	/* Read OLD loaded_mag BEFORE any write (design section 6). */
	ldr	x2, [x19, #CACHE_LOADED_MAG_OFFSET]
	str	x2, [x23]	/* *old_mag_out = OLD loaded_mag */

	/* Resolution A (section 4): rounds=0, loaded_mag=new, rounds=real. */
	str	wzr, [x19, #CACHE_ROUNDS_OFFSET]
	str	x21, [x19, #CACHE_LOADED_MAG_OFFSET]
	str	w22, [x19, #CACHE_ROUNDS_OFFSET]	/* COMMIT: last store */

	mov	w0, #1

.Lrseq_reload_alloc_post_commit:
	LOAD_RSEQ_OFFSET x1
	add	x1, x25, x1
	str	xzr, [x1, #8]
	ldr	x25, [sp, #64]
	ldp	x23, x24, [sp, #48]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #128
	.cfi_adjust_cfa_offset -128
	ret

	.align	5
	.inst	RSEQ_SIG
.Lrseq_reload_alloc_abort:
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	add	x1, x1, #1
	str	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	LOAD_RSEQ_OFFSET x1
	add	x1, x25, x1
	str	xzr, [x1, #8]
	mov	w0, #0
	ldr	x25, [sp, #64]
	ldp	x23, x24, [sp, #48]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #128
	ret
	.cfi_endproc
.size umem_rseq_reload_alloc_commit, .-umem_rseq_reload_alloc_commit

/*
 * int umem_rseq_reload_free_commit(umem_rseq_cache_t *cache, int cpu_id,
 *     umem_magazine_t *new_mag, umem_magazine_t **old_mag_out)
 *
 * Free-side phase 2: swap in a fresh EMPTY magazine (rounds always 0),
 * simpler than the alloc side (no section 4 subtlety).  Write loaded_mag
 * first, rounds=0 last.
 *
 * AAPCS64: x0=cache w1=cpu_id x2=new_mag x3=old_mag_out.
 */
.globl umem_rseq_reload_free_commit
.type umem_rseq_reload_free_commit, %function
.align 4
umem_rseq_reload_free_commit:
	.cfi_startproc
	stp	x29, x30, [sp, #-128]!
	.cfi_adjust_cfa_offset 128
	.cfi_offset 29, -128
	.cfi_offset 30, -120
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	.cfi_offset 19, -112
	.cfi_offset 20, -104
	stp	x21, x22, [sp, #32]
	.cfi_offset 21, -96
	.cfi_offset 22, -88
	str	x24, [sp, #48]
	.cfi_offset 24, -80
	str	x25, [sp, #56]
	.cfi_offset 25, -72

	mov	x19, x0		/* cache */
	mov	w20, w1		/* cpu_id */
	mov	x21, x2		/* new_mag (empty) */
	mov	x22, x3		/* old_mag_out */

	add	x24, sp, #80	/* &rseq_cs */

	stp	wzr, wzr, [x24]
	adrp	x0, .Lrseq_reload_free_start
	add	x0, x0, :lo12:.Lrseq_reload_free_start
	str	x0, [x24, #8]
	adrp	x1, .Lrseq_reload_free_post_commit
	add	x1, x1, :lo12:.Lrseq_reload_free_post_commit
	sub	x1, x1, x0
	str	x1, [x24, #16]
	adrp	x2, .Lrseq_reload_free_abort
	add	x2, x2, :lo12:.Lrseq_reload_free_abort
	str	x2, [x24, #24]

	mrs	x25, tpidr_el0
	LOAD_RSEQ_OFFSET x0
	add	x0, x25, x0
	str	x24, [x0, #8]

.Lrseq_reload_free_start:
	ldr	w1, [x0, #4]
	cmp	w1, w20
	b.ne	.Lrseq_reload_free_abort

	ldr	x2, [x19, #CACHE_LOADED_MAG_OFFSET]
	str	x2, [x22]	/* *old_mag_out = OLD loaded_mag */

	str	x21, [x19, #CACHE_LOADED_MAG_OFFSET]	/* new empty mag */
	str	wzr, [x19, #CACHE_ROUNDS_OFFSET]	/* COMMIT: last store */

	mov	w0, #1

.Lrseq_reload_free_post_commit:
	LOAD_RSEQ_OFFSET x1
	add	x1, x25, x1
	str	xzr, [x1, #8]
	ldr	x24, [sp, #48]
	ldr	x25, [sp, #56]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #128
	.cfi_adjust_cfa_offset -128
	ret

	.align	5
	.inst	RSEQ_SIG
.Lrseq_reload_free_abort:
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	add	x1, x1, #1
	str	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	LOAD_RSEQ_OFFSET x1
	add	x1, x25, x1
	str	xzr, [x1, #8]
	mov	w0, #0
	ldr	x24, [sp, #48]
	ldr	x25, [sp, #56]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #128
	ret
	.cfi_endproc
.size umem_rseq_reload_free_commit, .-umem_rseq_reload_free_commit

#endif /* __aarch64__ && __linux__ */

/* Mark stack as non-executable (outside #if to always emit) */
#if defined(__linux__)
.section .note.GNU-stack, "", %progbits
#endif

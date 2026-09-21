# rseq lock-free per-CPU reload: precise assembly design spec (2026-09-09)

**Status of this document:** a mechanical implementation spec for the
reload-commit assembly the 2026-08-06 analysis concluded was needed. Written
after (a) fixing six independent, pre-existing bugs in the fast path that had
to be corrected before this question could even be evaluated on trustworthy
ground (see `docs/results/2026-09-09-rseq-reload-analysis-v2.md` §2), and (b)
empirically reproducing the migration-race hazard on real x86_64 and aarch64
hardware (`test/stress/repro_naive_reload_race.c`, §5 of that document). Do
**not** implement this in the same session that reads it — hand-writing rseq
asm is exactly the kind of change that needs dedicated, unhurried review and
its own EC2 validation pass. This spec exists so that pass is mechanical
translation, not fresh research.

Read `docs/results/2026-09-09-rseq-reload-analysis-v2.md` first for *why* no
C-only or lock-based design works. This document is only the *what to build*.

## 1. What already exists, and what changes

`umem_rseq_alloc_fastpath` / `umem_rseq_free_fastpath` (in
`umem_rseq_x86_64.S` / `umem_rseq_aarch64.S`) are correct, single-critical-
section rseq implementations of "pop/push one round from/to the CPU's
currently loaded magazine." Their structure (rseq_cs descriptor built on the
stack, `LOAD_FS_OFFSET`/`LOAD_RSEQ_OFFSET` GOT-indirect access to the active
rseq area, abort-handler-with-signature-before-label) is the template the
reload-commit routines below must follow *exactly* — same descriptor-build
prologue, same signature/alignment discipline, same "compute return value
first, single last commit store" discipline that fixing bug 6 (the trailing-
store hazard) required.

**The new thing this spec adds**: two new asm functions,
`umem_rseq_reload_alloc_commit()` and `umem_rseq_reload_free_commit()`, each
containing its OWN rseq critical section (own `.Lrseq_reload_*_start` /
`.Lrseq_reload_*_abort` pair, own descriptor, own signature). These are
**not** replacements for the existing fast path functions — they are the
missing piece that lets `umem_rseq_alloc_slowpath()` /
`umem_rseq_free_slowpath()` (currently plain C, in `umem.c`, `#if 0`'d out /
uncalled) publish a magazine swap to `cache_rseq[cpu]` under the same
kernel-enforced atomicity-with-respect-to-CPU-occupancy guarantee the fast
path already has.

## 2. The core design decision: split "prepare" from "commit"

The depot pull (`umem_depot_alloc()`/`umem_depot_free()`) is already
internally lock-protected (per-CPU-stripe `umem_maglist_t.ml_lock`) and must
stay in C — a magazine pull/return cannot happen inside an rseq critical
section (it may block on a mutex, and rseq critical sections must be
straight-line, non-blocking, bounded-length). So the reload is necessarily
two phases:

1. **PREPARE (plain C, can block, can migrate freely)**: pull a full/empty
   magazine from the depot. This does **not** touch `cache_rseq[cpu]` at
   all, so it is unconditionally safe regardless of which CPU the thread
   ends up on by the time it returns. This is exactly what
   `umem_rseq_alloc_slowpath()`/`umem_rseq_free_slowpath()` already do in
   their first few lines — keep that part unchanged.

2. **COMMIT (new asm, rseq-protected, must run atomically w.r.t. the target
   CPU)**: given the freshly-pulled magazine (from phase 1) and the OLD
   magazine (to hand back to the depot afterward), atomically swap
   `cache_rseq[cpu].loaded_mag` and `.rounds` — but **only if this thread is
   still executing on the CPU whose slot it is about to write**, exactly the
   same check the fast path does. If the check fails (thread migrated
   between phase 1 finishing and phase 2 starting), **retry from phase 1
   against the NEW cpu** — do NOT retry phase 2 against the old cpu's slot,
   and do NOT skip phase 1 (the pulled magazine's *contents* are unaffected
   by which CPU eventually receives them, but which `cache_rseq[]` slot
   receives it MUST match the CPU the thread is on at commit time).

This is structurally identical to how a correct lock-free stack push
"prepares" a new node's contents fully before an atomic CAS publishes it —
the expensive/blocking work happens outside the atomic region; only the
final pointer-sized publish needs the hardware-atomic (here: rseq-atomic)
operation.

## 3. Why the commit must be a SINGLE rseq critical section covering BOTH stores

`cache_rseq[cpu]` has two fields the commit must update together:
`loaded_mag` (offset 0) and `rounds` (offset 16 x86_64/16 aarch64 — see
`umem_rseq.h`). The existing fast path treats "there is a valid magazine
with N rounds" as a single logical state, checked as
`rounds > 0 && loaded_mag != NULL` (alloc) — see
`umem_rseq_x86_64.S:92-98`. If the reload commit writes `loaded_mag` and
`rounds` as two separate stores (even both inside SOME rseq critical
section, but with, e.g., a `previous_mag` bookkeeping step for the OLD
magazine sandwiched between them), a fast path running concurrently *on a
different CPU* is irrelevant (different slot, no interaction) — but a fast
path re-entering *the same CPU* after this thread's own commit critical
section aborts partway through would see a torn state (new `loaded_mag`,
stale `rounds`, or vice versa). Concretely:

- The commit critical section must treat "publish `loaded_mag` and `rounds`
  together" as its single unconditional last action, following the exact
  discipline the trailing-store fix (bug 6, §2.6 of the v2 analysis)
  established: **every** register/memory computation happens first; the
  **only** stores inside the checked window are, in order:
  1. (optional, safe-to-waste) old-magazine bookkeeping into
     `previous_mag`/`prounds` if the design chooses to stash the outgoing
     magazine there instead of immediately returning it to the depot (see
     §6) — safe to waste because nothing else reads `previous_mag` except
     this same reload path;
  2. `loaded_mag = new_mag` (safe to waste: the fast path's emptiness check
     is `rounds > 0`, and `rounds` has not been updated yet, so a stale old
     `rounds` value paired with the new `loaded_mag` pointer means the fast
     path either sees the OLD `rounds` with the OLD `loaded_mag` — pre-abort
     memory state, consistent — or, if `loaded_mag` retired but `rounds`
     did not, the fast path could theoretically read `rounds` (old, small)
     against `loaded_mag` (new, full) → this is why `rounds` must be
     written strictly AFTER `loaded_mag`, so that any abort before the
     final `rounds` store leaves `rounds` unchanged relative to the
     PREVIOUS logical state, and `loaded_mag`'s new value is simply ignored
     because `rounds` (old value) still correctly reflects "how many valid
     rounds are actually in whatever magazine `loaded_mag` currently
     points to" — see the CRITICAL SUBTLETY in §4 below, this is the one
     place this spec's ordering differs from a naive "always write
     lowest-offset field first" rule);
  3. `rounds = new_rounds` — **the unconditional last store**, exactly
     mirroring `.Lrseq_alloc_start`'s `rounds` store in the already-fixed
     fast path.

## 4. CRITICAL SUBTLETY: `loaded_mag`-then-`rounds` order requires the OLD magazine to still be valid at the `loaded_mag` store

Read this section twice — it is the one place a naive implementation will
introduce a NEW bug symmetrical to the one fixed in commit `a7414d1`.

If the commit critical section aborts strictly between writing `loaded_mag`
(step 2) and writing `rounds` (step 3), the fast path (or a subsequent
re-entry of this same reload, after the abort restarts it) will read
`loaded_mag` = NEW magazine, `rounds` = OLD count. If the OLD count is, say,
3 (three rounds left in what is now logically the wrong magazine, since
`loaded_mag` already points at the NEW one), the fast path will pop 3
"rounds" from indices 2,1,0 of the **NEW** magazine — objects that were
never logically issued as "available" by this reload, because the reload's
intent was "0 old rounds, N new rounds" or "swap in a full magazine with
`magsize` rounds," not "3 old rounds against a new magazine." This is
memory-safe (the NEW magazine's slots 0..2 are valid pointers — whatever the
depot magazine contained) but **logically wrong**: it can hand out objects
whose slots the reload's caller (`umem_rseq_alloc_slowpath`) already
decremented `rounds` for in its own accounting, or (worse, for the free-side
commit) can make the fast path believe there is room to push into a magazine
that is a different magazine than the one the reload thinks is "empty."

**Two valid resolutions, pick ONE and document the choice in the
implementation** (this spec deliberately does not mandate which, because
the right choice depends on whether `previous_mag`/`prounds` are activated —
see §6):

- **Resolution A (recommended, simpler): make `rounds` the gate for
  `loaded_mag`, not the other way around.** Write `rounds = 0` FIRST (a
  transient "magazine temporarily appears empty" state — safe: the fast
  path's `rounds > 0` check will simply treat this CPU as needing another
  reload, which is always a safe fallback, never a correctness violation),
  THEN write `loaded_mag = new_mag`, THEN write the REAL `rounds =
  new_rounds` as the final unconditional store. Now there are two stores
  after the transient zero, but the abort-safety argument holds at each
  boundary: abort after step 1 (`rounds=0`) → fast path sees empty magazine
  under the OLD `loaded_mag`, safe (falls through to cc_lock, which is
  always correct). Abort after step 2 (`loaded_mag=new_mag`, `rounds` still
  0) → fast path sees the NEW magazine but `rounds=0`, so it treats it as
  empty and does not touch `mag_round[]` at all — safe, no logical-wrongness
  hazard, because `rounds=0` short-circuits before any indexing happens.
  Only the final store (step 3) exposes the real N rounds, and by
  construction `loaded_mag` already points at the correct magazine by
  then. This is the pattern to implement.

- **Resolution B (only if a design needs to avoid the transient-zero
  visible window for some other reason, NOT recommended without a specific
  justification): use a SEPARATE, unused "staging" field and a single
  8-byte-or-less atomic store that changes both logical values at once.**
  Not applicable here without growing the struct (ruled out — see §7) or
  packing `rounds`+a magazine-table-index into one word (a bigger redesign,
  out of scope for this spec).

**Implement Resolution A.**

## 5. x86_64 exact instruction sequence

New file section in `umem_rseq_x86_64.S` (append after the existing free
fast path, before `.section .note.GNU-stack`).

### 5.1 Signature

```
void umem_rseq_reload_alloc_commit(umem_rseq_cache_t *cache, int cpu_id,
    umem_magazine_t *new_mag, int new_rounds);
```

Register mapping (System V AMD64 ABI): `%rdi`=cache, `%esi`=cpu_id,
`%rdx`=new_mag, `%ecx`=new_rounds.

Return value: `int` — 1 if committed, 0 if aborted (caller must retry phase
1 against the CPU's current cpu_id; see §8 for the C-side retry loop).

### 5.2 Body

```asm
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

	movq	%rdi, %rbx		/* cache */
	movq	%rdx, %r12		/* new_mag */
	movl	%ecx, %r13d		/* new_rounds */
	/* %esi (cpu_id) is used directly below before it's clobbered */

	subq	$64, %rsp
	andq	$-32, %rsp

	/* Build rseq_cs descriptor (same layout as the fast path) */
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
	/* Verify CPU hasn't changed since the caller read cpu_id */
	LOAD_FS_OFFSET %rax
	movl	%fs:RSEQ_CPU_ID_OFFSET(%rax), %ecx
	cmpl	%esi, %ecx
	jne	.Lrseq_reload_alloc_abort

	/* Resolution A step 1: rounds = 0 (transient safe-empty state) */
	movl	$0, CACHE_ROUNDS_OFFSET(%rbx)
	/* step 2: loaded_mag = new_mag */
	movq	%r12, CACHE_LOADED_MAG_OFFSET(%rbx)
	/* step 3 (COMMIT: unconditional last store): rounds = new_rounds */
	movl	%r13d, CACHE_ROUNDS_OFFSET(%rbx)

	movl	$1, %eax		/* return value: committed */

.Lrseq_reload_alloc_post_commit:
	LOAD_FS_OFFSET %rcx
	movq	$0, %fs:RSEQ_CS_OFFSET(%rcx)
	leaq	-24(%rbp), %rsp
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	ret

	.align	32
	.long	RSEQ_SIG
.Lrseq_reload_alloc_abort:
	incq	CACHE_RESTART_COUNT_OFFSET(%rbx)
	LOAD_FS_OFFSET %rax
	movq	$0, %fs:RSEQ_CS_OFFSET(%rax)
	xorl	%eax, %eax		/* return value: aborted, caller retries */
	leaq	-24(%rbp), %rsp
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
	.cfi_endproc
.size umem_rseq_reload_alloc_commit, .-umem_rseq_reload_alloc_commit
```

**Do not return early from inside `.Lrseq_reload_alloc_start` for any
reason** (e.g. do not add an "already have enough rounds, skip" fast-exit
branch) — every path from `.Lrseq_reload_alloc_start` must either fall into
the three-step commit and reach `.Lrseq_reload_alloc_post_commit` with
`%eax` already set, or jump to `.Lrseq_reload_alloc_abort`. This mirrors why
the existing fast path's `.Lrseq_alloc_empty` jumps INTO
`.Lrseq_alloc_post_commit` rather than duplicating the epilogue (bug 4's
fix, conversely, is about NOT sharing an epilogue when the return values
differ — here they don't differ in structure, only in the value placed in
`%eax` before the shared jump, so sharing is fine and matches the existing
`.Lrseq_alloc_empty` pattern).

### 5.3 The free-side commit: `umem_rseq_reload_free_commit`

```
int umem_rseq_reload_free_commit(umem_rseq_cache_t *cache, int cpu_id,
    umem_magazine_t *new_mag);
```

Same shape, but the "reload" here is: the OLD `loaded_mag` was full (rounds
== magsize), phase 1 already pushed it to the depot and pulled a fresh empty
`new_mag`, and phase 2 needs to swap it in with `rounds = 0` (empty, ready to
receive). This is actually SIMPLER than the alloc side because the target
`rounds` value is always 0 — there is no transient-state subtlety from §4
(writing `rounds=0` then `loaded_mag=new_mag` then `rounds=0` again is
redundant but harmless; simplify to: write `loaded_mag` first, `rounds=0`
last, since the "logically wrong new_mag paired with stale rounds" hazard
from §4 cannot occur when the final value being written IS zero regardless
of the old value):

```asm
.Lrseq_reload_free_start:
	LOAD_FS_OFFSET %rax
	movl	%fs:RSEQ_CPU_ID_OFFSET(%rax), %ecx
	cmpl	%esi, %ecx
	jne	.Lrseq_reload_free_abort

	movq	%r12, CACHE_LOADED_MAG_OFFSET(%rbx)	/* new_mag (empty) */
	movl	$0, CACHE_ROUNDS_OFFSET(%rbx)		/* COMMIT: last store */

	movl	$1, %eax
```

(Full prologue/epilogue/descriptor-build identical in shape to §5.2, with
`.Lrseq_reload_free_*` labels; omitted here for brevity but must be written
out in full in the actual file — do not try to share labels between the
alloc and free commit functions, exactly as the existing fast path does not
share `.Lrseq_alloc_*`/`.Lrseq_free_*` labels.)

## 6. What happens to the OLD magazine (`previous_mag`/`prounds`)

`umem_rseq_cache_t` already has `previous_mag`/`prounds` fields (offsets 8
and 20 on aarch64; x86_64's asm currently never references them — see
`CACHE_PREVIOUS_MAG_OFFSET`/`CACHE_PROUNDS_OFFSET`, defined only in the
aarch64 file today). **This spec does NOT use them.** The commit functions
above receive the NEW magazine from phase 1 (already pulled from the
depot) and simply overwrite `loaded_mag`; the OLD magazine that was
previously in `loaded_mag` must be captured by the CALLER (in C, in
`umem_rseq_alloc_slowpath`/`umem_rseq_free_slowpath`) BEFORE calling the
commit function, by reading `rc->loaded_mag` — but this read is a plain C
read with no synchronization, and by the time the commit function runs, the
fast path could have already changed `loaded_mag` again (if this thread
migrated away and back, or if the commit itself just ran once already in a
retry loop).

**Resolution**: don't try to hand the OLD magazine back to the depot AT ALL
as part of this commit. Instead:

1. Phase 1 pulls `new_mag` from the depot (this is the ONLY depot
   interaction in the whole reload).
2. The commit function, INSIDE its own critical section, in addition to
   steps 1-3 in §3/§4, also captures the OLD `loaded_mag` value into a
   register and returns it to the CALLER as a second output (e.g. via an
   output pointer parameter, or by returning it in `%rdx` alongside the
   `%eax` success flag) — reading it as the LAST thing inside the critical
   section, atomically with the swap, so the caller gets the value that was
   truly in place at the instant of the swap, never a stale/torn read.
3. The caller (still in C, back in `umem_rseq_alloc_slowpath`), AFTER the
   commit function returns, frees the returned OLD magazine to the depot
   normally (`umem_depot_free()`) — this is now safe: nothing else can be
   holding a reference to that OLD magazine's pointer once the commit has
   published the NEW one in its place (any fast path running now sees only
   the NEW `loaded_mag`).

Updated x86_64 signature:

```
int umem_rseq_reload_alloc_commit(umem_rseq_cache_t *cache, int cpu_id,
    umem_magazine_t *new_mag, int new_rounds, umem_magazine_t **old_mag_out);
```

Add before the `movl $1, %eax` in §5.2's commit path:

```asm
	movq	CACHE_LOADED_MAG_OFFSET(%rbx), %rax	/* read OLD before overwrite */
	/* ... steps 1-3 from §3/§4 using %r12/%r13 as before ... */
	movq	%rax, (%r14)		/* *old_mag_out = OLD loaded_mag;
					 * %r14 must be saved/loaded from the
					 * 5th argument register per the ABI
					 * (stack, since x86_64 SysV only has
					 * 6 integer arg regs and this makes
					 * 5 -- %rdi,%rsi,%rdx,%rcx,%r8 are
					 * args 1-5; old_mag_out arrives in
					 * %r8, not %r14 -- update the
					 * register-save prologue accordingly:
					 * save %r8 into a callee-saved reg
					 * (e.g. %r14) alongside %rbx/%r12/
					 * %r13 at function entry, BEFORE the
					 * `subq $64,%rsp` stack realignment,
					 * the same way %rdi/%rdx/%ecx are
					 * captured into %rbx/%r12/%r13) */
```

**Read the OLD `loaded_mag` value BEFORE step 1 of §3/§4** (i.e. before
writing the transient `rounds=0`), not after — the C caller needs the value
that was live at critical-section entry, and nothing in steps 1-3
overwrites `loaded_mag` until step 2, so reading it as literally the first
instruction inside `.Lrseq_reload_alloc_start` (right after the cpu_id
check) is correct and simplest.

## 7. aarch64 exact instruction sequence

Mechanically the same transformation applied to `umem_rseq_aarch64.S`'s
existing patterns (`LOAD_RSEQ_OFFSET`, the `stp`/`adrp`/`:lo12:` descriptor
build, the post-fix ordering discipline). Signature via AAPCS64: `x0`=cache,
`w1`=cpu_id, `x2`=new_mag, `w3`=new_rounds, `x4`=old_mag_out (pointer).
Return `w0` = 1/0 committed flag (the OLD magazine pointer goes through the
`old_mag_out` pointer parameter, not a second return register, to keep the
ABI simple and symmetric with x86_64 above).

```asm
.globl umem_rseq_reload_alloc_commit
.type umem_rseq_reload_alloc_commit, %function
.align 4
umem_rseq_reload_alloc_commit:
	.cfi_startproc
	stp	x29, x30, [sp, #-96]!
	.cfi_adjust_cfa_offset 96
	.cfi_offset 29, -96
	.cfi_offset 30, -88
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	.cfi_offset 19, -80
	.cfi_offset 20, -72
	stp	x21, x22, [sp, #32]
	.cfi_offset 21, -64
	.cfi_offset 22, -56
	stp	x23, x24, [sp, #48]
	.cfi_offset 23, -48
	.cfi_offset 24, -40

	mov	x19, x0		/* cache */
	mov	w20, w1		/* cpu_id */
	mov	x21, x2		/* new_mag */
	mov	w22, w3		/* new_rounds */
	mov	x23, x4		/* old_mag_out */

	add	x24, sp, #64	/* &rseq_cs, now that x23 uses slot 48-55 */

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

	mrs	x25, tpidr_el0		/* NOTE: needs a 7th callee-saved reg;
					 * add x25 to the stp set above and
					 * grow the frame to #112 accordingly
					 * -- shown here as x25 for clarity,
					 * adjust the frame size/offsets to
					 * fit when writing the real file */

	LOAD_RSEQ_OFFSET x0
	add	x0, x25, x0
	str	x24, [x0, #8]

.Lrseq_reload_alloc_start:
	ldr	w1, [x0, #4]
	cmp	w1, w20
	b.ne	.Lrseq_reload_alloc_abort

	/* Read OLD loaded_mag BEFORE any writes, per §6 */
	ldr	x2, [x19, #CACHE_LOADED_MAG_OFFSET]
	str	x2, [x23]		/* *old_mag_out = OLD loaded_mag */

	/* Resolution A (§4): rounds=0, then loaded_mag=new, then rounds=real */
	str	wzr, [x19, #CACHE_ROUNDS_OFFSET]
	str	x21, [x19, #CACHE_LOADED_MAG_OFFSET]
	str	w22, [x19, #CACHE_ROUNDS_OFFSET]	/* COMMIT: last store */

	mov	w0, #1

.Lrseq_reload_alloc_post_commit:
	LOAD_RSEQ_OFFSET x1
	add	x1, x25, x1
	str	xzr, [x1, #8]
	ldp	x23, x24, [sp, #48]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	.cfi_adjust_cfa_offset -96
	ret

	.align	5
	.inst	0xd428bc00		/* RSEQ_SIG, aarch64 value (bug 5 fix) */
.Lrseq_reload_alloc_abort:
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	add	x1, x1, #1
	str	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
	LOAD_RSEQ_OFFSET x1
	add	x1, x25, x1
	str	xzr, [x1, #8]
	mov	w0, #0
	ldp	x23, x24, [sp, #48]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #96
	ret
	.cfi_endproc
.size umem_rseq_reload_alloc_commit, .-umem_rseq_reload_alloc_commit
```

**This listing is illustrative of the exact stores and ordering, not
byte-exact final asm** — the register/stack-frame bookkeeping (which
callee-saved register holds what, exact frame size) must be re-verified by
whoever implements this against the actual assembler (a mismatched
`.cfi_offset`/frame size is a silent unwinding bug, not a build failure).
The parts that MUST be preserved exactly as specified: (a) the three-step
write order in the critical section (§4 Resolution A), (b) reading
`old_mag_out`'s target BEFORE any of the three writes, (c) the signature
value `0xd428bc00` before the abort label (bug 5), (d) the single
unconditional last store discipline (bug 6) — i.e. nothing may write to
`cache_rseq[cpu]` after the final `rounds` store and before
`post_commit`.

`umem_rseq_reload_free_commit` follows the same simplified pattern as §5.3
(no §4 subtlety since new rounds is always 0), symmetric register mapping.

## 8. The C-side retry loop (what `umem_rseq_alloc_slowpath` becomes)

```c
static void *
umem_rseq_alloc_slowpath(umem_cache_t *cp, int cpu_id)
{
	umem_rseq_cache_t *rc;
	umem_magazine_t *fmp, *old_mag;
	void *buf;
	int cpu, committed;

	cpu = cpu_id;
	for (;;) {
		rc = &cp->cache_rseq[cpu];

		/* PHASE 1 (plain C, may block, may migrate freely --
		 * does not touch cache_rseq[cpu] at all). */
		fmp = umem_depot_alloc(cp, &cp->cache_full);
		if (fmp == NULL)
			return (NULL);

		/* PHASE 2 (new asm, rseq-protected). */
		committed = umem_rseq_reload_alloc_commit(rc, cpu, fmp,
		    rc->magsize - 1, &old_mag);
		if (committed) {
			if (old_mag != NULL)
				umem_depot_free(cp, &cp->cache_empty, old_mag);
			/* The commit already decremented to magsize-1 and
			 * left the popped round's value implicit -- the
			 * caller (umem_rseq_alloc_fastpath, called next by
			 * _umem_cache_alloc after this slowpath returns)
			 * will do the actual pop. OR: have the commit
			 * function pop the top round itself and return it
			 * as a THIRD output, avoiding a second rseq entry
			 * immediately after this one -- an optimization,
			 * not a correctness requirement; either is safe.
			 * Simplest correct version: commit with
			 * new_rounds = magsize (not magsize-1), return, and
			 * let the caller's normal fast-path call (which
			 * _umem_cache_alloc already does right after the
			 * slowpath in the existing control flow -- verify
			 * this against the current umem.c call site before
			 * wiring it in) perform the actual pop. */
			buf = umem_rseq_alloc_fastpath(rc, cpu);
			if (buf != NULL) {
				rc->alloc_count++; /* if not already counted */
				return (buf);
			}
			/* Vanishingly unlikely: committed, then immediately
			 * aborted again before the very next call even
			 * started (another migration right away). Bounded
			 * retry, not an infinite loop -- fall through. */
		}

		/* Aborted (migrated between reading cpu_id and the commit),
		 * or the immediate fast-path re-pop above also aborted.
		 * fmp was NOT consumed by a failed commit (the commit
		 * function only writes cache_rseq[cpu] AFTER confirming the
		 * CPU match; on abort it writes nothing) -- but fmp WAS
		 * already pulled from the depot. Two choices: (a) return
		 * it to the depot and retry phase 1 fresh against the new
		 * cpu, or (b) retry the commit directly against the new cpu
		 * with the SAME fmp (still valid, un-published). (b) is
		 * strictly cheaper -- do that, bounded to a small retry
		 * count (e.g. 3) before falling back to (a) to guarantee
		 * forward progress under a pathological migration storm. */
		cpu = umem_rseq_get_cpu();
		if (cpu < 0) {
			umem_depot_free(cp, &cp->cache_full, fmp);
			return (NULL);
		}
		/* loop retries the commit (not phase 1) against the new
		 * cpu using the same fmp -- restructure the loop body
		 * above to separate "pull fmp once" from "commit, retry
		 * commit-only up to N times" accordingly when implementing;
		 * this sketch conflates them for readability. */
	}
}
```

**This C sketch is illustrative, not final** — in particular the interplay
between this slowpath and the immediately-following fast-path call in
`_umem_cache_alloc()`'s existing control flow (`umem.c` ~2746-2762) must be
re-read and reconciled by whoever implements this: does `_umem_cache_alloc`
call the slowpath and then separately call the fast path again, or does the
slowpath need to return the popped buffer directly? Trace the ACTUAL current
call site before writing the real retry loop; do not assume this sketch's
control flow matches without checking.

The free-side slowpath (`umem_rseq_free_slowpath`) follows the same
prepare/commit/retry shape using `umem_rseq_reload_free_commit`.

## 9. Validation checklist for the implementer

1. Build both new asm functions; confirm `objdump -d` shows the exact store
   order from §4/§5/§7 (no compiler is involved, but a hand-typo in field
   offsets is the single most likely mistake — verify `CACHE_ROUNDS_OFFSET`
   / `CACHE_LOADED_MAG_OFFSET` against `umem_rseq.h`'s actual struct layout,
   not against memory of what they "should" be).
2. Run `test/stress/repro_naive_reload_race.c`'s STRUCTURE (two threads, one
   hammering the commit function, one hammering the real fast path against
   the same shared slot) but with the NEW commit function instead of the
   naive C reload — zero `double_issue`/`bad_pointer` over a multi-minute
   run on BOTH x86_64 and aarch64 hardware is the bar, matching the rigor
   already applied to the fast path fixes in this session.
3. Run `test/stress/repro_rseq_trailing_store.c`'s SIGALRM-storm technique
   against the new commit functions directly (adapt the harness) to confirm
   no trailing-store hazard was reintroduced.
4. Only then wire `umem_rseq_alloc_slowpath`/`umem_rseq_free_slowpath` into
   `_umem_cache_alloc`/`_umem_cache_free`'s live call path, and run the full
   `test/stress/stress_concurrency_oracle` gate (192 threads, 60s, all
   size-classes/patterns, default AND `--enable-asan`, both intel-hi and
   arm-hi) — 0 failures required, per the task's own gate.
5. Confirm `rseq_alloc`/`rseq_free`/`rseq_restart` counters
   (`umem_dump_contention()`'s `rseq_*` columns) go from 0 to nonzero under
   a real migration-forcing workload (e.g. `numactl --physcpubind` bouncing
   threads across a subset of CPUs, or simply the existing 192-thread
   oracle at `--pattern=multi` on a busy box, which naturally produces
   scheduler migrations) — this is the signal that the reload path is
   actually being exercised, not silently falling through to `cc_lock`
   every time.
6. Only after 1-5 pass: measure the actual perf delta against
   `docs/results/2026-09-08-allocator-shootout.md`'s sustained `prodcons`
   methodology and the 192-thread `multi` scaling point, per the original
   task's OUTCOME A checklist — this spec does not presume the result will
   be a net win; measure it.

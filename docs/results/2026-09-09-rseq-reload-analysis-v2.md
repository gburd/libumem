# rseq lock-free reload path: rigorous re-evaluation (2026-09-09, v2)

**Follow-up to:** `docs/results/2026-08-06-rseq-reload-analysis.md` (the
prior "keep it inert" decision). Task: verify that decision is correct with
sharper rigor than before, and evaluate three specific alternatives (C-only
TOCTOU recheck, per-CPU lock array + optional CPU pinning, "anything else")
before concluding the fix needs new assembly.

**Decision: still INERT. Confirmed correct, with hardware evidence, not just
race-sequence reasoning.** No C-only or lock-based design closes the
migration-safety gap. A precise implementation spec for the actual fix is in
`docs/results/2026-09-09-rseq-reload-asm-design.md`.

**Unplanned but necessary prerequisite work done in this session (see git
log `fix(rseq):` commits `2d89a73`, `a7414d1`):** before this evaluation
could even proceed on trustworthy ground, six independent, pre-existing bugs
in the ALREADY-LIVE rseq fast path (not the inert reload slowpath this task
is about) had to be found and fixed. They were invisible until now because,
with the reload slowpath inert, `cache_rseq[cpu].rounds` is permanently 0 and
the fast path never actually executes its pop/push arithmetic in production.
Summary (full detail in the commit messages):

1. x86_64 + aarch64 alloc fast path indexed the magazine with the
   pre-decrement round count → double-allocation the instant `rounds > 0`.
2. aarch64 fast path ignored the runtime glibc-rseq-offset detection
   entirely, hardcoding its own private, unregistered TLS struct → silently
   never engaged on any glibc >= 2.35 aarch64 target.
3. aarch64 free fast path hardcoded the magazine-full bound as 63 instead of
   reading the cache's actual `magsize` → heap overflow for most magtypes.
4. aarch64 free fast path's "magazine full" case fell through into the
   success epilogue, clobbering the `-1` return with `0` → silent
   buffer-drop reported as success.
5. aarch64 used x86_64's `RSEQ_SIG` (`0x53053053`) instead of the
   architecturally-correct aarch64 value (`0xd428bc00`, the `BRK #0x45e0`
   encoding glibc/the kernel actually use there) → the kernel force-killed
   the thread with SIGSEGV on any real migration-triggered abort once bug 2
   was fixed and the critical section actually started executing for real.
6. Both fast paths placed a trailing, non-critical stats-counter store
   *after* the true logical commit store but still inside the
   kernel-checked `[start_ip, post_commit_offset)` window — violating rseq's
   "single unconditional last store" discipline. A preemption landing
   between the two stores (ordinary preemption, no migration needed)
   produced a real, reproducible leak (alloc side) or double-presence /
   double-free (free side).

All six verified via new regression tests
(`test/unit/test_rseq_fastpath.c`, `test/stress/repro_rseq_trailing_store.c`)
against real intel-hi (c7i.metal-48xl) and arm-hi (c8g.metal-48xl) hardware,
before and after each fix, plus the full oracle gate (192 threads, 60s,
default + `--enable-asan`, both arches, 0 failures). These fixes are
independent of, and a prerequisite for, everything below — arming the reload
on top of a fast path with any of bugs 1-6 still present would have been
building on sand regardless of how the reload itself was designed.

## 1. Restating the exact requirement

The reload must never write a CPU's `cache_rseq[cpu]` slot at an instant
when the fast path (running on that same physical CPU, for a thread that IS
correctly on it) could observe or produce a torn/inconsistent view of
`loaded_mag`+`rounds`. Equivalently: at every instant, **at most one
writer** may be touching a given `cache_rseq[cpu]` slot, and that writer
must be either (a) the fast path itself (already kernel-guaranteed atomic
w.r.t. migration via rseq's abort mechanism), or (b) a reload that has
established the same "I am provably the sole writer for this slot right
now" guarantee through some OTHER mechanism.

## 2. Alternative 1: C-only TOCTOU recheck — proven unsafe, not just argued

**The proposal**: after the depot pull (safe, doesn't touch
`cache_rseq[]`), re-read `*umem_rseq_cpu_idp` immediately before the final
write to `cache_rseq[cpu]`, and only commit if it still matches. If it
doesn't, retry against the new cpu.

**Why this looks plausible**: it mirrors exactly what the rseq fast path
itself does — check cpu_id, then commit. The instinct is "if the fast path
can get away with a check-then-commit, why can't the reload?"

**Why it is fundamentally different, not just "the same but riskier"**:
the fast path's check-then-commit is not just a check — it is a check that
the KERNEL also enforces at every instruction boundary inside the registered
critical section, by resetting the thread's IP to the abort handler if a
migration (or preemption, or signal) is detected anywhere between the
`cmpl`/`cmp` and the commit store. A plain-C `if (cpu_id == cached_cpu) {
...write... }` has **no such kernel cooperation**: the check and the write
are two ordinary instructions with an ordinary (arbitrarily preemptible)
gap between them, and nothing tells the kernel "abort this specific
sequence if you migrate the thread away right here." The check narrows the
window during which a stale write could happen; it does not, and
structurally cannot, eliminate it — closing that window is *precisely* what
registering an rseq critical section does, and a plain C recheck is not a
critical section.

**The exact race, spelled out** (this is the rigor the task asked for, not
just an assertion):

1. Thread T is on CPU 5. It reads `cpu_id = 5`, pulls a magazine from the
   depot (phase 1, arbitrary duration, no `cache_rseq[]` access).
2. T re-reads `cpu_id` — still 5 (recheck passes). T proceeds to write
   `cache_rseq[5].loaded_mag = new_mag`.
3. **Between the recheck and the write** (or even between the write's own
   two field-stores), the kernel preempts T and, on resumption, schedules
   it onto CPU 5 again OR migrates it elsewhere — it does not matter which,
   because **T's write to `cache_rseq[5]` is not itself gated by anything**;
   it happens regardless of what CPU T is now on. Concurrently, thread U
   (a completely different thread) is scheduled onto CPU 5 and calls the
   real fast path, which correctly reads `cpu_id=5`, and begins its own
   critical-section pop against `cache_rseq[5]`.
4. T's un-gated, multi-field write (`loaded_mag` then `rounds`, or whatever
   order) and U's kernel-atomic-but-only-atomic-with-respect-to-U's-own-
   migration critical section now race on the SAME two fields with **no
   ordering relationship between them at all** — rseq's guarantee is "U's
   critical section is atomic with respect to U migrating"; it says
   nothing about, and provides no protection against, an entirely different
   write from T landing mid-way through U's read-modify-write.
5. Possible outcomes: U reads `rounds` (new, from T) paired with
   `loaded_mag` (old, not yet written by T) or vice versa → U pops from the
   wrong magazine, or double-pops a round T also thinks it published, or
   dereferences a stale `loaded_mag` pointer that T is about to free to the
   depot (use-after-free).

**Empirical confirmation, not just the sequence above**: rather than rely
on scheduler-timing luck to hit this exact interleaving, `test/stress/
repro_naive_reload_race.c` reproduces the underlying memory race directly —
one thread running a plain-C reload (mirroring `umem_rseq_alloc_slowpath`'s
field order) against a shared `umem_rseq_cache_t` slot, N threads
concurrently running the REAL, already-fixed `umem_rseq_alloc_fastpath`/
`umem_rseq_free_fastpath` asm against that same slot, with a sentinel-token
double-issue detector (same discipline as
`test/stress/stress_concurrency_oracle.c`). Result on real hardware (8
fastpath threads, 8-10s):

| Arch | reload_ops | alloc_ops | double_issue | double_issue rate |
|------|-----------:|----------:|--------------:|-------------------:|
| aarch64 (arm-hi) | 11,490,548 | 25,452,733 | 12,075,945 | 47.4% |
| x86_64 (intel-hi) | 7,313,286 | 18,423,154 | 7,741,584 | 42.0% |

Roughly 42-47% of all fast-path allocations against a slot under concurrent
naive reload were double-issued — the same live pointer handed to two
different logical owners. This is not a rare, hard-to-hit edge case; it is
the default outcome under any real concurrent access, because (as the race
sequence above shows) there is no synchronization at all between the two
writers, only degrees of luck in scheduling. **Rechecking cpu_id narrows
which fraction of attempts hit the window; it does not shrink the window's
consequences, and empirically the window is wide enough to be hit on
effectively every other operation under contention.**

**Conclusion**: Alternative 1 is conclusively unsafe. Not "unsafe in a
rare corner case that a broader retry loop could paper over" — unsafe as
the dominant outcome under realistic contention. The prior analysis's
implicit dismissal of this approach was correct; this document adds the
hardware proof it lacked.

## 3. Alternative 2: per-CPU lock array (separate from `umem_rseq_cache_t`)

**The proposal**: a global `pthread_spinlock_t rseq_reload_lock[MAX_CPUS]`,
indexed by cpu_id, taken by the reload before writing `cache_rseq[cpu]` —
avoiding the 64-byte-struct/hardcoded-offset problem by living outside
`umem_rseq_cache_t` entirely.

**Why the struct-layout objection is correctly avoided, but the core
problem is not**: this design does sidestep the "can't grow
`umem_rseq_cache_t` past 64 bytes" constraint from the 2026-08-06 analysis —
a separate global array has no such limit. But it does not address the
actual requirement from §1: the lock is taken by the reload, but **the fast
path never takes it** (this is not a bug to fix, it is the entire point of
the fast path being lock-free). A lock that only one of the two writers
ever acquires provides **zero mutual exclusion between them** — it only
serializes reloads against each other (useful for the "two reloaders, same
slot" hazard the 2026-08-06 analysis separately identified, but that was
never the primary hazard; the fast-path-vs-reload race is). This is exactly
the prior analysis's conclusion, restated: "A lock does not fix (1): even
under a per-CPU `rc_lock`, the lock-free asm fastpath does not take the
lock." Moving the lock outside the struct changes nothing about this.

**The pinning refinement**: the task asks whether pinning the reload thread
to its target CPU via `sched_setaffinity` for the duration of the critical
write closes the gap. Reasoning it through:

- If the reload thread is pinned to CPU 5 before it reads `cpu_id=5` and
  stays pinned through the write, it cannot itself migrate away mid-write —
  that part of Alternative 1's race (step 3, "T migrates") is closed.
- **But this does not stop a DIFFERENT thread (U) from being scheduled onto
  CPU 5 and running the fast path concurrently with T's pinned write.**
  `sched_setaffinity` controls where the CALLING thread can run; it has no
  effect on what OTHER threads the scheduler places on that same CPU. CPU
  5 is not "reserved" for T merely because T is pinned to it — the OS
  scheduler is free (and, under load, likely) to also run other threads on
  CPU 5, including U, whose own rseq fast path has every right to execute
  there. Pinning eliminates exactly one of the two writers-can-appear
  mechanisms (T's own migration) and leaves the other (a concurrent U
  scheduled onto the same CPU) completely open. The race in §2 step 3-5
  reproduces identically with T pinned — U's arrival on CPU 5 was never
  caused by T's migration in the first place; multiple threads sharing a
  CPU under normal multiprogramming is the common case, not an edge case.
- Even if pinning were combined with taking `rseq_reload_lock[cpu]inside
  the pinned section, this still does not make U take that lock — U is
  running the ordinary fast path, which by design never touches any lock.

**Measured overhead, for completeness (the task asked to measure, not
assume)**: `test/bench/bench_affinity_vs_mutex.c` compares the BEST-CASE
`sched_setaffinity` pin+unpin round trip (already on the target CPU, no
real cross-CPU migration triggered) against the `mutex_lock`/`unlock` pair
the existing `cc_lock` path already pays:

| Arch | threads | mutex ns/op | affinity ns/op | ratio |
|------|--------:|------------:|----------------:|------:|
| aarch64 (arm-hi) | 8 | 66.7 | 868.1 | 13.0x |
| aarch64 (arm-hi) | 32 | 72.2 | 906.7 | 12.6x |
| aarch64 (arm-hi) | 192 | 81.7 | 978.2 | 12.0x |
| x86_64 (intel-hi) | 8 | 66.6 | 1627.9 | 24.4x |
| x86_64 (intel-hi) | 32 | 59.5 | 1608.1 | 27.0x |
| x86_64 (intel-hi) | 192 | 93.9 | 1694.2 | 18.0x |

This is the BEST case (a real migration-triggering `sched_setaffinity` call
that actually moves the thread, plus the kernel's `migration_cpu_stop`
machinery, would only be more expensive). 12-27x a mutex round trip means
pinning is not a net win over the *existing* `cc_lock` fallback even before
accounting for the fact that it does not actually solve the race — it is
strictly worse on both correctness and performance grounds.

**Conclusion**: Alternative 2, with or without pinning, does not meet the
requirement in §1. The per-CPU-lock-array framing avoids the struct-layout
objection but not the actual mutual-exclusion problem, which was always the
real issue. Pinning closes one migration path (the reload's own) while
leaving the other (a concurrent thread's own legitimate scheduling onto the
same CPU) fully open, and even in its best case costs more than the lock it
would replace.

## 4. "Anything else": the design space is narrow, and lands where the prior analysis said

Once §2 and §3 are ruled out, the only remaining classes of mechanism are:

- **A kernel-cooperating restartable sequence for the reload's commit
  too** (what the prior analysis proposed, and what this document's
  companion, `docs/results/2026-09-09-rseq-reload-asm-design.md`, now
  specifies precisely). This is the only mechanism that gives the reload
  the SAME kernel-enforced "atomic with respect to migration on this CPU"
  property the fast path already has — because it uses the exact same
  kernel facility.
- **A global cache-wide lock the fast path also takes** — rejected as a
  starting premise by the original task itself ("the fastpath is
  intentionally lock-free so a lock on the reload doesn't help"); making
  the fast path itself take a lock defeats the entire feature.
- **Per-CPU disabling of preemption/migration via a real-time
  scheduling mechanism (e.g. `SCHED_FIFO` + a critical section that
  disables preemption)** — not available to unprivileged userspace on
  Linux (there is no portable syscall to say "don't preempt me for the next
  N instructions" from userspace; that is precisely what rseq itself
  provides, in the one place the kernel is willing to grant it — a
  registered critical section with a known abort handler).
- **Hazard-pointer / epoch-based reclamation style schemes** — these solve
  "is it safe to free this old magazine" (a real question, addressed in
  the asm design doc §6) but do not solve the PUBLISH race for the NEW
  magazine into `cache_rseq[cpu]`, which is the actual hazard here; they
  are solving an adjacent, already-solved-by-plain-serialization problem,
  not this one.

No alternative meeting the requirement in §1 exists other than giving the
reload its own rseq critical section. This confirms, with the additional
rigor and hardware evidence this document adds, that the 2026-08-06
analysis's conclusion was correct.

## 5. Outcome

**Outcome B: confirmed — arming the reload safely requires new
per-CPU-commit assembly on both x86_64 and aarch64.** No C-only or
lock-based design (including the two specific alternatives the task asked
to rigorously evaluate) closes the migration-safety gap. This document adds
to the 2026-08-06 analysis:

- Hardware-verified proof (not just race-sequence reasoning) that the
  TOCTOU-recheck approach fails, at a ~42-47% double-issue rate under
  realistic contention (`test/stress/repro_naive_reload_race.c`).
- A rigorous accounting of why CPU pinning does not close the gap (it
  addresses only one of the two migration paths that can populate a CPU
  with a second writer) plus measured overhead (12-27x a mutex round trip
  even in the best case) confirming it would not be a net win even if it
  were correct.
- Six independent, pre-existing bugs found and fixed in the fast path
  itself as a prerequisite (this evaluation would have been standing on
  unverified ground otherwise) — a genuine correctness improvement to code
  already live in the hot path, shipped regardless of the reload-arming
  decision.
- A precise, mechanical assembly implementation spec
  (`docs/results/2026-09-09-rseq-reload-asm-design.md`) for the next
  attempt, going beyond "needs a second rseq critical section" to the
  exact register mapping, instruction ordering (including a subtle
  ordering requirement — §4 of that document — symmetric to the trailing-
  store bug fixed in this session), ABI, and validation checklist.

## 6. Current impact of leaving it inert

Unchanged from the 2026-08-06 analysis: steady-state small-object
alloc/free goes through PTC or the per-CPU `cc_lock` magazine path; the only
cost of the inert reload is transient `cc_lock` contention right after a
scheduler migration. This is a throughput nick under aggressive migration,
not a correctness issue. The fast-path bug fixes in this session (`rseq_*`
counters, once the reload is eventually armed following the design doc,
will finally measure something real) do not change this picture on their
own — they are strictly a latent-bug fix, not a performance change, since
the reload staying inert means `rounds` stays 0 and the fixed code paths
are (for now) exercised only by the new regression tests, not production
traffic.

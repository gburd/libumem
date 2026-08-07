# rseq lock-free reload path: analysis and decision to keep it inert (2026-08-06)

**Follow-up:** "arm the currently-inert rseq per-CPU reload slowpath" (from
`docs/results/2026-07-23-scaling-diagnosis.md` "Fix target (D2)").

**Decision: keep it INERT for now.** Arming it correctly requires new
per-CPU-commit assembly on both x86_64 and aarch64; a plain-C reload is
unfixably racy against the lock-free asm fastpath. Since the rseq path is a
pure optimization (PTC already serves the steady state correctly and soundly),
shipping hand-written commit asm under the concurrency oracle for a
non-correctness win is the wrong risk trade right now. This documents the
analysis so the next attempt starts from the real constraint.

## What exists

- Lock-free asm fastpath (`umem_rseq_alloc_fastpath` / `_free_fastpath` in
  `umem_rseq_x86_64.S` / `umem_rseq_aarch64.S`), correct and built; its abort
  handler was fixed in v2.1.0. Wired into the alloc/free hot path in `umem.c`
  (~line 2733 / 2976): read `cpu = *umem_rseq_cpu_idp`, call the asm fastpath;
  on NULL (empty magazine or rseq abort) fall through to the `cc_lock` path.
- Uncalled reload slowpaths `umem_rseq_alloc_slowpath(cp, cpu_id)` /
  `umem_rseq_free_slowpath(cp, cpu_id, buf)` (`umem.c` ~2456/2483): pull a full
  magazine from the depot and write `rc->loaded_mag` / `rc->rounds` for
  `rc = &cp->cache_rseq[cpu_id]`. Never invoked, so `rseq_*` counters read 0
  and every op actually takes the per-CPU `cc_lock`.

## Why a plain-C reload cannot be armed safely

The asm fastpath commit is a restartable sequence: it does
`rounds-- ; buf = loaded_mag->mag_round[rounds]` inside a critical region the
kernel **aborts (resets IP) if the thread is preempted or migrated** before the
post-commit label. That is what makes it atomic *with respect to the CPU it
runs on* — and only that CPU, since a per-CPU `umem_rseq_cache_t` is only ever
touched by whoever is currently on that CPU.

`umem_rseq_alloc_slowpath(cp, cpu_id)` is plain C with no rseq protection and
no lock. Two failure modes:

1. **Migration mid-reload.** A thread reads `cpu`, enters the slowpath, and is
   migrated to a different physical CPU before/while it writes
   `cache_rseq[cpu].loaded_mag/rounds`. It is now mutating the per-CPU slot of
   a CPU it is no longer on, concurrently with a fastpath that legitimately
   runs on that CPU. The fastpath's `rounds--`/load can tear against the
   slowpath's `loaded_mag=`/`rounds=` stores → double-hand-out of a buffer or a
   read past the magazine. (This is the "arming it naively thrashes under
   migration" the diagnosis warned about.)
2. **Two reloaders, same slot.** Two threads that both observe the same `cpu`
   (one stale across a migration) both run the slowpath for that slot with no
   mutual exclusion → the depot magazine swap races (one leaks/​double-frees a
   magazine).

A lock does not fix (1): even under a per-CPU `rc_lock`, the lock-free asm
fastpath does **not** take the lock (that is the whole point), so a locked
reload on CPU A still races a lock-free fastpath on CPU B for the same slot
after a migration.

## What a correct arming actually requires

The reload must itself run as a restartable sequence **on the target CPU**, so
that (a) it aborts and retries if it migrates (guaranteeing it only ever writes
the slot of the CPU it is executing on), and (b) its publication of
`loaded_mag`+`rounds` is ordered against the fastpath's per-CPU commit the same
way the kernel orders rseq regions. Concretely: hand-written asm for the reload
commit on **both** `umem_rseq_x86_64.S` and `umem_rseq_aarch64.S` (a second
rseq critical section per arch, with its own abort handler + signature), plus
careful publication order (magazine contents visible before `loaded_mag` is
swapped and `rounds` set). The depot pull itself can stay in C (it is already
internally locked); only the final per-CPU publish needs the rseq commit.

Adding a lock field to `umem_rseq_cache_t` is not an option: the struct is
exactly one 64-byte cache line and the asm uses hardcoded field offsets
(`CACHE_LOADED_MAG_OFFSET 0`, `CACHE_ROUNDS_OFFSET`, ...); a `pthread_mutex_t`
would grow the line and shift those offsets.

## Gate for the next attempt

`test/stress/stress_concurrency_oracle` at `--threads=192 --duration=60
--size-class=mixed --pattern=all`, default AND `--enable-asan`, on BOTH x86_64
(intel-hi) and aarch64 (arm-hi), plus `scripts/ec2/oracle_matrix.sh 192 60`.
The armed path must show `rseq_*` counters non-zero under a
`numactl --physcpubind` migration workload while the oracle stays clean, with
no regression to single-thread or the winning `prodcons` case.

## Current impact of leaving it inert

Steady-state small-object alloc/free goes through PTC (thread-local, lock-free
in the common case) or the per-CPU `cc_lock` magazine path. The only cost is
transient `cc_lock` contention right after a scheduler migration (two threads
briefly share a slot until the next magazine reload) — the futex blip in the
D1 perf capture. It is a throughput nick under aggressive `numactl` migration,
not a correctness issue, and does not affect the default (unpinned) workload
materially. PTC + the v2.2.0 slab/depot fixes carry the contended case.

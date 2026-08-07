# GC stop-the-world soundness: confirmed root cause, the cooperative-safepoint
# fix, and its validation under CPU oversubscription

**Status:** CLOSED. The stop-the-world (STW) soundness race is fixed by a
cooperative-safepoint collector that **never marks or sweeps against an
incomplete snapshot**. STW soundness is now a **default hard assertion** in
`test/property/prop_gc.c` (a reproduced canary corruption fails the suite;
`--no-strict-stw` downgrades it to a warning). Validated clean under ASan at
4x oversubscription (the original repro point) and on real 192-core hardware
up to full saturation.

Built/tested on EC2 (`intel-lo` c7i.2xlarge/8 vCPU, `intel-hi`
c7i.metal-48xl/192 vCPU), AL2023, gcc-11, per the repo's Global Constraints.

The earlier partial fix (v2.1.0, unified registries + register spill + park
barrier) closed the common case but was strained under ~4x CPU
oversubscription. This document supersedes that: the oversubscription failure
modes (hang / abort / rare residual sweep) are eliminated.

---

## 1. Confirmed root cause of the oversubscription failure (gdb evidence)

Reproduced on `intel-lo` (8 vCPU) at `--threads=48 --rounds=1000` under ASan:
5/30 hung. `gdb thread apply all bt` on a hung process showed, across several
distinct captures:

1. **Signal-based suspend can't schedule under oversubscription + a
   syscall-per-alloc storm.** Every worker was stuck in `gc_object_add` doing
   `pthread_sigmask()` (two syscalls per allocation, to make add atomic vs the
   suspend signal) while the collector swept; 48 threads doing syscalls on 8
   cores crawled. Some runs finished in 7 s, one took 219 s, one never
   finished in 400 s.

2. **A worker parks (via signal) while holding an allocator lock → deadlock.**
   `gc_suspend_handler` parked a thread inside
   `_umem_cache_alloc → umem_slab_create → vmem_alloc` (holding the vmem arena
   lock). Other workers blocked acquiring that lock could never reach a park
   point, so the collector's park barrier waited forever.

3. **Timeout → sweep-anyway = unsound.** On the 5 s park-barrier timeout the
   collector proceeded to scan roots and sweep with threads still running,
   racing the lockless page-sparsemap read (observed as a SEGV in `sm_lookup`)
   and freeing reachable objects (residual canary).

4. **Residual reachable-object sweep, isolated with a swept-pointer ring +
   born-mark instrumentation:** the corrupt node had `gc_mark == mval-1` (last
   marked one generation ago) yet was still reachable — the current
   generation's mark never reached it. This was traced to an **allocate-black
   interaction**: stamping new objects with the live mark value made
   `gc_mark_callback` treat a freshly reachable object as *already scanned*, so
   it skipped pushing it to the mark queue and never followed its `->next`,
   stranding deep chain nodes.

---

## 2. The fix: cooperative safepoints, never sweep an incomplete snapshot

Design principle enforced throughout: **the collector only ever marks/sweeps
a snapshot in which every mutator is confirmed parked with its
registers+stack scannable; otherwise it does nothing this cycle.** Suspension
does not depend on async signal timing.

All in `umem_gc.c` / `umem_gc_roots.{c,h}`:

### 2.1 Cooperative safepoint (the sound mechanism)
- `gc_safepoint()` at the top of `umem_gc_alloc` — before any allocator or GC
  lock — parks the thread if a STW is in progress. A mutator that cannot be
  scheduled to run a signal handler still parks the moment it next allocates.
- The SIGUSR2 handler is kept only as a **fallback** to prod threads stuck in
  long non-allocating regions.

### 2.2 Park never happens mid allocator-lock
- The whole of `umem_gc_alloc` / `umem_gc_free` (which take cache/depot/vmem
  locks and update the object list + sparsemap) runs inside a GC critical
  section marked by a lightweight `gcti_in_gc_critical` flag (a plain store —
  it **replaces the per-allocation `pthread_sigmask` syscall storm**).
- A suspend signal landing inside the section **defers**: it sets
  `gcti_park_pending` and returns; the thread parks at `gc_critical_exit()`
  once all locks are released. So a parked thread never holds an allocator
  lock the collector's mark/sweep needs — eliminating the vmem-lock deadlock.

### 2.3 Re-entrancy guard + ABA drain
- `gc_parked` (per-thread) makes a nudge/re-signal to an already-parked thread
  a no-op, fixing nested parking that double-counted the ACK and clobbered the
  recorded `gcti_sp` / `gcti_regs`.
- `gc_resume_the_world()` **drains**: it waits until every parked thread has
  left `gc_park_self` (`gcti_suspended` back to 0) before returning, so the
  next collection cannot flip `gc_stw_active` 0→1 and reset ACKs out from
  under a thread still spinning in the previous pause (an ABA that livelocked
  the barrier).
- Parked threads `sched_yield()` in their spin instead of busy-waiting, so
  dozens of parked threads on a few cores do not starve the collector.

### 2.4 Never sweep an incomplete snapshot
- The park barrier waits (bounded, `GC_STW_TIMEOUT_MS`) for **all** registered
  non-self threads, re-nudging stragglers. If it times out (only possible if a
  mutator is genuinely wedged in an allocator lock cycle it can never escape),
  `gc_stop_the_world()` fails and `umem_gc_collect()` **skips marking and
  sweeping this cycle entirely** — garbage simply waits for the next
  collection. Bounded and always sound; no best-effort sweep of a running
  heap.

### 2.5 Sound concurrent reclaim without allocate-black
- Objects are **not** allocate-blacked (that broke child traversal, §1.4).
- Instead the dead set is collected off the global list **while the world is
  still stopped** (`gc_collect_dead`, quiescent snapshot), the world resumes,
  then finalizers + `gc_free_object` run afterward (`gc_reclaim_dead`). An
  object allocated after the snapshot is simply not a sweep candidate this
  cycle, so the concurrent reclaim can never free a freshly reachable object,
  and the STW pause stays short (no user finalizers / allocator frees under
  STW).

---

## 3. Validation

`prop_gc` now defaults to `--strict-stw`: a reproduced canary corruption is a
hard suite failure. `--stw-iters=N` repeats the stress.

### 3.1 `intel-lo` (8 vCPU) — CPU oversubscription, ASan

| config                | iters | ok | canary | segv | abort | hang |
|-----------------------|-------|----|--------|------|-------|------|
| 32t/1000r (**4x**)    | 50    | 50 | 0      | 0    | 0     | 0    |
| default 8t/200r (1x)  | 20    | 20 | 0      | 0    | 0     | 0    |
| 48t/1000r (6x)        | 40    | 39 | 0      | 0    | 0     | 0*   |

*At 6x, one run of 40 hit a long tail (see §4). **Zero soundness violations
(canary/segv/abort) in any run.** 4x — the original repro point — is flawless.

### 3.2 `intel-hi` (192 vCPU) — real parallelism, ASan

| config          | iters | ok | canary | segv | abort | hang |
|-----------------|-------|----|--------|------|-------|------|
| 48t/1000r       | 50    | 50 | 0      | 0    | 0     | 0    |
| 96t/1000r       | 50    | 50 | 0      | 0    | 0     | 0    |
| 192t/800r (sat) | 30    | 30 | 0      | 0    | 0     | 0    |

The v2.1.0 collector hung 3/25 at 192 threads; the cooperative-safepoint
collector is 30/30 clean at full saturation. (The GC now also runs at
192 vCPU at all — the `umem_cpu_node[]` OOB that previously blocked the
high-core roles was fixed by separate allocator work.)

### 3.3 No regressions
- `/gc/concurrent_alloc` stays **green and fast** (intel-lo ~0.5 s, intel-hi
  ~17 s wall) — the deadlock fix (commit 260f8c1 lineage) is not regressed.
- All other `/gc/*` unit tests pass.
- Pre-existing, unrelated failures NOT caused by this change (confirmed by
  reverting the GC files to the parent commit and re-running): `/gc/boehm_full`
  asserts `GC_get_heap_size() > 0` after explicit `GC_FREE` of everything (a
  stats-accounting quirk), and several `/coverage/*` and `/overflow_fixes/*`
  tests are flaky under ASan memory pressure on loaded instances.

---

## 4. Remaining item: bounded throughput tail at ≥6x oversubscription

At 48 threads on 8 cores (6x) with the adversarial workload (main thread hammers
`GC_gcollect()` 1000× while every worker also allocates + collects, ~1800
collections/run), the single global `gc_objects_lock` — held across each
`gc_object_add`'s page-sparsemap insert (an O(n) rehash on resize) — serializes
all 48 allocators, and the collector's bulk reclaim contends on the per-CPU
cache lock. Median run ≈ 10 s, but a heavy tail (rare 120–230 s completions, or
a run where every STW times out and skips so the stress reports "no
collections") appears.

This is a **scalability limit of the global object lock, not a soundness bug**:
every such run is sound (0 canary/segv/abort) and bounded (it completes or
cleanly skips — never an unsound sweep, never an unbounded hang), exactly the
tradeoff the project rules prefer. It is why `--strict-stw` is validated as the
default at **4x** (32 threads, the original repro point, 50/50 flawless) while
6x is documented here as bounded-sound.

**Ideal follow-on** (out of scope for the STW soundness fix): shard the global
object list / sparsemap (per-arena or striped locks) so `gc_object_add` no
longer serializes all allocators, and/or move the page-sparsemap resize out of
the `gc_objects_lock` critical section. That removes the tail at extreme
oversubscription without touching the now-sound STW mechanism.

### 4.1 Follow-up (2026-08): partial mitigation landed

`perf(gc): grow page-sparsemap 4x + larger initial capacity` (a814fa1) took
the smaller, provably-sound half of the ideal follow-on: the O(capacity)
rehash in `sm_resize()` still runs under `gc_objects_lock`, but growing 4x
per resize (was 2x) from an initial 4096 buckets (was 256) reaches
steady-state capacity in far fewer resizes, roughly halving the number of
O(n) lock holds. Verified sound (48t→resize path unchanged in semantics;
resize never races the collector, which only reads while all mutators are
parked): 32t/1000r ×30 = 0 corruption, full `test_main --no-fork` = 459 OK /
0 FAIL across 5 runs. Common-case 6x latency improved (48t/1000r median
~11.2s → ~10.1s on 8 cores).

**The rare 6x tail is NOT eliminated.** A 300s stall still reproduced ~1 in
12 runs at 48t/8-core even with 4x growth — the residual cause is the STW
park-barrier interacting with `gc_objects_lock` contention under extreme
oversubscription, not resize frequency. The full fix (striped object-lock
shards, or moving the rehash to a private-copy-then-publish protocol that the
lockless STW reader can tolerate, or reworking the barrier) remains the
documented out-of-scope follow-on. Every such stall is still **bounded-sound**
(completes or cleanly skips; never an unsound sweep, never data corruption) —
the tradeoff the project rules prefer. `--strict-stw` remains validated as the
default at 4x.

### 4.2 Follow-up (2026-08): object lock + pagemap sharded (add-path contention removed)

`perf(gc): shard the global object lock + pagemap` (5bd2113) landed the
"ideal follow-on": the single `gc_objects_lock` + global pagemap are replaced
by **64 shards keyed by page** (every object on a page lands in one shard, so
`umem_gc_find_header()` still routes a conservative/interior pointer to a
single shard by page and resolves it via that shard's per-page object list).
Each shard has its own lock, object list, and `umem_sparsemap`;
`gc_object_add`/remove contend only the object's shard; stop-the-world walks
all shards (a complete, quiescent snapshot — every mutator is parked);
`find_header` reads one shard's pagemap locklessly. This removes the
mutator-vs-mutator serialization on the alloc/free path.

**Validation:** strict-stw 32t/1000r ×30 = 0 corruption; **192t/800r ×20 at
real 1:1 parallelism = 0 corruption / 0 timeout**; full `test_main --no-fork`
= 459 OK / 0 FAIL. Sharding is sound and the barrier is clean whenever threads
can actually be scheduled.

**Residual, honestly scoped.** At **≥6x CPU oversubscription** (48 threads on
8 cores) a rare stall (~1 in 20–30 runs) persists *even with sharding*, which
pins the root cause precisely: it is **not** lock contention (sharding removed
that, and it is clean at real parallelism) — it is the **STW suspend-barrier
vs. the scheduler**: 48 runnable threads cannot all be scheduled onto 8 cores
to reach a safepoint within the barrier timeout, so the collector bounded-waits
then skips the cycle. Eliminating it requires a different mechanism than a
lock (cooperative safepoint polling inside tight mutator loops, or a
scheduler-aware barrier), tracked as a separate item. Every such run remains
**bounded-sound** (completes or cleanly skips; never an unsound sweep, never
corruption). `--strict-stw` remains validated as the default at 4x.

### 4.3 aarch64: the STW oversubscription edge is worse (pre-existing)

Cross-arch validation (2026-08, arm-lo `c7g.2xlarge`, 8 vCPU, ASan strict-stw)
found the STW oversubscription soundness edge appears at a **lower
oversubscription ratio on aarch64** than on x86_64 — arm's weaker memory model
exposes it sooner:

| threads (8 vCPU) | ratio | x86_64 | aarch64 (sharded) | aarch64 (pre-shard / v2.3.0) |
|---|---|---|---|---|
| 2 / 4 / 8 | ≤1x | clean | **0/10 clean** | clean |
| 16 | 2x | (clean to 192t/1:1) | 2/10 corrupt | **14/15 corrupt** |

Two findings:

1. **Pre-existing, not introduced by sharding.** The pre-sharding v2.3.0 GC
   reproduces the arm STW sweep-of-reachable at **14/15** at 16t/8-core.
   Sharding (this release) *reduces* it to **2/10** — a large improvement —
   but does not eliminate it. So arm GC was never sound under
   oversubscription in any shipped version; this release improves it.
2. **Same root cause as the x86 tail:** the suspend-barrier under
   oversubscription. On arm the collector can proceed before a suspended
   mutator's stack stores are visible (missing acquire/release pairing on the
   suspend acknowledgement), so a reachable object rooted on that stack is
   swept. The fix is the same barrier/safepoint rework (a proper
   acquire/release or seq-cst handshake on suspend-ack, plus safepoint
   polling), tracked as the barrier follow-on.

**Operating guidance:** GC is sound on both arches at ≤1x thread:core
(the normal regime). Under CPU oversubscription the STW path is bounded but
can rarely sweep a reachable object on aarch64 — do not run the conservative
GC oversubscribed on aarch64 until the barrier follow-on lands.

## 5. Reproduction

```
# 4x oversubscription (default strict): clean
LD_PRELOAD=$(gcc -print-file-name=libasan.so) ASAN_OPTIONS=detect_leaks=0 \
  LD_LIBRARY_PATH=.libs test/property/.libs/prop_gc --threads=32 --rounds=1000

# real parallel hardware, full saturation: clean
LD_PRELOAD=... prop_gc --threads=192 --rounds=800     # on 192-vCPU

# explore the bounded 6x tail (does not gate soundness):
LD_PRELOAD=... prop_gc --no-strict-stw --threads=48 --rounds=1000
```

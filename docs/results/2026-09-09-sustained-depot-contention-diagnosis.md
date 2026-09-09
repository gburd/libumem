# libumem sustained-load depot-lock-convoy diagnosis and fix — 2026-09-09

**Follow-up to:** `docs/results/2026-09-08-allocator-shootout.md` §7
(`prodcons-sustained`, umem's worst/tied-worst-in-field p999 tail latency)
and `docs/results/2026-07-23-scaling-diagnosis.md` (the earlier isolated
umem-only diagnosis that first identified depot/`cc_lock` contention as the
multi-thread bottleneck).

**Status: FIXED.** Confirmed mechanism with counters + perf, root-caused a
flawed first attempt with an A/B before trusting it, shipped the corrected
fix, and re-verified on a dedicated instance. See §5 for before/after
numbers.

## 1. Reproduction

`intel-hi` (`c7i.metal-48xl`, 192 vCPU, x86_64, performance governor).
Reproduced the shootout's own `scripts/ec2/sustained_load.sh umem 180 192`
methodology directly: prodcons-sustained at 192 threads for ~150-230s
(calibrated). Baseline p999 measured **156,736 ns / 163,190 ns** across two
runs — matches the report's documented 157,137 ns almost exactly. Confirmed:
this is not a burst artifact, it reproduces on demand.

## 2. Confirmed mechanism (counters + perf, not just the hypothesis)

`umem`'s depot/magazine design already has a working non-blocking refill
path for the PTC layer (`umem_depot_alloc_trylock`, bounded to
`umem_depot_steal_max` = 8 stripes, used when the PTC fast path misses).
That path was never the problem — its own counters and perf samples
(`perf-pre-fix/perf-depot-trylock-graph.txt`: 0.87% total, all in
`pthread_mutex_trylock`, no blocking) show it behaving exactly as designed.

The actual long pole is one level down, in the **magazine layer's** depot
refill: `umem_depot_alloc()`, called from `_umem_cache_alloc()` /
`_umem_cache_free()` / the `_batch` variants **while the caller holds its
own per-CPU `cc_lock`**. Before this fix, on a miss it scanned same-NUMA-node
then remote-NUMA-node per-CPU depot stripes **unconditionally out to
`ncpus-1`** (`ncpus` = `umem_max_ncpus`, rounded to a power of 2 — 256 on
this 192-vCPU box, so up to ~510 stripe visits worst case), and each stripe
visit was `umem_depot_pop()`: `mutex_trylock`, and **on failure, a blocking
`mutex_lock`** (`umem.c` `umem_depot_pop()`, `cache_depot_contention` counter
increments right there).

**Counter evidence** (`umem_dump_contention()`, `bench_contention -w
prodcons -t 192`, same workload as the shootout's sustained point):

| cache | cc_alloc | dep_local | dep_remote | dep_conten (baseline) |
|---|---|---|---|---|
| umem_alloc_96  | 9,518,758  | 1,014 | 122,521 | **89,167** |
| umem_alloc_160 | 19,028,902 | 1,351 | 246,193 | **152,175** |
| umem_alloc_192 | 19,037,660 | 1,346 | 246,265 | **142,560** |
| umem_alloc_256 | 19,002,516 | 1,370 | 246,143 | **152,217** |

`dep_local` (the freeing thread's own CPU stripe having what the allocating
thread needs) is negligible — a few thousand hits against hundreds of
thousands of `cc_alloc` operations. `dep_remote` (successful steals from
another CPU's stripe) is the dominant supply path, exactly as expected: in
`prodcons`, producers allocate and consumers free, on largely disjoint CPU
sets, so a buffer only ever becomes available on the *freeing* thread's own
stripe (`umem_depot_free()` pushes to `get_cached_cpu_hint()`'s CPU only) —
a producer's own stripe is essentially always empty. `dep_conten` — the
blocking-mutex-fallback count for exactly this scan — runs **60-100% as high
as the successful-steal count itself**. That is hundreds of thousands of
"thread blocks on a futex while holding its own `cc_lock`" events per cache,
over a single sustained run.

**Why a short burst doesn't show it, but sustained duration does:** each
individual blocking `mutex_lock()` call is cheap in isolation (the stripe is
usually only held for a handful of instructions). The tail-latency cost is
a **lock convoy**: under 192-way concurrency, enough concurrent misses are
in flight simultaneously that a thread blocking on stripe X's lock is itself
now the reason a *different* thread later blocks on `cc_lock` waiting for
the first thread to finish its scan and release `cc_lock`. This queuing
effect only builds up once there is sustained concurrent pressure long
enough for the queue to grow — a short burst's total instruction count
isn't enough for it to compound. This matches the report's own observation
that umem's burst-`prodcons` result (best falloff in the field) directly
contradicts its sustained-`prodcons` result (worst p999 in the field): two
different regimes of the *same* mechanism.

**perf caveat (important, and honestly reported):** a `perf record -F 997
--call-graph dwarf` cycles-based capture over the full run does **not**
strongly show this mechanism (`umem_depot_pop`/`mutex_lock` self time is
<0.1% of samples) — because a thread blocked on a futex is **off-CPU** and
is not sampled by a cycles PMU counter. The scheduler-side symptoms are
visible (`__schedule` 1.26%, `raw_spin_rq_lock_nested` 0.86%, `pick_next_task`
family ~0.7% combined — consistent with threads parking/waking around
mutex contention) but are easy to misattribute to the benchmark harness's
own `sched_yield()` spin-wait (2.5%+, `ring_pop`'s busy-wait when the ring
is empty, which is a harness artifact, not an allocator cost). **The
`umem_dump_contention()` counters, not the perf profile, are the load-bearing
evidence here** — a cycles profile is the wrong tool for diagnosing blocking
contention; a lock-contention/off-CPU profile (`perf record -e
sched:sched_switch` or similar) would show it directly, but the counters
already made the mechanism unambiguous without needing that.

## 3. First fix attempt: bounding scan breadth — WRONG, caused a regression

The obvious-looking fix: apply the same `UMEM_DEPOT_STEAL_MAX` (8-stripe)
bound the PTC trylock path already uses to the blocking scan too. **This
was implemented, measured, and reverted after the A/B showed it made things
worse, not better:**

| | baseline | 8-stripe-bounded scan |
|---|---|---|
| p999 (ns) | 156,736 - 169,231 | **348,377 - 423,572** |
| peak RSS (bytes) | ~176-185M | **3.16G - 5.38G** |

**Why it failed:** bounding breadth, not blocking behavior, broke the
search. `umem_depot_free()` pushes a freed magazine only onto the *freeing*
thread's own stripe. Under `prodcons`, a producer's 8 nearest stripes (by
the `(cpu+i) & (ncpus-1)` scan order) are statistically dominated by *other
producer* CPUs, not consumer CPUs — the magazine a producer needs can
legitimately be sitting on any of the other ~255 stripes. Capping the scan
at 8 stripes meant producers usually failed to find it and fell through to
allocating brand-new magazine shells from the slab layer instead — which is
exactly why RSS exploded (continuously growing magazine count) and latency
got worse (slab allocation is slower than a depot hit, and now happening
constantly instead of rarely). **Breadth is load-bearing for this workload;
it is not a knob that can be shrunk without breaking the cross-thread
handoff the depot exists for.**

## 4. Actual fix: keep full scan breadth, make each stripe visit non-blocking

The defect was never "the scan visits too many stripes" — it was "each
stripe visit can **block**." `umem_depot_alloc()`'s steal loops (steps 2/3)
now call `umem_depot_pop_trylock()` (already existed, used by the PTC
refill path) instead of the blocking `umem_depot_pop()`, over the **same
full stripe range** as before (no breadth reduction). A trylock miss on a
momentarily-busy stripe simply moves to the next candidate in the same
pass; no thread ever blocks on another CPU's depot lock while holding its
own `cc_lock`. Step 1 (the caller's own local stripe) and step 4 (the
global depot fallback) are unchanged — still `umem_depot_pop()` — since
step 1 is never contended by another thread's *steal* (only by that same
CPU's own future ops) and step 4 is the existing, already-accepted final
blocking fallback if nothing was found anywhere. `umem.c`, `umem_depot_alloc()`.

**Post-fix counter evidence** (same `bench_contention -w prodcons -t 192`
workload, shorter run):

| cache | cc_alloc | dep_local | dep_remote | dep_conten (fixed) |
|---|---|---|---|---|
| umem_alloc_96  | 1,968,751 | 604 | 25,253 | **667** |
| umem_alloc_160 | 3,896,648 | 657 | 51,300 | **2,419** |
| umem_alloc_192 | 3,914,135 | 650 | 51,268 | **1,867** |
| umem_alloc_256 | 3,912,981 | 674 | 51,284 | **1,246** |

`dep_remote` (successful steals) is proportionally as strong as before —
breadth is intact. `dep_conten` (blocking fallback) as a fraction of
successful steals (`dep_conten / dep_remote`) drops from **0.62** at
baseline (`umem_alloc_256`: 152,217/246,143) to **0.024** post-fix
(1,246/51,284) — a **~25x** drop in the trylock-miss-forces-a-block rate,
consistent with the ~60-100x drop in raw `dep_conten` counts seen over the
longer baseline run (sustained duration compounds the effect further).

## 5. Before/after sustained p999 (the numbers that matter)

Measured on a **dedicated, single-tenant** `c7i.metal-48xl` instance
(launched solely for this verification, tag
`Role=intel-hi-verify-depotfix`, terminated at the end of this session —
the shared `intel-hi` role instance had concurrent unrelated agent activity
during this session and was avoided for these specific numbers to keep them
clean). Full raw data: `sustained-ab.toml` in this directory.

| | baseline run 1 | fixed run 1 | baseline run 2 | fixed run 2 |
|---|---|---|---|---|
| p999 (ns) | 156,736 | **92,621** | 163,190 | **83,830** |
| p99 (ns) | 20,820 | 20,613 | 20,347 | 18,534 |
| ops/sec | 1,983,813 | 1,970,343 | 1,932,244 | 1,947,226 |
| peak RSS (bytes) | 181,440,512 | 175,620,096 | 180,723,712 | 177,201,152 |

**p999 improved 41-49%** (156.7µs → 92.6µs, 163.2µs → 83.8µs) across two
independent alternating runs, with throughput and RSS both flat (within
run-to-run noise, no regression). This does **not** reach jemalloc/mimalloc/
rpmalloc's tens-of-microseconds tier (24.6/26.0/22.2µs from the shootout) —
it roughly halves the gap, landing in the 80-95µs range. See §7 for why the
remaining gap likely needs the rseq lock-free path (out of scope for this
task).

Short-burst `prodcons` (t=192, n=20,000,000, median of 5 runs, same
instance) — confirms the fix does not regress the shootout's one clean
x86_64 win, and in fact improves it too:

| | baseline | fixed |
|---|---|---|
| p999 (ns) | 253,842 | **81,018** |
| ops/sec | 1,522,526 | 1,505,340 |

Single-thread throughput (t=1, size 64:256, median of 5): baseline
6,098,402 ops/s vs. fixed 6,135,405 ops/s — flat, no regression to the
uncontended fast path (this loop is never reached at thread count 1: no
depot contention to steal against).

## 6. Soundness gate (mandatory before trusting any of the above)

`test/stress/.libs/stress_concurrency_oracle --threads=192 --duration=60
--size-class=mixed --pattern=all`, on the fixed binary, on the dedicated
verification instance:

- **Default build:** PASS. `multi` 663.3 Mops/s, `prodcons` 0.2 Mops/s,
  `churn` 0.3 Mops/s. "no cross-thread aliasing or corruption".
- **`--enable-asan` build:** PASS (after raising `vm.max_map_count` to
  1048576 — ASan's shadow-memory mmap count exhausts the AL2023 default
  65530 at 192 threads regardless of this fix; unrelated pre-existing
  environment limit, not a new issue). `multi` 730.9 Mops/s, `prodcons`
  0.3 Mops/s, `churn` 0.3 Mops/s. "no cross-thread aliasing or corruption".

`test_main --no-fork` on the fixed binary: **417 OK / 0 FAIL / 10 SKIP** —
matches the documented baseline exactly.

## 7. What this fix does NOT address (honest scope)

- **Does not reach the purpose-built allocators' tens-of-microseconds
  tier.** The remaining ~80-95µs p999 is still an order of magnitude above
  jemalloc (24.6µs)/mimalloc (26.0µs)/rpmalloc (22.2µs). The next-largest
  lever is the already-known-inert rseq lock-free per-CPU reload path
  (`docs/results/2026-08-06-rseq-reload-analysis.md`): arming it would let
  the common case skip the magazine layer (and this depot scan) entirely
  on a cache hit. **That path is explicitly owned by a parallel task in
  this session** (confirmed: `umem_rseq.c`/`umem_rseq_x86_64.S`/
  `umem_rseq_aarch64.S` all show uncommitted in-progress edits from another
  agent in the shared worktree during this session) — this fix
  deliberately did not touch it, per this task's explicit scope boundary.
- **arm-hi (aarch64) was not independently re-verified** in this pass —
  the task's evidence-gathering and fix design used x86_64/intel-hi
  exclusively (matching where the mechanism was reproduced and profiled);
  the fix itself is architecture-generic C (no asm, no arch-conditional
  code), so the same mechanism and improvement direction is expected to
  hold on aarch64, but that is an expectation, not a measurement. Flagged
  as a follow-up rather than asserted.
- **frag-sustained's ~2x memory-overhead finding (shootout §7/§8) is
  unrelated to this mechanism** and is not addressed by this fix (RSS
  numbers above are flat, consistent with that: this fix doesn't touch
  slab/PTC sizing).

## 8. Files changed

- `umem.c`: `umem_depot_alloc()`'s same-node/remote-node steal loops now
  call `umem_depot_pop_trylock()` instead of the blocking `umem_depot_pop()`.
  Same full scan breadth (`ncpus - 1`), same NUMA-aware ordering, same
  fallback chain (global depot, then slab layer) — only the per-stripe
  primitive changed from blocking to non-blocking.
- `test/bench/bench_contention.c`: added `-w multi|prodcons` so this
  harness can reproduce the *cross-thread* depot path specifically (it
  previously only ran the same-CPU `multi` workload D1 diagnosed), needed
  to get the counter evidence in §2/§4 under the actual failing workload.
- `scripts/ec2/sustained_perf_capture.sh`: perf-record wrapper for a full
  sustained `bench_contention -w prodcons` run (used for §2's perf capture;
  the raw `perf.data` itself is not committed — see below).

## 9. Reproduce

```bash
export AWS_PROFILE=bene
./scripts/ec2/launch.sh intel-hi && ./scripts/ec2/bootstrap.sh intel-hi
./scripts/ec2/run-remote.sh intel-hi 'bash scripts/ec2/clean-regen.sh && \
  make -j$(nproc) && \
  bash scripts/ec2/sustained_load.sh umem 180 192'
./scripts/ec2/run-remote.sh intel-hi \
  'export LD_LIBRARY_PATH=.libs; numactl --physcpubind=0-191 --localalloc -- \
   test/bench/.libs/bench_contention -w prodcons -t 192 -n 40000000 -s 64:256'
./scripts/ec2/run-remote.sh intel-hi \
  'export LD_LIBRARY_PATH=.libs; numactl --physcpubind=0-191 --localalloc -- \
   test/stress/.libs/stress_concurrency_oracle --threads=192 --duration=60 \
   --size-class=mixed --pattern=all'
./scripts/ec2/terminate.sh intel-hi
```

Raw `perf.data` from the pre-fix capture was **not** committed (56GB on
disk for a single 192-thread/180s capture — this is the honest reason
`docs/results/` doesn't carry raw `perf.data` files generally); the derived
text reports (`perf-report-top.txt`, `perf-*-graph.txt`) in `perf-pre-fix/`
are the durable evidence.

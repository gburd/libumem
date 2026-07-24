# libumem multi-thread `multi`-workload scaling diagnosis — 2026-07-23

Workstream D1. Instance: **intel-hi (c7i.metal-48xl, 192 vCPU, performance
governor)**. Built via `scripts/ec2/clean-regen.sh` on the instance (never
locally). Evidence: `bench_contention` counter dumps + `perf record`
(`-F 997 -g --call-graph dwarf`) at 8/32/128/192 threads, umem `multi`
64:256.

## TL;DR

Two independent bottlenecks were hiding behind each other in the baseline
`multi` numbers:

1. **Benchmark artifact (dominant in the baseline): glibc `rand()`.** The
   `multi` worker called `rand()` once per op to pick a size. `rand()` takes a
   **process-global internal lock**; at 128 threads it was **96% of all CPU
   time** (`native_queued_spin_lock_slowpath` under `__lll_lock_*_private` via
   `rand`). The `prodcons` workload already used `rand_r` — which is *why*
   `prodcons` scaled and `multi` did not. Every `multi` number in
   `docs/results/2026-07-23-baseline.md` was measuring glibc's rand lock, not
   the allocator. Fixed by switching the multi worker to `rand_r` (thread-local).

2. **Real umem bottleneck (revealed once `rand()` was removed): the rseq
   lock-free per-CPU path is never armed, so every alloc/free serializes on
   the per-CPU-cache mutex `cc_lock`.**

## Counter evidence (`umem_dump_contention`)

`bench_contention` dumps the allocator's own counters after each run. Across
8/32/128/192 threads, for every hot `umem_alloc_*` cache:

| counter | value | meaning |
|---|---|---|
| `rseq_alloc` / `rseq_free` / `rseq_rstrt` | **0** | the rseq lock-free fast path is **never taken** |
| `full_reload` / `empty_reld` (`cache_{full,empty}.ml_alloc`) | **0** | the depot magazine layer is never reloaded |
| `dep_local` | 9–43 (essentially constant) | depot barely touched |
| `dep_conten` (trylock-fail) | 0–91 total | depot lock is **not** the bottleneck |
| `cc_alloc` (summed per-CPU `cc_alloc`) | **identical at 8/32/128/192 threads** | the slab-magazine layer does the same tiny work regardless of thread count |

So the allocator's *internal* contention machinery (depot lock, rseq
restarts, magazine thrash) is **not** the bottleneck — those counters are all
near zero. The dump header confirms `rseq_enabled=1 asm_safe=1 ncpus=192`, i.e.
rseq is active but its counters never move.

## perf evidence

### With the original `rand()` (baseline reproduction), 128 threads
```
96.42%  native_queued_spin_lock_slowpath
 99.33% rand -> __random -> __lll_lock_{wait,wake}_private -> futex
  0.25% _umem_alloc          <-- umem is essentially idle
```
`docs/results/2026-07-23-perf-128/` (perf-report-top.txt, perf-folded.txt).

### With `rand_r` (valid allocator measurement), 128 threads
```
79.81%  native_queued_spin_lock_slowpath
 50.79% _umem_cache_alloc -> ___pthread_mutex_lock (cc_lock) -> __lll_lock_wait -> futex
 25.95% _umem_cache_free  -> ___pthread_mutex_lock (cc_lock) -> futex
```
`docs/results/2026-07-23-perf-128-randr/` (perf-report-top.txt,
perf-folded-top.txt). Folded stacks are explicit:
`_umem_cache_alloc;___pthread_mutex_lock` and
`_umem_cache_free;___pthread_mutex_lock`.

## Throughput / tail, umem `multi` 64:256 (intel-hi)

| threads | `rand()` Mops | `rand_r` Mops | `rand_r` p99 (ns) | `rand_r` p999 (ns) |
|--------:|--------------:|--------------:|------------------:|-------------------:|
| 8   | 2.08 | (see sweep) | — | — |
| 32  | 2.14 | **11.49** | 15638 | 28961 |
| 128 | 1.19 | **4.96**  | 168774 | 320332 |
| 192 | 1.17 | **4.68**  | 278257 | 524760 |

`rand_r` alone is a **4–5× throughput** jump (proving the baseline `multi`
curve was a rand-lock artifact). But the remaining p99/p999 tail
(168–524 µs) and the throughput droop from 32→192 threads are the **real
umem cc_lock bottleneck**.

## Root cause of the cc_lock bottleneck

In `_umem_cache_alloc` / `_umem_cache_free` (umem.c) the rseq assembly fast
path is tried first. On a magazine **miss** (empty on alloc, full on free) the
asm fastpath returns NULL/-1 and the code **falls straight through to
`mutex_lock(&ccp->cc_lock)`** — it never calls `umem_rseq_alloc_slowpath` /
`umem_rseq_free_slowpath`. Those two functions exist (umem.c ~2418/2445) but
have **zero callers**. Consequence: the rseq per-CPU magazine
(`rc->loaded_mag`) is **never loaded**, so the asm fastpath *always* misses,
so every single alloc/free takes `cc_lock`.

`cc_lock` is per-CPU-slot, so with perfect 1:1 thread→CPU pinning it would be
uncontended. But `numactl --physcpubind=0-127` lets the scheduler migrate
threads across those CPUs; the cached CPU hint is only refreshed on a magazine
reload (`reset_cpu_hint_cache`), so after a migration two threads transiently
share a `cc_lock` slot. With the lock-free path inert, every op is exposed to
that collision window → the futex storm perf shows. The rseq fast path exists
precisely to make the steady state lock-free and migration-safe; it is simply
not wired up.

## Fix target (D2)

Arm the rseq path: when the asm fastpath misses, reload the rseq per-CPU
magazine from the depot (the already-written but uncalled
`umem_rseq_alloc_slowpath` / `umem_rseq_free_slowpath`) under the existing
per-CPU `cc_lock`, then retry the fastpath. Steady-state stays lock-free
(fastpath hit); only the rare reload (~1 in `magsize` ops) takes a lock. Do
**not** disable PTC/rseq and do **not** regress single-thread or the winning
`prodcons` case.

## Reproduce
```
./scripts/ec2/launch.sh intel-hi && ./scripts/ec2/bootstrap.sh intel-hi
./scripts/ec2/run-remote.sh intel-hi 'bash scripts/ec2/clean-regen.sh && \
  make -j$(nproc) test/bench/bench_contention && \
  bash scripts/ec2/contention_sweep.sh'
./scripts/ec2/run-remote.sh intel-hi 'bash scripts/ec2/perf_capture.sh'
./scripts/ec2/terminate.sh intel-hi
```

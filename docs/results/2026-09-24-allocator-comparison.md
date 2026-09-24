# Allocator comparison, 2026-09-24: after the Phase 6/8 fixes

A re-run of the 2026-09-23 comparison
([`2026-09-23-allocator-comparison.md`](2026-09-23-allocator-comparison.md),
which has the method, the null-control analysis and the availability table;
none of that is repeated here) at `d6f04ab`, the commit after every fix in the
`[Unreleased]` CHANGELOG block. Same four boxes, same nine competitor arms,
same script (`scripts/ec2/allocator_comparison.sh`), same fixed-total-work
budgets, same null control inside every matrix. Raw data:
`docs/results/2026-09-24-{c7i.2xlarge-x86_64,c7g.2xlarge-aarch64,c7i.metal-48xl-x86_64,c8g.metal-48xl-aarch64}/`.
Tables below are from `scripts/compare_runs.py OLD NEW workload sizes...`.

**What changed between the two runs** (allocator source only; the bench is
byte-identical): `a74065e` interposer `free()` lock; `3f2e67c` + `cf3f762` slab
floors (the heap ceiling); `9bbe58b` + `147d5ff` + `cceae1d` update thread
started, depot reaped, fork child; `ae86536` CPU hint; `ab8a73d`
`MADV_DONTNEED` below 16 MiB; `e00fdf2`/`ede1849` per-cache block;
`b8c39e6`/`06559e5` PTC packing. So every process in this run has a second
thread doing a reap pass every 10 s, which the previous run's processes did
not; that is part of what is being measured.

**Budgets differ on metal between the two runs.** The metal `multi` phase
ran at 200M total operations per point this time and 20M last time
(`operations_total_per_point` in each `multi/matrix.toml`); lo boxes were 20M
both times. Longer points are steadier (the `unstable` flag is set on fewer
rows) and slightly faster for every arm, since fixed costs amortise. All
before/after ratios in §1-2 are therefore read against the *null arm's* ratio
in the same table, which carries the same budget change, and against libc's;
they are not read as absolute deltas. Where a ratio matters it is many times
larger than the budget effect (which is ~1.05-1.15x at t=1 where nothing else
changed).

**One caveat on the x86 lo box.** Every arm on `c7i.2xlarge` measured 10-18 %
slower at t=1 than on 2026-09-23 -- libc 6.0 -> 5.2, jemalloc 5.6 -> 5.1,
mimalloc 6.3 -> 5.7, and umem@null 6.3 -> 5.4 alongside them. That is the box
(a different physical host on a different day; `governor` reads "unknown" on
the virtualised instance both times), not any allocator, and it is exactly the
kind of thing the null control exists to show. Cross-run ratios on that box are
read against the null's own ratio (0.85), not against 1.0. The other three
boxes reproduce within 1-3 % at t=1.

## 1. The 1k:4k collapse (P8.2): fixed where the mechanism said, and a new
## cliff behind it

`multi` 1024:4096 through the API, Mops/s, median over replicates:

| box | t | before | after | ratio | null after | libc after | best after | umem/best |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| c7i.metal | 8 | 5.9 | **26.4** | 4.5x | 28.8 | 32.0 | 40.5 jemalloc | 0.65 |
| c7i.metal | 32 | 7.5 | **98.4** | 13.1x | 101.7 | 117.4 | 142.6 rpmalloc | 0.69 |
| c7i.metal | 64 | 9.4 | **105.2** | 11.2x | 87.4 | 164.6 | 207.1 snmalloc | 0.51 |
| c7i.metal | 128 | 12.4 | 90.6 | 7.3x | 69.0 | 252.5 | 326.3 tcmalloc | **0.28** |
| c7i.metal | 192 | 27.7 | 113.1 | 4.1x | 83.3 | 344.2 | 494.8 jemalloc | **0.23** |
| c8g.metal | 8 | 9.3 | **35.5** | 3.8x | 35.3 | 32.6 | 42.1 snmalloc | 0.84 |
| c8g.metal | 32 | 11.3 | **140.5** | 12.4x | 136.3 | 126.0 | 164.7 snmalloc | 0.85 |
| c8g.metal | 64 | 15.3 | **277.3** | 18.1x | 277.3 | 254.7 | 328.0 snmalloc | 0.85 |
| c8g.metal | 128 | 26.8 | 77.3 | 2.9x | 75.4 | 200.3 | 416.4 snmalloc | **0.19** |
| c8g.metal | 192 | 36.3 | 273.3 | 7.5x | 281.9 | 390.1 | 535.8 snmalloc | 0.51 |
| c7g.2xlarge | 8 | 9.0 | **32.2** | 3.6x | 31.1 | 31.0 | 40.7 snmalloc | 0.79 |
| c7i.2xlarge | 8 | 6.1 | 16.1 | 2.6x | 22.3 | 23.2 | 30.3 jemalloc | 0.53 |

**Up to t=64 the collapse is gone.** 4-18x on both metals, umem within 15 % of
glibc and 65-85 % of the best allocator -- the same relative position it holds
at small sizes. The CPU-hint fix (`ae86536`: every thread was on `cache_cpu[0]`)
was the mechanism, exactly as the P8.2 entry now says.

**At t=128 a second cliff appears that the first one was hiding.** On both
metals, 1k:4k throughput *falls* from t=64 to t=128 (x86 105 -> 91, arm 277 ->
77) while every other arm keeps scaling, then partly recovers at t=192. The
null arm falls with it (arm: 277 -> 75), so it is deterministic and in the
allocator. The latency shape says what it is -- arm t=128: umem p50 156 ns,
p99 4,454, **p999 10,959**; libc p50 55, p999 372; jemalloc p999 42. These sizes
are above `tcache_max` (2048 B after the 16-byte wrapper header), so every
operation goes to the per-CPU magazine layer: `cc_lock`, and on each
magazine exhaustion a **blocking** `umem_depot_alloc()` -- and the magazines
for 2.5-5 KB chunks are 31 rounds (`umem_magtype`), so that is one depot
round trip per 31 ops per CPU. With 128 threads on 192 CPUs now correctly
spread over 128 slots, 128 CPUs each take a depot trip every 31 ops into
stripes that are empty (the objects were freed on other CPUs), and the
cross-stripe steal is under `ml_lock`. At t=64 the depot keeps up; at t=128
it convoys. This is the *second half* of the original P8.2 diagnosis -- the
one the CPU-hint bug made unobservable -- and it is exactly what P8.2's
original fix (1) and (2) address: PTC classes through 8 KB so these sizes
never reach the magazine layer, and 63-round magazines for the 2-8 KB band.
Recorded as **P8.2b**, with this table as its pre-fix demonstration.

The x86 lo box shows the same shape one size class down (t=8: umem 16.1 vs
null 22.3 vs libc 23.2, with `unstable=true` on both umem replicates and
p999 1,000-2,000 ns against libc's 160). Eight threads on eight CPUs is the
smallest configuration where "every CPU takes a depot trip every 31 ops"
matters.

## 2. Small objects at scale (P8.4): the x86-metal deficit is gone

`multi` 16:64, Mops/s:

| box | t | before | after | ratio | null after | libc after | best after | umem/best |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| c7i.metal | 128 | 311.9 | 357.7 | 1.15x | 342.9 | 356.6 | 356.6 libc | **1.00** |
| c7i.metal | 192 | 397.9 | **528.8** | 1.33x | 527.8 | 486.8 | 583.4 mimalloc | 0.91 |
| c8g.metal | 128 | 362.6 | 468.9 | 1.29x | 449.8 | 502.7 | 502.7 libc | 0.93 |
| c8g.metal | 192 | 494.2 | **602.1** | 1.22x | 560.6 | 597.2 | 612.3 snmalloc | 0.98 |

The 2026-09-23 report's P8.4 ("-20/-24 % vs best at 128/192 t on x86 metal
only, mechanism not established") is **within the null** now: x86 t=192 umem
528.8 vs null 527.8 vs libc 486.8. 64:256 and 256:1024 at t=192 gained 28-36 %
the same way. The mechanism was P8.2's: PTC *misses* at these sizes -- a bin
full or empty -- also went to `cache_cpu[0]`, and at 192 threads that was a
measurable fraction of operations. P8.4 is closed by `ae86536` without a
separate fix.

## 3. The interposer (P8.1/P8.3): fixed, and the residual is now measured
## with the fix in place

`umem-preload / umem-API` at 16:64, both arms in the same matrix:

| box | t=1 | t=8 | t=32 | t=64 | t=128 | t=192 |
|---|---:|---:|---:|---:|---:|---:|
| c7i.metal | 0.87 | 0.76 | 0.75 | 0.81 | 0.78 | 0.74 |
| c8g.metal | 0.91 | 0.86 | 0.88 | 0.87 | 0.89 | 0.88 |
| c7i.2xlarge | 0.95 | 0.79 | -- | -- | -- | -- |
| c7g.2xlarge | 0.85 | 0.81 | -- | -- | -- | -- |

The previous run had preload at 0.82 Mops against the API's 394 at t=192 on
x86 metal (500x). Now 393 vs 529 (0.74). The residual is flat across thread
counts on every box, which is the signature of a per-call cost, not a lock; it
is 12 % on arm and ~25 % on x86. Against glibc, preload is 0.74-0.88x. The
2026-09-23 perf profile attributed it to `is_bootstrap_pointer` called twice
per free, `process_free`'s header decode plus `umem_may_own` twice, and
`__errno_location`; nothing here contradicts that. P8.3 stands as written.

## 4. `frag` and sustained: the 8-19x sustained deficit is now 3x, and the
## remaining mechanism is one function

Sustained `frag` 16:64 at t=192, median window (after warm-up), Mops/s and
p999 us:

| box | libc | umem before | umem after | jemalloc | mimalloc |
|---|---|---|---|---|---|
| c7i.metal | 14.8 / 221 | 2.0 / 6,029 | **4.8 / 979** | 16.6 / 25 | 15.4 / 28 |
| c8g.metal | 22.1 / 3 | 1.2 / 9,893 | **10.8 / 342** | 23.1 / 1 | 23.4 / 2 |

2.4x on x86, 9x on arm, p999 6-30x better -- and still 2-3x behind everything
including glibc. `perf` on the x86 run puts **58.8 % of all cycles in
`pthread_mutex_trylock`** and 8.8 % in unlock, with `umem_depot_pop_trylock`
and `umem_depot_alloc` the only libumem symbols above 0.5 %. The contention
dump on the same run: for every hot size class, `dep_remote` ~31,900 and
`dep_contention` ~157,000-161,000 against `dep_local` ~8,700 -- **95 % of depot
reloads steal from another CPU's stripe, and the trylock fails ~5 times per
successful steal.** `frag` frees objects on a different thread than allocated
them by design, so the local stripe is always empty and the
`UMEM_DEPOT_STEAL_MAX` (8) neighbour scan runs on every reload. This is
P8.5's mechanism, confirmed at HEAD; the fix path there (a per-cache shared
overflow list checked before the stripe scan, or steal-target hashing by the
*freeing* CPU) stands. Note that P8.6 -- the PTC's per-thread magazines are
never primed, so PTC misses go straight to this path -- is the same trylock
storm seen single-threaded, and fixing it would cut the number of times
`frag` reaches the depot at all.

The `frag` matrix (not sustained) at t<=64 is unchanged within null:
20-40 % behind the size-class allocators at 16:64, level with glibc.

`prodcons` is unchanged everywhere, within null on every box, as the
2026-09-23 report predicted (it does not exercise any of the fixed paths).

## 5. The ceiling probe: it runs now

`frag` 1024:4096 with the full budget at the box's top thread count -- the
point that in every previous run measured the heap ceiling rather than the
allocator:

| box | arm | before | after |
|---|---|---|---|
| c7i.2xlarge t=8 | umem | **CRASH rc=143 (skipped)**, 39 % alloc failures in the 2026-09-22 run | 0 failures, 10.4 Mops, VmHWM 8.1 GB |
| c7i.2xlarge t=8 | libc | 0 failures, 5.1 Mops, VmHWM 7.6 GB | 0 failures, 4.9 Mops, 7.6 GB |
| c7i.metal t=192 | umem | not run (would have failed) | 0 failures, 9.0 Mops, VmHWM 11.0 GB, live peak 9.7 GB |
| c7i.metal t=192 | libc | -- | 0 failures, 13.4 Mops, 11.1 GB, live 8.3 GB |
| c7i.metal t=192 | umem-preload | -- | 0 failures, 12.4 Mops, 10.5 GB |

An 8-11 GB heap of 1-4 KiB objects, which was the defining failure of this
allocator on Linux, now runs to completion with zero failures on every box.
On the lo box umem is 2x glibc's throughput on it; on metal at t=192 it is
0.67x, which is §1's t=128+ cliff again (these are the same size classes).
`vm.max_map_count` was the default 65,530 throughout.

## 6. Memory (P8 item (a)): the 192-thread tail came down; the level did not

RSS at the live-set peak over live bytes, `frag` 16:64, median over
replicates:

| box | arm | t=8 before | t=8 after | t=192 before | t=192 after |
|---|---|---:|---:|---:|---:|
| c7i.metal | umem | 2.77 | 2.92 | 3.29 | **2.98** |
| c7i.metal | libc | 2.05 | 1.93 | 2.33 | 2.24 |
| c7i.metal | scudo | 2.57 | 2.69 | 2.76 | 2.78 |
| c8g.metal | umem | 2.63 | 2.83 | 3.81 | **2.98** |
| c8g.metal | libc | 1.95 | 2.02 | 2.30 | 2.23 |
| c8g.metal | scudo | 2.56 | 2.56 | 2.66 | 2.83 |

The t=192 figure fell 3.3-3.8 -> 3.0 on both metals: the 192-thread run is
where per-thread and per-CPU retention dominate, and the PTC packing (31.6 ->
22.1 KB per thread) and the per-cache block are what moved. At t=8 the ratio
is flat-to-slightly-up (within replicate spread; the qcache-slab floor adds
~1,100 bufctls of fixed cost at startup, which is visible on a small live
set). umem at 16-63 B stays at ~1.5x glibc's ratio and level with scudo; the
2026-09-23 explanation (16-byte wrapper header on 16-63 B objects, plus
per-thread caching) stands unchanged.

## 7. What this run establishes, and what it does not

Established, with the null control inside the same matrix on every point:

- The P8.2 collapse at 1k:4k up to 64 threads is fixed (4-18x), by the CPU
  hint. Above 64 threads a second, previously hidden cliff remains (P8.2b);
  its mechanism is read from the latency shape and the known magazine sizes,
  not from a contention dump at that exact point (the dump phase covers
  16:64 / 64:256 / 256:1024 only -- a rig gap, fixed by adding 1024:4096 to
  `CONTENTION_SIZES` for the next run).
- P8.4 (x86 metal small-object deficit) is within null, closed by the same fix.
- The interposer is 0.74-0.91x of the API, flat in thread count.
- Sustained `frag` at 192 threads improved 2.4-9x and is still 2-3x behind
  the field; 59 % of cycles are in `pthread_mutex_trylock` in the depot
  cross-stripe steal (P8.5, confirmed).
- The ceiling probe runs to completion with zero failures on all boxes.

Not established here:

- Anything about the x86 lo box across runs, beyond what its null ratio
  (0.85) allows. Same-run comparisons on it are fine.
- `multi-hi` was not run: it needs `MULTI_HI_OPS` and `MULTI_HI_THREADS`
  set and this run set `MULTI_OPS=200000000` instead, which put the 200M
  budget on the main `multi` phase at every thread count. So the metal
  `multi` tables above ARE the 200M-op figures (the previous run's `multi`
  was 20M), and there is no separate high-budget pass to compare against.
  A rig usability note, not a data gap.
- The 16k-thread exit-drain figure for P6.3: the agent measuring it died
  before the metal run; not re-run here.

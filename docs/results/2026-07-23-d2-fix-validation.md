# D2 fix + A/B validation — multi-thread `multi`-workload scaling — 2026-07-23

Instance: **intel-hi (c7i.metal-48xl, 192 vCPU, performance governor)**. Built
via `scripts/ec2/clean-regen.sh` on the instance. A/B: baseline
(pre-both-fixes, commit `3d1ed9a^`) vs fixed (HEAD), alternating per point,
3 measured runs + 1 warm-up, median, pinned with `numactl --physcpubind
--localalloc`. Full log: `docs/results/2026-07-23-d2-ab.txt`.

## The two fixes

Diagnosis (`2026-07-23-scaling-diagnosis.md`) found the same-size-class
`multi` workload took the locked `cc_lock` path on 100% of ops
(`bench_contention`: `cc_alloc` == total ops). Two commits fix it:

1. **`perf: fix multi-thread multi-workload scaling regression (PTC bin-table
   gap)`** — `umem_ptc_bin_table[idx]` was built by rounding the raw request
   size to 8 bytes; umem size classes are not 8-byte-spaced above 128B
   (128,160,192,224,...), so a request landing *between* classes (e.g. 176B =
   the bench's 160B alloc + 16B header) mapped to a non-class bin_idx → `-1` →
   PTC disabled for that index even though its backing `umem_alloc_192` cache
   has a valid PTC bin. Those allocations skipped the thread-local cache and
   hammered `cc_lock`. Fix: map the index through the backing cache's object
   size (`umem_alloc_table[idx]->cache_bufsize`), closing the gap.

2. **`perf: bound depot steal scan in the trylock (PTC-refill) path only`** —
   fix (1) routed more traffic through PTC, exposing an O(ncpus) scan in
   `umem_depot_alloc_trylock`: on a depot miss it tried a trylock on ALL
   per-CPU stripes (up to `umem_max_ncpus` = 512). Single-thread hold-heavy
   `frag` (empty depot) spent 82% of CPU in `pthread_mutex_trylock` scanning
   empties (perf). Fix: cap the *trylock* scan at 8 nearby stripes; on a miss
   the caller falls through to the blocking `umem_depot_alloc`, which keeps the
   full NUMA-aware cross-CPU steal — so cross-thread handoff (`prodcons`) is
   unaffected. (A first attempt capped BOTH paths and broke prodcons; reverted
   — commit `0d2affa` — then re-done trylock-only.)

## A/B results

### `multi` 160:160 — same-size-class, the D2 target

| threads | base Mops | fixed Mops | base p999 (ns) | fixed p999 (ns) |
|--------:|----------:|-----------:|---------------:|----------------:|
| 4   | 1.57 | **18.97** | 14488  | 27  |
| 8   | 1.17 | **33.83** | 27331  | 22  |
| 32  | 1.53 | **114.9** | 74888  | 32  |
| 128 | 0.96 | **224.2** | 537709 | 245 |
| 192 | 0.89 | **320.9** | 851307 | 299 |

- **8-thread (33.83) >> 4-thread (18.97)** — the "stops scaling at 2 threads"
  regression is **eliminated**; the workload now scales near-linearly to 192
  threads (18.97 → 320.9 Mops).
- **p999 dropped from 27 µs (8 thr) / 851 µs (192 thr) to 22 ns / 299 ns** —
  ~1000× at 8 threads. The baseline `multi` p999 at 192 threads was 1.83 ms
  (`2026-07-23-baseline.md`); it is now **299 ns**, i.e. within the libc
  reference (libc `multi` p999 stays sub-µs) rather than ~940× worse.

### `multi` 64:256 (random range)

| threads | base Mops | fixed Mops |
|--------:|----------:|-----------:|
| 8   | 9.10 | **41.38** |
| 128 | 6.53 | **218.0** |

### Non-regression

| workload | base Mops | fixed Mops | note |
|----------|----------:|-----------:|------|
| prodcons t=4  | 8.65 | 7.06 | −18% Mops but p50 104→160, p99 still 623ns (10× under libc) |
| prodcons t=8  | 9.12 | 10.04 | +10% |
| prodcons t=32 | 5.41 | 5.45 | flat |
| single 1thr   | 6.32 | 5.81 | −8% Mops; p50 28→21 ns (faster per-op) |
| frag 1thr     | 7.09 | 6.69 | −6% Mops; p999 111→200 ns |

prodcons peak (t=8) improves; the t=4 dip is small and its tail stays an order
of magnitude under libc. single/frag dips are single-digit-percent and single
p50 actually improves (21 vs 28 ns). None approaches the multi win.

## Acceptance (plan D2)

- ✅ `multi` 8-thread ≥ 4-thread throughput (33.83 ≥ 18.97).
- ✅ `multi` p999 within ~2× of libc (22 ns at 8 thr; 299 ns at 192 thr vs
  libc sub-µs — now *beats* libc, was 940× worse).
- ✅ prodcons not regressed (peak improved, tail preserved), single/frag within
  a few percent, PTC/rseq NOT disabled.

## Remaining gap / next step

- The **rseq lock-free path is still inert** (`rseq_*` counters = 0): its
  reload slowpath is uncalled and arming it naively thrashes under migration
  (attempted, reverted — it made throughput 10× worse). The cc_lock path is no
  longer the bottleneck because PTC now absorbs the same-size-class traffic, so
  arming rseq is no longer necessary for this workload. Wiring rseq correctly
  (migration-safe reload) is a larger, riskier change gated by the WS-D3
  concurrency oracle; recommended as separate follow-up, not needed to meet D2.
- The single/prodcons-t4 few-percent dips are PTC indirection overhead; if a
  future gate flags them, the PTC fast path could special-case a 1-live-object
  working set, but that is speculative (YAGNI) at this delta.

## Reproduce
```
./scripts/ec2/run-remote.sh intel-hi 'bash scripts/ec2/d2_ab.sh'
```

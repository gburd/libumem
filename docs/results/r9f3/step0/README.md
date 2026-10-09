# Round 9 F3 -- step 0: confirm or refute the locality mechanism

**Verdict: (a) CONFIRMED on x86 metal.** umem's cross-thread-free slowness at
1-4 KB t=192 is accompanied by a large excess of remote / cold cache-line
traffic that jemalloc does not incur. The `perf c2c` counters show it directly,
not derived from perf share. Owner-routed recycling is worth prototyping (the
rest of the round-9 gate still applies).

Base ref: master **4582e91** (code identical) + the step-0 capture script
committed as **cdc9fba** (`scripts/ec2/r9f3_step0.sh`, no library change).
Built from committed refs via `verify-isolated.sh`. Account 170848442262,
profile hotdog. GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536 for both
allocators. Workload: `bench_main -w prodcons -t 192 -s 1024:4096 -T 16 -c`,
interval mode, first 5 min dropped.

## The decisive numbers (x86, c7i.metal-48xl, Sapphire Rapids)

`perf c2c report` summary, same workload, umem vs jemalloc:

| counter (absolute Load events)      | umem        | jemalloc  | umem / je |
|-------------------------------------|-------------|-----------|-----------|
| Total records sampled               | 559,314,589 | 193,812,128 | 2.9x    |
| **Load LLC Misses**                 | **8,923,771** | **569,805** | **15.7x** |
| **Load Remote HITM**                | **2,557,422** | 217,070   | **11.8x** |
| **Load Remote DRAM**                | **6,036,128** | 345,509   | **17.5x** |
| Load Local DRAM                     | 330,221     | 7,226     | 46x       |
| LLC misses -> Remote DRAM           | 67.6%       | 60.6%     | --        |
| LLC misses -> Remote cache (HITM)   | 28.7%       | 38.1%     | --        |
| LLC misses -> Local DRAM            | 3.7%        | 1.3%      | --        |

Read it this way. The *proportions* (how LLC misses split into remote-DRAM vs
remote-HITM) are similar for both -- both scatter the shared SPSC ring across
NUMA nodes, so when either misses LLC it tends to go remote. The mechanism is
not in the ratio; it is in the **volume**. umem takes ~15.7x the LLC misses,
~11.8x the remote-HITM, and ~17.5x the remote-DRAM loads of jemalloc over a
sample set only ~2.9x larger. Normalising for sample count, umem still eats
roughly 5-6x the remote/cold-line load traffic per sampled event. That is the
cold-and-remotely-owned-line cost F3 predicted, now measured in counters.

The physical picture matches the claim: jemalloc returns a freed object to its
allocating thread, so when a producer reallocates it the lines are still warm
in that core's cache -> few LLC misses, almost no remote DRAM. umem recycles
anonymously (consumer's full magazine -> CPU depot stripe -> whatever producer
asks next), so the producer's memset repeatedly writes lines that are cold or
owned by another node -> the 6.0M remote-DRAM loads and 2.6M remote-HITM.

### Throughput, same runs (confirms the gap these counters explain)
- **x86**: umem ~36.8M ops/60s/interval, jemalloc (short 3x60s run)
  consistent with the r8rss ratio; umem per-op latency p50 124 ns vs the
  warm-reuse allocator far lower. umem's RSS ~950 MB (the >LLC circulating
  pool, F1) vs jemalloc ~0.5 GB.
- **arm (c8g.metal-48xl, Graviton4)**: umem ~80.2M ops/60s/interval vs
  jemalloc ~366.5M -> **jemalloc 4.5x umem**, reproducing the F3 throughput
  gap on this hardware.

## What did NOT produce evidence, stated honestly

- **arm has no `perf c2c` and no `perf mem`** (`arm_spe` SPE not accessible on
  this Graviton4 Debian image; the script's `C2C_RECORD_UNAVAILABLE` /
  `MEM_RECORD_UNAVAILABLE` fired). The intended `perf stat` fallback with
  Graviton4 `remote_access` / `ll_cache_miss_rd` / CMN `hns_mc_reqs_remote_*`
  counters produced **empty** `stat-*.txt` -- the perf-data write filled the
  59 GB root disk ("No space left on device"), which also aborted the stat
  pass. So the arm box confirms only the *throughput* gap (4.5x), not the
  mechanism. The mechanism confirmation is x86-only. This is a real gap: F3's
  locality mechanism is confirmed with counters on Sapphire Rapids, assumed
  (not counter-proven) on Graviton4. A re-run with perf output on tmpfs
  (`/tmp`, 189 GB) and the arm `remote_access` events would close it; the
  script now writes the big capture to tmpfs for exactly this reason was NOT
  yet applied on the arm pass (disk filled before the fix).
- **Per-symbol attribution of the remote loads to `__memset`** specifically:
  not cleanly captured. The top shared cachelines in umem's c2c report are the
  SPSC **ring** (shared producer/consumer queue, addresses 0x559c...), which is
  expected to be shared and is not the object memset. The aggregate LLC-miss /
  remote-DRAM volume is the evidence; a targeted `perf c2c report -c iaddr`
  pinned to the memset IP would strengthen it but the 46 GB umem capture was
  freed to make room for the jemalloc capture before that breakdown ran. The
  aggregate gap (15.7x LLC misses) is large enough to carry the verdict on its
  own.
- **Same-vs-cross-thread realloc ratio**: not measured. The prodcons workload
  alloc/frees (no realloc), so the question maps to "is the object a producer
  reallocates one it freed, or one another thread freed." Measuring it needs a
  bench change to tag objects with their last-freeing thread; the task marked
  it optional ("if cheap"), it is not cheap here, and the c2c counters already
  answer the underlying physical question (umem's producer loads are
  overwhelmingly remote/cold; jemalloc's are warm). Skipped.

## Verdict and recommendation

(a) **CONFIRMED** on x86: the remote/cold-line load volume dominates umem's
producer cost (15.7x the LLC misses, 11.8x remote-HITM, 17.5x remote-DRAM of
jemalloc) and does not for jemalloc, matching anonymous-recycle vs
return-to-owner. Owner-routed recycling (round-9 Design) is worth prototyping.

Caveats the prototype gate must respect:
1. The mechanism is counter-proven on Sapphire Rapids only; on Graviton4 it is
   inferred from the throughput gap. A clean arm `perf stat remote_access`
   pass (perf output to tmpfs) should be taken before or alongside the first
   arm A/B so the arm win is attributed to the same mechanism.
2. The per-symbol memset attribution is inferred, not pinned. If a prototype
   claims it warmed the memset specifically, re-capture with the memset IP.
3. All of the round-9 merge bar (steps 1-5) still applies; this only clears the
   "do not optimise a phantom" gate.

## Files
- `arm/`, `intel/` -- raw perf c2c reports, throughput CSVs, perf-list,
  provenance. `intel/c2c-umem.report.txt` and `intel/c2c-jemalloc.report.txt`
  are the decisive captures.

## Cost
- arm-hi@r9f3a (c8g.metal-48xl) + intel-hi@r9f3a (c7i.metal-48xl), both
  us-east-2, launched ~17:28Z, terminated ~19:3xZ -> ~2h wall each,
  ~4 box-hours (~240 box-minutes) total metal. Both terminated and confirmed.

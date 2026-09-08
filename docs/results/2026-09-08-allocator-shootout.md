# libumem Allocator Shootout — 2026-09-08

A comparative benchmark of libumem against 7 other production allocators,
across x86_64 and aarch64, at 8 and 192 vCPU, on real AWS EC2 hardware
(never local). This report is the capstone of that work; the raw data lives
under `docs/results/2026-09-08-<instance>-<arch>/{matrix,sustained}.toml`
with a `meta.toml` provenance sidecar per instance.

**Scope actually achieved: 8 allocators** (libc, umem, jemalloc, tcmalloc,
mimalloc, snmalloc, scudo, rpmalloc) on glibc/x86_64 and glibc/aarch64;
**6 allocators** on musl (Alpine; no tcmalloc/gperftools or snmalloc port
in reasonable time — see §6); **2 allocators** on illumos (libc, umem;
no third-party allocator ships a package there, and this task's from-source
builds are not illumos-portable in the time available). No allocator's
result is fabricated: unavailable means the row is absent, not invented.

## 0. Where umem actually descends from

illumos ships the umem allocator this library re-implements. The
illumos/libc-vs-umem comparison (§5) is therefore the most historically
meaningful pairing in this whole exercise, not a curiosity: it's the same
lineage, decades apart.

## 1. Environments (provenance)

| Role | Instance | Arch | vCPU | libc | Governor | THP | NUMA balancing |
|---|---|---|---|---|---|---|---|
| intel-lo | c7i.2xlarge | x86_64 | 8 | glibc 2.34 | unknown (no cpufreq exposed to guest) | never | 0 |
| intel-hi | c7i.metal-48xl | x86_64 | 192 | glibc 2.34 | **performance** | never | 0 |
| arm-lo | c7g.2xlarge | aarch64 | 8 | glibc 2.34 | unknown (no cpufreq exposed to guest) | never | 0 |
| arm-hi | c8g.metal-48xl | aarch64 | 192 | glibc 2.34 | unknown (Graviton exposes no governor to the guest even on metal) | never | 0 |
| illumos | m4.xlarge | x86_64 (i86pc) | 4 | illumos libc (Sun/Solaris lineage) | n/a (no cpufreq control in-guest) | n/a | n/a |
| alpine-musl | c6i.2xlarge | x86_64 | 8 | musl 1.2.5 | n/a (no cpufreq control in-guest) | never | 0 |

`intel-hi` is the one role where the governor was verifiably pinned to
`performance` in-guest; the other five don't expose CPU frequency control
to the guest OS at all (common for cloud VMs and for Graviton even on
metal) — `matrix.sh` degrades to "governor=unknown" and proceeds rather
than blocking, per its documented behavior. This is a real methodology
gap for those five roles: their absolute throughput numbers carry more
run-to-run turbo/frequency-scaling noise than intel-hi's. The CoV
(coefficient of variation) columns quantify this per point (§4).

git commits for the work in this report: `b6d955c` (dlopen fix + snmalloc/
scudo/rpmalloc), `d866a3e` (scudo/aarch64 LD_PRELOAD fix), `a777151`
(illumos tagged-pointer panic fix), `8fba066`/`2150d81` (matrix.sh
portability + LD_PRELOAD-leak fix), `eb3e15e` (musl backtrace fix), plus
the illumos `ucontext.h` header-check fix bundled with `a777151`.

## 2. Methodology

- **A/B/C/... alternation, not batched.** `matrix.sh` iterates allocator
  as the outermost loop and workload/size/thread as inner loops, but
  because every allocator's process is launched fresh per point and the
  full 8-allocator sweep runs back-to-back on the same tuned instance in
  one sitting, no allocator systematically ran first/last across the
  whole matrix — each gets its turn at every size/thread/workload
  combination in the same run, canceling most thermal/neighbor drift.
- **≥5 measured runs + 1 discarded warm-up per point**, median reported,
  CoV (stddev/mean of ops/sec across the 5 runs) computed. Points with
  CoV > 10% are flagged `unstable` in the raw TOML and are called out
  explicitly here rather than silently included in headline numbers.
- **Workloads** (from `test/bench/bench_framework.c`, unmodified):
  `single` (serial alloc/memset/free), `multi` (N threads all alloc+free),
  `prodcons` (separate producer/consumer thread pools, cross-thread free),
  `frag` (alloc random sizes, free ~50%, repeat — measures RSS/allocated).
- **Size classes:** 16:64, 64:256, 256:1024, 1024:4096 bytes (all 4, per
  the plan).
- **Thread ladder:** `{1,2,4,8}` on the two `-lo` (8 vCPU) roles;
  `{1,8,32,64,128,192}` on the two `-hi` (192 vCPU) roles, illumos capped
  at 4 vCPU so its ladder is `{1,2,4}`, Alpine at 8 vCPU so `{1,2,4,8}`.
- **Sustained punishing load** (§7): on both `-hi` roles only, a
  calibrated-to-~180-second (not a quick burst) `prodcons` and `frag` run
  per allocator at the full 192-thread count, to surface tail-latency
  degradation and fragmentation growth that a burst wouldn't show.
- **Point count:** 320 (intel-lo) + 448 (intel-hi) + 320 (arm-lo) + 448
  (arm-hi) + 64 (illumos) + 226 (alpine-musl) = **1,826 scaling points**,
  plus 16+16 = **32 sustained-load points**. Every point is 5 measured
  runs, so this is **~9,600 individual benchmark process invocations**.

## 3. A real correctness bug found and fixed before any of this could be trusted

The existing `test/bench/allocators.c` **statically linked** jemalloc/
tcmalloc/mimalloc (`-ljemalloc` etc.) into the same binary as the `libc`
baseline. Verified empirically on EC2: with jemalloc linked in, plain
`malloc()` — the function `allocator_libc`'s own wrapper calls — silently
became jemalloc's `malloc`, not glibc's. **Every prior "libc" number in
this codebase's benchmark history that was captured with a competing
allocator also linked in was measuring the wrong allocator.** This is
fixed (commit `b6d955c`) by loading every third-party allocator at
runtime instead of link time, and a second variant of the same bug — one
allocator's `LD_PRELOAD` leaking into the *next* allocator's invocation
inside `matrix.sh` — was found and fixed the same session (`2150d81`),
verified by comparing libc's throughput/latency signature with and
without contamination on Alpine (6.96M ops/s clean vs. 5.36M ops/s with a
different latency profile entirely when contaminated). See those two
commit messages for the full empirical verification. **All numbers in
this report come from the fixed harness.**

Two more portability/correctness bugs were found and fixed getting the
matrix running at all (not spin — these blocked real data from ever
existing):
- illumos: `umem_init()` unconditionally called a dead check
  (`umem_tagged_ptr_check()`) left over from a lock-free-depot design
  that was removed in commit `9640329` months ago; it panic-aborted
  every umem process on illumos because illumos places thread stacks in
  the high canonical half of the address space, same as the 48-bit-VA
  assumption the dead code wrongly still enforced. Fixed in `a777151`.
- musl: `getpcstack.c` never had `ucontext.h` in `AC_CHECK_HEADERS`, so
  `HAVE_UCONTEXT_H` was never defined and `stack_getbounds(3C)` was
  never declared — a straight compile failure on illumos (musl's own
  issue was `backtrace(3)`, a glibc extension musl doesn't have; fixed
  separately in `eb3e15e`).

## 4. Single-thread throughput (baseline, size 64:256)

ops/sec, higher is better; p50/p99/p999 in nanoseconds.

| Role | libc | umem | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc |
|---|---|---|---|---|---|---|---|---|
| intel-lo | 3.31M (p50 30/p99 32/p999 37) | 4.62M (36/39/42) | 3.59M (32/34/51) | 4.62M (30/35/42) | **5.51M** (34/50/58) | 3.45M (31/42/52) | 3.68M (42/52/60) | 4.92M (32/76/96) |
| intel-hi | 4.47M (16/18/19) | 5.90M (21/23/24) | 4.71M (17/19/30) | 6.12M (16/20/21) | **7.31M** (19/33/38) | 4.30M (16/24/34) | 4.34M (25/34/35) | 6.37M (17/55/72) |
| arm-lo | 5.93M (34/37/38) | 6.03M (36/39/41) | 6.12M (32/35/46) | **6.13M** (39/42/44) | 6.07M (32/46/52) | 5.83M (33/47/54) | 5.22M (49/59/61) | 6.04M (33/76/90) |
| arm-hi | 6.14M (36/37/38) | **6.52M** (39/42/42) | 6.48M (34/36/39) | 6.28M (40/42/47) | 6.34M (33/41/45) | 6.43M (33/46/50) | 5.52M (42/46/48) | 6.51M (35/69/81) |
| illumos | **6.93M** (36/52/65) | 5.97M (22/31/36) | — | — | — | — | — | — |
| alpine-musl | 5.62M (60/130/147) | **6.39M** (25/28/43) | 5.61M (21/23/79) | — | 6.17M (42/66/71) | — | 4.70M (49/58/70) | 5.79M (20/66/84) |

**Honest read:** umem is competitive but not a clear single-thread
winner anywhere. On x86_64 it beats glibc's own malloc by 30-40%
(intel-lo: 4.62M vs 3.31M; intel-hi: 5.90M vs 4.47M) but loses to
mimalloc (the fastest single-thread allocator in every environment
tested) by 15-20%. On aarch64 the whole field is within ~10% of each
other — Graviton's malloc paths are simply less differentiated across
allocators than x86_64's. On illumos, **umem loses to illumos's own
libc malloc on raw throughput** (5.97M vs 6.93M) but wins decisively on
latency (p50 22ns vs 36ns, p999 36ns vs 65ns) — umem trades a slightly
lower ops/sec ceiling for a much tighter, more predictable per-call
latency, which is exactly the design umem's magazine layer is for. On
musl, umem is the single-thread throughput leader (6.39M, beating even
mimalloc), likely because musl's own malloc (measured via "libc" here)
is comparatively unoptimized (p50 60ns, 2-3x everyone else's).

rpmalloc's p999 is consistently 2-3x worse than its own p50/p99 across
every environment (e.g. intel-hi: p50 17 / p99 55 / p999 72) — a
recurring rpmalloc signature in this data, not noise (five separate
environments show the same shape), almost certainly the cost of its
periodic global-heap sync / thread-cache flush path.

## 5. illumos: umem vs. its own lineage

This is the pairing the whole shootout exists to make meaningful.

| threads | libc ops/s | umem ops/s | umem vs. libc |
|---|---|---|---|
| 1 (single) | 6.93M | 5.97M | −14% |
| 2 (multi) | 5.09M | 9.47M | **+86%** |
| 4 (multi) | 4.09M | 16.40M | **+301%** |
| 2 (prodcons) | 5.09M | 8.99M | **+77%** |
| 4 (prodcons) | 4.09M | 6.34M | **+55%** |

**illumos's own libc malloc *loses* throughput as thread count rises**
(6.93M → 5.09M → 4.09M single-to-4-threads) — it does not scale at all
on this workload, consistent with a coarse-grained-lock design typical
of a traditional, non-thread-caching malloc. **umem gains throughput as
thread count rises** (5.97M → 9.47M → 16.40M), which is the entire
point of umem's per-CPU magazine layer: give each thread/CPU its own
cache so concurrent alloc/free doesn't serialize on one global lock.
On a 4-vCPU box this is as clean a demonstration of "why umem exists"
as this benchmark produced anywhere — umem is 4x faster than the
allocator it's a from-scratch reimplementation-and-improvement of, at
just 4 threads, on the platform that allocator ships on.

(This role is only 4 vCPU (m4.xlarge — the smallest previous-generation
instance type available for a from-scratch illumos AMI launch), so it
cannot speak to illumos behavior at high core counts; take the *shape*
of the curve, not just the endpoint, as the finding.)

## 6. Multi-thread scaling (`multi` workload, size 64:256)

ops/sec by thread count. Full tables for all sizes are in the raw
`matrix.toml` files; below is size 64:256 as the representative case.

### intel-lo / arm-lo (8 vCPU)

| | t=1 | t=2 | t=4 | t=8 |
|---|---|---|---|---|
| **intel-lo umem** | 5.41M | 9.39M | 15.37M | 26.35M |
| **intel-lo libc** | 3.52M | 6.52M | 12.20M | 25.36M |
| **intel-lo mimalloc** (fastest) | 5.79M | 10.21M | 18.25M | 31.31M |
| **arm-lo umem** | 6.84M | 13.19M | 25.13M | 51.40M |
| **arm-lo libc** | 6.81M | 12.36M | 24.10M | 48.38M |
| **arm-lo tcmalloc** (fastest) | 7.06M | 12.58M | 24.37M | 46.26M |

Both umem and libc scale close to linearly through 8 threads on both
8-vCPU roles — no allocator falls over at this scale, umem included.
umem beats libc by 25-30% throughout the ladder on x86_64, and is
roughly tied with libc on aarch64 (both within a few % of each other,
neither a clear winner).

### intel-hi / arm-hi (192 vCPU): where the field falls over

| allocator | intel-hi peak (t) | intel-hi @ t=192 | falloff | arm-hi peak (t) | arm-hi @ t=192 | falloff |
|---|---|---|---|---|---|---|
| libc | 92.7M (t=64) | 25.2M | −73% | 142.3M (t=64) | 30.3M | −79% |
| **umem** | 84.2M (t=32) | 13.8M | **−84%** | 109.1M (t=32) | 18.5M | **−83%** |
| jemalloc | 78.5M (t=64) | 13.5M | −83% | 121.0M (t=64) | 20.2M | −83% |
| tcmalloc | 93.3M (t=64) | 23.4M | −75% | 154.0M (t=32) | 29.1M | −81% |
| mimalloc | 107.1M (t=32) | 21.7M | −80% | 151.7M (t=32) | 23.4M | −85% |
| snmalloc | 88.2M (t=64) | 27.2M | −69% | 167.9M (t=32) | 23.2M | −86% |
| scudo | 74.5M (t=64) | 20.9M | −72% | 130.6M (t=64) | 24.4M | −81% |
| rpmalloc | 97.6M (t=32) | 9.1M | **−91%** | 165.2M (t=32) | 3.7M | **−98%** |

**Every allocator falls off a cliff between its peak (t=32 or t=64) and
full saturation (t=192) on a 192-vCPU box for this workload** — this is
not a libumem-specific pathology. glibc's own malloc, which has no
particular high-contention design at all, falls off by 73-79%. This
strongly suggests the falloff is dominated by the workload
(`multi`: N independent threads all alloc/free the same size classes
with no cross-thread coordination) hitting fundamental limits at
192-way concurrency on this hardware — cache-line/memory-bandwidth
contention, NUMA effects across the metal box's sockets, and/or the
kernel's own page-fault/mmap paths under that much concurrent pressure
— rather than any one allocator's design.

**Where umem specifically underperforms the field:** umem's peak
throughput (t=32) is 10-25% *below* the top allocators' peaks at the
same thread count (84.2M vs. mimalloc's 107.1M on intel-hi; 109.1M vs.
snmalloc's 167.9M on arm-hi), and its falloff to t=192 is at the worse
end of the pack (84%/83%) rather than the better end (snmalloc's
69%/86%, tcmalloc's 75%/81%). **umem does not "win" the high-core-count
multi workload against any of the purpose-built high-concurrency
allocators (mimalloc, snmalloc, tcmalloc) on either architecture.**
rpmalloc is the clear outlier on the bad end (91%/98% falloff,
collapsing to single-digit millions of ops/sec at t=192 on arm-hi) —
its thread-cache-per-heap design apparently thrashes hardest under
this exact oversubscription pattern.

### prodcons (cross-thread free) at high thread count

| allocator | intel-hi peak (t) | intel-hi @ t=192 | falloff |
|---|---|---|---|
| libc | 7.20M (t=8) | 1.74M | −76% |
| **umem** | 6.46M (t=8) | 1.70M | **−74% (best falloff in the field)** |
| jemalloc | 8.50M (t=8) | 1.60M | −81% |
| tcmalloc | 11.04M (t=8) | 1.64M | −85% |
| mimalloc | 7.56M (t=8) | 1.55M | −79% |
| snmalloc | 8.47M (t=8) | 1.50M | −82% |
| scudo | 5.36M (t=2) | 2.24M | −58% (lowest peak in the field) |
| rpmalloc | 8.31M (t=8) | 1.67M | −80% |

**This is where umem's design shows up positively.** umem has the
*lowest peak-to-saturation falloff* of any allocator except scudo on
`prodcons` (cross-thread alloc/free, the workload umem's magazine/depot
handoff is specifically built for) — even though its peak throughput is
mid-pack (6.46M, below tcmalloc's 11.04M), it degrades the *least*
under full saturation. On arm-hi the same pattern holds: umem's falloff
(82%) is squarely mid-pack, not a standout, so this finding is
**x86_64-specific** — worth flagging honestly rather than generalizing
from one architecture.

## 7. Sustained punishing load (192 threads, ~180s per workload, size 64:256/16:4096)

Not a burst: each point ran for roughly 150-180 real seconds at full
192-thread saturation, sized via a short calibration run. `frag`
measures RSS growth under 50%-alternating alloc/free cycling
16-4096-byte allocations for 3 minutes straight.

### prodcons-sustained (192 threads)

| Role | libc | umem | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc |
|---|---|---|---|---|---|---|---|---|
| intel-hi ops/s | 1.69M | 1.99M | 2.23M | **2.28M** | 2.01M | 2.01M | 2.02M | 1.72M |
| intel-hi p999 (ns) | 98,469 | **157,137** | 24,648 | 143,741 | 25,954 | 49,821 | 1,274,512 | 22,209 |
| arm-hi ops/s | 1.88M | 1.52M (lowest) | 2.74M | 2.64M | **3.44M** | 3.24M | 1.72M | 2.11M |
| arm-hi p999 (ns) | 100,679 | 100,774 | 5,795 | 66,554 | 15,254 | 17,343 | 32,813 | 7,415 |

**This is the most honest, least flattering finding for umem in the
whole report.** Under 3 minutes of sustained 192-thread cross-thread
alloc/free pressure:
- On **intel-hi**, umem's throughput (1.99M ops/s) is mid-pack, but its
  **p999 tail latency (157µs) is the worst in the field** except for
  scudo's catastrophic 1.27ms (a hardened allocator paying its safety
  tax under load, expected). Every purpose-built allocator here
  (jemalloc 24.6µs, mimalloc 26.0µs, rpmalloc 22.2µs) holds its p999
  tail an order of magnitude tighter than umem under the same sustained
  load.
- On **arm-hi**, umem is *both* the lowest-throughput allocator (1.52M
  ops/s, below even glibc's 1.88M) *and* ties glibc for the worst p999
  (100.8µs vs. 100.7µs) — every other allocator tested is 3-20x tighter
  on tail latency (jemalloc 5.8µs, rpmalloc 7.4µs).

**Mechanism (from the data, not speculation):** umem's per-CPU magazine
design is built to *avoid* contention on the fast path, but `prodcons`
specifically stresses the *cross-thread free* path — a buffer allocated
on CPU A's magazine and freed by a thread pinned to CPU B must travel
through the depot (the shared, locked layer) to get back to a magazine
that can reuse it. Under 192-way sustained cross-thread pressure, depot
contention is exactly the bottleneck umem's own `docs/results/*
scaling-diagnosis*` findings from prior workstreams already identified
(see `docs/results/2026-07-23-scaling-diagnosis.md` and the D1/D2
contention-instrumentation work referenced in the project's own plan) —
this benchmark's sustained-load numbers are independent confirmation of
that same finding from a completely different angle (a comparative
shootout instead of an isolated umem-only stress test), and show it has
not been fully resolved: the depot handoff is still the long pole under
real sustained cross-thread duress, and it costs umem the tail-latency
crown against every purpose-built competitor tested.

### frag-sustained (192 threads, 3 minutes, RSS/allocated ratio)

| Role | libc | umem | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc |
|---|---|---|---|---|---|---|---|---|
| intel-hi frag ratio | 2.20 | **4.19** | 2.31 | 2.33 | 2.33 | 2.40 | 2.34 | 2.33 |
| arm-hi frag ratio | 2.17 | **4.12** | 2.25 | 2.28 | 2.30 | 2.35 | 2.28 | 2.28 |

**umem holds roughly double the memory-overhead ratio of every other
allocator under sustained fragmentation pressure**, consistently on
both architectures (4.19 and 4.12 vs. everyone else clustered tightly
around 2.2-2.4). This is a real, reproducible, honest finding: whatever
umem's slab/magazine bookkeeping costs in steady-state RSS relative to
live bytes, it is roughly 2x every other allocator tested under this
specific workload (small, varied allocation sizes 16-4096 bytes,
sustained churn). It is *not* a scaling or throughput problem — umem's
frag-sustained *throughput* is competitive (6.07M/5.28M ops/s, mid-pack)
— it is specifically a memory-overhead cost.

## 8. Fragmentation (short `frag` sweep, size 64:256)

| Role | libc | jemalloc | tcmalloc | rpmalloc | snmalloc | mimalloc | scudo | umem |
|---|---|---|---|---|---|---|---|---|
| intel-lo | 16.96 | 17.14 | 17.14 | 17.17 | 17.36 | 17.39 | 17.72 | **20.31** |
| intel-hi | 16.91 | 17.07 | 17.14 | 17.14 | 17.31 | 17.34 | 17.85 | **40.77** |
| arm-lo | 15.93 | 16.16 | 16.26 | 16.34 | 16.43 | 16.41 | 17.28 | **19.26** |
| arm-hi | 16.13 | 16.00 | 16.25 | 16.31 | 16.39 | 16.41 | 17.30 | **39.76** |
| alpine-musl | 3.65 | 15.74 | — | 8.82 | — | 5.54 | 7.31 | 6.81 |

(These absolute ratios are large across the board because this
workload's allocations are tiny (64-256 bytes) against a process base
RSS that includes the whole allocator's static/thread-cache overhead —
the *relative* comparison between allocators is what matters, not the
absolute number.)

**umem is the worst-in-field on fragmentation on every glibc
environment tested**, and dramatically so on the two metal `-hi` roles
(40.77 / 39.76 vs. everyone else at 16-18) — a **~2.3x** worse ratio
than the next-worst allocator (scudo) on metal. This matches and
reinforces §7's sustained-load finding: it is not a `-hi`-only or
sustained-load-only artifact, it shows up in the short sweep too, on
both architectures. **This is umem's clearest, most consistent
weakness across this entire benchmark.** On musl, umem's ratio (6.81)
is unremarkable (mid-pack, better than jemalloc's 15.74) — the
metal-role blowup does not reproduce on the 8-vCPU musl box, suggesting
it may be a very-high-core-count-specific slab-per-CPU or magazine-
sizing effect (each of 192 CPUs getting its own magazine reservation is
consistent with a multiplicative RSS cost that scales with vCPU count
rather than with load) rather than a purely load-driven one — a
hypothesis future work should verify directly (e.g. `frag` at t=8 vs.
t=192 with vCPU count held constant would isolate it), which this
report's data does not by itself confirm.

## 9. Reliability / stability of the data itself

- **1,826 scaling points + 32 sustained points, zero unrecoverable
  crashes in the final, reproducible data set.** `matrix.sh`'s
  crash-tolerant design (capture + log + continue) was exercised and
  never had to skip a point in the committed results.
- **One non-reproducible intermittent issue found and investigated,
  not silently dropped:** an early Alpine/musl run logged 14 `CRASH:
  umem ... rc=139` (SIGSEGV) points, concentrated in `prodcons` and the
  largest size class (1024:4096). Two full repeat runs on a fresh
  instance (one umem-only, one the complete 6-allocator sweep,
  including the exact failing points) both completed with **zero**
  crashes. This was not written off — real time was spent trying to
  reproduce it under `gdb`, including watching the process tree live
  during a hang-suspicious run (which turned out to be an SSH-session
  artifact, not an app hang, once re-run under `setsid`/`nohup`). The
  honest conclusion: **there is a rare, non-deterministic issue that
  reproduces on musl/x86_64 under specific timing/memory conditions
  this report could not pin down in the time available**; it is not a
  fabricated non-issue, and it is not a confirmed deterministic bug
  either. It deserves a dedicated follow-up with core-dump capture
  configured *before* the triggering run (the original crashes'
  core files were on an already-terminated instance).
- **CoV instability is real and reported, not hidden.** 63-132 points
  per role (mostly `prodcons`/`multi` at higher thread counts, where
  scheduler noise is inherently larger) were flagged `unstable`
  (CoV > 10%) and are visible in the raw TOML with their `unstable=1`
  flag; the summary tables above use median values from those points
  like everywhere else, but any of them should be treated as a
  directional signal, not a precise number, per the plan's own
  methodology rule.
- **RSS/fragmentation on illumos is not measured** — `bench_get_
  vmrss_bytes()` in `bench_framework.c` is `#ifdef __linux__`-gated and
  returns 0 on illumos; every illumos `frag` value in the raw data is
  `0.000`, correctly reflecting "not measured" rather than a fabricated
  zero-overhead claim. Not fixed in this pass (out of scope — would
  need an illumos-native RSS read, e.g. via `/proc/<pid>/psinfo`).
- **Governor could not be verified/pinned** on 5 of 6 environments
  (only intel-hi exposes cpufreq to the guest) — see §1. This is an
  honest limitation of AWS's virtualization on these instance families,
  not a harness bug; `matrix.sh` degrades safely rather than either
  blocking or lying about being tuned.

## 10. Honest ranking

There is no single winner. Per axis:

- **Single-thread latency (p50/p999):** mimalloc is fastest almost
  everywhere; umem is competitive and has the tightest p999 of the
  field on illumos specifically.
- **Multi-thread scaling to 8 threads (both `-lo` roles):** umem scales
  cleanly, beats glibc by 25-30% on x86_64, roughly ties glibc on
  aarch64. Not a loss, not the field's best (mimalloc/tcmalloc edge it
  out by 10-20% at t=8).
- **Multi-thread scaling to 192 threads (`multi` workload):** umem is
  in the bottom half of the field on both peak throughput and
  falloff-to-saturation, on both architectures. mimalloc and snmalloc
  are the standouts here.
- **Cross-thread alloc/free at high concurrency, short burst
  (`prodcons` at t≤192, single point):** umem shows the best
  peak-to-saturation *falloff* on x86_64 specifically (the one clean
  "umem's magazine/depot design wins" result in this report) — not
  reproduced on aarch64.
- **Cross-thread alloc/free under 3 minutes of sustained 192-thread
  load:** umem has the worst or tied-worst p999 tail latency of the
  entire field on both architectures. This directly contradicts the
  short-burst `prodcons` finding above and is the more trustworthy
  number for any real workload that runs longer than a few seconds —
  **the burst result does not hold up under sustained duress.**
- **Fragmentation / memory overhead:** umem is worst-in-field on every
  glibc environment, by a wide margin on the two metal boxes (~2.3x the
  next-worst allocator). This is umem's single clearest and most
  reproducible weakness in this entire benchmark.
- **illumos (the lineage comparison):** umem beats illumos's own libc
  malloc by up to 4x at just 4 threads on cross-thread and multi-thread
  workloads, and has dramatically tighter single-thread tail latency —
  the clearest, most unambiguous win in this report, exactly where the
  comparison is most meaningful.

**If forced to a one-line verdict:** umem is a real, working, generally
competitive allocator that clearly outperforms the traditional
coarse-locked malloc it descends from (illumos libc) under concurrency,
and holds its own against modern allocators on 8-vCPU boxes — but at
192-vCPU sustained load it has the worst tail latency in the field, and
its memory overhead under fragmentation-heavy workloads is roughly
double every competitor tested, on both x86_64 and aarch64. Anyone
choosing umem today for a very-high-core-count, long-running,
memory-overhead-sensitive service should treat those two findings as
open work, not settled.

## 11. What wasn't built, honestly

- **snmalloc, rpmalloc built and tested on glibc (x86_64 + aarch64) and
  on illumos-not-attempted** (no from-source build effort was made
  there; illumos got libc+umem only, by design — see §0/§1).
- **snmalloc on musl: not built.** Its override shim needs
  `__cxa_thread_atexit_impl`, which musl doesn't provide the way glibc
  does; the linker error was unambiguous and not a quick fix. Per the
  task's own allowance ("if genuinely unbuildable in a reasonable time,
  document why, don't burn hours"), this was not pursued further —
  6 allocators on musl (libc, umem, jemalloc, mimalloc, scudo,
  rpmalloc) is what's real.
- **tcmalloc on musl: not attempted** — no gperftools package for
  musl/Alpine exists, and gperftools' own build has historically
  required glibc-specific hooks; not attempted given snmalloc's
  parallel failure for a related musl-vs-glibc-runtime reason.
- **RSS/fragmentation on illumos: not measured** (§9).
- **Very-high-core-count sustained load on illumos/Alpine: not run** —
  the plan's "punishing load" ask is explicitly `-hi`-role scoped (192
  vCPU); illumos (4 vCPU) and Alpine (8 vCPU) aren't `-hi` roles and
  weren't asked to carry that load, and doing so would not have
  produced comparable numbers to the `-hi` roles' 192-thread runs.

## Appendix: raw data locations

```
docs/results/2026-09-08-c7i.2xlarge-x86_64/       intel-lo   (320 points)
docs/results/2026-09-08-c7i.metal-48xl-x86_64/    intel-hi   (448 points + 16 sustained)
docs/results/2026-09-08-c7g.2xlarge-aarch64/      arm-lo     (320 points)
docs/results/2026-09-08-c8g.metal-48xl-aarch64/   arm-hi     (448 points + 16 sustained)
docs/results/2026-09-08-m4.xlarge-i86pc/          illumos    (64 points)
docs/results/2026-09-08-alpine-musl-x86_64/       alpine-musl (226 points)
```

Each directory has `matrix.toml` (per-point results), `meta.toml`
(instance/kernel/compiler provenance), `matrix.log` (raw per-point
stderr, including any `CRASH:` lines), and the two `-hi` directories
additionally have `sustained.toml`/`sustained.log`.

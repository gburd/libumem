# Allocator comparison, 2026-09-23

**Read the null control first (§2).** Every delta in this document is quoted
against the rig's own resolution, measured on the same box in the same run by
alternating libumem against a relabelled copy of itself. A delta inside that
band is noise and is reported as noise.

**Headline, stated so the two claims cannot be confused:**

1. **libumem's `LD_PRELOAD` malloc interposer at HEAD-before-fix (`f2a8267`)
   scales negatively with threads.** On `c7i.metal-48xl` (192 vCPU), `multi`
   16:64: 3.0 Mops at 1 thread, 0.8 Mops at 192 threads, while glibc does
   431 Mops. This is a ~500x gap and it is the largest performance finding in
   this project's history. Mechanism, fix (`a74065e`), and null-controlled A/B
   showing 0.79 -> 314 Mops (395x) are in §5.1.
2. **The allocator underneath is not the problem.** Through the `umem_alloc`
   API on the same box, same build, libumem does 394 Mops at 192 threads
   (glibc 431, best competitor jemalloc 517). That is -9% vs glibc and -24% vs
   the best, against a null resolution of about +/-8% at 192 threads (§2). It is
   a real but modest gap, and it is what anyone who links `-lumem` directly
   gets.

Every previous comparison in this repository measured only (2), through the
benchmark's 16-byte-header API wrapper, which is why (1) was never seen. The
2026-09-08 shootout's "holds its own against modern allocators on 8-vCPU
boxes" was true of the API path and false of the drop-in path by a factor of
2 at one thread and 20 at eight.

There is one other large, fixable gap that is in the allocator itself: the
API path collapses at 1k:4k object sizes under threads (§5.2: -78..-96%
vs best, 0.06x glibc at 64+ threads, p999 32-68 us). Its mechanism is
identified in source.

---

## 1. Provenance and method

| | |
|---|---|
| Commit measured | `f2a8267df7586db9e5e6b7dd052bda90e5cea571` via `scripts/ec2/verify-isolated.sh` (`git archive`, committed content only). Allocator sources identical to `v3.1.0`. The interposer fix `a74065e` is measured separately in §5.1. **Master moved while this ran**; every number here predates all of: `a74065e` (interposer), `3f2e67c` + `cf3f762` (slab floors: the ~5 GB and ~8 GB VMA ceilings), `147d5ff` (periodic pass reaps the depot), and **`9bbe58b` (the update thread is now started at `umem_init()`; at `f2a8267` no process in this run had one, so no periodic reclaim ever ran)**. None of those touch the alloc/free fast path, but a re-run at HEAD would have a second thread doing a reap pass per interval and a different small-object span layout; the RSS figures in §5.5 in particular are pre-`147d5ff`/`9bbe58b`. Not re-run. |
| Harness | `scripts/ec2/allocator_comparison.sh` -> `test/bench/matrix.sh`, `scripts/ec2/sustained_load.sh`, `test/bench/bench_contention`, `perf`. Analysis: `test/bench/analyze_comparison.py`. |
| Boxes | `c7i.2xlarge` (8 vCPU x86_64, Xeon 8488C), `c7g.2xlarge` (8 vCPU aarch64 Graviton3), `c7i.metal-48xl` (192 vCPU x86_64 bare metal), `c8g.metal-48xl` (192 vCPU aarch64 Graviton4 bare metal). **All four obtained**; `arm-hi` metal had capacity this time. |
| OS / toolchain | AL2023, kernel 6.12.103, gcc 11.5.0, glibc 2.34, `vm.max_map_count` 65530, THP `never`, `numa_balancing` 0, governor `performance` (metal; the 2xlarge boxes expose no governor). |
| Work per point | **Fixed TOTAL operations, identical for every arm at a point**, divided by thread count once inside `bench_main`. `ops_floor_raised = false` on **every** reported row (checked by the analyzer; a `true` would have been listed). Budgets: `single`/`multi` 20M; `multi-hi` 200M (metal, fast arms only); `prodcons` 10M (lo) / 9.6M (metal; divided over t/2 producers); `frag` 20M at 16:64 and 64:256, 8M at 256:1024 (t <= 64), 3.2M at 1k:4k (t <= 32) -- sized so the live set stays under umem's ~5 GB ceiling except in the deliberate ceiling probe (§4.4). |
| Repetition | 3 measured runs + 1 warm-up **inside** each process (bench_main median + CoV), and **2 replicate processes** per arm per point (`-R 2`), arms alternating at the innermost loop: `libc, umem, umem@null, umem-preload, umem-preload@null, jemalloc, ... , libc, umem, ...`. Reported value = median of the 2 replicates' medians. |
| Pinning | `numactl --physcpubind=0-(t-1) --localalloc`. |
| `alloc_failures` | Recorded on every row. **Zero on every row of every matrix** except the deliberate ceiling probe (§4.4). |
| Sustained | 4 windows x ~20 s per arm at the box's top thread count, interleaved A,B,C,...,A,B,C, matched work (calibrated to the slowest arm in the group). `prodcons` 64:256; `frag` 16:64 (not 16:4096: at 192 threads that live set exceeds umem's ceiling and would measure the ceiling, not fragmentation). |
| Raw data | `docs/results/2026-09-23-<instance>-<arch>/{single,multi,multi-hi,prodcons,frag,frag-mid,frag-big,ceiling}/matrix.toml` + `meta.toml`, `sustained*.toml`, `contention-*.txt`, `perf-*.txt`, `availability.toml`. Interposer A/B: `docs/results/2026-09-23-interpose-ab/`. |

### 1.1 Two libumem arms, labelled everywhere

| arm | what it measures | who gets this |
|---|---|---|
| `umem` | `umem_alloc()`/`umem_free()` through the benchmark's own 16-byte size header (`allocators.c`). The size class seen by umem is request+16. | programs that link `-lumem` and call the API (with their own size bookkeeping) |
| `umem-preload` | plain `malloc()`/`free()` with `LD_PRELOAD=libumem_malloc.so`, i.e. `malloc_interpose.c` -> `umem_malloc()` -> `_umem_alloc()`, 8-byte `malloc_data_t` header (+8 more above 16 bytes), `process_free()` validation on every free. | **drop-in users** |

The two arms run in the same process image type, same `libumem.so`, same box,
same instant. Their difference is the interposer.

### 1.2 What was NOT measured

- **Post-fix (`a74065e`) numbers for anything except the interposer A/B in
  §5.1.** The full matrix ran at `f2a8267`. The A/B was run on a separate
  identical metal box (`intel-hi@ab`) so it would not perturb the matrix.
- **Metal `multi-hi` (200M ops at 64/128/192 threads) is not reported.**
  The first attempt ran under double load (§6.1) and was discarded; the
  clean x86_64 rerun completed but was lost at instance termination
  (§4.2.1). `single` and `multi` completed before the overlap and are clean.
- **`umem-preload` at 200M ops on metal.** At 0.8 Mops a 200M-op point is
  250 s x 8 runs; its scaling is settled by the 20M-op `multi` pass and the
  A/B. Excluded from `multi-hi` deliberately.
- `scudo` on aarch64 is compiler-rt 15.0.7's `libclang_rt.scudo_standalone`
  (LD_PRELOADed, as `allocators.c` requires); nothing newer was available in
  the distro.
- The frag `1k:4k` API-arm rows on metal at t >= 64 (live set would exceed
  the ceiling).
- Debug modes, `--enable-introspect`, musl, illumos. This is the default
  build on Linux/glibc only.

---

## 2. Null control: the rig's resolution

`umem@null` and `umem-preload@null` are the **same binaries** re-labelled and
alternated with everything else at every grid point. Their delta from `umem` /
`umem-preload` is pure measurement noise: between-process placement, code
layout, which cores the pinning picked, neighbour drift. `bench_main`'s own
CoV cannot see any of this (it varies runs inside one process image), which is
why an earlier A/B in this repo produced -4..+16% "effects" from two XOR
instructions until a null control was added (`2026-09-23-p54-mangle-throughput.md`).

Per-replicate delta = (null - base) / base. n = replicate pairs.

| box | arm | workload | n | median | sd | \|delta\| p90 | range |
|---|---|---|---|---|---|---|---|
| c7i.2xlarge | umem | single | 8 | -0.25% | 2.6 | 2.4% | -7.3 .. +2.4 |
| c7i.2xlarge | umem | multi | 32 | +0.06% | 4.7 | 7.8% | -10.3 .. +12.4 |
| c7i.2xlarge | umem | prodcons | 32 | +0.78% | **11.0** | 17.4% | -31.8 .. +25.0 |
| c7i.2xlarge | umem | frag | 6 | +0.28% | 4.9 | 8.6% | -8.8 .. +3.4 |
| c7i.2xlarge | umem-preload | all | 78 | -0.33% | 6.3 | 10.0% | -18.7 .. +21.2 |
| c7g.2xlarge | umem | single | 8 | -0.16% | 0.7 | 0.6% | -1.6 .. +0.6 |
| c7g.2xlarge | umem | multi | 32 | -0.33% | 3.1 | 3.6% | -5.7 .. +12.5 |
| c7g.2xlarge | umem | prodcons | 32 | +0.22% | **47.4** | 34.6% | -68.2 .. **+243.8** |
| c7g.2xlarge | umem | frag | 5 | -0.43% | 1.7 | 2.6% | -2.6 .. +2.6 |
| c7g.2xlarge | umem-preload | all | 76 | -0.15% | 4.4 | 6.8% | -12.4 .. +19.7 |
| c7i.metal-48xl | umem | single | 8 | -0.32% | 1.5 | 1.5% | -4.0 .. +1.5 |
| c7i.metal-48xl | umem | multi (20M) | 48 | -0.12% | 8.5 | 11.9% | -27.8 .. +22.8 |
| c7i.metal-48xl | umem-preload | multi (20M) | 48 | -0.27% | 5.1 | 4.6% | -11.2 .. +26.6 |
| c8g.metal-48xl | umem | single | 8 | -1.38% | 4.3 | 5.3% | -5.3 .. +7.7 |
| c8g.metal-48xl | umem | multi (20M) | 48 | -0.02% | **13.4** | 18.0% | -50.8 .. +36.1 |
| c8g.metal-48xl | umem-preload | multi (20M) | 48 | -0.17% | 3.0 | 5.2% | -7.7 .. +7.7 |

**How to read the tables below.** Medians are all within +/-1.4% of zero, so
there is no systematic bias in the alternation. The **spread** is the
resolution, and it depends strongly on workload:

- `single`: **+/-2%** (x86_64) to **+/-5%** (Graviton4 metal). Anything under
  5% single-thread is noise.
- `multi`: **+/-8-12%** on the 20M-op points. A `multi` gap under ~10% is
  noise; the 192-thread points are 50 ms long and individually swing +/-25%.
- `prodcons`: **not resolvable on 8 vCPU** at this budget. The aarch64 null
  reached +244% because the workload is **bimodal**: identical processes land
  in either a ~4 Mops or a ~13 Mops regime (`umem` 16:64 t=2: 13.57 and 3.97
  Mops in consecutive replicates, each with CoV < 1%). This happens to umem,
  umem@null, libc, jemalloc and mimalloc alike (§4.3), so it is a property
  of the workload+scheduler, not of one allocator. Any `prodcons` claim from
  the 8-vCPU boxes would be fabricated.
- `frag`: **+/-3-9%**.

A gap is called a **finding** below only when it exceeds *both* the pooled
resolution for its workload *and* the null delta at that same grid point.

---

## 3. Allocator availability

All nine arms loaded on all four boxes. Nothing was silently dropped
(`availability.toml` and `meta.toml`'s `allocators_unavailable = []` on every
box).

| allocator | x86_64 | aarch64 | how loaded |
|---|---|---|---|
| glibc malloc (`libc`) | 2.34 | 2.34 | process libc; baseline |
| libumem `umem` (API) | `f2a8267` | `f2a8267` | linked |
| libumem `umem-preload` | `f2a8267` `libumem_malloc.so` | same | `LD_PRELOAD` |
| jemalloc | 5.2.1-7.amzn2023 | same | dlopen |
| tcmalloc (gperftools `_minimal`) | 2.9.1-1.amzn2023 | same | dlopen |
| mimalloc | git 31d034d 2026-09-16 (v3.x) | same | dlopen |
| snmalloc | git e9f7b2e 2026-09-21 | same | dlopen (`libsnmallocshim.so`) |
| scudo (compiler-rt standalone) | 15.0.7-3.amzn2023 | same | `LD_PRELOAD` |
| rpmalloc | git 5dacae8 2026-07-15 | same | dlopen |

---

## 4. Results

Throughput in Mops (alloc+free pairs per second), median of 2 replicate
processes each reporting median-of-3. **Bold** = best non-libumem arm. "null"
= that grid point's own umem-vs-umem@null delta. "umem vs best" = the API
arm's gap to the best competitor; "finding" only if beyond §2's band.

### 4.1 `single` (1 thread), all four boxes

| box | size | libc | **umem** | umem-preload | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc | umem/libc | umem vs best | null |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c7i.2xlarge | 16:64 | 5.73 | 5.80 | 2.91 | 5.72 | 5.34 | **6.10** | 5.79 | 5.27 | 5.74 | 1.01 | -5% | -3.4% |
| c7i.2xlarge | 64:256 | 5.49 | 6.26 | 2.99 | 5.40 | 6.19 | **6.80** | 5.50 | 5.13 | 6.51 | 1.14 | -8% | -0.5% |
| c7i.2xlarge | 256:1024 | 5.43 | 5.67 | 2.72 | 4.95 | 5.41 | **6.25** | 5.06 | 4.54 | 5.60 | 1.05 | -9% | -0.3% |
| c7i.2xlarge | 1k:4k | **5.15** | 5.01 | 2.52 | 4.74 | 4.87 | 5.14 | 4.79 | 4.26 | 5.02 | 0.97 | -3% | +1.2% |
| c7g.2xlarge | 16:64 | 5.95 | 6.17 | 1.99 | 6.08 | **6.18** | 5.62 | 5.63 | 5.29 | 5.97 | 1.04 | -0% | -1.1% |
| c7g.2xlarge | 64:256 | 6.00 | 6.01 | 1.97 | 5.94 | **6.20** | 5.94 | 5.66 | 5.20 | 6.14 | 1.00 | -3% | +0.0% |
| c7g.2xlarge | 256:1024 | 5.78 | 5.84 | 1.94 | 5.82 | 5.93 | **6.04** | 5.53 | 5.03 | 5.77 | 1.01 | -3% | +0.4% |
| c7g.2xlarge | 1k:4k | 4.69 | 4.64 | 1.77 | 5.12 | **5.18** | 4.98 | 5.17 | 4.51 | 4.90 | 0.99 | -10% | -0.1% |
| c7i.metal | 16:64 | 5.98 | **7.12** | 3.12 | 6.73 | 6.21 | 6.73 | 6.33 | 6.05 | 6.28 | 1.19 | **+6%** (best) | +0.6% |
| c7i.metal | 64:256 | 6.10 | 7.43 | 3.25 | 6.87 | 7.54 | **8.55** | 6.26 | 6.05 | 7.92 | 1.22 | -13% | -0.8% |
| c7i.metal | 256:1024 | 6.41 | 7.31 | 3.26 | 6.66 | 7.32 | **8.17** | 6.23 | 5.77 | 7.32 | 1.14 | -11% | -0.1% |
| c7i.metal | 1k:4k | 6.77 | 6.73 | 3.08 | 6.31 | 6.61 | **6.88** | 6.30 | 5.45 | 6.72 | 0.99 | -2% | -1.9% |
| c8g.metal | 16:64 | 6.32 | 6.33 | 2.21 | 6.57 | 5.87 | **6.58** | 6.51 | 5.54 | 6.56 | 1.00 | -4% | +0.8% |
| c8g.metal | 64:256 | 6.13 | 6.19 | 2.21 | 6.34 | 6.31 | **6.45** | 6.40 | 5.51 | 6.44 | 1.01 | -4% | -2.2% |
| c8g.metal | 256:1024 | 5.86 | 6.02 | 2.19 | 6.24 | 6.06 | **6.26** | 6.22 | 5.26 | 6.17 | 1.03 | -4% | -2.7% |
| c8g.metal | 1k:4k | 5.08 | 4.97 | 2.10 | 5.41 | 5.24 | 5.32 | **5.58** | 4.71 | 5.29 | 0.98 | -11% | +2.5% |

Reading: the API arm is at or above glibc everywhere (0.97-1.22x) and within
the field. The 8-13% gaps to mimalloc on x86_64 at 64:256 and 256:1024 exceed
the +/-2-3% single-thread resolution and are real; the 10-11% gaps at 1k:4k on
aarch64 likewise. Both are small next to the interposer arm, which is **0.47-
0.52x** of the API arm at one thread on every box (§5.1 explains the 2x).

Single-thread p999 (ns): umem 23-43 on x86_64, 38-67 on aarch64; glibc
18-57 / 38-64; mimalloc 32-58 / 46-63. No tail finding at one thread.

### 4.2 `multi` (every thread allocates and frees its own), all boxes

20M total ops per point (metal `multi-hi` at 200M ops for the fast arms at
64/128/192 threads is in §4.2.1). Mops; **bold** = best non-libumem; "umem vs
best" = API arm; a gap is marked only when it clears the multi null (§2:
+/-8 % lo x86_64, +/-4 % lo aarch64, +/-12 % metal x86_64, +/-18 % metal
aarch64) *and* the point's own null.

**8 vCPU:**

| box | size | t | libc | **umem** | umem-preload | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc | umem/libc | umem vs best | null |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c7i.2xlarge | 16:64 | 1 | 5.6 | 6.3 | 2.8 | 5.5 | 5.9 | 6.2 | 6.0 | 5.4 | 6.0 | 1.13 | **+1%** (best) | -0.3% |
| c7i.2xlarge | 16:64 | 8 | 34.0 | 31.6 | 1.5 | 32.7 | 31.2 | **36.4** | 35.3 | 27.9 | 35.4 | 0.93 | -13% | +7.5% |
| c7i.2xlarge | 64:256 | 8 | 31.1 | 30.6 | 1.4 | 31.7 | 30.9 | **35.9** | 34.9 | 26.5 | 32.9 | 0.98 | -15% | +2.2% |
| c7i.2xlarge | 256:1024 | 8 | 33.0 | 32.1 | 1.4 | 31.6 | **35.7** | 33.0 | 33.9 | 25.2 | 31.6 | 0.97 | -10% | +5.8% |
| c7i.2xlarge | 1k:4k | 1 | 5.3 | 5.3 | 2.6 | 5.2 | 6.0 | 6.0 | **6.1** | 4.6 | 6.0 | 1.00 | -13% | +0.0% |
| c7i.2xlarge | 1k:4k | 2 | 8.6 | 4.4 | 2.1 | 9.7 | 9.4 | 8.8 | 10.2 | 7.5 | **10.6** | 0.52 | **-58%** | +1.5% |
| c7i.2xlarge | 1k:4k | 8 | 23.5 | 6.0 | 1.4 | **30.9** | 30.2 | 25.7 | 28.0 | 23.4 | 29.4 | 0.26 | **-81%** | +1.3% |
| c7g.2xlarge | 16:64 | 1 | 6.8 | 7.0 | 2.0 | **7.0** | 6.9 | 6.5 | 6.6 | 6.0 | 6.8 | 1.03 | -1% | -0.0% |
| c7g.2xlarge | 16:64 | 8 | 47.2 | 50.6 | 1.5 | **52.0** | 49.6 | 49.0 | 48.9 | 41.9 | 49.7 | 1.07 | -3% | -2.0% |
| c7g.2xlarge | 64:256 | 8 | 47.1 | 48.9 | 1.5 | **50.0** | 48.2 | 49.6 | 48.3 | 42.6 | 49.6 | 1.04 | -2% | +1.7% |
| c7g.2xlarge | 256:1024 | 8 | 44.9 | 46.0 | 1.7 | 46.7 | 45.4 | **47.1** | 46.1 | 39.2 | 46.3 | 1.03 | -2% | +0.9% |
| c7g.2xlarge | 1k:4k | 1 | 4.5 | 4.7 | 1.8 | 5.7 | 5.7 | 5.5 | **5.7** | 5.0 | 5.5 | 1.03 | -18% | -0.1% |
| c7g.2xlarge | 1k:4k | 8 | 29.6 | 8.6 | 1.5 | **39.6** | 38.4 | 38.1 | 39.2 | 33.8 | 38.3 | 0.29 | **-78%** | +3.4% |

(t=2, t=4 rows in the raw data; same pattern.)

**192 vCPU metal:**

| box | size | t | libc | **umem** | umem-preload | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc | umem/libc | umem vs best | null |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c7i.metal | 16:64 | 1 | 6.2 | 6.7 | 3.0 | 7.1 | 6.6 | **8.1** | 6.7 | 6.4 | 6.5 | 1.07 | -18% | -3.0% |
| c7i.metal | 16:64 | 8 | 38.4 | 37.8 | 2.0 | 40.8 | 39.2 | **42.8** | 41.2 | 35.1 | 39.9 | 0.98 | -12% | -5.5% |
| c7i.metal | 16:64 | 32 | 128.7 | 140.7 | 1.6 | 148.4 | 137.8 | **164.3** | 147.4 | 128.2 | 142.0 | 1.09 | -14% | +9.5% |
| c7i.metal | 16:64 | 64 | 177.7 | 181.5 | 1.1 | 176.0 | 178.5 | **210.0** | 185.1 | 169.1 | 188.5 | 1.02 | -14% | +0.3% |
| c7i.metal | 16:64 | 128 | 298.5 | 277.7 | 0.9 | 296.7 | 285.4 | **348.3** | 307.8 | 261.1 | 279.4 | 0.93 | **-20%** | +8.0% |
| c7i.metal | 16:64 | 192 | 431.5 | 393.6 | **0.8** | **516.7** | 401.8 | 504.9 | 445.2 | 358.6 | 389.0 | 0.91 | **-24%** | +6.1% |
| c7i.metal | 64:256 | 192 | 414.4 | 430.8 | 0.8 | 428.7 | 425.6 | **501.4** | 450.8 | 385.6 | 430.9 | 1.04 | -14% | +0.0% |
| c7i.metal | 256:1024 | 192 | 409.4 | 431.6 | 0.8 | 409.6 | **467.6** | 452.9 | 445.3 | 372.0 | 287.6 | 1.05 | -8% | -6.8% |
| c7i.metal | 1k:4k | 8 | 28.5 | 5.8 | 1.7 | 35.5 | 34.7 | 33.1 | 38.1 | 31.6 | **38.6** | 0.20 | **-85%** | -1.0% |
| c7i.metal | 1k:4k | 64 | 146.2 | 9.0 | 1.0 | 185.1 | **204.1** | 168.0 | 199.7 | 147.3 | 192.3 | 0.06 | **-96%** | +0.1% |
| c7i.metal | 1k:4k | 192 | 325.7 | 23.0 | 0.8 | 411.2 | **458.2** | 351.8 | 392.7 | 316.5 | 338.2 | 0.07 | **-95%** | -8.0% |
| c8g.metal | 16:64 | 1 | 7.0 | 6.9 | 2.3 | 7.3 | 6.4 | **7.4** | 7.2 | 6.1 | 7.2 | 0.98 | -7% | -0.1% |
| c8g.metal | 16:64 | 8 | 51.2 | 51.7 | 1.5 | **54.0** | 49.5 | 53.9 | 53.9 | 43.6 | 52.6 | 1.01 | -4% | +1.4% |
| c8g.metal | 16:64 | 64 | 287.0 | 341.8 | 1.6 | 295.0 | 274.9 | 360.1 | 392.9 | 316.4 | **406.6** | 1.19 | -16% | +18.3% |
| c8g.metal | 16:64 | 192 | 466.1 | 487.5 | **1.2** | 458.4 | 455.4 | **504.9** | 485.9 | 388.5 | 408.4 | 1.05 | -3% | -2.0% |
| c8g.metal | 64:256 | 192 | 472.9 | 449.4 | 1.2 | 423.5 | 449.8 | 477.8 | **509.4** | 374.3 | 421.2 | 0.95 | -12% | -2.7% |
| c8g.metal | 256:1024 | 192 | 467.3 | 405.9 | 1.2 | 409.1 | 435.4 | 458.7 | **487.0** | 358.3 | 388.4 | 0.87 | -17% | +11.1% |
| c8g.metal | 1k:4k | 8 | 32.6 | 9.3 | 1.7 | 41.1 | 38.2 | 40.7 | **42.4** | 35.0 | 39.8 | 0.28 | **-78%** | -4.7% |
| c8g.metal | 1k:4k | 64 | 215.4 | 14.9 | 1.7 | 282.1 | 256.3 | **309.0** | 244.6 | 252.7 | 305.1 | 0.07 | **-95%** | +0.0% |
| c8g.metal | 1k:4k | 192 | 337.4 | 33.7 | 1.1 | 403.2 | 384.3 | 368.9 | **411.8** | 319.5 | 294.9 | 0.10 | **-92%** | -11.0% |

Reading, size range by size range:

- **16 B - 1 KB, API arm: within noise of glibc on every box** (0.87-1.43x,
  medians ~1.03), and **inside the null on aarch64 metal** at every thread
  count. On x86_64 metal it trails the best allocator by 8-24 % with the
  same sign at every thread count; only the t=128/192 16:64 points clear
  the +/-12 % band on their own (§5.4).
- **1k:4k, API arm: collapses at t >= 2 on every box** to 0.06-0.29x glibc,
  identically in the umem@null arm. §5.2.
- **`umem-preload` at `f2a8267`: negative scaling everywhere**, 2.8-3.3
  Mops at 1 thread down to 0.8-1.2 at 192, on all four boxes. §5.1.

The p999 columns (raw data) tell the same story: API arm 24-40 ns at 1
thread on par with the field; 130-640 ns at 8-192 threads for sizes under
1 KB (jemalloc 30-80, mimalloc 45-90, glibc 18-450); **10-68 us at 1k:4k
under threads** where everyone else is 40-900 ns.

#### 4.2.1 `multi-hi`: 200M ops at 64/128/192 threads -- NOT REPORTED

Two attempts, neither usable, stated plainly:

1. The first pass on both metal boxes ran concurrently with a job that
   `job.sh kill` had failed to stop (§6.1) and was discarded.
2. The clean rerun completed on `c7i.metal-48xl` (216 points, `rc=0`,
   23:33-23:57Z) but its output directory was not fetched before the
   instance was terminated on the coordinator's cost deadline -- the fetch
   used the wrong path pattern and returned nothing, and the box was gone
   before that was noticed. On `c8g.metal-48xl` the rerun was cancelled at
   the same deadline. Job logs (the sweep's stdout, not the TOML) are in
   `docs/results/jobs/intel-hi-perf-multihi/`.

The 20M-op `multi` pass in §4.2 covers the same thread counts at +/-12 %
(x86_64) / +/-18 % (aarch64) resolution; `multi-hi` would have tightened
that to ~+/-5 %. It is the one planned measurement this run did not deliver.
P8.4 (the 8-24 % x86_64 gap at 128-192 threads) is the finding that would
have benefited; it is filed as a diagnosis task partly for this reason.

### 4.3 `prodcons` (half the threads allocate, half free)

**8-vCPU boxes: not resolvable, and reported as such.** The umem-vs-umem@null
delta on `prodcons` is sd 11% (x86_64) and **sd 47%, range -68..+244%**
(aarch64). The cause is visible in the per-replicate data: identical
processes land in one of two regimes. On `c7g.2xlarge`, `umem` 16:64 t=2 ran
13.57 Mops in replicate 1 and 3.97 in replicate 2, each with CoV < 1% across
its 3 internal runs; `umem` 256:1024 t=2: 3.82 then 12.27; `umem@null`
256:1024 t=8: 9.94 then 15.56. The same bimodality hits libc (16:64 t=8:
13.04 / 8.21), jemalloc (256:1024 t=8: 10.35 / 15.63), mimalloc (7.71 /
12.60). With 2 replicates per arm a point's median is a coin flip between
regimes. **No `prodcons` conclusion is drawn from the 8-vCPU boxes.** A
future run wanting one needs >= 6 replicates per arm and should report the
regime split, not a median.

What can be said: the umem API arm's p999 on `prodcons` is consistently
**2-10x lower than glibc's** at t >= 2 on both lo boxes (e.g. c7g 64:256 t=2:
umem 565 ns vs glibc 6,669; t=8: 740 vs 5,321) and in the same tier as
jemalloc/mimalloc -- this is the cross-thread handoff path the depot exists
for, and it is doing its job. `umem-preload` p999 is 10-30x umem's on the
same points (the §5.1 lock).

**Metal (192 vCPU), 9.6M ops per point divided over t/2 producers, 2
replicates x median-of-2 runs.** Null (umem vs umem@null) on this workload:
x86_64 n=48 median -0.1 %, **sd 11.9 %, |delta| p90 17.5 %**, range
-28..+36 %; aarch64 sd 14.9 %, range -28..+52 %. Bimodality was rarer than
on 8 vCPU (flagged points: 9 of 48 per box, spread across allocators) but
the resolution is still +/-15-20 %, so only the largest gaps below mean
anything.

Throughput (Mops); "umem vs best" bold only where it exceeds the point's
null and 20 %:

| box | size | t | libc | **umem** | umem-preload | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc | umem/libc | umem vs best | null |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c7i.metal | 16:64 | 8 | 11.4 | 12.5 | 3.9 | 11.7 | 8.7 | 12.0 | **14.2** | 6.2 | 13.9 | 1.10 | -12% | -7.2% |
| c7i.metal | 16:64 | 64 | 4.6 | 4.2 | 1.9 | 4.7 | 4.1 | 4.9 | **5.0** | 2.7 | 4.8 | 0.91 | -16% | +1.7% |
| c7i.metal | 16:64 | 192 | 2.1 | 1.8 | 1.5 | 1.7 | 2.1 | **2.8** | 2.4 | 2.6 | 2.3 | 0.87 | **-35%** | -3.2% |
| c7i.metal | 64:256 | 8 | 7.0 | 10.9 | 3.6 | 12.3 | 11.1 | 13.7 | 13.2 | 5.9 | **14.3** | 1.55 | **-24%** | +2.5% |
| c7i.metal | 64:256 | 64 | 4.4 | 4.1 | 1.9 | 4.8 | 4.4 | 4.5 | **5.0** | 2.8 | 4.7 | 0.93 | -18% | +6.6% |
| c7i.metal | 64:256 | 192 | 1.7 | 1.7 | 1.4 | 1.8 | 1.9 | 2.2 | 2.0 | **2.5** | 2.3 | 0.99 | **-31%** | -6.6% |
| c7i.metal | 256:1024 | 8 | 5.2 | 11.1 | 4.1 | 10.9 | 10.2 | 10.2 | **13.7** | 4.5 | 10.0 | 2.15 | -19% | +4.1% |
| c7i.metal | 256:1024 | 192 | 1.6 | 1.7 | 1.5 | 1.7 | 1.7 | 1.8 | 1.9 | 1.4 | **2.0** | 1.05 | -17% | +0.0% |
| c7i.metal | 1k:4k | 8 | 5.1 | 10.1 | 4.0 | 10.7 | 9.9 | 11.3 | 11.7 | 4.1 | **13.3** | 1.97 | **-24%** | +0.0% |
| c7i.metal | 1k:4k | 192 | 1.6 | 1.7 | 1.6 | 1.8 | **2.1** | 1.9 | 1.8 | 1.6 | 1.8 | 1.03 | -20% | -2.2% |
| c8g.metal | 16:64 | 8 | 12.9 | 9.4 | 3.7 | 12.7 | 12.3 | 13.3 | 10.6 | 8.3 | **13.4** | 0.73 | **-30%** | -9.8% |
| c8g.metal | 16:64 | 32 | 17.5 | 16.8 | 3.3 | 17.9 | 13.3 | 17.8 | **19.8** | 7.4 | 17.6 | 0.96 | -15% | -0.7% |
| c8g.metal | 16:64 | 192 | 2.4 | 2.7 | 1.8 | 2.9 | 2.0 | **3.3** | 2.5 | 1.9 | 2.5 | 1.15 | -16% | -9.2% |
| c8g.metal | 64:256 | 8 | 11.2 | 11.4 | 3.7 | 15.5 | **16.6** | 11.4 | 9.7 | 8.2 | 8.1 | 1.02 | **-31%** | -8.8% |
| c8g.metal | 64:256 | 128 | 6.1 | 4.4 | 2.1 | 5.1 | 4.4 | 3.5 | 4.0 | 2.4 | 3.8 | 0.73 | **-27%** vs libc | -3.5% |
| c8g.metal | 256:1024 | 8 | 10.6 | 10.6 | 3.5 | 8.8 | **16.6** | 10.6 | 11.8 | 5.7 | 12.2 | 1.00 | **-36%** | -3.3% |
| c8g.metal | 1k:4k | 8 | 12.1 | 10.3 | 3.8 | 12.0 | **16.2** | 13.1 | 12.4 | 5.1 | 11.4 | 0.86 | **-36%** | -11.0% |

(All 24 thread/size points per box are in the raw data; rows shown are the
ones where a gap clears the band or where all allocators converge.)

Reading: `prodcons` is a workload the ring buffer, not the allocator,
dominates above ~32 threads -- every allocator converges to 1.4-2.8 Mops at
192 threads on both boxes, glibc included. umem is inside the band at most
points; the points that clear it are -24..-36 % behind the leader (usually
rpmalloc, snmalloc or tcmalloc, never the same one twice) at t=8, where umem
is also 1.5-2.2x **ahead of glibc**. There is no single mechanism to name
here; this is the depot's cross-CPU handoff cost against allocators whose
remote free is a lock-free push to the owning thread's list.

**p999 (ns), `prodcons`, metal.** The API arm's tail is 2-3x better than
glibc's at 8-64 threads (x86_64: 3.3-5.8 us vs 1.6-28.9 us) and in the
jemalloc/mimalloc tier there; at 128-192 threads it is 38-210 us against
jemalloc's 14-27 us, mimalloc's 17-35 us and rpmalloc's 9-19 us -- the
"tens-of-microseconds tier" the README said umem did not reach is still
not reached at 192 threads on `prodcons`. The interposer arm's p999 is
**1.1-1.4 ms** at 128-192 threads on x86_64 (the §5.1 lock convoy, exactly
as predicted), 15-50x the API arm's.

### 4.4 `frag` (grow a live pool, free ~50% at random, repeat) and the ceiling probe

Throughput, Mops, 8-vCPU boxes (full tables incl. the fragmentation pair are
in `analyze_comparison.py`'s output and the raw TOML; the pair is in §5.5).
Budgets: 20M ops at 16:64 and 64:256, 8M at 256:1024, 3.2M at 1k:4k, so the
live set is 0.2-2 GB and never near umem's ceiling.

| box | size | t | libc | **umem** | umem-preload | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc | umem/libc | umem vs best | null |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c7i.2xlarge | 16:64 | 1 | 3.7 | 4.2 | 2.9 | 5.3 | 5.0 | **6.1** | 5.9 | 3.9 | 6.0 | 1.16 | **-30%** | +0.3% |
| c7i.2xlarge | 16:64 | 8 | 18.6 | 18.9 | 2.5 | **24.9** | 20.1 | 23.7 | 24.2 | 19.2 | 23.7 | 1.02 | **-24%** | +0.2% |
| c7i.2xlarge | 64:256 | 1 | 2.4 | 3.9 | 2.7 | 5.1 | 4.5 | 5.3 | **5.7** | 3.4 | 5.1 | 1.60 | **-32%** | -1.0% |
| c7i.2xlarge | 64:256 | 8 | 14.7 | 17.6 | 2.7 | **23.8** | 18.5 | 21.9 | 19.5 | 16.5 | 19.6 | 1.20 | **-26%** | +3.5% |
| c7i.2xlarge | 256:1024 | 1 | 1.9 | 3.4 | 2.5 | 3.8 | 3.7 | 4.4 | **4.9** | 2.6 | 4.2 | 1.75 | **-31%** | -0.7% |
| c7i.2xlarge | 256:1024 | 8 | 10.9 | 15.8 | 2.4 | 19.6 | 17.7 | 19.3 | **20.3** | 11.3 | 17.1 | 1.46 | **-22%** | +5.2% |
| c7i.2xlarge | 1k:4k | 1 | 1.2 | 2.1 | 1.7 | 1.6 | 2.3 | 1.6 | **2.4** | 1.7 | 2.1 | 1.72 | -12% | +2.1% |
| c7i.2xlarge | 1k:4k | 8 | 5.5 | 11.1 | 2.4 | 9.2 | 10.7 | **12.9** | 11.8 | 8.9 | 10.1 | 2.01 | -14% | -0.1% |
| c7g.2xlarge | 16:64 | 1 | 4.4 | 4.2 | 2.4 | **6.0** | 5.6 | 5.4 | 5.7 | 3.9 | 5.5 | 0.95 | **-30%** | -0.3% |
| c7g.2xlarge | 16:64 | 8 | 22.5 | 22.5 | 2.8 | 24.7 | **25.4** | 22.3 | 22.0 | 21.0 | 20.7 | 1.00 | -11% | +3.7% |
| c7g.2xlarge | 64:256 | 1 | 2.8 | 3.7 | 2.2 | **5.5** | 4.4 | 4.9 | 5.0 | 3.3 | 4.6 | 1.30 | **-33%** | -0.6% |
| c7g.2xlarge | 64:256 | 8 | 14.1 | 21.7 | 2.7 | 20.2 | 19.7 | **27.0** | 25.8 | 17.9 | 20.5 | 1.54 | **-20%** | -7.4% |
| c7g.2xlarge | 256:1024 | 1 | 2.0 | 3.3 | 2.1 | **4.5** | 3.1 | 3.8 | 4.2 | 2.4 | 3.6 | 1.62 | **-26%** | +0.1% |
| c7g.2xlarge | 256:1024 | 8 | 12.7 | 19.9 | 2.7 | **21.5** | 17.3 | 21.2 | 21.4 | 11.7 | 19.6 | 1.56 | -8% | +0.6% |
| c7g.2xlarge | 1k:4k | 1 | 1.4 | 2.3 | 1.6 | 2.6 | 2.3 | 1.5 | **2.7** | 1.9 | 2.1 | 1.69 | -15% | -0.6% |
| c7g.2xlarge | 1k:4k | 8 | 6.5 | 14.8 | 2.7 | 14.5 | 13.1 | 15.6 | **16.6** | 9.8 | 12.1 | 2.28 | -11% | +1.1% |

(t=2 and t=4 rows omitted here; same pattern, in the raw data.)

**Metal** (frag null: x86_64 sd 10.6 %, aarch64 sd 7.7 %):

| box | size | t | libc | **umem** | umem-preload | jemalloc | tcmalloc | mimalloc | snmalloc | scudo | rpmalloc | umem/libc | umem vs best | null |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c7i.metal | 16:64 | 1 | 4.3 | 4.2 | 3.0 | 6.3 | 5.4 | **6.6** | 6.3 | 3.7 | 6.6 | 0.97 | **-37%** | -0.1% |
| c7i.metal | 16:64 | 8 | 16.5 | 13.3 | 3.0 | **20.8** | 16.8 | 13.2 | 15.3 | 16.7 | 17.4 | 0.81 | **-36%** | +16.0% |
| c7i.metal | 16:64 | 32 | 16.4 | 17.2 | 2.9 | 15.7 | **18.1** | 17.8 | 16.2 | 16.1 | 15.5 | 1.05 | -5% | +3.2% |
| c7i.metal | 16:64 | 192 | 15.1 | 15.6 | 1.5 | 14.9 | 15.7 | 15.3 | 15.5 | 15.1 | **15.7** | 1.04 | -1% | +0.1% |
| c7i.metal | 64:256 | 1 | 2.5 | 3.6 | 2.6 | **6.2** | 4.1 | 4.5 | 4.8 | 3.1 | 4.3 | 1.41 | **-42%** | +0.3% |
| c7i.metal | 64:256 | 8 | 12.8 | 15.0 | 3.1 | 15.1 | 14.3 | **17.8** | 14.9 | 15.0 | 16.8 | 1.17 | -16% | -2.1% |
| c7i.metal | 64:256 | 192 | 14.8 | 15.4 | 1.4 | 15.6 | **16.1** | 15.8 | 14.9 | 15.3 | 15.7 | 1.04 | -4% | +7.8% |
| c7i.metal | 256:1024 | 1 | 2.3 | 3.1 | 2.4 | **4.2** | 3.2 | 3.6 | 3.9 | 2.3 | 3.2 | 1.36 | **-27%** | -0.1% |
| c7i.metal | 256:1024 | 64 | 14.0 | 14.0 | 1.8 | 14.3 | 13.4 | 14.6 | **14.8** | 7.3 | 13.4 | 1.00 | -5% | +2.2% |
| c7i.metal | 1k:4k | 8 | 6.0 | 10.8 | 3.1 | 10.1 | **10.9** | 10.4 | 10.5 | 9.4 | 10.2 | 1.80 | -1% | -2.2% |
| c8g.metal | 16:64 | 1 | 4.9 | 4.4 | 2.7 | **6.3** | 5.5 | 5.8 | 6.0 | 4.1 | 5.7 | 0.89 | **-30%** | +0.1% |
| c8g.metal | 16:64 | 8 | 21.9 | 24.1 | 3.0 | 23.3 | **25.3** | 25.3 | 23.1 | 23.5 | 24.7 | 1.10 | -5% | -4.0% |
| c8g.metal | 16:64 | 192 | 21.6 | 22.5 | 2.1 | **23.4** | 22.9 | 22.2 | 22.5 | 22.3 | 22.7 | 1.04 | -4% | +0.2% |
| c8g.metal | 64:256 | 1 | 3.2 | 4.0 | 2.5 | **5.6** | 4.5 | 4.9 | 5.0 | 3.6 | 4.6 | 1.26 | **-29%** | +0.0% |
| c8g.metal | 64:256 | 8 | 15.6 | 20.9 | 3.1 | 24.1 | 20.8 | 22.4 | **24.3** | 21.1 | 23.8 | 1.34 | -14% | +9.5% |
| c8g.metal | 256:1024 | 1 | 2.4 | 3.7 | 2.4 | **4.5** | 3.4 | 4.0 | 4.3 | 2.8 | 3.7 | 1.55 | -19% | +0.0% |
| c8g.metal | 1k:4k | 8 | 7.3 | 15.8 | 3.0 | 15.1 | 14.1 | 16.3 | **17.3** | 11.3 | 13.7 | 2.17 | -9% | +0.4% |

At t >= 32 on metal every allocator converges to 14-25 Mops (the workload's
`memset` of every allocated buffer and the per-thread RNG saturate before the
allocator does), so the frag signal is at t=1..8, where umem is **27-42 %
behind the leader at 16..1024 B** with a t=1 null of +/-0.3 %. The lo-box and
metal figures agree. umem's frag p999 at t=1..8 is again best-in-tier (330-
870 ns); at 128-192 threads on x86_64 all allocators except snmalloc/jemalloc
are at 20-31 us, umem included.

Fragmentation pair on metal is the same as on the lo boxes to within 2 %
(16:64: umem 2.65x, glibc 1.85x, jemalloc 1.66x; 64:256: 1.57 / 1.25 / 1.20;
256:1024: 1.33 / 1.08 / 1.14; 1k:4k: 1.17 / 1.04 / 1.15). At 128-192 threads
the live set desynchronises and every allocator's ratio rises (umem 3.3-3.8x
at 16:64, glibc 2.1-2.2x, jemalloc 2.0-2.3x); `rss_at_live_peak` and `VmHWM`
still agree within 12 %, so the RSS is real, the denominator moved (P2.2's
documented artefact).

Reading: on this workload umem beats glibc clearly (1.2-2.3x at 64 B and up;
glibc's `free()` consolidation is the slow path here, its p999 is 10-16 us)
but trails the modern size-class allocators by a consistent **20-33% at
16..1024 B**, at every thread count including t=1, on both architectures,
against a frag null of +/-3-9%. That is a finding (§5.7). At 1k:4k the gap
is 8-15%, inside or at the edge of the band.

umem's frag **p999 is the best or second-best in the field** at every point
(400-1,700 ns vs glibc 1-16 us, tcmalloc 3-20 us, jemalloc 0.9-10 us); only
snmalloc and mimalloc match it. The tail is not where the frag gap is.

The ceiling probe is in §5.6.

### 4.5 Sustained (matched work, per-window, interleaved)

8-vCPU boxes, 8 threads, 4 windows x ~20 s per arm, all 11 arms interleaved,
`prodcons` 64:256 and `frag` 16:64. Matched work: every arm ran the same
total ops per window (56.7M / 45.4M on x86_64; 62.6M / 46.5M on aarch64).
`ops_floor_raised` false and `alloc_failures` 0 on all 88 windows per box.
p999 is the median over the 4 windows, with the window min..max.

| box | workload | arm | Mops | p50 ns | p99 ns | **p999 ns** (min..max over windows) | rss@peak MB | live MB | frag |
|---|---|---|---|---|---|---|---|---|---|
| c7i.2xlarge | prodcons | libc | 6.84 | 275 | 6,373 | 11,570 (11,340..12,164) | 14 | -- | -- |
| | | **umem** | 8.83 | 73 | 266 | **946** (861..989) | 23 | -- | -- |
| | | umem@null | 9.72 | 86 | 268 | 904 (862..925) | 22 | -- | -- |
| | | umem-preload | 2.95 | 348 | 11,386 | 18,844 (17,733..19,865) | 23 | -- | -- |
| | | jemalloc | 9.91 | 121 | 768 | 1,464 (1,419..1,602) | 13 | -- | -- |
| | | tcmalloc | 10.60 | 106 | 2,996 | 5,252 (4,925..5,849) | 13 | -- | -- |
| | | mimalloc | 13.33 | 104 | 330 | 7,947 (7,900..8,321) | 14 | -- | -- |
| | | snmalloc | 12.53 | 42 | 782 | 5,750 (5,072..5,970) | 14 | -- | -- |
| | | scudo | 5.46 | 371 | 3,997 | 13,198 (11,409..13,712) | 13 | -- | -- |
| | | rpmalloc | 10.37 | 127 | 338 | **468** (448..488) | 13 | -- | -- |
| c7i.2xlarge | frag 16:64 | libc | 19.31 | 64 | 1,174 | 2,222 | 414 | 222 | 1.86 |
| | | **umem** | 8.67 | 156 | 8,748 | **22,157** (22,021..22,634) | 510 | 237 | 2.15 |
| | | umem@null | 8.68 | 156 | 8,506 | 21,538 | 510 | 237 | 2.15 |
| | | umem-preload | 2.21 | 1,210 | 21,649 | 35,660 | 511 | 237 | 2.16 |
| | | jemalloc | 21.15 | 42 | 416 | 1,292 | 381 | 228 | 1.67 |
| | | tcmalloc | 19.28 | 43 | 1,732 | 8,014 | 372 | 226 | 1.65 |
| | | mimalloc | 22.49 | 42 | 316 | 954 | 383 | 229 | 1.65 |
| | | snmalloc | 21.64 | 38 | 92 | **379** | 374 | 226 | 1.66 |
| | | scudo | 19.61 | 126 | 692 | 3,019 | 559 | 237 | 2.36 |
| | | rpmalloc | 21.71 | 40 | 283 | 1,144 | 383 | 230 | 1.64 |
| c7g.2xlarge | prodcons | libc | 11.63 | 202 | 4,322 | 8,514 | 13 | -- | -- |
| | | **umem** | 19.54 | 48 | 126 | **838** (837..875) | 21 | -- | -- |
| | | umem@null | 20.03 | 48 | 123 | 920 | 21 | -- | -- |
| | | umem-preload | 3.25 | 362 | 9,002 | 14,874 | 21 | -- | -- |
| | | jemalloc | 16.34 | 104 | 430 | 1,028 | 13 | -- | -- |
| | | mimalloc | 21.49 | 84 | 190 | 2,526 | 13 | -- | -- |
| | | snmalloc | 22.18 | 35 | 342 | 3,048 | 12 | -- | -- |
| | | rpmalloc | 20.62 | 98 | 204 | **240** (236..250) | 13 | -- | -- |
| c7g.2xlarge | frag 16:64 | libc | 21.49 | 47 | 704 | 1,610 | 451 | 246 | 1.83 |
| | | **umem** | 7.69 | 126 | 10,819 | **22,282** (21,716..23,603) | 527 | 245 | 2.15 |
| | | umem@null | 7.98 | 124 | 11,282 | 22,640 | 526 | 245 | 2.15 |
| | | jemalloc | 22.53 | 33 | 439 | 1,022 | 406 | 247 | 1.65 |
| | | mimalloc | 19.10 | 32 | 187 | 709 | 402 | 246 | 1.63 |
| | | snmalloc | 25.18 | 32 | 46 | **296** | 402 | 246 | 1.64 |
| | | rpmalloc | 23.70 | 34 | 178 | 762 | 401 | 246 | 1.63 |

(tcmalloc/scudo aarch64 rows and the preload@null rows are in the TOML; they
add nothing.)

Two things stand out and both survive the null (umem vs umem@null agree to
within 10% on every row):

- **Sustained `prodcons` at 8 threads: umem's p999 is 0.9 us, second only to
  rpmalloc (0.24-0.47 us), and 5-13x better than glibc/scudo/mimalloc.** This
  is the cross-CPU handoff path (depot) and it is the healthiest number
  libumem has. The 2026-09-09 depot trylock fix is what made it so, and it
  holds at HEAD (§5.8 has the 192-thread comparison).
- **Sustained `frag` 16:64 at 8 threads: umem is the slowest allocator in
  the field, by 2.2-3x on throughput (8.7 Mops vs 19-22) and 10-60x on p999
  (22 us vs 0.3-2.2 us).** umem@null reproduces it exactly. This is the
  same mechanism as the matrix `frag` gap (§5.7) but ~3x larger because the
  sustained variant's live pool is 11M objects per thread instead of 5M
  (budget/4), so a larger fraction of allocations miss every cache layer and
  reach the slab under `cache_lock`. The 8-vCPU contention dump for `frag`
  16:64 t=8 (`contention-umem-frag-t8-16_64.txt`) shows the signature:
  `dep_conten` (depot trylock failures) 45,736-58,261 per size class against
  `dep_local` ~41,000 -- **more than half of all depot attempts fail the
  trylock** -- plus `cc_alloc` 500-1100 (magazine-layer allocations that
  bypassed the PTC).

**Metal (192 threads), 4 windows x ~20 s, matched work (38.4M `prodcons`
/ 36.5M `frag` ops per window, every arm), two groups: A = the nine fast
arms interleaved; B = `umem-preload` vs its null control, separately
(calibrating A's budget to the 500x-slower interposer would have given A
60 ms windows). Throughput is not comparable across groups; p999 is.
`ops_floor_raised` false, `alloc_failures` 0 on all 72 + 16 windows per box.**

| box | workload | arm | Mops | elapsed s | p50 | p99 | **p999 ns** (min..max) | rss@peak MB | frag |
|---|---|---|---|---|---|---|---|---|---|
| c7i.metal | prodcons 64:256 | libc | 1.56 | 24.6 | 746 | 23,920 | 82,786 (79k..93k) | 27 | -- |
| | | **umem** | 1.95 | 19.8 | 490 | 62,236 | **273,189** (256k..325k) | 161 | -- |
| | | umem@null | 1.87 | 20.6 | 586 | 65,545 | 240,454 (212k..285k) | 164 | -- |
| | | jemalloc | 1.99 | 19.4 | 188 | 9,524 | 20,652 | 39 | -- |
| | | tcmalloc | 1.73 | 22.3 | 252 | 29,516 | 77,675 | 18 | -- |
| | | mimalloc | 1.93 | 19.9 | 190 | 8,576 | 23,567 | 34 | -- |
| | | snmalloc | 1.87 | 20.6 | 31 | 7,238 | 53,414 | 51 | -- |
| | | scudo | 2.21 | 17.4 | 1,073 | 617,406 | 1,184,402 | 16 | -- |
| | | rpmalloc | 1.79 | 21.4 | 215 | 5,922 | **18,666** | 41 | -- |
| | | umem-preload (group B) | 1.20 | 32 | 1,340 | 226k | 1.1-2.7 ms | 165 | -- |
| c7i.metal | frag 16:64 | libc | 15.41 | 2.4 | 44 | 15,220 | 225,795 | 355 | 2.20 |
| | | **umem** | **1.98** | **18.4** | 214 | 2,285,288 | **6,005,597** (5.7..6.0 ms) | 423 | 2.29 |
| | | umem@null | 2.01 | 18.1 | 182 | 2,276,332 | 5,943,793 | 421 | 2.30 |
| | | jemalloc | 15.49 | 2.4 | 28 | 11,683 | 24,746 | 335 | 2.23 |
| | | tcmalloc | 15.79 | 2.3 | 43 | 17,754 | 687,192 | 314 | 1.80 |
| | | mimalloc | 16.10 | 2.3 | 27 | 11,723 | 30,730 | 322 | 1.95 |
| | | snmalloc | 15.58 | 2.3 | 24 | 12,010 | 29,324 | 329 | 2.05 |
| | | scudo | 14.42 | 2.5 | 108 | 32,302 | 394,147 | 425 | 2.37 |
| | | rpmalloc | 15.48 | 2.4 | 26 | 9,650 | 25,702 | 309 | 2.21 |
| | | umem-preload (group B) | 0.77 | 47 | -- | 4.0 ms | 9.8 ms | 424 | 2.3 |
| c8g.metal | prodcons 64:256 | libc | 2.09 | 18.5 | 233 | 7,514 | 30,844 (11k..53k) | 28 | -- |
| | | **umem** | 1.41 | 27.2 | 741 | 27,997 | **113,000** (83k..398k) | 160 | -- |
| | | umem@null | 1.43 | 26.9 | 708 | 27,234 | 404,140 (94k..467k) | 159 | -- |
| | | jemalloc | 2.70 | 14.2 | 156 | 1,176 | 5,212 | 55 | -- |
| | | mimalloc | 2.22 | 17.3 | 112 | 1,604 | 9,332 | 44 | -- |
| | | snmalloc | 2.95 | 13.0 | 35 | 1,458 | 13,102 | 56 | -- |
| | | rpmalloc | 2.32 | 16.5 | 143 | 1,086 | **4,833** | 45 | -- |
| c8g.metal | frag 16:64 | libc | 23.01 | 1.6 | 48 | 624 | 3,206 | 339 | 2.37 |
| | | **umem** | **1.17** | **31.2** | 162 | 3,947,146 | **9,874,842** (9.5..9.9 ms) | 423 | 2.28 |
| | | umem@null | 1.17 | 31.3 | 178 | 3,951,588 | 9,750,920 | 421 | 2.28 |
| | | jemalloc | 22.91 | 1.6 | 34 | 366 | 900 | 318 | 2.30 |
| | | mimalloc | 22.43 | 1.6 | 33 | 158 | 1,831 | 315 | 2.01 |
| | | snmalloc | 23.65 | 1.5 | 33 | 41 | **283** | 317 | 1.90 |
| | | rpmalloc | 22.63 | 1.6 | 36 | 168 | 1,759 | 309 | 2.09 |

Three results, each reproduced by umem@null to within 10 %:

1. **Sustained `frag` 16:64 at 192 threads is umem's worst point in the
   entire comparison: 2.0 Mops vs 15-16 for every other allocator (8x) on
   x86_64, 1.2 vs 22-24 (19x) on aarch64, with p999 of 6 ms and 10 ms
   against 0.3 us-0.7 ms for the field.** The window ran 18-31 s where every
   competitor finished in 1.6-2.4 s. The 192-thread `perf` capture of this
   point (`perf-umem-frag-t192-64_256.flat.txt`) puts **10 % of all cycles
   in `pthread_mutex_trylock` + 3 % in `pthread_mutex_unlock`** with
   `umem_depot_alloc` and `umem_depot_pop_trylock` as the callers, and the
   contention dump (`contention-umem-frag-t192-64_256.txt`) shows, per
   size class, `dep_remote` 4,800-13,900 against `dep_local` 2,000-5,000
   and `dep_conten` 2,000-6,100: **2-3 of every 4 depot reloads steal
   from another CPU's stripe, and roughly a third of trylocks fail.** With
   192 threads each holding a 5M-object live set and freeing half at
   random, freed objects land on the freeing CPU's stripe and are wanted
   by whichever CPU allocates next; the depot is a 192-way all-to-all
   exchange through per-stripe mutexes. glibc is 8x faster here because
   its `free()` returns to a per-thread tcache and then an arena bin with
   no cross-CPU stripe; jemalloc/mimalloc return to the owning thread's
   page. This is the metal face of §5.7 / P8.5 and it is the strongest
   evidence for that task.

2. **Sustained `prodcons` p999 at 192 threads: umem 240-273 us on x86_64,
   113-404 us on aarch64, against jemalloc/mimalloc/rpmalloc at 5-25 us.**
   This is the point the 2026-09-09 depot trylock fix was measured on
   (157 -> 84-93 us, whole-run, budget defect present). Per-window and
   matched-work it reads **3x worse than the 84-93 us that was recorded**;
   see §5.8 for why that is not a regression claim.

3. **umem `prodcons` throughput at 192 threads is at parity** with the
   field on x86_64 (1.95 Mops vs 1.56-2.21; the ring buffer dominates) and
   0.5x jemalloc/snmalloc on aarch64 (1.41 vs 2.70/2.95) -- the latter
   clears the aarch64 prodcons null (sd 15 %) and is the same depot cross-CPU
   cost.

---

## 5. Gaps, ranked, with mechanism

Only gaps that clear the null control. Category: **(b)** = defect or missing
optimisation, fixable -> Phase 8 task; **(a)** = design cost of something the
competitor does not do, accepted here; **(c)** = known-open item.

### 5.1 [b, FIXED in `a74065e`] The `LD_PRELOAD` interposer serialised every `free()` on a global mutex

**Measured (pre-fix, `f2a8267`/`5513c81`, `multi` 16:64, c7i.metal-48xl):**

| t | umem-preload | umem (API) | glibc | preload/API |
|---|---|---|---|---|
| 1 | 3.03 | 6.70 | 6.24 | 0.45 |
| 8 | 2.02 | 37.8 | 38.4 | 0.053 |
| 32 | 1.61 | 140.7 | 128.7 | 0.011 |
| 64 | 1.06 | 181.5 | 177.7 | 0.006 |
| 128 | 0.89 | 277.7 | 298.5 | 0.003 |
| 192 | **0.82** | **393.6** | **431.5** | **0.002** |

Same shape on c8g.metal (1.15 Mops at 192 vs 487 API), c7i.2xlarge (1.5 vs
31.6 at t=8) and c7g.2xlarge (1.5 vs 50.6). Preload-arm null control at these
points: sd 3-5%, so this is ~500x, not noise.

**Mechanism** (`malloc_interpose.c` at `f2a8267`): `free()` ->
`interpose_owner_of()` -> `is_libc_pointer()` takes the process-global
`libc_ptr_lock` and scans all 512 `libc_ptrs[]` slots on **every** call
(`:321-343`), although the table only ever holds bootstrap-phase `memalign`
pointers and is empty for the steady-state life of every process. Then
`process_free(ptr, 0, &size)` decodes the header to classify, and
`umem_malloc_free()` -> `process_free(ptr, 1, NULL)` decodes it **again**. One
global lock acquisition plus two header decodes per free, on the hottest path
in the library. Negative scaling is the expected result of a global mutex
taken by every thread on every operation.

**Fix** (`a74065e`, malloc_interpose.c only): an atomic live-entry count gates
the table scan (empty table = one relaxed load), and `free()` goes straight to
`umem_malloc_free()` once READY. Regression:
`test/stress/repro_interpose_free_scaling` (1 vs 8 threads aggregate: pre-fix
ratio **0.29x**, post-fix **4.10x**).

**A/B on `c7i.metal-48xl`** (`scripts/ec2/interpose_ab.sh`, three archives
built on the box: pre=`5513c81`, post=`a74065e`, null=`5513c81` rebuilt; 3
alternating pairs per point; `docs/results/2026-09-23-interpose-ab/`):

Null control (pre vs independent rebuild of pre, preload arm): t=1 median
+0.37% (-0.5..+0.4), t=8 -2.6% (-14.6..+9.2), **t=192 -0.5% (-3.5..+1.7)**.

| point | pre Mops | post Mops | post/pre | pre p999 ns | post p999 |
|---|---|---|---|---|---|
| multi 16:64 t=1 | 3.03 | 6.07 | **2.0x** | 25 | 24 |
| multi 16:64 t=8 | 1.93 | 28.8 | **14.9x** | 378 | 84 |
| multi 16:64 t=32 | 1.58 | 114.5 | **72x** | 393 | 103 |
| multi 16:64 t=64 | 1.07 | 154.7 | **145x** | 779 | 78 |
| multi 16:64 t=128 | 0.88 | 231.3 | **262x** | 694 | 156 |
| multi 16:64 t=192 | 0.79 | 314.2 | **396x** | 154 | 459 |
| multi 64:256 t=8 | 1.97 | 34.5 | 17.6x | 523 | 27 |
| multi 64:256 t=192 | 0.83 | 325.1 | 391x | 196 | 438 |
| prodcons 64:256 t=8 | 3.85 | 10.2 | 2.6x | 15,831 | 4,316 |
| prodcons 64:256 t=192 | 0.98 | 1.77 | 1.8x | 2,729,638 | 50,394 |

`ops_floor_raised` 0 and `alloc_failures` 0 on all 60 rows.

**Post-fix preload vs API, same build, alternating (the residual):**

| t | API Mops | preload Mops | preload/API (3 pairs) | API p999 | preload p999 |
|---|---|---|---|---|---|
| 1 | 7.5 | 6.1 | **0.814** (0.813..0.819) | 24 | 24 |
| 8 | 43.9 | 30.3 | 0.694 (0.681..0.702) | 189 | 105 |
| 32 | 154.4 | 114.3 | 0.741 (0.705..0.751) | 182 | 104 |
| 64 | 195.3 | 148.3 | 0.782 (0.751..0.824) | 204 | 111 |
| 128 | 288.8 | 232.7 | 0.810 (0.806..0.841) | 192 | 192 |
| 192 | 409.2 | 331.8 | **0.811** (0.664..0.833) | 559 | 473 |

The scaling defect is gone: post-fix preload scales 6 -> 332 Mops (55x over
192x threads, same slope as the API arm). What remains is a flat **~19-30%
per-operation cost** with no thread dependence, and `perf` at t=1 and t=192
(`perf-post-umem-preload-*.flat.txt`) attributes it to exactly the code that
runs per call: `process_free` 4.1-4.4%, `is_bootstrap_pointer` 1.0% (t=1) ->
**5.9%** (t=192; it is an out-of-line PLT call from `libumem_malloc.so` into
`libumem.so`, made twice per free), `umem_may_own` 1.6-1.7%, `free` 1.4-1.7%,
`umem_malloc` 1.1-2.0%, `is_libc_pointer` 0.7-0.8%, `umem_malloc_free` 0.7%
-- about 12-16% of all cycles in interposer-only functions, against an API
arm whose `_umem_alloc`+`_umem_free`+wrapper total ~6%. No lock, no
serialisation: `hull_refresh`/`vmem_walk` do not appear in any profile
(the hull is stable once the heap stops growing). This residual is **§5.3**.

### 5.2 [b] `umem_alloc` collapses at 1k:4k object sizes under threads

**Measured (API arm, `multi` 1024:4096, all boxes):**

| box | t | libc | umem | best | umem/libc | umem vs best | umem p999 ns | null here |
|---|---|---|---|---|---|---|---|---|
| c7i.2xlarge | 2 | 8.6 | 4.4 | 10.6 rpmalloc | 0.52 | -58% | 2,305 | +1.5% |
| c7i.2xlarge | 8 | 23.5 | 6.0 | 30.9 jemalloc | 0.26 | -81% | 10,173 | +1.3% |
| c7g.2xlarge | 8 | 29.6 | 8.6 | 39.6 jemalloc | 0.29 | -78% | 5,992 | +3.4% |
| c7i.metal | 8 | 28.5 | 5.8 | 38.6 rpmalloc | 0.20 | -85% | 9,413 | -1.0% |
| c7i.metal | 32 | 115 | 7.3 | 141 rpmalloc | 0.06 | -95% | 32,000 | +1.0% |
| c7i.metal | 64 | 146 | 9.0 | 204 tcmalloc | 0.06 | -96% | 50,824 | +0.1% |
| c7i.metal | 192 | 326 | 23.0 | 458 tcmalloc | 0.07 | -95% | 49,154 | -8.0% |
| c8g.metal | 64 | 215 | 14.9 | 309 mimalloc | 0.07 | -95% | 42,548 | +0.0% |
| c8g.metal | 192 | 337 | 33.7 | 412 snmalloc | 0.10 | -92% | 41,682 | -11.0% |

At t=1 the same size range is fine (0.97-1.10x glibc). The umem@null arm
shows the identical collapse (5.98 vs 5.94 Mops at c7i.metal t=8), so it is
deterministic, not placement. The preload arm is also slow here but for the
§5.1 reason. `prodcons` 1k:4k does **not** show it (umem 10-11 Mops at t=8,
at parity with the field), and neither does `frag` 1k:4k.

**Mechanism** (identified in source; the contention dump confirms the layer):

1. The API wrapper adds 16 bytes, so a 1024..4095 request becomes 1040..4111
   and lands in caches `umem_alloc_1280` .. `umem_alloc_5120`. Everything
   above **2048 bytes misses the per-thread cache entirely**:
   `umem_ptc_maxsize = 2048` (`umem_ptc.c:46`) and
   `umem_ptc_bin_table[idx] = -1` for larger objects (`umem_ptc.c:199-213`).
   For a uniform 1k:4k draw that is ~2/3 of all operations.
2. Those operations fall through `_umem_alloc()` (`umem.c:3813`) to
   `_umem_cache_alloc()` -> the rseq fast path, which serves **zero** hits
   (known, `cache_rseq[cpu].rounds` is never populated) -> `cc_lock` on the
   per-CPU magazine layer (`umem.c:3200`). On `multi` every thread does
   alloc-then-free of a **different** random size each iteration, so the
   loaded/previous magazine pair of one CPU cache is churned across 4-5 size
   classes at once, and the magazines for these classes are small: the
   `umem_magtype` table (`umem.c:549-558`) gives chunks 2560..4096 a
   **31-round** magazine and 5120 a 15-round one, against 63-127 for the
   sub-2 KB classes the PTC covers. A 31-round magazine drains in 31
   allocations and every drain is a depot round trip.
3. Each depot round trip is `umem_depot_alloc()` (blocking `ml_lock`), and
   the rest of the cost shows in the 8-vCPU `prodcons` contention dump for the
   neighbouring 1024/1280 classes (`contention-umem-prodcons-t8-256_1024.txt`:
   26,177 `dep_remote` vs 39 `dep_local` -- almost every reload steals from
   another CPU's stripe). On `multi` the dump is empty for these caches
   because the counters live on the PTC path; the p999 of 32-68 us and the
   `cc_alloc` column being the only nonzero one confirm the traffic is on the
   `cc_lock` path.

Competitors do not have this cliff because their thread caches extend to
32 KB (tcmalloc/jemalloc) or are the whole allocator (mimalloc/snmalloc).

**Diagnosis + fix approach (Phase 8 task P8.2).** (i) Confirm with a probe
that flips `UMEM_OPTIONS=ptc_maxsize=8192` and re-measures the 1k:4k point --
if the cliff moves to 4k:16k, the mechanism is proven. (ii) Raise
`umem_ptc_maxsize` and `PTC_NBINS` to cover through 8192 (the `umem_alloc_sizes`
table already has classes to 8192; `ptc_size_classes` stops at 2048 with two
padding zeros). (iii) Independently, the 31/15-round magazines for 2.5-8 KB
objects are Solaris-era tunings for a 64 KB slab; measure `umem_magtype`
`mt_magsize` 63 for the 2048..8192 band. Both are `umem_ptc.c`/`umem.c`
constants; neither is a design change.

### 5.3 [b] Interposer per-call overhead after the fix (~20-30%)

Measured in §5.1's last table. Mechanism from the profile: two out-of-line
calls to `is_bootstrap_pointer()` per free (once in `free()`, once again in
`umem_malloc_free()`), an `__errno_location()` call and two `umem_may_own()`
calls in `process_free()`, plus the `malloc_data_t` header decode -- none
individually large, all per-operation. **Fix approach (P8.3):** hoist the
bootstrap check to a single inline range compare (the bootstrap arena is a
bounded set of `mmap`s; `is_bootstrap_pointer()` reads `buf[-1]` and compares a
magic -- that is the read the P5.8 hull check exists to avoid, so replace it
with the hull, not another header read); make `umem_may_own()` inline in the
common (hull-hit) case; skip the second `is_bootstrap_pointer` in
`umem_malloc_free()` when called from the interposer (which has already
checked). Target: preload/API >= 0.95 at t=1 in the §5.1 reference protocol.

### 5.4 [b] API arm 8-24% below the best competitor at scale on x86_64 metal, `multi` 16:64

At `c7i.metal-48xl`, `multi` 16:64: umem 37.8/140.7/181.5/277.7/393.6 Mops at
t=8/32/64/128/192 vs mimalloc 42.8/164.3/210.0/348.3/504.9 and glibc
38.4/128.7/177.7/298.5/431.5. Gap to best: -12/-14/-14/-20/-24%. Null at
these points: -5.5/+9.5/+0.3/+8.0/+6.1% with a pooled sd of 8.5%, so the
t=128 and t=192 gaps (-20%, -24%) clear the band; t=8..64 do not by
themselves, but the sign is consistent across all five thread counts and
across the 64:256 and 256:1024 sizes (-5..-18%).

On **aarch64 metal the same gap is 3-6%** at every thread count and inside
the null, i.e. not a finding there. On the 8-vCPU boxes it is inside the
null at t <= 4 and -13/-15% at t=8 on x86_64 (null +7.5/+2.2%) -- borderline.

**Mechanism: not established** by this run. The contention dumps for `multi`
show zero depot traffic and zero `cc_alloc` (everything is served by the
PTC), so this is per-operation cost on the PTC hit path itself, not
contention. Candidates from the perf profile of the API arm at t=192
(`perf-post-umem-t192.flat.txt`): `_umem_alloc` 2.5% + `_umem_free` 2.3% +
the wrapper 3.4% -- versus `td_qsort`/`td_add` (the benchmark's own t-digest)
at 30%+, which dominates and limits what a flat profile can resolve here.
Phase 8 task P8.4 is a **diagnosis** task: a perf run with the latency
histogram disabled, and a per-op instruction count (`perf stat -e
instructions`) of umem vs mimalloc on this exact point.

### 5.5 [a] Memory overhead: umem holds 1.55-1.6x the live set at 64:256 and 2.6x at 16:64 where glibc holds 1.24x / 1.85x

`frag` pair on c7i.2xlarge (MB, median of 2 replicates; both arms of umem are
the same):

| size | t | libc rss / live / VmHWM | **umem** rss / live / VmHWM | jemalloc | mimalloc | scudo |
|---|---|---|---|---|---|---|
| 16:64 | 1 | 311 / 168 / 311 (1.85) | **439 / 168 / 439 (2.61)** | 282 (1.68) | 277 (1.65) | 431 (2.57) |
| 16:64 | 8 | 354 / 184 / 368 (1.92) | **517 / 192 / 516 (2.70)** | 321 (1.70) | 356 (1.94) | 511 (2.62) |
| 64:256 | 1 | 838 / 671 / 838 (1.25) | **1048 / 671 / 1048 (1.56)** | 815 (1.21) | 808 (1.20) | 1002 (1.49) |
| 64:256 | 8 | 943 / 768 / 1005 (1.23) | **1230 / 764 / 1233 (1.61)** | 937 (1.23) | 921 (1.21) | 1188 (1.50) |
| 256:1024 | 8 | 1336 / 1247 / 1378 (1.07) | **1673 / 1105 / 1674 (1.52)** | 1422 (1.15) | 1483 (1.37) | 1680 (1.32) |
| 1k:4k | 8 | 1996 / 1960 / 2129 (1.02) | **2308 / 1914 / 2310 (1.21)** | 2220 (1.12) | 2339 (1.16) | 2359 (1.24) |

Same figures on aarch64 within 2%. `rss_at_live_peak` and `VmHWM` agree to
within 1%, so RSS was not already high from an earlier phase: the ratio is
what the allocator holds.

**Accounted for, mostly by design.** For 16..63-byte requests: the API
wrapper's 16-byte header (and the interposer's 8+8) pushes every request one
size class up; a Monte-Carlo of the `umem_alloc_sizes` classes gives an
expected chunk/live of **1.59x** for umem vs **1.39x** for glibc's 16-byte
chunk rounding -- that is 1.15x of the 1.41x observed ratio (2.61/1.85). The
remainder is slab/magazine retention: `umem_reclaim_enabled` marks an empty
slab `SLAB_DIRTY` and leaves it for the update thread (`umem.c:1905`), and
each of the ~5 active size classes holds up to 2 x 127-round magazines per
CPU plus PTC bins. This is the classic slab-allocator trade (object caches
that stay warm) and scudo -- also a size-class allocator with per-thread
caches and no header-free path -- lands at the same 2.6x. **Not a task.**
What *would* be a task is the 1.52x at 256:1024 t=8 where umem's live set
also dipped (1105 vs 1247 MB for glibc at the same instant) -- a sampling
artefact of the threads desynchronising, as documented in P2.2, not a
separate finding.

### 5.6 [c] The ~5 GB heap ceiling, confirmed at HEAD, plus a harness defect it exposed

Ceiling probe (`frag` 1k:4k, 12M ops, t=8, live set 7.5 GB, 16 GiB boxes):

| box | arm | completed ops | alloc_failures | rss@peak | live@peak | VmHWM |
|---|---|---|---|---|---|---|
| c7i.2xlarge | libc | 23,062,582 | 0 | 7.72 GB | 7.55 GB | 7.95 GB |
| c7i.2xlarge | umem | -- (hung, killed) | 6,229,564 (stderr) | -- | -- | 4.52 GB (from /proc) |
| c7g.2xlarge | libc | 23,062,582 | 0 | 7.72 GB | 7.62 GB | 7.95 GB |
| c7g.2xlarge | umem | 9,975,603 | **6,741,130** | 4.45 GB | 3.84 GB | 4.45 GB |
| both | umem-preload | -- (hung, killed) | 5.9-6.9M (stderr) | -- | -- | 4.47-4.55 GB |
| c7i.metal (t=192, 19.2M ops) | libc | 19,200,000 | 0 | (in TOML) | | |
| c7i.metal | umem | -- (hung: 188/192 threads created, killed after 8.6 min) | 12,297,543 (stderr) | -- | -- | 4.74 GB, 65,532 VMAs |
| c7i.metal | umem-preload | -- (hung: 186/192, killed) | (stderr) | -- | -- | 4.73 GB, 65,532 VMAs |
| c8g.metal (t=192) | umem | -- (hung: 178/192, killed) | 12,289,016 (stderr) | -- | -- | 4.71 GB, 65,531 VMAs |
| c8g.metal | umem-preload | -- (hung: 192/192 created but pool `calloc` failed for some, killed after 15 min) | (stderr) | -- | -- | 4.85 GB, 65,531 VMAs |

umem stops at 4.45-4.85 GB with 65,531-65,532 VMAs against `max_map_count`
65,530 on all four boxes (16 GiB and 377-384 GiB alike) -- exactly the
2026-09-22 diagnosis, and independent of how much RAM the box has. **This is the known-open item;
`3f2e67c` (span-density floor in `umem_cache_create()`) landed after this
run and is not measured here.** It also caused a **harness hang**: once the
VMA budget was gone, `pthread_create()` failed for the 5th-8th frag worker,
and `bench_framework.c`'s "absorb" loop had the driver wait once per missing
barrier slot -- a single thread cannot fill several slots sequentially, so the
run blocked forever (37 min on x86_64 until killed; gdb showed 4 workers and
the driver all in `pthread_barrier_wait`). Fixed in `174e14c` (filler
threads); the umem/umem-preload ceiling rows are therefore incomplete and
the stderr failure counts above are the evidence.

### 5.7 [b] `frag`: umem is 20-33% behind the size-class allocators at 16..1024 B, and 2-3x behind under sustained load, with a 22 us p999

Measured in §4.4 and §4.5 on both 8-vCPU boxes at every thread count
including t=1, against a null of +/-3-9%. Absolute: umem 3.4-4.2 Mops at t=1
vs 4.9-6.1 for the best; 8.7 Mops sustained vs 19-22.

**Mechanism.** The workload holds a live set of `budget/4` objects per thread
(5M in the matrix, 11M sustained) and frees a random half of it every round.
Half the allocations in each round therefore cannot be served by anything
that was recently freed -- they must come from the slab layer -- and half the
frees are to objects that were allocated long ago from slabs the PTC/magazine
layers have never seen. umem's layers are sized for a hot working set:

- PTC bin: 128 slots for <= 256 B, 64 for <= 1 KB, 32 for <= 2 KB
  (`umem_ptc.h:46-48`); PTC magazine pair: 2 x 127 rounds.
- Below that, every object goes through `_umem_cache_alloc()` -> `cc_lock`
  -> `umem_depot_alloc()`/`umem_depot_alloc_trylock()` -> on a miss,
  `umem_slab_alloc()` under `cp->cache_lock` (`umem.c:1686-1745`), **one
  object per lock acquisition**, and `umem_slab_create()` -> `vmem_alloc()`
  + `umem_bufctl_cache` when a slab runs out (for non-HASH caches the bufctl
  is inline, but `umem_slab_t` still comes from `umem_slab_cache`).
- On the free side, `umem_slab_free()` (`umem.c:1839`) takes `cache_lock`,
  and the 8-thread contention dump shows the depot trylock failing on more
  than half of attempts (58,261 `dep_conten` vs 40,949 `dep_local` for
  `umem_alloc_48`).

jemalloc/mimalloc/snmalloc/rpmalloc serve exactly this pattern from
per-thread page/segment free lists with no global lock and no per-object
slab bookkeeping; glibc's tcache + fastbins do too, which is why glibc
matches umem's throughput here despite its 10-16 us `free()` tail.

**Diagnosis + fix approach (P8.5):** (i) `perf record` of umem `frag` 16:64
t=1 to split the time between `umem_slab_alloc`/`umem_slab_free` (lock +
list walk), `umem_slab_create`, and the PTC/magazine miss path; the metal
`perf-umem-frag-*` captures from this run are the first cut. (ii) The
cheapest structural fix is **batching at the slab layer**: `umem_slab_alloc`
already has `umem_cache_alloc_batch()` next to it (`umem.c:3328`) which
takes `cc_lock` once for N objects; the PTC refill path should use it and
`umem_slab_alloc` should hand out a whole magazine's worth per `cache_lock`
acquisition when the freelist has them. (iii) The trylock miss rate says the
per-CPU depot stripes are contended at 8 threads on 8 stripes; with
`get_cached_cpu_hint()` returning the real CPU under rseq this should not
happen unless threads migrate -- check `rseq_rstrt` (0 here) and whether the
stripe is `cpu & (ncpus-1)` with `ncpus` rounded up to 8 -> 8 stripes for 8
threads, so any two threads that share a CPU momentarily collide. Measured
first, then fixed.

### 5.8 Regressions against the last defensible numbers

- **P5.4 mangling A/B** (`6c8fabd`, -0.27% median inside a +/-4% null) is
  the same source as `f2a8267` for every allocator file; nothing to re-run.
  The single-thread umem figures here (7.12-7.43 Mops at c7i.metal 16:64 /
  64:256) are consistent with that A/B's 6.95-7.56 Mops at the same points
  on the same instance type.
- **Depot trylock fix, sustained p999 157 -> 84-93 us at 192 threads
  (x86_64 metal, prodcons 64:256, whole-run, budget defect present).**
  This run's matched-work, per-window sustained `prodcons` at 192 threads
  on the same instance type gives umem **p999 273 us (windows 256-325 us),
  umem@null 240 us (212-285)** -- about 3x the 84-93 us on record. Whether
  that is a regression **cannot be decided from this data**, and the doc
  does not claim one: the two measurements are not the same measurement.
  The 2026-09-09 figure was a whole-run p999 over a run whose budget was
  divided twice (P2.1: ~52k total ops at 192 threads, so each thread did
  ~270 operations and the "sustained" run was dominated by thread start-up);
  this run's windows are 38.4M ops each with 200k per producer. A 3-minute
  run of 270 ops per thread and a 20 s window of 200k per thread have
  different tails by construction. What *can* be said: (a) the mechanism the
  fix removed (a blocking mutex in the cross-CPU steal scan while holding
  `cc_lock`) is still absent from the code at `f2a8267` -- `umem.c:2435`
  `umem_depot_alloc_trylock` is trylock-only, and the blocking
  `umem_depot_alloc` (`umem.c:2611`) is reached only after the trylock path
  fails; (b) at 192 threads `prodcons` the field spans 5 us (rpmalloc) to
  1.2 ms (scudo) with glibc at 83 us, and umem at 240-273 us sits between
  tcmalloc (78 us) and scudo, i.e. it does **not** reach the
  tens-of-microseconds tier and the README no longer implies it might.
  **To settle regression-or-not**: run `scripts/ec2/sustained_load.sh umem`
  at the depot-trylock fix's parent and child commits (`2026-09-09`) under
  the current harness on metal, A/B with the null. That is a Phase 8 follow-
  up under P8.5, since the mechanism (depot cross-CPU stripes under
  all-to-all traffic) is the same one.
- **The 8-thread sustained `prodcons` p999 (§4.5) is 0.84-0.95 us**, second
  in the field, so the depot path is healthy where the stripes are not
  oversubscribed. The 192-thread tail is a scaling property of the stripe
  design, not a fast-path defect.

---

## 6. Things that went wrong in the rig, fixed, and what they cost

1. **`job.sh kill` did not kill.** The supervisor recorded its own pid as
   the "pgid", but `timeout(1)` runs its child in a new process group, so
   `kill -TERM -<pgid>` hit only the supervisor shell. A `matrix.sh` sweep
   survived the kill and ran for **1h40m alongside its replacement** on both
   metal boxes; every point in that window (the first `prodcons` and
   `multi-hi` passes) measured a box under double load and is **discarded**
   (`discarded-double-load/` on the boxes, not committed). Fixed in
   `20e82bb` (kill by session id, verify, report survivors); verified 0
   survivors on a `sleep &` test. This is exactly the failure mode `AGENTS.md`
   §4 warns about, and it happened through the tool meant to prevent it.
2. **Partial thread start deadlocked** (§5.6). Fixed in `174e14c`.
3. **`sustained_load.sh` calibration could go below the 100k/thread floor**
   at 192 threads for fast allocators, which would have flagged every window
   `ops_floor_raised`. Floored in `2d7b5af` before any sustained run.
4. The first `multi-hi` design included the preload arms at 200M ops
   (~13 h). Excluded in `2d7b5af`; their scaling is settled at 20M.

---

## 7. What this does and does not establish

**Established, with a null control beside each:**
- The interposer's negative scaling and its ~400x fix (§5.1).
- The 1k:4k `multi` collapse of the API arm, deterministic, all four boxes (§5.2).
- Single-thread parity with glibc on the API path, all boxes (§4.1).
- umem's 1.2-1.6x RSS/live vs glibc's 1.0-1.25x at 64 B and up (§5.5).
- The ceiling at 4.5 GB on 16 GiB boxes at HEAD-before-`3f2e67c` (§5.6).

- The sustained-`frag` collapse at 192 threads (8-19x, p999 6-10 ms) and
  its mechanism in the depot's per-stripe mutexes (§4.5, §5.7).
- Sustained `prodcons` p999 at 192 threads is 240-270 us, not in the
  tens-of-microseconds tier; whether that is a regression from the recorded
  84-93 us is undecidable against a measurement taken with a broken budget
  (§5.8).

**Not established:**
- Anything from `prodcons` *throughput* on 8 vCPU (bimodal; null +244%).
- The mechanism of the 8-24% x86_64-metal `multi` gap (§5.4).
- Whether `3f2e67c`/`cf3f762` lift the ceiling, or what `147d5ff`/`9bbe58b`
  (periodic depot reap; the update thread now actually exists) do to the RSS
  and sustained figures. Everything here is pre all four commits.
- aarch64 metal `multi-hi` (200M ops): its clean rerun was cancelled to
  release the box; the 20M `multi` pass covers the same points at +/-18 %.

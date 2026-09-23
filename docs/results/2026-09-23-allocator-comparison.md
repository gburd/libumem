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
| Commit measured | `f2a8267df7586db9e5e6b7dd052bda90e5cea571` via `scripts/ec2/verify-isolated.sh` (`git archive`, committed content only). Allocator sources identical to `v3.1.0` + `5513c81`; the interposer fix `a74065e` is measured separately in §5.1. |
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
- **Metal `multi-hi` (200M ops at 64/128/192 threads) had to be rerun** and is
  reported in §4.2 from the rerun only. The first attempt on both metal boxes
  ran concurrently with a job that `job.sh kill` had failed to stop (§6), so
  every point from that window measured a box under double load and was
  discarded. `single` and `multi` completed before the overlap and are clean.
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

<!-- MULTI TABLES -->

### 4.3 `prodcons` (half the threads allocate, half free)

<!-- PRODCONS -->

### 4.4 `frag` (grow a live pool, free ~50% at random, repeat) and the ceiling probe

<!-- FRAG -->

### 4.5 Sustained (matched work, per-window, interleaved)

<!-- SUSTAINED -->

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

umem stops at 4.45-4.55 GB with 65,531-65,532 VMAs against `max_map_count`
65,530 -- exactly the 2026-09-22 diagnosis. **This is the known-open item;
`3f2e67c` (span-density floor in `umem_cache_create()`) landed after this
run and is not measured here.** It also caused a **harness hang**: once the
VMA budget was gone, `pthread_create()` failed for the 5th-8th frag worker,
and `bench_framework.c`'s "absorb" loop had the driver wait once per missing
barrier slot -- a single thread cannot fill several slots sequentially, so the
run blocked forever (37 min on x86_64 until killed; gdb showed 4 workers and
the driver all in `pthread_barrier_wait`). Fixed in `174e14c` (filler
threads); the umem/umem-preload ceiling rows are therefore incomplete and
the stderr failure counts above are the evidence.

### 5.7 [a/b?] `prodcons` and `frag` p999 on the API arm vs the field

<!-- FRAG/PRODCONS TAIL -->

### 5.8 Regressions against the last defensible numbers

- **P5.4 mangling A/B** (`6c8fabd`, -0.27% median inside a +/-4% null) is
  the same source as `f2a8267` for every allocator file; nothing to re-run.
  The single-thread umem figures here (7.12-7.43 Mops at c7i.metal 16:64 /
  64:256) are consistent with that A/B's 6.95-7.56 Mops at the same points
  on the same instance type.
- **Depot trylock fix, sustained p999 157 -> 84-93 us at 192 threads
  (x86_64 metal, prodcons 64:256, whole-run, budget defect present).**
  <!-- SUSTAINED-REGRESSION -->

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

**Not established:**
- Anything from `prodcons` on 8 vCPU (bimodal; null +244%).
- The mechanism of the 8-24% x86_64-metal `multi` gap (§5.4).
- Whether `3f2e67c` lifts the ceiling, or `a74065e`'s effect on `frag`/RSS.
- aarch64 metal `multi-hi` and sustained -- see §4.2/§4.5 for what did land.

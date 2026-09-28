# Heavy-load comparison v2: libumem vs glibc vs jemalloc on 192-vCPU metal

Agent @r7cmp2, 2026-09-28. Replaces the v3.3.2 192-thread comparison
(`2026-09-27-heavy-load-comparison.md`), which measured harness contention
(`2026-09-27-harness-contention-fix.md`). The design was pre-registered in
`docs/plans/2026-09-27-round7.md` ("@r7cmp2 comparison") before any run, and
the summarizer (`2026-09-28-heavy-load-v2/summarize.py`, db5a3fd) was
committed before any data existed.

## What ran

- **Ref `2a5b850`** = 3816199 plus the `SUSTAINED_PRODCONS_SIZES` /
  `SUSTAINED_WORKLOADS` knobs (script-only). Every box built its own
  isolated `git archive` of that ref (`verify-isolated.sh`, `ISOLATED_PROVENANCE`
  in each TOML), default `./configure` (no flags).
- **Boxes:** `c8g.metal-48xl` (Graviton4, aarch64) and `c7i.metal-48xl`
  (Sapphire Rapids, x86_64). Both have 192 vCPU and 2 NUMA nodes and run Debian 13
  with kernel 6.12.107+deb13-cloud, gcc 14.2.0, glibc 2.41-12+deb13u4, and
  jemalloc = Debian `libjemalloc2 5.3.0-3` (sha256 `51952ffe…` amd64, `74ccd960…`
  arm64, dlopen'd). There were three boxes per arch (@r7cmp2a = cell 1,
  @r7cmp2b = cell 2, @r7cmp2c = cells 3 then 4), all launched in us-east-2
  with no capacity problem.
- **Every arm:** `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536`
  (usable thread stack 8.3 MB), `numactl --interleave=all`, threads not
  pinned, THP `never`, numa_balancing 0, and governor `powersave` on x86
  (`unknown` on arm). Each run is one `bench_main -D 60:15 -W 5` process: 15
  one-minute intervals, of which the first 5 are dropped and the next 10 kept.
  frag holds a 3 GiB live set (`-L 3072`) at every size. Latency is **sampled
  1/16** (`-T 16`, one op in 16 per thread is timed).
- **Order** per cell was umem,libc,jemalloc,libc@2,jemalloc@2,umem@2,jemalloc@3,umem@3,libc@3.
  Each allocator appears once in each position of each block of three, and
  frag walks the list reversed. The preflight (`bench_main -a X` must print a
  row) passed for every arm on every box.
- **Outcome:** 99 of 99 runs exited rc 0. Every run has exactly intervals 5-14,
  each lasting 60 s ± 5 %, with 0 allocation failures. `summarize.py` reports
  RESULT PASS for all 8 (cell, arch) files.
- **Rule:** per run, the metric is the median over its 10 intervals (peak RSS
  is the max over them). Per allocator, the table gives min / median / max
  over its 3 runs. A difference counts **only if the 3-run ranges do not
  overlap**. Otherwise it is "no resolvable difference" (≈ below).
- **What libumem was measured through:** the `umem_alloc` API, with the
  bench's 16-byte size header (`allocators.c`). glibc and jemalloc use plain
  malloc/free. The `libumem_malloc.so` drop-in was **not** measured.
- **Cut:** nothing. All four cells ran on both arches in about 32 box-hours
  (cap 40).

## Verdicts

`>` means better, with non-overlapping ranges. `≈` means no resolvable
difference. Medians of per-run values are in parentheses. Mops/s: higher is
better. p99/p999 (ns, sampled 1/16) and peak RSS (MiB): lower is better.

| cell | arch | Mops/s | p99 | p999 | peak RSS |
|---|---|---|---|---|---|
| 1 t=192 64:256 prodcons | arm | je > umem > libc (1330 / 532 / 289) | umem ≈ je > libc (511 / 535 / 6266) | je > umem > libc (1311 / 2378 / 10896) | libc > je > umem (65 / 160 / 276) |
| | x86 | je > umem; libc ≈ both (865 / 581 / 867) | umem > je ≈ libc (531 / 1217 / 1666) | umem ≈ je > libc (2932 / 3242 / 7252) | libc > je > umem (67 / 95 / 237) |
| 1 t=192 64:256 frag | arm | je > umem > libc (1146 / 734 / 472) | je ≈ umem > libc (971 / 976 / 2293) | umem > libc > je (2316 / 3078 / 3377) | je > libc > umem (3812 / 3906 / 4245) |
| | x86 | je > umem > libc (1120 / 647 / 430) | umem > je > libc (741 / 909 / 2091) | umem ≈ je > libc (1976 / 1940 / 3588) | je > libc > umem (3811 / 3907 / 4291) |
| 2 t=384 64:256 prodcons | arm | libc > je > umem (3415 / 1704 / 489) | libc > je > umem (346 / 471 / 563) | je > libc ≈ umem (1286 / 2341 / 2523) | libc > je > umem (125 / 342 / 426) |
| | x86 | libc > je > umem (2960 / 2429 / 624) | libc > je ≈ umem (230 / 462 / 471) | libc > je > umem (939 / 1465 / 2514) | libc > je > umem (126 / 186 / 435) |
| 2 t=384 64:256 frag | arm | je > umem > libc (1171 / 775 / 485) | je > umem > libc (847 / 944 / 2207) | umem > libc > je (1958 / 3072 / 3192) | je > libc > umem (3915 / 3953 / 4104) |
| | x86 | je > umem > libc (1136 / 663 / 456) | umem > je > libc (761 / 804 / 2034) | je > umem > libc (1974 / 2284 / 3596) | je > libc > umem (3903 / 3955 / 4106) |
| 3 t=192 1024:4096 prodcons | arm | je > umem > libc (1274 / 140 / 83) | je > umem > libc (698 / 1503 / 15579) | je > umem > libc (1660 / 4713 / 21095) | libc > je > umem (290 / 501 / 1020) |
| | x86 | je > libc > umem (304 / 179 / 133) | umem > je > libc (1414 / 6968 / 15660) | umem > je > libc (4430 / 25210 / 46730) | libc > je > umem (289 / 364 / 930) |
| 3 t=192 1024:4096 frag | arm | umem > je > libc (124.6 / 122.5 / 119.1) | umem > libc > je (5480 / 6367 / 12859) | libc > umem > je (13542 / 14668 / 25230) | libc > umem > je (3253 / 3558 / 3833) |
| | x86 | je > umem > libc (112.8 / 107.1 / 104.9) | umem > libc > je (3931 / 4861 / 5483) | umem > je > libc (7688 / 10719 / 13136) | libc > umem > je (3254 / 3450 / 3721) |
| 4 t=192 16384:65536 frag | arm | umem > je > libc (9.6 / 9.0 / 7.8) | libc > je > umem (23764 / 41845 / 53447) | je > umem > libc (57759 / 110532 / 1374545) | umem > libc > je (3196 / 3282 / 4466) |
| | x86 | umem > libc > je (8.3 / 8.1 / 8.0) | libc > umem > je (11954 / 12475 / 15477) | umem > je > libc (22479 / 25978 / 143802) | umem > libc > je (3199 / 3286 / 4139) |

Run-to-run spread was small (usually < 3 %), so most pairs resolve. The
exception is x86 t=192 prodcons: glibc's three runs spanned 373-1096 Mops/s
and 3.9-18.4 us p999, and jemalloc's spanned 781-982 Mops/s. That is why
several x86 cell-1 prodcons pairs are ≈.

## Caveats that bound these numbers

1. **t=384 prodcons: unexplained, do not headline.** Going from t=192 to
   t=384, glibc's prodcons throughput rises 11.8x on arm (289 -> 3415) and
   3.4x on x86 (867 -> 2960), and jemalloc's rises 1.3x / 2.8x, while umem's
   stays flat (532 -> 489, 581 -> 624). That is a change in the regime, not
   in any allocator. One untested hypothesis: oversubscription puts a
   producer and its consumer on one CPU, so the per-pair ring handoff
   becomes core-local (see the harness doc, "What remains" 2). I have not
   shown this. The t=384 verdicts follow the rule, but until the mechanism is
   known they describe the harness topology at 2x oversubscription as much
   as the allocators.
2. **The harness overhead is still in every throughput number.** At -T 16,
   `clock_gettime` plus the t-digest cost 4-8 % of the time (r7samp). Every
   allocator pays the same per-op cost, which pulls throughput ratios toward
   1. The prodcons handoff (`producer_thread`, 21.7 % self at t=192 umem in
   r7harness) is in every prodcons number.
3. **The umem arm carries a 16 B size header** (`umem_free_wrapper`, 37.6 %
   self in r7harness frag t=192). It belongs to the API arm, not to the
   harness, and I did not separate how much of it is the header's cache
   miss. The drop-in path, which has no header, was not measured.
4. **frag RSS is dominated by the fixed 3 GiB live set**, so frag RSS
   differences are overhead above ~3.2 GB. At 16-64 KB, jemalloc's 4.0-4.4
   GiB is 29-40 % above umem's.
5. **Latency is sampled 1/16 per thread**, and p999 is the median over 10
   intervals of each interval's p999. It is not the p999 of the whole run.
6. x86 ran on the `powersave` governor (the metal default), the same for
   every arm.

## Proposed README replacement (for the coordinator; README.md not edited)

Replace the whole section "Under heavy load: 192 cores, 2 NUMA nodes (v3.3.2)",
from its heading down to (but not including) "### Two libumem paths", with:

```markdown
### Under heavy load: 192 cores, 2 NUMA nodes

On Debian 13 metal (`c7i.metal-48xl` Sapphire Rapids and `c8g.metal-48xl`
Graviton4, 192 vCPU, 2 NUMA nodes, memory interleaved over both), each
allocator ran **three times for 15 minutes per workload**. The first 5 minutes
of each run were dropped. A difference is reported only if the three runs'
ranges do not overlap. Latency is sampled (1 op in 16 per thread). libumem
was measured through `umem_alloc()` with a 16-byte size header, not through
the `libumem_malloc.so` drop-in. Method, raw data and every verdict:
[`docs/results/2026-09-28-heavy-load-comparison-v2.md`](docs/results/2026-09-28-heavy-load-comparison-v2.md).
(This replaces the v3.3.2 table, which measured contention in the benchmark
harness rather than the allocators.)

| 192 threads, median of 3 runs (x86 / arm) | libumem | glibc 2.41 | jemalloc 5.3 |
|---|---|---|---|
| cross-thread alloc/free 64-256 B, Mops/s | 581 / 532 | 867\* / 289 | 865\* / **1330** |
| cross-thread alloc/free 64-256 B, p99 | **0.53** / **0.51**\* us | 1.67\* / 6.27 us | 1.22\* / **0.53**\* us |
| cross-thread alloc/free 64-256 B, peak RSS | 237 / 276 MiB | **67 / 65 MiB** | 95 / 160 MiB |
| fragmentation churn 64-256 B (3 GiB live), Mops/s | 647 / 734 | 430 / 472 | **1120 / 1146** |
| fragmentation churn 64-256 B, p999 | **1.98**\* / **2.32** us | 3.59 / 3.08 us | **1.94**\* / 3.38 us |
| cross-thread alloc/free 1-4 KB, Mops/s | 133 / 140 | 179 / 83 | **304 / 1274** |
| cross-thread alloc/free 1-4 KB, p999 | **4.4** / 4.7 us | 46.7 / 21.1 us | 25.2 / **1.7** us |
| fragmentation churn 16-64 KB, Mops/s | **8.3 / 9.6** | 8.1 / 7.8 | 8.0 / 9.0 |
| fragmentation churn 16-64 KB, p999 | **22** / 111 us | 144 / 1375 us | 26 / **58** us |
| fragmentation churn 16-64 KB, peak RSS | **3.1 / 3.1 GiB** | 3.2 / 3.2 GiB | 4.0 / 4.4 GiB |

\* no resolvable difference from the other starred entry in the same row
and arch. x86 cross-thread 64-256 B throughput: glibc is not distinguishable
from libumem or from jemalloc (jemalloc > libumem there).

- **Throughput (192 threads):** jemalloc is fastest in 64-256 B
  fragmentation churn (1.5-1.7x libumem) and in cross-thread free (1.5-9x
  libumem; 9x at 1-4 KB on arm). In 1-4 KB fragmentation churn all three
  are within 8 %.
  At 192 threads libumem is 1.5-1.8x glibc at 64-256 B, except x86
  cross-thread, where there is no resolvable difference. At 16-64 KB
  libumem is fastest, by 3-7 % over the next allocator.
- **Tail latency:** libumem has the lowest or tied-lowest p99 in five of the
  eight 64-256 B cells (192 and 384 threads, both workloads, both arches), and
  the lowest cross-thread p999 at 1-4 KB on x86. It has 5-11x lower
  cross-thread p999 than glibc at 1-4 KB (4.5x arm, 11x x86), and 6-12x
  lower fragmentation-churn p999 at 16-64 KB. jemalloc has the lowest
  cross-thread p999 on arm at both sizes and both thread counts.
- **Memory:** libumem uses the most RSS in cross-thread free (3.2-4.2x
  glibc, 1.2-2.6x jemalloc) and 4-13 % more than either in 64-256 B
  fragmentation churn. At 16-64 KB it uses the least (jemalloc takes 29-40 %
  more).
- **384 threads (2x oversubscribed):** fragmentation churn keeps the
  192-thread ordering. In cross-thread free, glibc becomes fastest (2,960 /
  3,415 Mops/s) and libumem slowest (624 / 489). The cause is not yet
  diagnosed (the 192 -> 384 change is 3-12x for glibc and flat for libumem),
  so treat that row as unexplained.
- **Not measured:** the `libumem_malloc.so` drop-in, and allocators other
  than these three. Throughput ratios include the benchmark's own per-op
  timing and hand-off cost, which every allocator pays equally, so the
  ratios are compressed toward 1.
```

## Raw data

`docs/results/2026-09-28-heavy-load-v2/<instance>-<arch>/`:
`c{1,2,3,4}-*.toml` (the per-interval rows plus full provenance: sha, binary
digests, allocator identity and package, and kernel/THP/NUMA settings),
`*.csv` (bench_main's raw output per run), `*.toml.log`, `job-*.out.log` (the
build, the preflight and every run), `prov-*.txt` (configure line, kernel,
libc6/libjemalloc2 versions), and `summary-*.md` (the output of
`summarize.py`, reproduced below).

## Per-cell tables (summarize.py output, verbatim)

### c8g.metal-48xl-aarch64 / summary-c1-c2.md

#### c1-t192-64-256.toml

c8g.metal-48xl aarch64, threads 192, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,146 / 1,146 / 1,146 | 954.5 / 970.5 / 972.5 | 3,356 / 3,377 / 3,386 | 3,812 / 3,812 / 3,812 |
| libc | 471.6 / 472.3 / 472.4 | 2,290 / 2,293 / 2,302 | 3,072 / 3,078 / 3,084 | 3,906 / 3,906 / 3,906 |
| umem | 734.0 / 734.1 / 741.8 | 964.0 / 976.0 / 976.5 | 2,190 / 2,316 / 2,351 | 4,213 / 4,245 / 4,264 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.43x); jemalloc better than umem (1.56x); umem better than libc (1.55x)
- p99 ns: jemalloc better than libc (2.36x); jemalloc vs umem: no resolvable difference; umem better than libc (2.35x)
- p999 ns: libc better than jemalloc (1.10x); umem better than jemalloc (1.46x); umem better than libc (1.33x)
- peak RSS MiB: jemalloc better than libc (1.02x); jemalloc better than umem (1.11x); libc better than umem (1.09x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,235 / 1,330 / 1,377 | 512.0 / 534.5 / 558.5 | 1,261 / 1,311 / 1,406 | 159.5 / 160.1 / 162.3 |
| libc | 279.0 / 289.1 / 313.3 | 5,694 / 6,266 / 6,685 | 10,404 / 10,896 / 11,076 | 65.2 / 65.2 / 65.2 |
| umem | 518.7 / 531.7 / 563.5 | 505.0 / 511.0 / 518.0 | 2,268 / 2,378 / 2,631 | 269.4 / 276.3 / 279.3 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (4.60x); jemalloc better than umem (2.50x); umem better than libc (1.84x)
- p99 ns: jemalloc better than libc (11.72x); jemalloc vs umem: no resolvable difference; umem better than libc (12.26x)
- p999 ns: jemalloc better than libc (8.31x); jemalloc better than umem (1.81x); umem better than libc (4.58x)
- peak RSS MiB: libc better than jemalloc (2.46x); jemalloc better than umem (1.73x); libc better than umem (4.24x)


#### c2-t384-64-256.toml

c8g.metal-48xl aarch64, threads 384, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,169 / 1,171 / 1,172 | 841.0 / 847.0 / 847.5 | 3,184 / 3,192 / 3,195 | 3,915 / 3,915 / 3,915 |
| libc | 483.7 / 484.5 / 485.6 | 2,202 / 2,207 / 2,207 | 3,070 / 3,072 / 3,082 | 3,953 / 3,953 / 3,953 |
| umem | 768.8 / 775.1 / 776.9 | 941.5 / 943.5 / 948.0 | 1,954 / 1,958 / 2,024 | 4,017 / 4,104 / 4,111 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.42x); jemalloc better than umem (1.51x); umem better than libc (1.60x)
- p99 ns: jemalloc better than libc (2.61x); jemalloc better than umem (1.11x); umem better than libc (2.34x)
- p999 ns: libc better than jemalloc (1.04x); umem better than jemalloc (1.63x); umem better than libc (1.57x)
- peak RSS MiB: jemalloc better than libc (1.01x); jemalloc better than umem (1.05x); libc better than umem (1.04x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,703 / 1,704 / 1,747 | 464.5 / 471.0 / 476.5 | 1,275 / 1,286 / 1,307 | 341.7 / 342.2 / 342.9 |
| libc | 3,341 / 3,415 / 3,501 | 342.0 / 345.5 / 376.0 | 2,061 / 2,341 / 2,506 | 124.5 / 124.5 / 124.5 |
| umem | 469.7 / 489.2 / 489.7 | 548.0 / 562.5 / 566.0 | 2,499 / 2,523 / 2,590 | 413.0 / 425.8 / 432.9 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: libc better than jemalloc (2.00x); jemalloc better than umem (3.48x); libc better than umem (6.98x)
- p99 ns: libc better than jemalloc (1.36x); jemalloc better than umem (1.19x); libc better than umem (1.63x)
- p999 ns: jemalloc better than libc (1.82x); jemalloc better than umem (1.96x); libc vs umem: no resolvable difference
- peak RSS MiB: libc better than jemalloc (2.75x); jemalloc better than umem (1.24x); libc better than umem (3.42x)


RESULT PASS (0 invariant failures)

### c8g.metal-48xl-aarch64 / summary-c3.md

#### c3-t192-1024-4096.toml

c8g.metal-48xl aarch64, threads 192, prodcons 1024:4096, frag 1024:4096, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 122.5 / 122.5 / 122.5 | 12,812 / 12,859 / 12,929 | 25,162 / 25,230 / 25,284 | 3,832 / 3,833 / 3,835 |
| libc | 119.1 / 119.1 / 119.1 | 6,338 / 6,367 / 6,372 | 13,449 / 13,542 / 13,728 | 3,253 / 3,253 / 3,253 |
| umem | 124.5 / 124.6 / 124.7 | 5,460 / 5,480 / 5,549 | 14,376 / 14,668 / 15,018 | 3,555 / 3,558 / 3,568 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.03x); umem better than jemalloc (1.02x); umem better than libc (1.05x)
- p99 ns: libc better than jemalloc (2.02x); umem better than jemalloc (2.35x); umem better than libc (1.16x)
- p999 ns: libc better than jemalloc (1.86x); umem better than jemalloc (1.72x); libc better than umem (1.08x)
- peak RSS MiB: libc better than jemalloc (1.18x); umem better than jemalloc (1.08x); libc better than umem (1.09x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,270 / 1,274 / 1,275 | 685.0 / 698.0 / 713.5 | 1,648 / 1,660 / 1,664 | 500.6 / 501.3 / 502.5 |
| libc | 80.3 / 82.5 / 118.3 | 11,996 / 15,579 / 15,659 | 15,864 / 21,095 / 21,120 | 286.6 / 290.0 / 291.6 |
| umem | 137.6 / 140.0 / 141.3 | 1,466 / 1,503 / 1,506 | 4,552 / 4,713 / 5,182 | 935.1 / 1,020 / 1,023 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (15.45x); jemalloc better than umem (9.10x); umem better than libc (1.70x)
- p99 ns: jemalloc better than libc (22.32x); jemalloc better than umem (2.15x); umem better than libc (10.37x)
- p999 ns: jemalloc better than libc (12.71x); jemalloc better than umem (2.84x); umem better than libc (4.48x)
- peak RSS MiB: libc better than jemalloc (1.73x); jemalloc better than umem (2.04x); libc better than umem (3.52x)


RESULT PASS (0 invariant failures)

### c8g.metal-48xl-aarch64 / summary-c4.md

#### c4-t192-frag-16384-65536.toml

c8g.metal-48xl aarch64, threads 192, prodcons 64:256, frag 16384:65536, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 9.0 / 9.0 / 9.0 | 41,824 / 41,845 / 42,433 | 57,518 / 57,759 / 58,346 | 4,465 / 4,466 / 4,470 |
| libc | 7.8 / 7.8 / 7.8 | 23,748 / 23,764 / 23,803 | 1,367,214 / 1,374,545 / 1,382,238 | 3,279 / 3,282 / 3,282 |
| umem | 9.5 / 9.6 / 9.6 | 52,812 / 53,447 / 98,680 | 106,986 / 110,532 / 195,280 | 3,195 / 3,196 / 3,214 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.15x); umem better than jemalloc (1.07x); umem better than libc (1.22x)
- p99 ns: libc better than jemalloc (1.76x); jemalloc better than umem (1.28x); libc better than umem (2.25x)
- p999 ns: jemalloc better than libc (23.80x); jemalloc better than umem (1.91x); umem better than libc (12.44x)
- peak RSS MiB: libc better than jemalloc (1.36x); umem better than jemalloc (1.40x); umem better than libc (1.03x)


RESULT PASS (0 invariant failures)

### c7i.metal-48xl-x86_64 / summary-c1-c2.md

#### c1-t192-64-256.toml

c7i.metal-48xl x86_64, threads 192, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,120 / 1,120 / 1,120 | 896.0 / 909.0 / 912.5 | 1,934 / 1,940 / 1,950 | 3,811 / 3,811 / 3,811 |
| libc | 429.1 / 429.5 / 429.7 | 2,090 / 2,091 / 2,093 | 3,532 / 3,588 / 3,599 | 3,907 / 3,907 / 3,907 |
| umem | 644.4 / 646.6 / 648.7 | 739.0 / 740.5 / 742.5 | 1,910 / 1,976 / 2,032 | 4,279 / 4,291 / 4,293 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.61x); jemalloc better than umem (1.73x); umem better than libc (1.51x)
- p99 ns: jemalloc better than libc (2.30x); umem better than jemalloc (1.23x); umem better than libc (2.82x)
- p999 ns: jemalloc better than libc (1.85x); jemalloc vs umem: no resolvable difference; umem better than libc (1.82x)
- peak RSS MiB: jemalloc better than libc (1.03x); jemalloc better than umem (1.13x); libc better than umem (1.10x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 780.8 / 865.0 / 981.8 | 1,005 / 1,217 / 1,467 | 2,893 / 3,242 / 3,470 | 94.7 / 94.8 / 95.9 |
| libc | 372.8 / 866.9 / 1,096 | 984.0 / 1,666 / 7,211 | 3,869 / 7,252 / 18,352 | 66.4 / 66.5 / 66.5 |
| umem | 566.7 / 581.4 / 597.0 | 521.5 / 530.5 / 559.5 | 2,856 / 2,932 / 3,089 | 236.8 / 237.1 / 245.8 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc vs libc: no resolvable difference; jemalloc better than umem (1.49x); libc vs umem: no resolvable difference
- p99 ns: jemalloc vs libc: no resolvable difference; umem better than jemalloc (2.29x); umem better than libc (3.14x)
- p999 ns: jemalloc better than libc (2.24x); jemalloc vs umem: no resolvable difference; umem better than libc (2.47x)
- peak RSS MiB: libc better than jemalloc (1.43x); jemalloc better than umem (2.50x); libc better than umem (3.57x)


#### c2-t384-64-256.toml

c7i.metal-48xl x86_64, threads 384, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,134 / 1,136 / 1,136 | 785.5 / 803.5 / 807.0 | 1,964 / 1,974 / 1,975 | 3,903 / 3,903 / 3,903 |
| libc | 455.4 / 456.4 / 457.7 | 2,030 / 2,034 / 2,040 | 3,566 / 3,596 / 3,613 | 3,955 / 3,955 / 3,955 |
| umem | 662.4 / 662.7 / 663.4 | 760.5 / 760.5 / 765.0 | 2,256 / 2,284 / 2,322 | 4,088 / 4,106 / 4,165 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.49x); jemalloc better than umem (1.71x); umem better than libc (1.45x)
- p99 ns: jemalloc better than libc (2.53x); umem better than jemalloc (1.06x); umem better than libc (2.68x)
- p999 ns: jemalloc better than libc (1.82x); jemalloc better than umem (1.16x); umem better than libc (1.57x)
- peak RSS MiB: jemalloc better than libc (1.01x); jemalloc better than umem (1.05x); libc better than umem (1.04x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 2,423 / 2,429 / 2,433 | 459.5 / 462.0 / 480.0 | 1,398 / 1,465 / 1,494 | 186.2 / 186.2 / 186.9 |
| libc | 2,896 / 2,960 / 2,966 | 224.0 / 229.5 / 263.0 | 911.0 / 939.0 / 1,006 | 126.4 / 126.4 / 126.4 |
| umem | 621.7 / 623.9 / 630.5 | 463.0 / 470.5 / 482.5 | 2,374 / 2,514 / 2,547 | 422.4 / 435.4 / 443.5 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: libc better than jemalloc (1.22x); jemalloc better than umem (3.89x); libc better than umem (4.74x)
- p99 ns: libc better than jemalloc (2.01x); jemalloc vs umem: no resolvable difference; libc better than umem (2.05x)
- p999 ns: libc better than jemalloc (1.56x); jemalloc better than umem (1.72x); libc better than umem (2.68x)
- peak RSS MiB: libc better than jemalloc (1.47x); jemalloc better than umem (2.34x); libc better than umem (3.45x)


RESULT PASS (0 invariant failures)

### c7i.metal-48xl-x86_64 / summary-c3.md

#### c3-t192-1024-4096.toml

c7i.metal-48xl x86_64, threads 192, prodcons 1024:4096, frag 1024:4096, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 112.7 / 112.8 / 112.8 | 5,458 / 5,483 / 5,486 | 10,712 / 10,719 / 10,736 | 3,721 / 3,721 / 3,721 |
| libc | 104.9 / 104.9 / 104.9 | 4,856 / 4,861 / 4,862 | 13,124 / 13,136 / 13,432 | 3,254 / 3,254 / 3,254 |
| umem | 106.9 / 107.1 / 107.3 | 3,916 / 3,931 / 3,948 | 7,618 / 7,688 / 7,698 | 3,446 / 3,450 / 3,457 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.07x); jemalloc better than umem (1.05x); umem better than libc (1.02x)
- p99 ns: libc better than jemalloc (1.13x); umem better than jemalloc (1.39x); umem better than libc (1.24x)
- p999 ns: jemalloc better than libc (1.23x); umem better than jemalloc (1.39x); umem better than libc (1.71x)
- peak RSS MiB: libc better than jemalloc (1.14x); umem better than jemalloc (1.08x); libc better than umem (1.06x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 296.7 / 304.2 / 369.6 | 5,389 / 6,968 / 7,093 | 17,851 / 25,210 / 29,174 | 334.9 / 363.9 / 409.8 |
| libc | 172.7 / 178.8 / 184.9 | 15,536 / 15,660 / 15,935 | 42,830 / 46,730 / 48,102 | 288.2 / 288.5 / 289.2 |
| umem | 126.8 / 133.0 / 137.8 | 1,360 / 1,414 / 1,482 | 4,374 / 4,430 / 4,456 | 921.8 / 929.5 / 931.5 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.70x); jemalloc better than umem (2.29x); libc better than umem (1.34x)
- p99 ns: jemalloc better than libc (2.25x); umem better than jemalloc (4.93x); umem better than libc (11.08x)
- p999 ns: jemalloc better than libc (1.85x); umem better than jemalloc (5.69x); umem better than libc (10.55x)
- peak RSS MiB: libc better than jemalloc (1.26x); jemalloc better than umem (2.55x); libc better than umem (3.22x)


RESULT PASS (0 invariant failures)

### c7i.metal-48xl-x86_64 / summary-c4.md

#### c4-t192-frag-16384-65536.toml

c7i.metal-48xl x86_64, threads 192, prodcons 64:256, frag 16384:65536, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 8.0 / 8.0 / 8.0 | 15,336 / 15,477 / 15,500 | 25,806 / 25,978 / 26,012 | 4,138 / 4,139 / 4,141 |
| libc | 8.1 / 8.1 / 8.1 | 11,909 / 11,954 / 12,038 | 141,752 / 143,802 / 144,870 | 3,285 / 3,286 / 3,287 |
| umem | 8.3 / 8.3 / 8.3 | 12,342 / 12,475 / 13,174 | 22,204 / 22,479 / 23,282 | 3,197 / 3,199 / 3,204 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: libc better than jemalloc (1.01x); umem better than jemalloc (1.04x); umem better than libc (1.03x)
- p99 ns: libc better than jemalloc (1.29x); umem better than jemalloc (1.24x); libc better than umem (1.04x)
- p999 ns: jemalloc better than libc (5.54x); umem better than jemalloc (1.16x); umem better than libc (6.40x)
- peak RSS MiB: libc better than jemalloc (1.26x); umem better than jemalloc (1.29x); umem better than libc (1.03x)


RESULT PASS (0 invariant failures)

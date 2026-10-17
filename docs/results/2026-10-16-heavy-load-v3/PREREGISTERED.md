# v3 heavy-load comparison -- pre-registration (before any data)

Agent @r12bench, 2026-10-16/17.  Re-runs the 2026-09-28 v2 comparison on the
CURRENT allocator (v4.2.1, master `2d8645f`), after the PAL/AAL refactor
(declared behaviour-preserving, ABI unchanged).  Hypothesis: results match
v3.3.x (the v2 ref `2a5b850`); any drift is surfaced.

## Fixed before any run
- Allocators: umem (master 2d8645f), libc (glibc 2.41), jemalloc (5.3.0-3).
  tcmalloc/mimalloc not installed on the base Debian image; not blocked on
  (same three-way comparison as v2).  GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536 for all.
- Boxes: c7i.metal-48xl (x86, Sapphire Rapids) + c8g.metal-48xl (arm, Graviton4),
  192 vCPU, 2 NUMA nodes, Debian 13.7, kernel 6.12.111, EC2_OS=debian, us-east-2.
- Workloads: prodcons (cross-thread) + frag, both in each cell.
- Cells: c1 t=192 64:256; c2 t=384 64:256; c3 t=192 1024:4096.
  (c4 t=384 1024:4096 deliberately CUT: no v2 counterpart, so it cannot feed
  the v2 delta; dropped to fit the ~30 box-hour budget.)
- 3 runs per allocator per cell (x, x@2, x@3), each run = bench_main -D 60:15 -W 5
  (15 one-min intervals, first 5 dropped, 10 kept).  frag live set -L 3072 (3 GiB).
  Latency sampled 1/16 (bench_main default BENCH_LAT_SAMPLE_DEFAULT).
- numactl --interleave=all, threads unpinned, for every arm.
- Metric per run: median of 10 intervals (Mops/s, p99, p999); peak RSS = max.
  Per allocator: min/med/max over 3 runs.  Difference counts ONLY if the two
  allocators' 3-run [min,max] ranges do not overlap (non-overlap rule).
- Summarizer: summarize.py (byte-identical copy of the v2 summarizer, committed
  before any data existed).

# libumem Allocator Benchmarks

Benchmark suite for comparing libumem against other memory allocators.

> **Harness defects P2.1/P2.2 are FIXED as of 2026-09-22.** Read this before
> comparing any new number against an older one: the fix changed both the
> arithmetic and the output format, so old and new results are not comparable.
>
> What was wrong, and what it means for previously published data:
>
> 1. **The operation budget was divided by thread count twice** — once in
>    `matrix.sh` (`ops=$(( OPERATIONS / t ))`) and again in `bench_main.c`
>    (`.operation_count = operation_count / thread_count`). Aggregate work
>    therefore fell as 1/threads², and the 192-thread points used to claim a
>    scaling cliff measured ~52k total operations in ~3.8 ms with >27% CoV.
>    Now `-n` is a **total** budget, divided exactly once, with a minimum of
>    `BENCH_MIN_OPS_PER_THREAD` (100k) operations per thread; a point below
>    that floor is raised and flagged `ops_floor_raised`. `bench_allocators.sh`
>    and `bench_contention.c` had the same bug and are fixed too.
> 2. **The fragmentation metric was wrong four ways**: the live-bytes
>    denominator accumulated bytes that had already been freed;
>    `peak_rss_bytes` was sampled *after* cleanup so it was not a peak and did
>    not correspond to the ratio; the pool was capped at 4096 objects so a
>    longer run did not grow its working set; and the `frag` workload ran on
>    one thread regardless of `-t` while being reported as 192-thread. Now
>    fragmentation is `peak_rss_bytes / live_bytes_at_peak` with both sampled
>    **at the same instant**, the pool grows with the budget, and the workload
>    honours `-t`.
> 3. **`single`/`multi`/`prodcons` no longer report a fragmentation number at
>    all.** They divided RSS by *cumulative* allocation traffic, which falls
>    towards zero the longer the run. They hold no live set, so the ratio is
>    undefined and the CSV column is left **empty** — not `0`, not `1.0`.
>
> Conclusions withdrawn on the strength of these defects (192-thread scaling,
> all fragmentation findings) have **not** been restored. They require
> re-measurement under the protocol below, not a recomputation.
>
> Tracked as P2.1/P2.2 in
> [`../../docs/plans/2026-09-21-production-readiness.md`](../../docs/plans/2026-09-21-production-readiness.md).
> The regressions are `test/bench/test_bench_accounting` (the arithmetic) and
> `test/bench/check_budget.sh` (end-to-end, because the published defect was a
> *composition* of two locally-defensible divisions).

## Re-measurement protocol

A number from this harness is citable only with all of these:

- **A work floor.** Never accept a point with `ops_floor_raised = true` as a
  measurement of the budget you asked for; raise `-n` instead.
- **Matched protocols.** Same warm-up count, window count, thread count and
  duration target for every allocator compared.
- **Alternating A/B, not batched.** `matrix.sh` alternates allocators at the
  innermost loop so compared points are adjacent in time; batching one
  allocator's whole sweep lets slow drift (thermal, neighbour noise) appear as
  a difference between allocators.
- **Per-window, not whole-run, for sustained loads.** `bench_main -A` emits one
  CSV row per measured window, each with its own percentiles and its own
  RSS/live-bytes pair. A single whole-run p999 cannot show a tail degrading and
  a single whole-run RSS cannot show fragmentation growing.
- **Full provenance.** Commit sha, configure flags, instance type, allocator
  library paths and digests, binary digests. `matrix.sh`/`sustained_load.sh`
  record these; pass `LIBUMEM_SHA=<sha>` or run under
  `scripts/ec2/verify-isolated.sh`, because `run-remote.sh` excludes `.git` and
  the sha would otherwise be recorded as `unknown`.

## Which driver to use

There are two, and they are not interchangeable:

| | `matrix.sh` | `bench_allocators.sh` |
|---|---|---|
| Binary | `bench_main` (built by `make`, in `.libs/`) | `bench_allocators` (**not built by `make`**) |
| Sweep | workloads × thread ladder (1…192) × 4 size ranges × allocators | workloads × thread list × size list |
| Output | `docs/results/<date>-<instance>-<arch>/matrix.toml` + `meta.toml` provenance | `results/bench_TIMESTAMP.csv` |
| Status | **the maintained driver**; every result under `docs/results/` came from it | legacy; kept because its CSV output and options are still referenced |

Use `matrix.sh`. It records provenance (commit, instance type, allocator
versions) alongside the numbers, which `bench_allocators.sh` does not, and it
is the driver every committed result set was produced with. Older revisions of
this file pointed users at `./bench_allocators.sh` with no such caveat, and at
a `bench_allocators` target that the autotools build does not produce — only
the hand-written `test/bench/Makefile` builds it.

```bash
# on EC2 (never locally -- see ../../AGENTS.md)
./scripts/ec2/job.sh intel-hi@w1 start matrix 7200 \
    './scripts/ec2/clean-regen.sh && make -j$(nproc) && test/bench/matrix.sh umem libc'
```

`bench_gate.sh` is a third, deliberately non-blocking driver used by CI: it
runs a short single-thread + 2-thread configuration against
`baseline/x86_64-ci.toml` and always exits 0, because a shared runner's
variance is too high to gate a build on.

## What the framework provides

- latency percentiles via t-digest (p50/p90/p99/p999)
- workloads: `single`, `multi`, `prodcons`, `frag`
- throughput, latency, RSS, and an RSS/allocated ratio (see the caveat above)
- comparison against libc, jemalloc, tcmalloc, mimalloc, snmalloc, scudo,
  rpmalloc — all `dlopen`'d at runtime by `allocators.c`; use
  `scripts/ec2/install_extra_allocators.sh` to build the ones without a
  distro package

## Building

`bench_main` and the other `test/bench/bench_*` targets are built by the
normal autotools build:

```bash
./configure && make -j"$(nproc)"
ls test/bench/.libs/bench_main
```

The hand-written `test/bench/Makefile` builds the legacy `bench_allocators`
binary separately; it is not part of `make` at the top level.

## Running benchmarks

### The maintained path (`matrix.sh` + `bench_main`)

```bash
# full matrix, one allocator
test/bench/matrix.sh umem

# umem vs libc, results into a chosen directory
test/bench/matrix.sh -o /tmp/run umem libc

# a single point, straight from the binary
LD_LIBRARY_PATH=.libs test/bench/.libs/bench_main \
    -a umem -w multi -t 8 -n 10000000 -s 64:256 -r 5 -W 1 -c
```

### The legacy path (`bench_allocators.sh` + `bench_allocators`)

Requires `make -C test/bench` first; the top-level build does not produce
`bench_allocators`. Prefer `matrix.sh` — this driver records no provenance.

```bash
./bench_allocators.sh -q                                  # quick
./bench_allocators.sh                                     # full
./bench_allocators.sh umem libc                           # specific allocators
./bench_allocators.sh -n 10000000 -t 1,4,8,16 -s 16:1024 umem
```

## Command Line Options

### bench_allocators

```
Usage: ./bench_allocators [OPTIONS]

Options:
  -a ALLOCATOR  Test specific allocator (libc,umem,jemalloc,tcmalloc,mimalloc,snmalloc,scudo,rpmalloc,all)
  -w WORKLOAD   Run specific workload (single,multi,prodcons,frag,all)
  -t THREADS    Thread count for multithreaded workloads (default: CPU count)
  -n TOTAL_OPS  TOTAL operations across ALL threads (default: 1000000);
                divided by the thread count once, with a 100k/thread floor
  -s MIN:MAX    Size range in bytes (default: 16:1024)
  -r RUNS       Measured runs; report median + CoV (default: 1)
  -W WARMUPS    Warm-up runs to discard before measuring (default: 0)
  -c            Output CSV format
  -h            Show help
```

### bench_allocators.sh

```
Usage: ./bench_allocators.sh [OPTIONS] [ALLOCATORS...]

OPTIONS:
    -n COUNT        TOTAL operations across all threads (default: 10000000)
    -t THREADS      Comma-separated thread counts (default: 1,2,4,8,16)
    -s SIZES        Comma-separated size ranges (default: 16:64,64:256,...)
    -o DIR          Output directory (default: results)
    -r RUNS         Measured runs per point; median + CoV (default: 5)
    -W WARMUPS      Warm-up runs to discard (default: 1)
    --pin           Pin threads (numactl/taskset) + require performance governor
    -q              Quick mode: fewer iterations
    -h              Show help
```

### Stability (`--pin`, warm-up, median-of-N)

Multi-thread numbers are unstable when threads migrate and caches start cold.
The harness stabilizes results by:

- **warm-up discard + median-of-N** (`-r N -W W`): the C framework runs each
  point `W` warm-up times (thrown away) then `N` measured times, reports the
  **median** run's full latency/memory picture and the **coefficient of
  variation** (`ops_cov` = stddev/mean of throughput) across the `N` runs. A
  point with CoV > 10% is flagged `unstable` (last CSV column) — do not gate
  on it.
- **`--pin`**: verifies every CPU is on the `performance` governor (aborts
  with a fix hint otherwise) and binds the process to a contiguous CPU set
  sized to the thread count via `numactl --physcpubind`/`taskset`.

The CSV gains three trailing columns: `ops_cov,runs,unstable`.

## Workloads

### Single-threaded (`single`)

Allocates, uses (memset), and frees memory in a tight loop. Measures individual allocation latency.

**Use case**: Baseline performance, low-contention scenario

### Multi-threaded (`multi`)

Multiple threads concurrently allocating and freeing memory. Tests scalability and contention handling.

**Use case**: Realistic multi-threaded application behavior

### Producer-Consumer (`prodcons`)

Separate threads for allocation (producers) and deallocation (consumers). Tests cross-thread memory management.

**Use case**: Server applications with separate I/O and processing threads

### Fragmentation (`frag`)

Builds a live working set, then churns it: allocate a batch into a live pool,
sample RSS and live bytes together, free a random ~50% of the pool to create
holes, repeat. The pool's capacity grows with `-n`, so a longer run holds a
**larger** working set rather than cycling a fixed one for longer.

**Use case**: Long-running applications with varied allocation patterns

**Honours `-t`.** Each thread owns a private live pool; live bytes are summed
across threads because RSS is process-wide, so the reported ratio pairs
process RSS with process-wide live bytes. Before 2026-09-22 `bench_main.c`
hard-coded `thread_count = 1` here, so every result labelled "192-thread
fragmentation" was single-threaded.

## Metrics

### Throughput

- **ops/sec**: Operations per second (alloc+free pairs)
- Higher is better

### Latency

All latency measurements in nanoseconds:

- **min/max**: Minimum and maximum observed latencies
- **p50 (median)**: 50th percentile - typical case
- **p90**: 90th percentile - slower cases
- **p99**: 99th percentile - tail latency
- **p99.9**: 99.9th percentile - extreme tail
- **mean**: Average latency

Lower is better for all latency metrics.

### Memory

- **peak_rss_bytes**: for the `frag` workload, the largest RSS observed
  *during* the run; for the others, process RSS at the end. Never
  post-cleanup RSS presented as a peak.
- **allocated_bytes**: *cumulative* bytes requested over the whole run. This is
  traffic, not a live-set size, and must never be used as a denominator for
  memory overhead.
- **live_bytes_at_peak**: bytes simultaneously live at the instant
  `peak_rss_bytes` was sampled.
- **frag**: `peak_rss_bytes / live_bytes_at_peak`, both sampled at the **same
  instant**. 1.0 means the allocator's RSS equals the bytes the program was
  actually holding; higher means more overhead.
  - **Only the `frag` workload defines this.** `single`, `multi` and
    `prodcons` free every buffer immediately, hold no live set, and report an
    **empty** `frag` column. An empty value means undefined, and must not be
    read, plotted, or averaged as zero.

### Operation counts

- **total_ops**: operations completed, summed over **all** threads.
- **ops_per_thread**: `total_ops / threads`, as actually run.
- **threads**: threads that actually ran — which may differ from
  `threads_requested`. `prodcons` forces at least one producer and one
  consumer, so `-t 1` honestly runs 2. A difference between the two is
  information, not an error.
- **ops_floor_raised**: `true` means the per-thread share of `-n` was below
  `BENCH_MIN_OPS_PER_THREAD` and was raised to it, so the point ran **more**
  work than requested. Do not cite such a point as a measurement of `-n`.

## Output

### Human-Readable

```
========================================
Allocator: umem
Workload:  single-thread
========================================
Throughput: 5423156.32 ops/sec (1.84 s total)
Operations: 10000000 total across 1 thread

Latency (ns):
  min:  42
  p50:  156
  p90:  189
  p99:  234
  p999: 487
  max:  15234
  mean: 162

Memory:
  RSS:          125.45 MB
  Allocated:    100.00 MB
  Fragmentation: 1.25
========================================
```

### CSV Output

Results are saved to `results/bench_TIMESTAMP.csv` with all metrics for analysis.

Import into:
- **Excel/LibreOffice**: For charts and pivot tables
- **R/Python/pandas**: For statistical analysis
- **gnuplot**: For publication-quality graphs

## Analysis Examples

### Compare Allocators

```bash
# Run full comparison
./bench_allocators.sh

# Generate summary
cat results/bench_*.csv | column -t -s,
```

### Plot Results (Python)

```python
import pandas as pd
import matplotlib.pyplot as plt

# Load results
df = pd.read_csv('results/bench_20260327_120000.csv')

# Throughput comparison
df.groupby('allocator')['ops_per_sec'].mean().plot(kind='bar')
plt.ylabel('Operations/sec')
plt.title('Allocator Throughput Comparison')
plt.show()

# Latency comparison
df.groupby('allocator')['lat_p99'].mean().plot(kind='bar')
plt.ylabel('p99 Latency (ns)')
plt.title('Allocator p99 Latency Comparison')
plt.show()
```

## Interpreting Results

### Good Performance Indicators

1. **High throughput**: >1M ops/sec for single-threaded, >500K ops/sec/thread for multi-threaded
2. **Low p99 latency**: <500ns for small allocations (<1KB)
3. **Low fragmentation**: <1.5 for the `frag` workload (peak RSS / live
   bytes at that instant; the other workloads do not define it)
4. **Linear scaling**: 2x threads = 2x throughput (up to core count)

These are rules of thumb for reading a run, not thresholds this project
gates on. Authoritative comparison is the EC2 matrix with its provenance
file; a single run on an unpinned box says nothing. Any point with
`ops_cov` > 10% is flagged `unstable` in the CSV — do not draw a conclusion
from it.

### Red Flags

1. **High p99/p99.9**: Indicates contention or lock issues
2. **Poor multi-threaded scaling**: Suggests serialization bottlenecks
3. **High fragmentation**: Memory overhead problems
4. **Throughput regression**: Performance decreased vs baseline

## Benchmarking Best Practices

### System Setup

```bash
# Disable frequency scaling (for consistent results)
sudo cpupower frequency-set -g performance

# Disable turbo boost
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# Increase process priority
sudo nice -n -20 ./bench_allocators.sh
```

### Running Multiple Times

```bash
# Run 5 times and average results
for i in {1..5}; do
    ./bench_allocators.sh -q umem
done
```

### Comparing Changes

A/B on the same instance, via the maintained driver — the branch is `master`,
not `main`, and each side must be a from-scratch build on the *same* box or
the comparison is noise:

```bash
# on EC2, one instance, one job (see ../../AGENTS.md §4)
./scripts/ec2/job.sh intel-hi@ab start ab 7200 '
  git checkout master && ./scripts/ec2/clean-regen.sh && make -j$(nproc) &&
    test/bench/matrix.sh -o /tmp/base umem &&
  git checkout feature-branch && ./scripts/ec2/clean-regen.sh && make -j$(nproc) &&
    test/bench/matrix.sh -o /tmp/feat umem'
```

`scripts/ec2/d2_ab.sh` and `scripts/ec2/aarch64_ab.sh` already implement this
pattern; prefer them over hand-rolling it.

## Adding Allocators

Allocators are loaded at **runtime** by `allocators.c` via
`dlopen(RTLD_NOW|RTLD_LOCAL)` — not linked at build time. Read the comment
block at the top of `allocators.c` first; it explains why (a TLS
initial-exec model in some allocators makes late `dlopen` fail outright, so
some are `LD_PRELOAD`ed and detected via `RTLD_DEFAULT` instead).

To add one:

1. Add a path list and a `dlopen_malloc_syms_t` entry in `allocators.c`,
   following an existing allocator (`jemalloc`, `mimalloc`, ...). Give it an
   `..._LIB` environment override so a user can point at a custom build.
2. Register its `allocator_ops_t` in the allocator table so `-a <name>`
   finds it.
3. If it needs building from source on the test host, extend
   `scripts/ec2/install_extra_allocators.sh`.

No `configure` change, no `pkg-config` check, and no `-DHAVE_*` macro is
needed — earlier revisions of this file described exactly that, which is a
scheme this harness no longer uses.

## Troubleshooting

### Build Errors

```bash
# Everything under test/bench/ except bench_allocators is built by the
# top-level autotools build:
./configure && make -j"$(nproc)"

# Legacy bench_allocators only (hand-written Makefile in this directory):
make -C test/bench
```

### Runtime Errors

```bash
# Increase stack size
ulimit -s unlimited

# Which allocators actually loaded (they are dlopen'd; a missing one is
# reported on stderr at startup, not by a --help flag)
LD_LIBRARY_PATH=.libs test/bench/.libs/bench_main -a all -w single -n 1000 2>&1 | \
    grep -i 'warning\|not available'
```

## References

- **t-digest**: [github.com/tdunning/t-digest](https://github.com/tdunning/t-digest)
- **jemalloc**: [jemalloc.net](http://jemalloc.net/)
- **tcmalloc**: [github.com/google/tcmalloc](https://github.com/google/tcmalloc)
- **mimalloc**: [github.com/microsoft/mimalloc](https://github.com/microsoft/mimalloc)

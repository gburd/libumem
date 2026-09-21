# libumem Allocator Benchmarks

Benchmark suite for comparing libumem against other memory allocators.

> **Known-invalid measurements (2026-09-21).** Two defects in this harness
> invalidate results previously published from it. Fix them, or account for
> them, before trusting a number:
>
> 1. **The operation budget is divided by thread count twice** — once in
>    `matrix.sh` (`ops=$(( OPERATIONS / t ))`) and again in `bench_main.c`
>    (`.operation_count = operation_count / thread_count`). High-thread-count
>    points therefore run a small fraction of the intended work: 192-thread
>    points measured ~52k total operations in ~3.8 ms with >27% CoV.
> 2. **The fragmentation metric is wrong three ways**: the live-bytes
>    denominator in `bench_framework.c` accumulates bytes that were already
>    freed; `peak_rss_bytes` is sampled after cleanup, so it is not a peak;
>    and the `frag` workload runs on **one** thread regardless of `-t`
>    (`bench_main.c` sets `thread_count = 1` for it), so results labelled
>    "192-thread fragmentation" are single-threaded.
>
> Tracked as P2.1/P2.2 in
> [`../../docs/plans/2026-09-21-production-readiness.md`](../../docs/plans/2026-09-21-production-readiness.md).

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
  -n COUNT      Operation count (default: 1000000)
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
    -n COUNT        Number of operations (default: 10000000)
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

Allocates various sizes with specific free patterns to measure memory fragmentation over time.

**Use case**: Long-running applications with varied allocation patterns

**Single-threaded regardless of `-t`**: `bench_main.c` hardcodes
`thread_count = 1` for this workload. A result labelled with any other thread
count is mislabelled.

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

- **RSS (Resident Set Size)**: Physical memory used by process
- **Allocated**: Total bytes requested by benchmark
- **Fragmentation**: RSS / Allocated ratio
  - 1.0 = perfect (no overhead)
  - Higher = more fragmentation/overhead

**Do not use the fragmentation ratio as reported.** See the warning at the
top of this file: the denominator includes freed bytes, the "peak" RSS is
sampled after cleanup, and the `frag` workload ignores the thread count.
All three have to be fixed before this number means anything.

## Output

### Human-Readable

```
========================================
Allocator: umem
Workload:  single-thread
========================================
Throughput: 5423156.32 ops/sec (1.84 s total)
Operations: 10000000

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
3. **Low fragmentation**: <1.5 for mixed workloads
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

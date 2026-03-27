# libumem Allocator Benchmarks

Comprehensive benchmark suite for comparing libumem against other memory allocators.

## Overview

This benchmark framework provides:

- **Accurate latency measurement** using t-digest for percentile tracking
- **Multiple workloads**: Single-threaded, multi-threaded, producer-consumer, fragmentation
- **Comprehensive metrics**: Throughput, latency (p50/p90/p99/p99.9), memory overhead, fragmentation
- **Comparison support**: Test against libc, jemalloc, tcmalloc, mimalloc

## Building

```bash
# Build benchmarks
make

# Build with optional allocators (if installed)
# jemalloc: sudo apt install libjemalloc-dev
# tcmalloc: sudo apt install libgoogle-perftools-dev
# mimalloc: sudo apt install libmimalloc-dev
make
```

## Running Benchmarks

### Quick Test

```bash
./bench_allocators.sh -q
```

### Full Comparison

```bash
./bench_allocators.sh
```

### Specific Allocator

```bash
./bench_allocators.sh umem libc
```

### Custom Configuration

```bash
./bench_allocators.sh -n 10000000 -t 1,4,8,16 -s 16:1024 umem
```

## Command Line Options

### bench_allocators

```
Usage: ./bench_allocators [OPTIONS]

Options:
  -a ALLOCATOR  Test specific allocator (libc,umem,jemalloc,tcmalloc,mimalloc,all)
  -w WORKLOAD   Run specific workload (single,multi,prodcons,frag,all)
  -t THREADS    Thread count for multithreaded workloads (default: CPU count)
  -n COUNT      Operation count (default: 1000000)
  -s MIN:MAX    Size range in bytes (default: 16:1024)
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
    -q              Quick mode: fewer iterations
    -h              Show help
```

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

```bash
# Baseline
git checkout main
make clean && make
./bench_allocators.sh umem > baseline.txt

# New code
git checkout feature-branch
make clean && make
./bench_allocators.sh umem > feature.txt

# Compare
diff -u baseline.txt feature.txt
```

## Adding Allocators

To add a new allocator:

1. Edit `allocators.c`:

```c
#ifdef HAVE_MYALLOC
#include <myalloc.h>

allocator_ops_t allocator_myalloc = {
    .name = "myalloc",
    .alloc = my_malloc,
    .calloc = my_calloc,
    .realloc = my_realloc,
    .free = my_free,
    .cleanup = NULL
};
#endif
```

2. Update `Makefile`:

```makefile
HAVE_MYALLOC := $(shell pkg-config --exists myalloc && echo 1)
ifeq ($(HAVE_MYALLOC),1)
    CFLAGS += -DHAVE_MYALLOC $(shell pkg-config --cflags myalloc)
    LDFLAGS += $(shell pkg-config --libs myalloc)
endif
```

3. Add to `bench_main.c`:

```c
extern allocator_ops_t allocator_myalloc;

allocator_ops_t *allocators[] = {
    // ...
    &allocator_myalloc,
    NULL
};
```

## Troubleshooting

### Build Errors

```bash
# Missing tdigest
make -C .. tdigest.o

# Missing libumem
make -C ../..

# Clean rebuild
make distclean && make
```

### Runtime Errors

```bash
# Increase stack size
ulimit -s unlimited

# Check allocator availability
./bench_allocators -h | grep "not available"
```

## References

- **t-digest**: [github.com/tdunning/t-digest](https://github.com/tdunning/t-digest)
- **jemalloc**: [jemalloc.net](http://jemalloc.net/)
- **tcmalloc**: [github.com/google/tcmalloc](https://github.com/google/tcmalloc)
- **mimalloc**: [github.com/microsoft/mimalloc](https://github.com/microsoft/mimalloc)

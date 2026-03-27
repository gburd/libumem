# Benchmarking Guide

Comprehensive guide to performance analysis for libumem.

## Quick Start

```bash
cd test/bench
make
./bench_allocators.sh -q
```

## Benchmark Framework

The benchmark suite in `test/bench/` provides:

- Accurate percentile tracking using t-digest algorithm
- Multiple workload patterns (single-thread, multi-thread, producer-consumer, fragmentation)
- Comparison against major allocators (libc, jemalloc, tcmalloc, mimalloc)
- CSV output for detailed analysis

See `test/bench/README.md` for complete documentation.

## Performance Targets

Based on industry-leading allocators, libumem should achieve:

### Throughput

| Workload | Target ops/sec | Notes |
|----------|---------------|-------|
| Single-thread, small (<256B) | >2M | Baseline performance |
| Single-thread, medium (<4KB) | >1M | Most common case |
| Multi-thread (8 cores), small | >1M/core | Scaling target |
| Multi-thread (8 cores), medium | >500K/core | Acceptable scaling |

### Latency

| Allocation Size | p50 target | p99 target | p99.9 target |
|----------------|-----------|-----------|-------------|
| 16-64 bytes | <100ns | <300ns | <1μs |
| 64-256 bytes | <150ns | <400ns | <2μs |
| 256-1024 bytes | <200ns | <500ns | <5μs |
| 1-4 KB | <300ns | <800ns | <10μs |
| 4-16 KB | <500ns | <2μs | <20μs |

### Memory Overhead

- **Metadata overhead**: <5% for allocations >128 bytes
- **Fragmentation ratio**: <1.5 for mixed workloads
- **Peak RSS**: <1.3x total allocated for varied sizes

### Scalability

- **Linear scaling**: Up to 8 cores
- **Acceptable scaling**: 8-16 cores (>70% efficiency)
- **Graceful degradation**: 16+ cores

## Baseline Comparison

### Allocator Characteristics

**glibc malloc (ptmalloc2)**
- Throughput: ~1M ops/sec single-thread
- p99 latency: ~500ns (small allocations)
- Fragmentation: Can be high (>2.0) under stress
- Strengths: Universal, well-tested
- Weaknesses: Poor scaling, high fragmentation

**jemalloc**
- Throughput: ~3-5M ops/sec single-thread
- p99 latency: ~200-300ns
- Fragmentation: Low (<1.3)
- Strengths: Excellent scaling, low fragmentation
- Weaknesses: Higher memory overhead

**tcmalloc**
- Throughput: ~4-6M ops/sec single-thread
- p99 latency: ~150-250ns
- Fragmentation: Very low (<1.2)
- Strengths: Very fast, excellent caching
- Weaknesses: Complex, high baseline memory

**mimalloc**
- Throughput: ~5-7M ops/sec single-thread
- p99 latency: ~100-200ns
- Fragmentation: Low (<1.25)
- Strengths: Fastest single-thread, secure
- Weaknesses: Newer, less battle-tested

**libumem (target)**
- Throughput: >2M ops/sec single-thread
- p99 latency: <300ns (small allocations)
- Fragmentation: <1.4
- Strengths: Per-thread caching (PTC), debugging features
- Focus: Balance performance with observability

## Running Benchmarks

### Standard Comparison

```bash
cd test/bench
./bench_allocators.sh umem libc jemalloc tcmalloc mimalloc
```

### Regression Testing

Before making changes:

```bash
# Capture baseline
git checkout main
make clean && make
./bench_allocators.sh -o results/baseline umem

# Make changes
git checkout feature-branch
make clean && make
./bench_allocators.sh -o results/feature umem

# Compare
python3 compare_results.py results/baseline results/feature
```

Acceptable regression: <5% throughput decrease, <10% p99 increase

### Profiling Specific Scenarios

```bash
# Small allocations, single-threaded
./bench_allocators -a umem -w single -n 10000000 -s 16:64

# Large allocations, multi-threaded
./bench_allocators -a umem -w multi -t 16 -n 1000000 -s 4096:16384

# Mixed sizes, stress test
./bench_allocators -a umem -w frag -n 5000000 -s 16:8192
```

## Analysis Workflow

### 1. Collect Data

```bash
./bench_allocators.sh -o results/$(date +%Y%m%d) umem
```

### 2. Identify Bottlenecks

Look for:
- High p99/p99.9 latency (contention)
- Poor multi-thread scaling (locking issues)
- High fragmentation (allocation patterns)
- Throughput regression (algorithm changes)

### 3. Profile with perf

```bash
# CPU profiling
perf record -g ./bench_allocators -a umem -w multi -t 8 -n 10000000
perf report

# Cache misses
perf stat -e cache-references,cache-misses ./bench_allocators -a umem -w single

# Lock contention
perf lock record ./bench_allocators -a umem -w multi -t 16
perf lock report
```

### 4. Drill Down

```bash
# Profile specific function
perf record -g -e cycles:pp --call-graph dwarf ./bench_allocators -a umem

# Memory access patterns
perf mem record ./bench_allocators -a umem -w single -n 1000000
perf mem report
```

## Optimization Strategies

### 1. Reduce Contention

- **Per-thread caches**: Minimize shared state
- **Lock-free algorithms**: Use atomics where possible
- **Batching**: Amortize locking costs
- **Partitioning**: Split global structures

### 2. Improve Cache Locality

- **Size classes**: Group similar sizes
- **LIFO allocation**: Reuse hot cache lines
- **Prefetching**: Hint next access
- **Alignment**: Avoid false sharing

### 3. Minimize Fragmentation

- **Best-fit allocation**: Within size classes
- **Coalescing**: Merge adjacent free blocks
- **Segregated storage**: Separate size ranges
- **Periodic compaction**: Background defragmentation

### 4. Optimize Fast Paths

- **Inline common cases**: Small allocations
- **Branch prediction**: Likely/unlikely hints
- **Assembly optimization**: Critical sections
- **Register allocation**: Reduce memory access

## Debugging Performance Issues

### High Latency

```bash
# Check for lock contention
perf lock record ./bench_allocators -a umem -w multi

# Check for page faults
perf stat -e page-faults ./bench_allocators -a umem

# Check for system calls
strace -c ./bench_allocators -a umem -n 100000
```

### Poor Scaling

```bash
# Thread efficiency
./bench_allocators -a umem -w multi -t 1,2,4,8,16,32

# Expected: 2x threads ≈ 2x throughput (up to core count)
# If not scaling: lock contention or serialization
```

### High Fragmentation

```bash
# Long-running test
./bench_allocators -a umem -w frag -n 50000000

# Monitor RSS growth
while true; do
    ps aux | grep bench_allocators
    sleep 1
done
```

## Continuous Performance Testing

### CI Integration

```yaml
# .github/workflows/performance.yml
name: Performance Tests

on: [push, pull_request]

jobs:
  benchmark:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build
        run: ./configure && make
      - name: Run benchmarks
        run: |
          cd test/bench
          make
          ./bench_allocators.sh -q umem
      - name: Check for regression
        run: python3 check_regression.py baseline.csv results.csv
```

### Automated Alerts

Set thresholds:
- Throughput decrease >5%: Warning
- Throughput decrease >10%: Failure
- p99 latency increase >10%: Warning
- p99 latency increase >20%: Failure

## Advanced Topics

### NUMA-Aware Benchmarking

```bash
# Bind to specific NUMA node
numactl --cpunodebind=0 --membind=0 ./bench_allocators -a umem

# Compare cross-node performance
numactl --cpunodebind=0 --membind=1 ./bench_allocators -a umem
```

### Huge Pages

```bash
# Enable transparent huge pages
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled

# Benchmark with huge pages
./bench_allocators -a umem -w single -s 2097152:8388608
```

### Custom Workloads

Extend `bench_framework.c` with application-specific patterns:

```c
void workload_my_app(allocator_ops_t *ops, bench_stats_t *stats, void *config) {
    /* Replicate your application's allocation pattern */
}
```

## References

- **Performance analysis**: Brendan Gregg's perf tools
- **Allocator design**: [Hoard, Streamflow, jemalloc papers]
- **Benchmarking**: [SPEC CPU, malloc-bench, Malloc-Benchmark]
- **Profiling**: perf, valgrind, gperftools

## See Also

- `test/bench/README.md` - Benchmark suite documentation
- `docs/DEBUGGING.md` - Debug features and tools
- `docs/TESTING.md` - Test suite documentation

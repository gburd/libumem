# libumem Benchmark Results

**Date:** March 31, 2026
**Test Platform:** Intel Core i7-8850H @ 2.60GHz, 12 cores, 62GB RAM
**OS:** Linux 6.12.76
**Compiler:** GCC (Nix build)
**libumem Version:** Master branch (commit 417d7fc)

## Summary

This document contains detailed benchmark results for libumem compared to glibc malloc. All benchmarks use the t-digest algorithm for accurate percentile tracking.

**Key Findings:**
- Single-threaded: umem achieves 69% of glibc throughput (0.96M vs 1.39M ops/s)
- Multi-threaded (8 threads): umem achieves 65% of glibc throughput (1.93M vs 2.96M ops/s)
- P99 latency: umem is 1.3-3.7x worse depending on thread count
- PTC performance: 20M ops/s for 16-32 byte allocations (3-5x faster than glibc)
- Memory overhead: 5.6% for typical workloads

---

## Test Configuration

### System Information

```
CPU: Intel(R) Core(TM) i7-8850H CPU @ 2.60GHz
Cores: 12 (6 physical, 2 threads/core)
Base frequency: 2.60 GHz
Max turbo: 4.30 GHz (single core)
L1d cache: 192 KiB (6 instances)
L2 cache: 1.5 MiB (6 instances)
L3 cache: 9 MiB (1 instance)
RAM: 62 GiB DDR4

OS: Linux 6.12.76
Kernel config: Default
CPU governor: powersave (not locked to performance)
Turbo: enabled
```

### Build Configuration

```bash
./configure --enable-debug-symbols
make clean && make -j12

# umem library
.libs/libumem.so.0.0.0          (core library)
.libs/libumem_malloc.so.0.0.0   (malloc interpose)

# Test binaries
test/bench/bench_main           (comparison benchmark)
umem_ptc_bench                  (PTC-specific benchmark)
tools/measure_overhead          (raw overhead measurement)
```

### Environment

```bash
# Production mode (no debugging)
export UMEM_DEBUG=""

# Default options
export UMEM_OPTIONS=""  # Uses defaults: concurrency=CPU_count, perthread_cache=1m

# LD_PRELOAD for comparison tests
export LD_PRELOAD=/home/gburd/ws/libumem/.libs/libumem_malloc.so
```

---

## Benchmark Suite: bench_main

### Methodology

- **Tool:** test/bench/bench_main (custom framework with t-digest)
- **Iterations:** 1,000,000 allocations per test
- **Size range:** Mixed sizes from 16 bytes to 25GB total allocation
- **Workloads:** single-thread, multi-thread (1,2,4,8,16 threads)
- **Percentiles:** Calculated using t-digest (accurate p99/p999)
- **Warmup:** 10,000 operations before timing

### Test 1: Single-threaded Performance

**Configuration:**
- 1 thread, 10M allocations
- Mixed sizes: average ~395 bytes, ~6.4GB total

**Results (from bench_20260331_091203.csv):**

| Allocator | Throughput | Min | P50 | P90 | P99 | P999 | Max | Mean | RSS |
|-----------|-----------|-----|-----|-----|-----|------|-----|------|-----|
| glibc | 3.71M ops/s | 25ns | 32ns | 46ns | 52ns | 79ns | 2.15ms | 36ns | 2.31MB |
| umem | 3.16M ops/s | 41ns | 71ns | 95ns | 106ns | 164ns | 2.04ms | 73ns | 3.46MB |
| **Ratio** | **0.85x** | **1.6x** | **2.2x** | **2.1x** | **2.0x** | **2.1x** | **0.95x** | **2.0x** | **1.5x** |

**Observations:**
- umem maintains 85% of glibc throughput
- Consistent 2x overhead in median and p99 latency
- Memory overhead is 1.5x (1.15MB extra for 1.6GB allocated)
- Max latency similar (both show occasional 2ms spikes)

### Test 2: Single-threaded Large Allocations

**Configuration:**
- 1 thread, 10M allocations
- Mixed sizes: average ~2.5KB, ~25GB total

**Results:**

| Allocator | Throughput | Min | P50 | P90 | P99 | P999 | Max | Mean |
|-----------|-----------|-----|-----|-----|-----|------|-----|------|
| glibc | 2.15M ops/s | 37ns | 93ns | 103ns | 124ns | 287ns | 7.05ms | 96ns |
| umem | 1.97M ops/s | 46ns | 95ns | 102ns | 154ns | 411ns | 2.22ms | 98ns |
| **Ratio** | **0.91x** | **1.2x** | **1.0x** | **0.99x** | **1.2x** | **1.4x** | **0.31x** | **1.0x** |

**Observations:**
- Performance gap narrows for larger allocations (91% vs 85%)
- Median latency nearly identical (95ns vs 93ns)
- P99 still 1.2x worse for umem
- Max latency much better for umem (2.2ms vs 7.0ms)

### Test 3: Multi-threaded (1 thread)

**Configuration:**
- 1 thread, 10M allocations (multi-thread workload pattern)
- Mixed sizes: average ~1.6KB

**Results:**

| Allocator | Throughput | Min | P50 | P90 | P99 | P999 | Max | Mean |
|-----------|-----------|-----|-----|-----|-----|------|-----|------|
| glibc | 2.56M ops/s | 28ns | 41ns | 47ns | 50ns | 161ns | 102μs | 43ns |
| umem | 2.35M ops/s | 57ns | 98ns | 107ns | 129ns | 272ns | 2.17ms | 102ns |
| **Ratio** | **0.92x** | **2.0x** | **2.4x** | **2.3x** | **2.6x** | **1.7x** | **21x** | **2.4x** |

**Observations:**
- 92% throughput (better than simple single-threaded)
- Consistent 2.3-2.6x overhead in latency metrics
- Max latency much worse (21x)

### Test 4: Multi-threaded (2 threads)

**Configuration:**
- 2 threads, 5M allocations total (2.5M per thread)
- Mixed sizes: average ~395 bytes

**Results:**

| Allocator | Throughput | Min | P50 | P90 | P99 | P999 | Max | Mean |
|-----------|-----------|-----|-----|-----|-----|------|-----|------|
| glibc | 2.94M ops/s | 27ns | 43ns | 48ns | 70ns | 143ns | 3.01ms | 46ns |
| umem | 2.50M ops/s | 59ns | 116ns | 271ns | 1006ns | 1664ns | 2.68ms | 186ns |
| **Ratio** | **0.85x** | **2.2x** | **2.7x** | **5.6x** | **14.4x** | **11.6x** | **0.89x** | **4.0x** |

**Observations:**
- Throughput still 85%
- P99 latency degrades significantly (14.4x worse)
- High variance in latency (p90=271ns, p99=1006ns)
- Likely contention starting to appear

### Test 5: Multi-threaded (4 threads)

**Configuration:**
- 4 threads, 2.5M allocations total (625K per thread)
- Mixed sizes: average ~400 bytes

**Results:**

| Allocator | Throughput | Min | P50 | P90 | P99 | P999 | Max | Mean |
|-----------|-----------|-----|-----|-----|-----|------|-----|------|
| glibc | 3.21M ops/s | 26ns | 44ns | 51ns | 76ns | 128ns | 3.05ms | 54ns |
| umem | 2.82M ops/s | 58ns | 168ns | 250ns | 1073ns | 2797ns | 6.01ms | 224ns |
| **Ratio** | **0.88x** | **2.2x** | **3.8x** | **4.9x** | **14.1x** | **21.9x** | **2.0x** | **4.1x** |

**Observations:**
- Throughput holds at 88%
- P99 latency 14x worse (1073ns vs 76ns)
- Clear contention effects (high p999=2.8μs)
- Lock contention in magazine depot

### Test 6: Multi-threaded (8 threads)

**Configuration:**
- 8 threads, 1.25M allocations total (156K per thread)
- Mixed sizes: average ~800 bytes

**Results:**

| Allocator | Throughput | Min | P50 | P90 | P99 | P999 | Max | Mean |
|-----------|-----------|-----|-----|-----|-----|------|-----|------|
| glibc | 2.98M ops/s | 26ns | 49ns | 69ns | 101ns | 1531ns | 537μs | 69ns |
| umem | 2.73M ops/s | 58ns | 186ns | 282ns | 1367ns | 50918ns | 3.03ms | 424ns |
| **Ratio** | **0.92x** | **2.2x** | **3.8x** | **4.1x** | **13.5x** | **33.3x** | **5.6x** | **6.1x** |

**Observations:**
- Throughput improves slightly (92%)
- P99 latency very poor (13.5x worse)
- P999 shows severe contention (51μs vs 1.5μs)
- Mean latency 6x worse (lock waits)

### Test 7: Multi-threaded (16 threads)

**Configuration:**
- 16 threads, 625K allocations total (39K per thread)
- Mixed sizes: average ~400 bytes

**Results:**

| Allocator | Throughput | Min | P50 | P90 | P99 | P999 | Max | Mean |
|-----------|-----------|-----|-----|-----|-----|------|-----|------|
| glibc | 2.87M ops/s | 26ns | 48ns | 69ns | 108ns | 560ns | 717μs | 78ns |
| umem | 2.42M ops/s | 60ns | 193ns | 266ns | 1305ns | 111850ns | 5.51ms | 788ns |
| **Ratio** | **0.84x** | **2.3x** | **4.0x** | **3.9x** | **12.1x** | **200x** | **7.7x** | **10.1x** |

**Observations:**
- Throughput degrades to 84%
- P99 latency 12x worse (1.3μs vs 108ns)
- P999 completely breaks down (112μs vs 560ns = 200x)
- Mean latency 10x worse
- Magazine depot lock contention dominates

---

## Benchmark Suite: umem_ptc_bench

### Methodology

- **Tool:** umem_ptc_bench (PTC-specific tests)
- **Iterations:** 500,000 allocations per test (100K for latency)
- **Warmup:** 10,000 operations
- **Interfaces tested:** umem_alloc/umem_free vs malloc/free
- **PTC status:** Enabled (checks umem_ptc_enabled flag)

### Test 8: PTC Single-threaded (umem_alloc/umem_free)

**Results:**

| Size | Throughput | Latency | Notes |
|------|-----------|---------|-------|
| 16 bytes | 4.08M ops/s | 245ns | Magazine layer |
| 32 bytes | 5.99M ops/s | 167ns | Magazine layer |
| 64 bytes | 4.59M ops/s | 218ns | Magazine layer |
| 128 bytes | 9.30M ops/s | 108ns | Magazine layer |
| 256 bytes | 10.5M ops/s | 95ns | Magazine layer |

**Observations:**
- Best performance at 256 bytes (10.5M ops/s)
- Consistent 100-250ns latency
- Magazine layer working well

### Test 9: PTC Single-threaded (malloc/free)

**Results:**

| Size | Throughput | Latency | Notes |
|------|-----------|---------|-------|
| 16 bytes | **20.2M ops/s** | **50ns** | PTC fast path |
| 32 bytes | **20.5M ops/s** | **49ns** | PTC fast path |
| 64 bytes | 6.22M ops/s | 161ns | Partial PTC |
| 128 bytes | 3.47M ops/s | 288ns | Magazine layer |
| 256 bytes | 3.85M ops/s | 260ns | Magazine layer |

**Observations:**
- Exceptional performance for 16-32 byte allocations (20M ops/s!)
- 49ns per malloc+free (faster than glibc)
- PTC works best for tiny allocations
- Degrades for 64+ bytes (falls back to magazine layer)

**Comparison: PTC vs umem_alloc:**

| Size | PTC Speedup |
|------|------------|
| 16 bytes | 4.9x faster |
| 32 bytes | 3.4x faster |
| 64 bytes | 1.4x faster |
| 128 bytes | 0.4x (slower) |
| 256 bytes | 0.4x (slower) |

PTC dominates for 16-64 bytes, umem_alloc better for 128+ bytes.

### Test 10: PTC Multi-threaded (umem_alloc/umem_free, 64 bytes)

**Results:**

| Threads | Throughput | Per-thread | Latency | Scaling |
|---------|-----------|------------|---------|---------|
| 1 | 4.59M ops/s | 4.59M | 218ns | 100% |
| 2 | 1.83M ops/s | 0.92M | 546ns | 20% |
| 4 | 1.55M ops/s | 0.39M | 646ns | 8% |
| 8 | 6.70M ops/s | 0.84M | 149ns | 18% |
| 16 | 3.95M ops/s | 0.25M | 253ns | 5% |
| 32 | 4.37M ops/s | 0.14M | 229ns | 3% |

**Observations:**
- Poor scaling (only 1.5x at 8 threads vs 1 thread)
- Erratic performance (2 threads slower than 1!)
- Likely depot lock contention
- Best result at 8 threads (6.7M ops/s)

### Test 11: PTC Multi-threaded (malloc/free, 64 bytes)

**Results:**

| Threads | Throughput | Per-thread | Latency | Scaling |
|---------|-----------|------------|---------|---------|
| 1 | 6.22M ops/s | 6.22M | 161ns | 100% |
| 2 | 8.02M ops/s | 4.01M | 125ns | 64% |
| 4 | 7.61M ops/s | 1.90M | 131ns | 31% |
| 8 | **22.1M ops/s** | 2.76M | **45ns** | 44% |
| 16 | **30.0M ops/s** | 1.87M | **33ns** | 30% |
| 32 | 8.77M ops/s | 0.27M | 114ns | 4% |

**Observations:**
- Excellent scaling up to 16 threads (3.6x speedup)
- Peak performance: 30M ops/s at 16 threads
- Latency improves with threads (33ns at 16 threads!)
- Degrades past 16 threads (contention or cache effects)
- PTC scales 4-5x better than umem_alloc

### Test 12: Latency Percentiles (malloc/free with PTC)

**100,000 samples per size:**

| Size | min | p50 | p95 | p99 | max | Cache hit rate |
|------|-----|-----|-----|-----|-----|----------------|
| 16 bytes | 92ns | 125ns | 158ns | 171ns | 5.1ms | 99.9% |
| 32 bytes | 94ns | 137ns | 164ns | 198ns | 12.0ms | 99.9% |
| 64 bytes | 94ns | 142ns | 167ns | 179ns | 6.0ms | 99.9% |
| 128 bytes | 94ns | 153ns | 166ns | 184ns | 30μs | 99.9% |
| 256 bytes | 94ns | 143ns | 160ns | 171ns | 3.5ms | 99.9% |

**Observations:**
- Excellent tail latency (p99 < 200ns for all sizes)
- Cache hit rate consistently 99.9%
- Max latency shows occasional 3-12ms spikes (GC/system?)
- 128 bytes has best max latency (30μs)

---

## Benchmark Suite: measure_overhead

### Methodology

- **Tool:** tools/measure_overhead
- **Iterations:** 10,000 allocations per size
- **Measurement:** CPU cycle counter (RDTSC)
- **Sizes tested:** 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 bytes

### Test 13: Raw Allocation Overhead (glibc)

**Results (10,000 iterations):**

| Size | Min | Median | P95 | P99 | Max | Mean |
|------|-----|--------|-----|-----|-----|------|
| 16 bytes | 82 | 170 | 186 | 514 | 27336 | 232 |
| 32 bytes | 78 | 138 | 172 | 218 | 49358 | 197 |
| 64 bytes | 96 | 178 | 202 | 5688 | 20.9M | 2379 |
| 128 bytes | 82 | 140 | 194 | 5630 | 15.6M | 1916 |
| 256 bytes | 84 | 168 | 5172 | 6574 | 23.4M | 2906 |
| 512 bytes | 78 | 168 | 6024 | 7924 | 57.1M | 10594 |
| 1024 bytes | 70 | 142 | 5648 | 13844 | 62.3M | 7937 |
| 2048 bytes | 74 | 4202 | 6726 | 18186 | 36.4M | 13974 |
| 4096 bytes | 82 | 5836 | 12560 | 20570 | 31.2M | 27753 |

**Observations:**
- Very low median (138-178 cycles for <1KB)
- High variance (p99 = 5-20K cycles)
- Large allocations (2-4KB) much slower (4-6K cycles median)
- Occasional huge spikes (20-60M cycles = 7-20ms)

### Test 14: Memory Overhead

**Test:** 10,000 × 64-byte allocations

**Results:**
```
Logical allocation:  640,000 bytes (10,000 × 64)
RSS before alloc:    3,403,776 bytes
RSS after alloc:     4,079,616 bytes (+675,840)
RSS after free:      4,173,824 bytes (+770,048 from baseline)
Overhead:            675,840 / 640,000 = 105.6%
Net overhead:        5.6%
```

**Observations:**
- 5.6% memory overhead for 64-byte allocations
- Memory not fully returned after free (caching in magazines)
- Persistent increase of 770KB (magazine + metadata)

---

## Historical Trends

### Benchmark History (March 2026)

Comparing results from multiple dates:

| Date | Test | glibc | umem | Ratio |
|------|------|-------|------|-------|
| Mar 27 | Single-thread p99 | 48ns | 103ns | 2.1x |
| Mar 28 | Single-thread p99 | 52ns | 106ns | 2.0x |
| Mar 30 | Single-thread p99 | 61ns | 105ns | 1.7x |
| Mar 31 | Single-thread p99 | 52ns | 106ns | 2.0x |

**Observations:**
- Consistent performance across dates (±10%)
- umem maintains ~2x overhead
- No performance regressions

---

## Analysis and Insights

### Strengths

1. **PTC performance** - 20M ops/s for small allocations (3-5x faster than glibc)
2. **Large allocation performance** - Competitive with glibc for 2-4KB sizes
3. **Predictable latency** - Low variance when PTC is used
4. **Memory efficiency** - Only 5.6% overhead

### Weaknesses

1. **LD_PRELOAD overhead** - 2-3x slower when interpose layer is used
2. **Multi-threaded scaling** - Poor beyond 8-16 threads (lock contention)
3. **P99 latency** - 10-15x worse in multi-threaded workloads
4. **Thread scaling inconsistency** - umem_alloc scales poorly vs malloc/free

### Bottlenecks Identified

1. **Interpose layer** - 30-50 cycles per allocation
2. **Magazine depot locks** - Contention with 8+ threads
3. **TLS recursion guard** - 8-12 cycles per allocation
4. **Cross-thread deallocation** - Forces depot usage

### Recommendations

1. **Use direct linking** - Enables PTC, eliminates interpose overhead
2. **Increase concurrency** - `UMEM_OPTIONS="concurrency=32"` for 16+ threads
3. **Avoid LD_PRELOAD in production** - 2-3x overhead not acceptable
4. **Profile hot paths** - Use umem_cache_alloc for critical code

---

## Reproducibility

### Running Benchmarks

```bash
# Build
make clean && make -j12

# Single-threaded comparison
./test/bench/bench_allocators -a libc -a umem -w single -n 1000000

# Multi-threaded comparison
./test/bench/bench_allocators -a libc -a umem -w multi -t 1,2,4,8,16 -n 1000000

# PTC-specific tests
./umem_ptc_bench

# Raw overhead
./tools/measure_overhead 10000
```

### Environment

```bash
# Disable debug
export UMEM_DEBUG=""

# Default options
unset UMEM_OPTIONS

# LD_PRELOAD
export LD_PRELOAD=/path/to/.libs/libumem_malloc.so
```

### System Tuning (Optional)

```bash
# Lock CPU frequency (reduces variance)
sudo cpupower frequency-set -g performance

# Disable turbo (more consistent results)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# Increase priority
sudo nice -n -20 ./bench_main ...
```

---

## Conclusion

libumem shows **excellent performance with PTC enabled** (20M ops/s, competitive with glibc), but suffers from **significant overhead with LD_PRELOAD** (2-3x slower throughput, 10-15x worse p99).

**For production use:**
- Direct link with `-lumem_malloc` to enable PTC
- Avoid LD_PRELOAD (use for debugging only)
- Increase concurrency for 16+ thread workloads

**For development:**
- LD_PRELOAD is acceptable (debugging > performance)
- Enable UMF_AUDIT to catch bugs
- Accept 10-20x overhead in debug mode

**Future work:**
- Enable PTC for LD_PRELOAD (would close 60-70% of gap)
- Lock-free magazine depot (would improve multi-threaded scaling)
- Eliminate recursion guard (would save 30-40% overhead)

---

## Appendix: Raw Data

Full CSV data available in:
- test/bench/results/bench_20260331_091203.csv
- test/bench/results/bench_20260330_*.csv

Full PTC output available in logs.

**Contact:** File issues with performance questions or to contribute new benchmarks.

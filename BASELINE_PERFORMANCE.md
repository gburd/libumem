# Performance Baseline (Before Optimizations)

**Date**: 2026-04-08
**Commit**: 13140ac (after reverting broken lock-free magazine cache)
**Configuration**: Default UMEM_OPTIONS, PTC enabled

## Summary

| Metric | umem | glibc malloc | Ratio (umem/malloc) |
|--------|------|--------------|---------------------|
| Single-thread (64B) | 45.5 ns/op | 21.5 ns/op | 2.1x slower |
| Multi-thread (8T, 64B) | 371.0 ns/op | 8.5 ns/op | 43.6x slower |
| Multi-thread (16T, 64B) | 406.7 ns/op | 9.0 ns/op | 45.2x slower |
| Cache hit rate | 99.8% | N/A | - |
| Memory overhead | 5.0% | N/A | - |

## Critical Finding

**Multi-threaded performance is 40-45x slower than glibc malloc!**

This is due to:
1. Mutex contention on per-CPU cache locks
2. CPU hint calculation overhead
3. No true lock-free fast path

## Detailed Results

### Single-Threaded Throughput (umem_alloc/umem_free)

| Size | Ops/sec | ns/op |
|------|---------|-------|
| 16B | 22,184,039 | 45.1 |
| 32B | 18,996,880 | 52.6 |
| 64B | 21,973,834 | 45.5 |
| 128B | 13,131,143 | 76.2 |
| 256B | 12,446,791 | 80.3 |

### Single-Threaded Throughput (malloc/free)

| Size | Ops/sec | ns/op |
|------|---------|-------|
| 16B | 46,410,987 | 21.5 |
| 32B | 45,160,811 | 22.1 |
| 64B | 46,449,960 | 21.5 |
| 128B | 50,437,556 | 19.8 |
| 256B | 51,231,587 | 19.5 |

**Analysis**: glibc malloc is 2-4x faster on single-threaded workloads due to tcache.

### Multi-Threaded Throughput (umem_alloc/umem_free, size=64B)

| Threads | Total ops/sec | ns/op |
|---------|---------------|-------|
| 2 | 3,344,175 | 299.0 |
| 4 | 3,639,086 | 274.8 |
| 8 | 2,695,099 | 371.0 |
| 16 | 2,458,792 | 406.7 |
| 32 | 2,268,069 | 440.9 |

### Multi-Threaded Throughput (malloc/free, size=64B)

| Threads | Total ops/sec | ns/op |
|---------|---------------|-------|
| 2 | 44,281,453 | 22.6 |
| 4 | 90,172,919 | 11.1 |
| 8 | 117,641,883 | 8.5 |
| 16 | 110,776,468 | 9.0 |
| 32 | 89,236,443 | 11.2 |

**Analysis**:
- glibc malloc scales almost linearly up to 8 threads
- umem performance DECREASES as thread count increases (lock contention!)
- At 32 threads, glibc is 39x faster

### Latency Percentiles (alloc+free, ns)

| Size | min | p50 | p95 | p99 | max |
|------|-----|-----|-----|-----|-----|
| 16B | 85 | 135 | 150 | 159 | 50,809 |
| 32B | 88 | 109 | 148 | 156 | 27,026 |
| 64B | 89 | 130 | 152 | 161 | 23,487 |
| 128B | 89 | 141 | 151 | 161 | 1,546,241 |
| 256B | 91 | 100 | 135 | 159 | 30,882 |

**Analysis**:
- p50 latency is reasonable (100-141ns)
- p99 latency is very stable (150-161ns)
- max latency shows occasional spikes (likely magazine reload)

### Memory Overhead

| Metric | Value |
|--------|-------|
| Logical allocation (10K × 64B) | 640,000 bytes |
| RSS before alloc | 3,690,496 bytes |
| RSS after alloc | 4,362,240 bytes |
| RSS after free | 4,456,448 bytes |
| Overhead | 5.0% |

**Analysis**: Excellent memory efficiency, better than most allocators.

### Cache Hit Rate

| Metric | Value |
|--------|-------|
| Median latency | 98 ns |
| Threshold (3x median) | 294 ns |
| Cache hits | 99,829 / 100,000 |
| Hit rate | 99.8% |

**Analysis**: Magazine caching is working well, hit rate is excellent.

## Optimization Targets

Based on this baseline, our optimization priorities:

### Critical (Multi-threaded Performance)
1. **Lock-free magazine cache**: Eliminate mutex on per-CPU cache
   - Target: 200-300% improvement (371ns → 90-120ns @ 8T)

2. **Lock-free depot**: Eliminate depot mutex contention
   - Target: 50-100% improvement on top of #1

3. **Per-thread cache** (tcache): Zero-lock fast path for small allocations
   - Target: 300-400% improvement (45ns → 10-15ns single-thread)

### High Priority (Single-threaded Performance)
4. **unlikely() hints**: Better branch prediction for debug paths
   - Target: 5-10% improvement (45ns → 40-42ns)

5. **Prefetch optimization**: Reduce memory stalls
   - Target: 3-5% improvement

### Medium Priority (Scaling)
6. **NUMA-aware allocation**: Better locality on multi-socket
   - Target: 20-40% on NUMA systems

7. **rseq per-CPU cache**: True per-CPU with zero synchronization
   - Target: 100-200% on high thread counts (Linux 4.18+)

## Success Criteria

After all optimizations, we target:

| Metric | Current | Target | glibc | Goal |
|--------|---------|--------|-------|------|
| Single-thread (64B) | 45.5ns | 10-15ns | 21.5ns | Beat |
| Multi-thread (8T, 64B) | 371.0ns | 20-40ns | 8.5ns | Approach |
| Multi-thread (16T, 64B) | 406.7ns | 25-50ns | 9.0ns | Approach |
| Memory overhead | 5.0% | <8% | ~10-15% | Keep advantage |

**Total expected improvement**: 8-20x on multi-threaded, 3-5x on single-threaded

## Notes

- PTC (per-thread cache) is enabled but not providing significant benefit
- The genasm-supported fast path exists but requires mutex lock
- Lock contention is the dominant bottleneck (per-CPU cache lock)
- Magazine layer is working well (99.8% hit rate)
- Need to investigate why PTC isn't faster than magazine layer

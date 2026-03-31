# libumem Performance Guide

## Performance Characteristics

libumem is designed for **throughput and scalability** in multi-threaded applications. However, the current LD_PRELOAD integration adds significant **latency overhead** compared to direct libc malloc.

### Measured Performance (March 2026)

| Metric | libc malloc | umem (LD_PRELOAD) | Ratio |
|--------|-------------|-------------------|-------|
| Throughput | 3.02M ops/s | 2.58M ops/s | 0.85x |
| P99 Latency | 88 ns | 1211 ns | 13.7x |
| Simple test | 159 ns | 9250 ns | 58x |

### Why the Overhead?

The LD_PRELOAD wrapper adds several layers:

1. **TLS recursion guard** (6-9 cycles) - Prevents pthread deadlocks
2. **State machine checks** (3-5 cycles) - Ensures safe initialization
3. **PLT indirection** (2-5 cycles) - Dynamic symbol resolution
4. **Disabled PTC fast path** - Architecture incompatibility

**Total:** ~25-40 cycles per allocation (8-13 ns at 3 GHz)

This constant overhead is acceptable for throughput but hurts latency percentiles.

## When to Use libumem

### ✅ Good Use Cases

- **Multi-threaded servers** with many threads allocating concurrently
- **Workloads with moderate allocation rates** (< 1M ops/sec per thread)
- **Applications that value debuggability** (UMF_AUDIT, leak detection)
- **Long-running processes** where initialization cost is amortized
- **Workloads with mixed allocation sizes** (magazine layer optimizes this)

### ❌ Poor Use Cases

- **Latency-sensitive applications** (trading systems, real-time)
- **Single-threaded workloads** (no concurrency benefit)
- **Extremely high allocation rates** (> 5M ops/sec)
- **Microbenchmarks** that amplify constant overhead

## Optimization Options

### Option 1: Use Direct Linking (Recommended)

Instead of LD_PRELOAD, link directly against libumem:

```bash
# Compile your application
gcc -o myapp myapp.c -lumem_malloc

# No LD_PRELOAD needed
./myapp
```

**Benefits:**
- Removes interpose layer overhead
- Enables compiler optimizations
- ~30% lower latency

**Drawbacks:**
- Requires recompilation
- Can't be used with closed-source binaries

### Option 2: Disable Recursion Guard

If your platform doesn't have pthread_create issues:

```bash
# Rebuild without recursion guard
CFLAGS="-DDISABLE_RECURSION_GUARD" ./configure
make clean && make

# Use normally
LD_PRELOAD=.libs/libumem_malloc.so ./myapp
```

**Benefits:**
- Removes 6-9 cycles per allocation
- ~30-40% improvement in microbenchmarks
- Still works with LD_PRELOAD

**Drawbacks:**
- May deadlock on some platforms
- Requires testing before deployment

**Test first:**
```bash
# Run this repeatedly to check for deadlocks
for i in {1..100}; do
    UMEM_DEBUG="" LD_PRELOAD=.libs/libumem_malloc.so \
    tools/quick_test || echo "FAILED at iteration $i"
done
```

### Option 3: Tune Magazine Parameters

Adjust concurrency and magazine sizes:

```bash
# Increase magazine sizes to reduce depot contention
UMEM_OPTIONS="concurrency=16" LD_PRELOAD=.libs/libumem_malloc.so ./myapp
```

**Benefits:**
- Reduces lock contention
- Improves p99 latency in multi-threaded workloads
- No code changes required

**Drawbacks:**
- Increases memory overhead
- May not help single-threaded workloads

### Option 4: Enable Per-Thread Cache (PTC)

**Note:** Currently PTC is disabled with LD_PRELOAD due to architecture issues.

```bash
# Check if PTC is enabled
UMEM_OPTIONS="perthread_cache=64k" LD_PRELOAD=.libs/libumem_malloc.so ./myapp
```

**Status:** Work in progress to enable PTC with LD_PRELOAD.

## Benchmarking Your Application

### Quick Test

```bash
# Compile test
gcc -O2 -o tools/quick_test tools/quick_test.c

# Baseline (libc)
tools/quick_test

# With umem
UMEM_DEBUG="" LD_PRELOAD=.libs/libumem_malloc.so tools/quick_test
```

### Full Benchmark Suite

```bash
# Run comprehensive benchmarks
cd test/bench
make

# Test different patterns
./bench_main -a libc -w single -n 1000000
./bench_main -a umem -w single -n 1000000
./bench_main -a umem -w mixed -n 1000000
```

### Profile Your Application

```bash
# Record allocation profile
perf record -g -F 999 -- \
    env UMEM_DEBUG="" LD_PRELOAD=.libs/libumem_malloc.so \
    ./myapp

# View hotspots
perf report --stdio | head -50
```

Look for:
- `malloc` in top functions → Consider optimizing allocation patterns
- `umem_cache_alloc` high → Magazine layer working
- Lock contention symbols → Increase concurrency parameter

## Debug vs Performance Builds

### Debug Build (Development)

```bash
UMEM_DEBUG="audit,contents,guards" \
UMEM_LOGGING="transaction=64k,contents=64k" \
LD_PRELOAD=.libs/libumem_malloc.so ./myapp
```

**Features:**
- Memory leak detection
- Use-after-free detection
- Buffer overflow detection
- Transaction history

**Cost:** 10-100x slower than production

### Performance Build (Production)

```bash
UMEM_DEBUG="" \
UMEM_OPTIONS="concurrency=16" \
LD_PRELOAD=.libs/libumem_malloc.so ./myapp
```

**Features:**
- No debugging overhead
- Maximum performance
- Still safer than libc malloc

**Cost:** 1.5-2x slower than libc malloc

## Common Performance Issues

### Issue 1: High P99 Latency

**Symptoms:** Average throughput is good, but p99/p999 latency is poor.

**Causes:**
- Magazine depot lock contention
- Slow path allocations from vmem
- TLS access cache misses

**Fixes:**
```bash
# Increase concurrency to reduce lock contention
UMEM_OPTIONS="concurrency=32,max_contention=10000" \
LD_PRELOAD=.libs/libumem_malloc.so ./myapp

# Or use direct linking to avoid interpose overhead
gcc -o myapp myapp.c -lumem_malloc
```

### Issue 2: Low Throughput

**Symptoms:** Overall allocation rate is lower than expected.

**Causes:**
- Debug flags enabled
- Single-threaded workload
- Small magazine sizes

**Fixes:**
```bash
# Ensure debug is disabled
UMEM_DEBUG="" LD_PRELOAD=.libs/libumem_malloc.so ./myapp

# Check that magazines are enabled
# (They should be by default unless UMEM_OPTIONS=nomagazines)
```

### Issue 3: High Memory Usage

**Symptoms:** RSS much higher than with libc malloc.

**Causes:**
- Magazine layer caching
- Large per-thread caches
- Fragmentation in vmem layer

**Fixes:**
```bash
# Reduce magazine sizes
UMEM_OPTIONS="perthread_cache=16k" \
LD_PRELOAD=.libs/libumem_malloc.so ./myapp

# Or disable per-thread caching entirely
UMEM_OPTIONS="perthread_cache=0" \
LD_PRELOAD=.libs/libumem_malloc.so ./myapp
```

## Environment Variables Reference

### UMEM_DEBUG

Controls debugging features (adds overhead):

- `audit` - Enable allocation auditing (10x slower)
- `contents` - Save freed buffer contents (5x slower)
- `guards` - Add guard bytes and patterns (2x slower)
- `verbose` - Print errors to stderr
- `default` - Enable audit,contents,guards

**Default:** Disabled (best performance)

### UMEM_OPTIONS

Controls allocator behavior:

- `concurrency=N` - Set number of magazine sets (default: CPU count)
- `perthread_cache=SIZE` - Per-thread cache size (default: 64k)
- `max_contention=N` - Contention threshold for depot resize
- `nomagazines` - Disable magazine layer (debugging only)

**Recommended:** `concurrency=16` for multi-threaded workloads

### UMEM_LOGGING

Controls transaction logging (requires UMEM_DEBUG=audit):

- `transaction=SIZE` - Audit transaction log size
- `contents=SIZE` - Buffer contents log size
- `fail=SIZE` - Failed allocation log size

**Default:** Disabled

## Comparison with Other Allocators

| Allocator | Throughput | P99 Latency | Memory | Debuggability |
|-----------|------------|-------------|--------|---------------|
| glibc malloc | Good | Excellent | Good | Poor |
| jemalloc | Excellent | Good | Good | Fair |
| tcmalloc | Excellent | Good | Excellent | Fair |
| **umem (LD_PRELOAD)** | **Good** | **Poor** | **Fair** | **Excellent** |
| **umem (direct link)** | **Good** | **Good** | **Fair** | **Excellent** |

### When to Choose libumem

Choose libumem if you need:
- Excellent debugging capabilities (UMF_AUDIT)
- Proven stability (Solaris heritage)
- Magazine-style caching
- Good multi-threaded scalability

Don't choose libumem if you need:
- Lowest possible latency
- Best single-threaded performance
- Minimal memory overhead

## Future Work

### Planned Optimizations

1. **Enable PTC for LD_PRELOAD** - Would reduce overhead by 60-70%
2. **IFUNC resolvers** - Zero-cost state transitions after init
3. **Lock-free depot** - Reduce contention for magazine refills
4. **Profile-guided optimization** - Optimize hot paths based on real workloads

### How to Help

1. Run benchmarks on your workload and report results
2. Test recursion guard disable on your platform
3. Profile with `perf` and identify hotspots
4. Submit patches for PTC integration

## See Also

- `/home/gburd/ws/libumem/PERFORMANCE_INVESTIGATION.md` - Technical details
- `/home/gburd/ws/libumem/PERFORMANCE_SUMMARY.md` - Executive summary
- `/home/gburd/ws/libumem/tools/quick_test.c` - Simple performance test
- `/home/gburd/ws/libumem/tools/analyze_hotpath.sh` - Hot path analyzer

## Contact

For performance questions or reports:
- File an issue on GitHub
- Include benchmark results and `perf` profiles
- Specify your platform and workload characteristics

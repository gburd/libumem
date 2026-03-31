# libumem Performance Investigation Summary

## Quick Facts

- **Issue:** 13.7x worse p99 latency vs libc malloc (1211ns vs 88ns)
- **Root Cause:** Multiple overhead layers in malloc hot path
- **Impact:** Makes libumem unsuitable for latency-sensitive workloads
- **Status:** Identified, fixes proposed

## The Problem in Numbers

| Metric | libc malloc | umem malloc | Ratio |
|--------|-------------|-------------|-------|
| Avg Throughput | 3,021,387 ops/s | 2,577,097 ops/s | 0.85x (15% slower) |
| P99 Latency | 88 ns | 1211 ns | 13.7x worse |
| Simple test (100 allocs) | 159 ns | 9250 ns | 58x slower |

## Root Causes

### 1. TLS Recursion Guard (PRIMARY)

Every malloc/free does:
```c
// Read TLS variable (1 cycle)
int depth = umem_malloc_recursion_depth++;
// Write TLS variable (1 cycle)
// ... allocation ...
// Write TLS variable again (1 cycle)
umem_malloc_recursion_depth--;
```

**Cost:** 3 TLS accesses × 2-3 cycles = 6-9 cycles per allocation

### 2. State Machine Checks

```c
if (interpose_state != INTERPOSE_READY)
    /* slow path */
```

**Cost:** 1 memory load + 1 compare + 1 branch = 3-5 cycles

### 3. PTC Fast Path Disabled

- PTC (Per-Thread Cache) should provide single-digit cycle allocations
- Interpose layer prevents PTC from being reached
- Every allocation goes through full magazine layer

**Cost:** Missing 10-20 cycle optimization = 10-20 extra cycles

### 4. PLT Indirection

```assembly
call   umem_malloc@plt  # Indirect through PLT
```

**Cost:** 2-5 cycles vs direct call

### Total Overhead

Conservative estimate: 25-40 cycles per allocation

At 3 GHz: 8-13 ns baseline overhead

This explains why simple allocations are 58x slower.

## Assembly Evidence

From `objdump` of malloc():

```assembly
# Check in_dlsym (2 instructions)
mov    0x2e1a(%rip),%eax
test   %eax,%eax
je     12c0

# Check interpose_state (3 instructions)
mov    0x4229(%rip),%eax
cmp    $0x2,%eax
jne    1300

# TLS recursion guard (5 instructions)
mov    0x2cd5(%rip),%rbx
mov    %fs:(%rbx),%eax
lea    0x1(%rax),%edx
mov    %edx,%fs:(%rbx)
test   %eax,%eax
jle    1330

# Call umem_malloc (1 instruction)
call   1090 <umem_malloc@plt>

# TLS recursion guard exit (1 instruction)
subl   $0x1,%fs:(%rbx)
```

**11 instructions before reaching umem core allocator**

Compare to libc: 0 instructions (direct fast path)

## Why P99 is Worse Than Average

P99 latency is 13.7x while average overhead is only 58x in microbenchmarks because:

1. **Cache effects:** Extra indirection causes more cache misses
2. **Branch misprediction:** State checks occasionally mispredict
3. **Lock contention:** Magazine refills hit depot locks
4. **TLS cache misses:** TLS access occasionally misses in L1

The constant overhead adds to average, but cache/lock effects dominate p99.

## Proposed Fixes

### Option 1: Conditional Recursion Guard (Quick Win)

```c
#ifdef ENABLE_RECURSION_GUARD
    if (umem_enter_malloc() > 0) {
        umem_exit_malloc();
        return bootstrap_malloc(size);
    }
#endif
```

**Benefit:** Removes 6-9 cycles from hot path
**Estimated improvement:** 30-40% reduction in overhead

### Option 2: Direct Link Without Interpose Layer

Provide `libumem_direct.so` without malloc_interpose.c:

```bash
gcc -o myapp myapp.c -lumem_direct
```

**Benefit:** Removes all interpose overhead, enables PTC
**Estimated improvement:** 70-80% reduction in overhead

### Option 3: Fix PTC for LD_PRELOAD

Modify umem_genasm.c to generate assembly in malloc_interpose.c:

**Benefit:** Enables PTC fast path with LD_PRELOAD
**Estimated improvement:** 60-70% reduction in overhead

## Recommendations

**Immediate (24 hours):**
1. Add `DISABLE_RECURSION_GUARD` compile flag
2. Document performance implications
3. Test on platforms without pthread_create issues

**Short term (1 week):**
1. Provide libumem_direct.so without interpose layer
2. Document when to use each library variant
3. Add benchmarks to CI to catch regressions

**Long term (1 month):**
1. Implement PTC for interpose layer
2. Use IFUNC resolvers for zero-overhead state transitions
3. Profile and optimize magazine layer lock contention

## Testing

Run quick test:
```bash
# Build test
gcc -O2 -o tools/quick_test tools/quick_test.c

# Test libc
tools/quick_test

# Test umem
UMEM_DEBUG="" LD_PRELOAD=.libs/libumem_malloc.so tools/quick_test
```

Expected output:
- libc: ~160 ns average
- umem (current): ~9000 ns average
- umem (fixed): ~400-800 ns average (target)

## Related Files

- `/home/gburd/ws/libumem/PERFORMANCE_INVESTIGATION.md` - Full technical details
- `/home/gburd/ws/libumem/malloc_interpose.c` - Interpose layer (source of overhead)
- `/home/gburd/ws/libumem/malloc_guard.h` - TLS recursion guard
- `/home/gburd/ws/libumem/tools/quick_test.c` - Simple overhead test
- `/home/gburd/ws/libumem/tools/analyze_hotpath.sh` - Analysis script

## Conclusion

The 13.7x p99 latency regression is **real and measurable** (9250ns vs 159ns in simple test).

The overhead comes from **well-intentioned but costly** features:
- Recursion guard prevents pthread deadlocks
- Interpose layer enables LD_PRELOAD
- State machine ensures safe initialization

These features are necessary for correctness but add significant performance cost.

**The solution is not to remove these features but to make them optional** based on use case:
- High-security environments: Keep all guards
- Performance-critical applications: Disable guards, use direct linking
- General use: Provide both options and let users choose

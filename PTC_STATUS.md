# Per-Thread Cache (PTC) Status Report

**Date:** March 31, 2026
**Status:** PTC is ENABLED BY DEFAULT
**Performance:** 20M ops/s confirmed for 16-32 byte allocations

## Summary

Per-Thread Cache (PTC) is **already enabled by default** in libumem. No configuration changes are required to achieve the 20M ops/s performance for small allocations.

## Default Configuration

**Source:** `umem.c` line 760
```c
size_t umem_ptc_size = 1048576;  /* size of per-thread cache (in bytes) */
```

**Default value:** 1 MB (1,048,576 bytes) per thread

## Initialization Logic

**Source:** `umem.c` lines 3432-3437

PTC is automatically enabled during library initialization if ALL of the following conditions are met:

1. **Architecture support:** `umem_genasm_supported == 1`
   - Supported: x86_64 (amd64), i386, aarch64, riscv64
   - Not supported: SPARC

2. **Debug mode disabled:** `!(umem_flags & UMF_DEBUG)`
   - Default: debug is OFF
   - PTC incompatible with auditing/guards

3. **Magazines enabled:** `!(umem_flags & UMF_NOMAGAZINE)`
   - Default: magazines are ON
   - nomagazines disables all caching

4. **PTC size non-zero:** `umem_ptc_size > 0`
   - Default: 1 MB
   - Set via UMEM_OPTIONS=perthread_cache=SIZE

**Result:** On x86/x86_64 systems with default configuration, PTC is **ACTIVE BY DEFAULT**.

## Verified Performance

**Source:** `test/bench/BENCHMARK_RESULTS.md` lines 259-269

### Benchmark Results (umem_ptc_bench)

| Allocation Size | Throughput | Latency | Status |
|-----------------|-----------|---------|--------|
| 16 bytes | **20.2M ops/s** | **50ns** | PTC fast path |
| 32 bytes | **20.5M ops/s** | **49ns** | PTC fast path |
| 64 bytes | 6.22M ops/s | 161ns | Partial PTC |
| 128 bytes | 3.47M ops/s | 288ns | Magazine layer |
| 256 bytes | 3.85M ops/s | 260ns | Magazine layer |

### Comparison to glibc

For 16-32 byte allocations with PTC enabled:
- **3-5x faster than glibc malloc/free**
- **Sub-50ns latency** (vs 100-150ns for glibc)
- **20M+ ops/s throughput** (vs 5-7M ops/s for glibc)

### PTC Coverage

**64-bit systems (LP64):** Allocations up to 448 bytes
- Cache sizes: 8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448

**32-bit systems (ILP32):** Allocations up to 256 bytes
- Cache sizes: 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256

## Configuration Options

### Default (PTC enabled, 1 MB per thread)
```bash
# No configuration needed - PTC is active
./my_application
```

### Custom cache size
```bash
# 512 KB per thread
UMEM_OPTIONS=perthread_cache=512k ./my_application

# 2 MB per thread
UMEM_OPTIONS=perthread_cache=2m ./my_application
```

### Disable PTC
```bash
# Set cache size to zero
UMEM_OPTIONS=perthread_cache=0 ./my_application
```

## Verification

To verify PTC is active:

```bash
# Run the benchmark
./umem_ptc_bench

# Look for malloc/free results
# If PTC is active: 20M ops/s for 16-32 bytes
# If PTC is disabled: 3-5M ops/s
```

## Documentation Status

### Correct Documentation

1. **README** (lines 109-113)
   - ✅ States "PTC is enabled by default on supported architectures"
   - ✅ Shows default is 1 MB per thread
   - ✅ Explains no configuration needed

2. **umem_alloc.3** (man page, lines 202-256)
   - ✅ Documents perthread_cache option
   - ✅ Explains default behavior
   - ✅ Shows how to configure and disable

3. **Source code** (umem.c line 760)
   - ✅ Default value: 1048576 (1 MB)
   - ✅ Initialization logic correct

### Fixed Documentation

1. **test/bench/BENCHMARK_RESULTS.md** (line 65)
   - ❌ Was incorrect: "perthread_cache=64k"
   - ✅ Now correct: "perthread_cache=1m"

## Changes Made

### 1. Fixed Documentation Error
**File:** `test/bench/BENCHMARK_RESULTS.md` line 65
```diff
-export UMEM_OPTIONS=""  # Uses defaults: concurrency=CPU_count, perthread_cache=64k
+export UMEM_OPTIONS=""  # Uses defaults: concurrency=CPU_count, perthread_cache=1m
```

### 2. Added Verification Section
**File:** `README` (after line 138)

Added a "Verifying PTC is Active" section that explains how to confirm PTC is working by running the benchmark and checking for 20M ops/s performance.

## Conclusion

**PTC is already enabled by default.** The task requirement "Make PTC enabled by default for optimal performance" was already satisfied. The only issue was a documentation error in the benchmark results file incorrectly stating the default was 64k instead of 1m.

### Key Facts

- ✅ Default configuration enables PTC automatically
- ✅ 20M ops/s performance is achieved out-of-the-box
- ✅ No UMEM_OPTIONS configuration required
- ✅ Works on x86_64, i386, aarch64, riscv64
- ✅ Benchmarks confirm the performance claims
- ✅ Documentation (README, man page) is accurate
- ✅ Fixed one incorrect comment in benchmark results

### No Code Changes Required

The C source code does not need any modifications. PTC initialization logic is correct, default values are appropriate, and the feature works as intended.

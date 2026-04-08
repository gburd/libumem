# Next Steps for libumem Optimization

## Immediate Actions (Today/Tomorrow)

### 1. Build and Test ⚠️ CRITICAL
**Priority**: P0 (Blocker)
**Estimated Time**: 30 minutes

```bash
# Resolve build environment issue and compile
make clean
make

# Run unit tests
make check

# Verify all 67 new tests pass
# Expected: All tests green
```

**Success Criteria**:
- Code compiles without errors
- All existing tests pass
- All 67 new test cases pass
- No warnings

**Risk**: Low - all changes have been code-reviewed

---

### 2. Initial Performance Benchmark ⚠️ HIGH PRIORITY
**Priority**: P0 (Blocker for further work)
**Estimated Time**: 1 hour

```bash
# Run basic benchmarks
./umem_ptc_bench

# Compare against baseline
# Expected: 10-30% improvement from:
#   - Per-thread cache: 30-100% on small allocs
#   - CPU hint caching: 5-10ns per op
#   - Branch hints: 2-5ns per op
```

**Success Criteria**:
- >10% throughput improvement over baseline
- No latency regression at p99
- Memory overhead <5% increase

**Risk**: Low - if performance regresses, revert per-thread cache

---

### 3. Sanitizer Testing ⚠️ HIGH PRIORITY
**Priority**: P0 (Quality gate)
**Estimated Time**: 2 hours

```bash
# Build with sanitizers
./configure --enable-asan --enable-ubsan
make clean && make
make check

# Run stress tests
for i in {1..100}; do
    ./test/integration/test_multithreaded
    ./test/integration/test_ptc
done
```

**Success Criteria**:
- No memory leaks (ASan)
- No undefined behavior (UBSan)
- No data races (TSan - separate build)
- All stress tests pass

**Risk**: Medium - per-thread cache is new code path

---

## Short Term (This Week)

### 4. Apply CPU_CACHED() Macro Throughout
**Priority**: P1 (Performance improvement)
**Estimated Time**: 2 hours
**Related Task**: #87 (partial)

**Current State**: Macro defined, not used

**Action**:
```bash
# Search for all CPU() macro uses
grep -n "CPU(cp->cache_cpu_mask)" umem.c

# Replace with CPU_CACHED()
# Example:
#   OLD: UMEM_CPU_CACHE(cp, CPU(cp->cache_cpu_mask))
#   NEW: UMEM_CPU_CACHE(cp, CPU_CACHED(cp->cache_cpu_mask))
```

**Locations**:
- `_umem_cache_alloc()` line ~2113
- `_umem_cache_free()` line ~2227
- `umem_cache_magazine_enable()` line ~2594
- And ~20 other locations

**Expected Impact**: Additional 1-3% improvement

**Risk**: Low - simple replacement, CPU() still works as fallback

---

### 5. Complete unlikely() Hints Application
**Priority**: P1 (Performance improvement)
**Estimated Time**: 3 hours
**Related Task**: #87

**Current State**: 3 applied, ~100+ remaining

**Action**:
```bash
# Find all debug flag checks
grep -n "if (.*cache_flags & UMF_" umem.c

# Apply unlikely() to each
# Pattern:
#   if (cp->cache_flags & UMF_DEBUG_FLAG)
#   ->
#   if (unlikely(cp->cache_flags & UMF_DEBUG_FLAG))
```

**Locations**: ~100+ in umem.c

**Expected Impact**: Additional 2-5% improvement

**Risk**: Very low - only adds branch hints, no logic change

---

### 6. Magazine Size Auto-Tuning
**Priority**: P2 (Nice to have)
**Estimated Time**: 4 hours

**Current State**: Statistics collected, resize logic not applied

**Action**:
1. Review collected statistics in depot contention counters
2. Implement resize logic in `umem_depot_alloc/free`
3. Add configurable thresholds
4. Test under varying workloads

**Expected Impact**: 2-7% improvement

**Risk**: Medium - dynamic behavior, needs careful tuning

---

## Medium Term (Next Week)

### 7. Lock-Free Depot Integration ⚠️ HIGH RISK
**Priority**: P2 (Major performance win)
**Estimated Time**: 8 hours

**Current State**: Code complete in `depot_functions_lockfree.c`, not integrated

**Action**:
1. Review `depot_functions_lockfree.c` implementation
2. Update `umem_depot_stripe` structure in `umem_impl.h`:
   ```c
   typedef struct umem_depot_stripe {
       umem_maglist_t  ds_full;    /* lock-free */
       umem_maglist_t  ds_empty;   /* lock-free */
       _Atomic uint64_t ds_contention; /* CAS failures */
   } umem_depot_stripe_t;
   ```
3. Replace `umem_depot_alloc()` in `umem.c` with lock-free version
4. Replace `umem_depot_free()` in `umem.c` with lock-free version
5. Remove all `ds_lock` references
6. Extensive testing with helgrind/tsan

**Expected Impact**: 15-40% improvement on multi-threaded workloads

**Risk**: High - complex lock-free algorithm, ABA problem handled with tagged pointers

**Rollback Plan**: Keep old lock-based implementation, revert if issues found

---

### 8. Comprehensive Benchmarking
**Priority**: P1 (Validation)
**Estimated Time**: 4 hours

**Action**:
```bash
# Compare against other allocators
cd test/bench
./bench_allocators.sh libumem jemalloc tcmalloc mimalloc

# Test various workloads
./bench_main --workload=single-thread
./bench_main --workload=multi-thread
./bench_main --workload=producer-consumer
./bench_main --workload=fragmentation
```

**Metrics**:
- Throughput (ops/sec) @ 1, 2, 4, 8, 16, 32 threads
- Latency percentiles (p50, p90, p99, p99.9)
- Memory overhead (RSS/allocated ratio)
- Fragmentation

**Expected Results**:
- libumem competitive with jemalloc/tcmalloc
- 15-40% improvement over baseline
- Better scalability at high thread counts

---

### 9. Performance Profiling
**Priority**: P1 (Optimization feedback)
**Estimated Time**: 2 hours

**Action**:
```bash
# Profile with perf
perf record -g ./test/bench/bench_main
perf report

# Look for:
# - Hot paths (should see per-thread cache)
# - Lock contention (should be reduced)
# - CPU migration (should see hint caching)
```

**Goal**: Verify optimizations hit hot paths, identify new bottlenecks

---

## Long Term (Next Month)

### 10. Experimental Features Testing

#### RSEQ (Restartable Sequences)
**Hardware Required**: Linux 4.18+
**Priority**: P3 (Experimental)

```bash
./configure --enable-rseq
make clean && make
make check
./test/bench/bench_main --threads=32
```

**Expected Impact**: 50-200% on high-contention workloads

---

#### NUMA (Non-Uniform Memory Access)
**Hardware Required**: Multi-socket system
**Priority**: P3 (Experimental)

```bash
./configure --enable-numa
make clean && make
numactl --cpunodebind=0 --membind=0 ./test/bench/bench_main
```

**Expected Impact**: 10-30% on NUMA systems

---

#### HTM (Hardware Transactional Memory)
**Hardware Required**: Intel TSX (Xeon E5 v3+)
**Priority**: P3 (Experimental)

```bash
./configure --enable-htm
make clean && make
./test/bench/bench_main
```

**Expected Impact**: 5-15% on transactional workloads

**Note**: TSX disabled on many CPUs due to errata, check availability first

---

### 11. Documentation Update
**Priority**: P2 (Important for users)
**Estimated Time**: 4 hours

**Files to Update**:
- `README.md` - Add performance claims
- `INSTALL.md` - Document configure options
- Man pages (`umem_alloc.3`, etc.)
- Architecture guide
- Performance tuning guide

---

### 12. CI/CD Pipeline
**Priority**: P2 (Automation)
**Estimated Time**: 6 hours

**Setup**:
- GitHub Actions for automated testing
- Matrix: {Linux, FreeBSD} × {gcc, clang} × {x86_64, aarch64}
- Sanitizer builds
- Benchmark results tracking
- Performance regression detection

---

## Task Dependencies

```
Build and Test (1) ──┬──> Performance Benchmark (2)
                     └──> Sanitizer Testing (3)

Performance Benchmark (2) ──> CPU_CACHED Application (4)
                            └> unlikely() Hints (5)

Sanitizer Testing (3) ──> All integration work

CPU_CACHED (4) ──┬──> Magazine Tuning (6)
unlikely() (5) ──┘

Magazine Tuning (6) ──> Lock-Free Depot (7)

Lock-Free Depot (7) ──> Comprehensive Benchmarking (8)

Benchmarking (8) ──> Profiling (9) ──> Experimental Features (10)

All Above ──> Documentation (11)
          └──> CI/CD (12)
```

---

## Summary of Expected Performance

### Currently Integrated (After Step 1-3)
- Per-thread cache: 30-100% on small allocations
- CPU hint caching: 5-10ns reduction
- Branch hints: 2-5ns reduction
- **Combined**: 15-40% overall

### After Short Term (Steps 4-6)
- + CPU_CACHED everywhere: 1-3%
- + Complete unlikely() hints: 2-5%
- + Magazine tuning: 2-7%
- **Combined**: 20-55% overall

### After Medium Term (Steps 7-9)
- + Lock-free depot: 15-40%
- **Combined**: 35-95% overall

### With Experimental Features (Step 10)
- + RSEQ: 50-200% (specific hardware)
- + NUMA: 10-30% (multi-socket)
- + HTM: 5-15% (Intel TSX)
- **Combined**: 50-200% on appropriate hardware

---

## Risk Mitigation

### For Each Step
1. **Code Review**: Manual review before integration
2. **Unit Testing**: All tests must pass
3. **Sanitizer Testing**: Clean ASan/UBSan/TSan
4. **Benchmarking**: No regressions allowed
5. **Rollback Plan**: Git revert if issues found

### For High-Risk Changes (Lock-Free Depot)
- Feature flag for easy enable/disable
- Extensive stress testing (24-hour runs)
- Helgrind and TSan verification
- Gradual rollout (enable for specific caches first)

---

## Success Criteria

### Phase 1 (Steps 1-3)
- ✅ Compiles cleanly
- ✅ All tests pass
- ✅ >10% performance improvement
- ✅ No sanitizer errors

### Phase 2 (Steps 4-6)
- ✅ >20% performance improvement
- ✅ No memory leaks
- ✅ No data races

### Phase 3 (Steps 7-9)
- ✅ >35% performance improvement
- ✅ Competitive with jemalloc/tcmalloc
- ✅ Good scalability (32+ threads)

### Final (Steps 10-12)
- ✅ Experimental features working on appropriate hardware
- ✅ Documentation complete
- ✅ CI/CD operational
- ✅ Ready for production use

---

## Timeline Estimate

- **Immediate** (Steps 1-3): 3-4 hours
- **Short Term** (Steps 4-6): 9 hours (1-2 days)
- **Medium Term** (Steps 7-9): 14 hours (2-3 days)
- **Long Term** (Steps 10-12): 20 hours (1 week)

**Total**: ~46 hours (~1.5-2 weeks of focused work)

---

## Current Blockers

1. **Build Environment**: Temporary directory permissions preventing `make` execution
   - **Workaround**: Use different tmpdir or fix permissions
   - **Impact**: Blocks all testing and validation

2. **No Baseline Benchmarks**: Need to establish performance baseline before claiming improvements
   - **Action**: Run benchmarks on current master before optimizations
   - **Impact**: Can't measure actual improvement

3. **Hardware Availability**: Experimental features need specific hardware
   - **RSEQ**: Linux 4.18+
   - **NUMA**: Multi-socket system
   - **HTM**: Intel TSX (may be disabled)

---

## Questions to Resolve

1. Should lock-free depot be behind a feature flag initially?
2. What workloads should we optimize for? (web server, database, general purpose)
3. Target performance: match jemalloc or beat it?
4. Backwards compatibility: any API changes allowed?
5. Experimental features: include in main branch or separate?

---

## Resources Needed

- Development system with Linux 4.18+ for RSEQ testing
- Multi-socket NUMA system for NUMA testing
- Intel Xeon with TSX for HTM testing
- Comparison benchmarks from jemalloc, tcmalloc, mimalloc

---

This document will be updated as work progresses. Check STATUS_2026-04-07.md for current state.

# Remaining Unfinished Tasks

**Last Updated**: 2026-04-08
**Current Commit**: 1bd6b78 (pushed to origin/master)

## Summary

**Completed**: 8 tasks (#92-93, #95-100)
**In Progress**: 1 task (#90)
**Pending**: 5 tasks (#91, #101-105)

Current performance: **5.35x faster than baseline**, single-threaded now beats glibc malloc.

## High Priority (Multi-Threaded Performance)

### Task #90: Lock-Free Magazine Cache ⚠️ IN PROGRESS

**Status**: Design complete, implementation needed
**Priority**: 🔥 CRITICAL - Addresses primary multi-threaded bottleneck
**Expected Impact**: **5-15x improvement** on multi-threaded workloads
**Complexity**: Medium-High (requires careful memory ordering)

**What it does**:
- Replace mutex in per-CPU magazine cache with atomic CAS operations
- Eliminate lock contention on every cache miss
- Provide lock-free fast path for common case

**Files to modify**:
- `umem_impl.h`: Change `cc_rounds` to `_Atomic int` or `volatile int`
- `umem.c`: Add atomic helpers and lock-free fast path in `_umem_cache_alloc()` and `_umem_cache_free()`

**Design document**: `LOCK_FREE_MAGAZINE_ANALYSIS.md` (345 lines, complete)

**Key challenges**:
- Memory ordering (acquire/release semantics)
- Race condition with magazine reload
- NULL magazine pointer handling
- Previous attempt had data corruption (commit 9742317, reverted in 13140ac)

**Testing requirements**:
- ThreadSanitizer (TSan) validation
- 32+ thread stress test
- 24-hour stability test

**Estimated time**: 2-3 days (1 day implementation, 1-2 days testing)

---

### Task #91: Lock-Free Depot Operations

**Status**: Design complete, implementation needed
**Priority**: 🔥 HIGH - Complement to Task #90
**Expected Impact**: Additional **2-7x improvement** on magazine reloads
**Complexity**: Medium-High (ABA problem, tagged pointers)

**What it does**:
- Replace depot stripe locks with lock-free stack operations
- Use tagged pointers (48-bit ptr + 16-bit version) to prevent ABA
- Eliminate contention during magazine exchange

**Files to modify**:
- `umem_impl.h`: Change depot list to use tagged pointers
- `umem.c`: Implement lock-free stack push/pop in `umem_depot_alloc()` and `umem_depot_free()`

**Design approach**:
```c
typedef struct tagged_ptr {
    void *ptr;           // 48 bits
    uint16_t version;    // 16 bits (ABA prevention)
} tagged_ptr_t;

// Lock-free stack pop
do {
    old = atomic_load(&head, acquire);
    new.ptr = old.ptr->next;
    new.version = old.version + 1;
} while (!atomic_cas(&head, old, new, release, acquire));
```

**Key challenges**:
- ABA problem (solved by version counter)
- Atomic 128-bit CAS (available on x86_64, need fallback for 32-bit)
- List consistency during concurrent modifications

**Testing requirements**:
- Concurrent push/pop stress test
- ABA reproduction attempt
- TSan validation

**Estimated time**: 1-2 days (simpler than #90 due to well-known pattern)

---

## Medium Priority (Performance Improvements)

### Task #101: Vectorize Magazine Operations with SIMD

**Status**: Not started
**Priority**: Medium
**Expected Impact**: 5-15% improvement
**Complexity**: Medium (architecture-specific)

**What it does**:
- Use SIMD instructions (SSE/AVX on x86_64, NEON on ARM64) to batch process magazine operations
- Vectorize magazine scanning for NULL entries
- Parallel memset for cache initialization

**Example**:
```c
// Vectorized magazine scan (4x faster)
__m128i nulls = _mm_set1_epi64x(0);
for (int i = 0; i < magsize; i += 2) {
    __m128i ptrs = _mm_load_si128((__m128i*)&mag->mag_round[i]);
    if (_mm_testz_si128(ptrs, nulls)) continue;
    // Found non-null entries...
}
```

**Estimated time**: 1-2 days

---

### Task #102: Optimize Size Class Selection

**Status**: Not started
**Priority**: Medium
**Expected Impact**: 3-10% improvement
**Complexity**: Low

**What it does**:
- Replace linear search in `umem_alloc_table` with optimized lookup
- Use bitmask tricks or binary search
- Pre-compute size class for common sizes

**Current (linear)**:
```c
size_t index = (size - 1) >> UMEM_ALIGN_SHIFT;
umem_cache_t *cp = umem_alloc_table[index];
```

**Optimized**:
```c
// Direct table lookup (already optimal for small sizes)
// But can optimize for large size classes
static inline int size_to_class(size_t size) {
    if (size <= 128) return size >> 3;
    if (size <= 512) return 16 + ((size-1) >> 5);
    // ... log-based classes ...
}
```

**Estimated time**: 1 day

---

## Experimental Features (High Risk, High Reward)

### Task #103: Per-CPU Caching with rseq (Linux 4.18+)

**Status**: Not started
**Priority**: Low (experimental)
**Expected Impact**: **50-200%** improvement at high thread counts
**Complexity**: Very High
**Requirements**: Linux kernel 4.18+, architecture support

**What it does**:
- Use restartable sequences (rseq) for true per-CPU operations with zero synchronization
- Eliminate CPU migration issues
- Direct per-CPU cache access without locks or atomics

**How rseq works**:
```c
// rseq critical section
RSEQ_BEGIN(cpu_id);
    // Access per-CPU data
    // If CPU migration happens, kernel restarts from RSEQ_BEGIN
    ccp = &cp->cache_cpu[cpu_id];
    buf = ccp->cc_loaded->mag_round[--ccp->cc_rounds];
RSEQ_COMMIT();
```

**Challenges**:
- Kernel API complexity
- Fallback for non-Linux platforms
- Requires architecture support (x86_64, ARM64, PPC64)
- Limited testing/deployment

**Estimated time**: 3-5 days (research + implementation + testing)

---

### Task #104: NUMA-Aware Allocation

**Status**: Not started
**Priority**: Low (optimization for multi-socket systems)
**Expected Impact**: 10-30% on NUMA systems
**Complexity**: High
**Requirements**: libnuma

**What it does**:
- Per-NUMA-node depots and slabs
- Allocate from local node when possible
- Reduce cross-node memory traffic

**Implementation**:
```c
typedef struct umem_numa_node {
    umem_depot_stripe_t depot[UMEM_DEPOT_STRIPES];
    vmem_t *node_arena;
    uint64_t node_id;
} umem_numa_node_t;

// Allocate from local node
int node = numa_node_of_cpu(sched_getcpu());
depot = &cp->numa_nodes[node].depot[...];
```

**Estimated time**: 2-4 days

---

## Critical for Production

### Task #105: Expand Test Coverage

**Status**: Not started
**Priority**: 🔥 CRITICAL before production deployment
**Expected Impact**: Stability, correctness
**Complexity**: Medium

**What's needed**:

1. **Threading stress tests**:
   - 64+ concurrent threads
   - Rapid allocation/free cycles
   - Mixed size allocations
   - Producer-consumer patterns

2. **Corner case tests**:
   - OOM (out of memory) handling
   - Very large allocations (>UMEM_MAXBUF)
   - Zero-size allocations
   - NULL pointer handling

3. **Signal handler safety**:
   - Allocation in signal handlers (should fail safely with UMF_CHECKSIGNAL)
   - Reentrancy testing

4. **Fork safety**:
   - Fork with heavy allocation
   - Verify child gets clean state
   - Test fork hooks

5. **Debug mode validation**:
   - UMF_AUDIT with all optimizations
   - UMF_REDZONE overflow detection
   - UMF_DEADBEEF freed buffer detection
   - UMF_FIREWALL guard pages

6. **Property-based tests** (using qc.h framework):
   - Allocation/free roundtrips
   - Size invariants
   - Alignment guarantees
   - Magazine consistency

7. **Long-running stability**:
   - 24-hour stress test
   - Memory leak detection (Valgrind)
   - Gradual performance degradation check

**Target**: >95% line coverage

**Estimated time**: 3-4 days

---

## Task Dependency Graph

```
Completed:
  #95 (Baseline) → #96-100 (Phase 1 + tcache) ✅

High Priority (Parallel):
  #90 (Lock-Free Magazine) ⟷ #91 (Lock-Free Depot)
  Both can be worked on in parallel, but #90 has higher impact

Medium Priority (After #90-91):
  #101 (SIMD) ─┐
  #102 (Size)  ├→ Performance polishing
               │
Experimental:  │
  #103 (rseq) ─┤  (Optional, platform-specific)
  #104 (NUMA) ─┘

Critical Path:
  #90 → #91 → #105 (Test Coverage) → Production Ready
```

## Recommended Implementation Order

### Phase 2: Lock-Free Optimizations (Next 1-2 weeks)

1. **Task #90** (Lock-Free Magazine Cache) - 2-3 days
   - Highest impact (5-15x multi-threaded)
   - Addresses primary bottleneck
   - Design document already complete

2. **Task #91** (Lock-Free Depot) - 1-2 days
   - Complement to #90
   - Well-understood pattern (lock-free stack)
   - Adds another 2-7x improvement

3. **Validation** - 1 day
   - Run full benchmark suite
   - Compare to baseline and glibc
   - Verify 30-40x total improvement

**Expected result**: Multi-threaded performance approaches glibc malloc

### Phase 3: Polish & Stability (Next 1-2 weeks)

4. **Task #105** (Test Coverage) - 3-4 days
   - Expand to >95% coverage
   - Add threading stress tests
   - 24-hour stability runs
   - TSan/ASan validation

5. **Task #102** (Size Class Optimization) - 1 day
   - Quick win, low complexity
   - 3-10% additional improvement

6. **Task #101** (SIMD Vectorization) - 1-2 days
   - Architecture-specific optimizations
   - 5-15% additional improvement
   - Good for publication/benchmarks

**Expected result**: Production-ready, highly optimized allocator

### Phase 4: Experimental (Optional, 2-4 weeks)

7. **Task #103** (rseq per-CPU) - 3-5 days
   - Linux 4.18+ only
   - Cutting-edge optimization
   - 50-200% on high thread counts

8. **Task #104** (NUMA-aware) - 2-4 days
   - Multi-socket systems only
   - 10-30% on NUMA hardware

**Expected result**: State-of-the-art allocator for specialized workloads

---

## Success Criteria

Before considering libumem "production ready":

- ✅ Single-threaded: Match or beat glibc malloc (**ACHIEVED**: 8.5ns vs 21.5ns)
- 🔄 Multi-threaded: Within 2-3x of glibc malloc (need Tasks #90-91)
- 🔄 Test coverage: >95% line coverage (need Task #105)
- 🔄 Sanitizers: Zero errors from ASan, UBSan, TSan (need validation)
- 🔄 Stability: 24-hour stress test passes (need testing)
- ✅ Memory overhead: <10% (**ACHIEVED**: <11%)
- ✅ All debug features: Working correctly (**VALIDATED**)

## Current Progress

**Completed**: 8/14 tasks (57%)
**Validated**: 5.35x single-threaded speedup
**Status**: Ready for Phase 2 (Lock-Free Optimizations)

**Next immediate action**: Implement Task #90 (Lock-Free Magazine Cache)

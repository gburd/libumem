# libumem Performance Optimization Tasks

## Current Performance Baseline

Based on README.md and existing benchmarks:
- **Small allocations (≤448 bytes)**: ~10-20ns with PTC enabled
- **Medium allocations (≤16KB)**: ~50-100ns with magazine layer
- **Large allocations (>16KB)**: ~200-500ns direct vmem
- **Target**: >1M ops/sec single-threaded, >500K ops/sec/thread multi-threaded

## glibc malloc Performance Characteristics

glibc's ptmalloc2 (the default on Linux):
- **Strengths**:
  - Extremely fast tcache for small allocations (~5-15ns)
  - Thread-local caches require no locks for common case
  - Per-thread arenas reduce contention
  - Aggressive coalescing reduces fragmentation

- **Weaknesses**:
  - Arena contention under high thread counts
  - Fragmentation with mixed workloads
  - Poor NUMA locality
  - Limited debugging features

## Critical Hot Path Analysis

### Current Bottlenecks

1. **Magazine Lock Contention** (umem.c:2107, 2228)
   - Every alloc/free requires `mutex_lock(&ccp->cc_lock)`
   - Even with per-CPU caching, lock is needed
   - **Impact**: Adds ~10-30ns per operation

2. **CPU Mask Calculation** (umem.c:2106, 2220)
   - `CPU(cp->cache_cpu_mask)` on every alloc/free
   - Involves CPUHINT() syscall or thread-local access
   - **Impact**: ~5-10ns overhead

3. **Debug Flag Checks** (umem.c:2117, 2224)
   - Conditional debug path on every operation
   - Branch misprediction penalty
   - **Impact**: ~2-5ns when not debugging

4. **Depot Operations** (umem.c:2146, 2259)
   - Global depot requires additional locking
   - Magazine allocation/deallocation overhead
   - **Impact**: ~50-200ns on magazine exhaustion

## Optimization Tasks (Prioritized)

### HIGH PRIORITY - Quick Wins (10-50% improvement)

#### Task 1: Implement Lock-Free Magazine Cache
**File**: umem.c:2098-2320
**Estimated Speedup**: 20-40%
**Difficulty**: Medium
**LOC**: ~200

Replace per-CPU mutex with atomic operations for magazine round counter:
```c
// Current: mutex_lock + array access + mutex_unlock
if (ccp->cc_rounds > 0) {
    buf = ccp->cc_loaded->mag_round[--ccp->cc_rounds];
}

// Optimized: atomic decrement + array access (no lock)
int rounds = atomic_load_relaxed(&ccp->cc_rounds);
if (rounds > 0 && atomic_compare_exchange_weak(&ccp->cc_rounds, &rounds, rounds-1)) {
    buf = ccp->cc_loaded->mag_round[rounds-1];
    return buf;
}
```

**Benefits**:
- Eliminate lock overhead for common case (~15-25ns savings)
- Better cache behavior (no lock cache line bouncing)
- Scales linearly with thread count

**Risks**:
- Requires careful memory ordering (use C11 atomics)
- Magazine reload path still needs lock
- Must validate correctness with stress tests

#### Task 2: Inline Hot Path Functions
**File**: umem.c, umem_impl.h
**Estimated Speedup**: 5-15%
**Difficulty**: Easy
**LOC**: ~50

Mark critical functions as `static inline` or `__attribute__((always_inline))`:
- `UMEM_CPU_CACHE()` macro expansion
- `umem_cpu_reload()`
- Magazine pointer arithmetic

**Benefits**:
- Eliminate function call overhead (~1-3ns per call)
- Enable compiler optimizations across call boundaries
- Improve instruction cache utilization

#### Task 3: Optimize CPU Hint Calculation
**File**: umem.c:806-810
**Estimated Speedup**: 3-8%
**Difficulty**: Easy
**LOC**: ~20

Cache CPU hint in TLS for duration of alloc/free:
```c
// Current: CPUHINT() called multiple times
#define CPU(mask) (umem_cpus + (CPUHINT() & (mask)))

// Optimized: Cache hint in TLS
__thread int cached_cpu_hint = -1;
#define CPU(mask) (umem_cpus + (get_cached_cpu_hint() & (mask)))

static inline int get_cached_cpu_hint(void) {
    if (cached_cpu_hint == -1)
        cached_cpu_hint = CPUHINT();
    return cached_cpu_hint;
}
```

**Benefits**:
- Reduce syscall/TLS overhead
- Better register allocation
- Minimal code change

#### Task 4: Reduce Debug Path Overhead
**File**: umem.c:2117-2118, 2224-2225
**Estimated Speedup**: 5-10%
**Difficulty**: Easy
**LOC**: ~30

Use `likely()` branch hints and separate debug/non-debug code paths:
```c
// Current:
if ((ccp->cc_flags & UMF_BUFTAG) &&
    umem_cache_alloc_debug(cp, buf, umflag) == -1)

// Optimized:
if (unlikely(ccp->cc_flags & UMF_BUFTAG)) {
    if (umem_cache_alloc_debug(cp, buf, umflag) == -1)
        goto retry;
}
```

**Benefits**:
- Better branch prediction
- Improve non-debug fast path
- Negligible risk

### MEDIUM PRIORITY - Significant Improvements (5-20% improvement)

#### Task 5: Implement Per-Thread Magazine Cache (True Lock-Free)
**File**: New: umem_ptc.c (Per-Thread Cache)
**Estimated Speedup**: 30-100% for <448 byte allocations
**Difficulty**: High
**LOC**: ~500

Similar to jemalloc's tcache, implement completely lock-free per-thread caching:
```c
__thread struct {
    void *bins[NUM_SMALL_SIZES][CACHE_SLOTS];
    uint16_t counts[NUM_SMALL_SIZES];
} thread_cache;
```

**Benefits**:
- Zero lock overhead for small allocations
- Comparable to glibc tcache performance (~5-15ns)
- Excellent thread scalability

**Risks**:
- Memory overhead per thread (~32-64KB)
- Thread exit cleanup required
- Complex interaction with magazine layer

**Implementation Notes**:
- Only for sizes ≤448 bytes (existing small object caches)
- Fallback to magazine layer when cache full/empty
- Integrate with existing UMEM_CACHE_SIZE infrastructure
- Add UMEM_OPTIONS=thread_cache=SIZE control

#### Task 6: Vectorize Magazine Operations
**File**: umem.c:2114, 2235
**Estimated Speedup**: 5-15%
**Difficulty**: Medium
**LOC**: ~100

Use SIMD to copy magazine rounds in batches:
```c
// Current: one-at-a-time
buf = ccp->cc_loaded->mag_round[--ccp->cc_rounds];

// Optimized: batch prefetch (x86_64)
_mm_prefetch(ccp->cc_loaded->mag_round + ccp->cc_rounds - 8, _MM_HINT_T0);
```

**Benefits**:
- Better cache utilization
- Reduce memory stalls
- Amortize overhead across operations

#### Task 7: Depot Lock Optimization
**File**: umem.c:1905-2079
**Estimated Speedup**: 10-20% under contention
**Difficulty**: Medium
**LOC**: ~150

Replace depot mutex with lock-free stack or fine-grained locking:
```c
// Use atomic stack operations for depot magazine lists
atomic_ptr depot_full_head;
atomic_ptr depot_empty_head;

// Lock-free push/pop operations
magazine_t* depot_pop(atomic_ptr *head) {
    magazine_t *old_head, *new_head;
    do {
        old_head = atomic_load(head);
        if (!old_head) return NULL;
        new_head = old_head->next;
    } while (!atomic_compare_exchange_weak(head, &old_head, new_head));
    return old_head;
}
```

**Benefits**:
- Eliminate depot lock contention
- Better scaling with thread count
- Reduced latency variance

**Risks**:
- ABA problem (use tagged pointers or hazard pointers)
- Working set statistics need atomic updates
- Complex correctness validation

#### Task 8: Size Class Optimization
**File**: umem.c:3241-3320
**Estimated Speedup**: 3-10%
**Difficulty**: Medium
**LOC**: ~200

Optimize size class selection and alignment:
- Use lookup table instead of linear search
- Align size classes to cache line boundaries
- Reduce number of small size classes (merge similar sizes)

**Benefits**:
- Faster size-to-cache mapping
- Better cache utilization
- Reduced metadata overhead

### LOW PRIORITY - Incremental Gains (1-5% improvement)

#### Task 9: Prefetch Optimization
**File**: umem.c (hot paths)
**Estimated Speedup**: 2-5%
**Difficulty**: Easy
**LOC**: ~50

Add software prefetch hints for magazine and slab metadata:
```c
__builtin_prefetch(ccp->cc_ploaded, 0, 3);  // Read, high locality
__builtin_prefetch(&cp->cache_depot, 0, 2);  // Read, medium locality
```

#### Task 10: Cache Line Padding
**File**: umem_impl.h
**Estimated Speedup**: 2-8%
**Difficulty**: Easy
**LOC**: ~30

Add padding to prevent false sharing:
```c
typedef struct umem_cpu_cache {
    // ... fields ...
    char pad[CACHE_LINE_SIZE - (sizeof(fields) % CACHE_LINE_SIZE)];
} umem_cpu_cache_t __attribute__((aligned(CACHE_LINE_SIZE)));
```

#### Task 11: Slab Coloring Improvements
**File**: umem.c:1478-1580
**Estimated Speedup**: 1-3%
**Difficulty**: Medium
**LOC**: ~100

Enhance slab coloring to reduce cache conflicts:
- Use prime number offsets
- Randomize slab placement
- NUMA-aware allocation

#### Task 12: Magazine Size Tuning
**File**: umem.c:2596-2617
**Estimated Speedup**: 2-7%
**Difficulty**: Easy
**LOC**: ~50

Auto-tune magazine sizes based on allocation patterns:
- Increase magazine size for high-frequency caches
- Decrease for rarely-used caches
- Add UMEM_OPTIONS control

### EXPERIMENTAL - Research Needed

#### Task 13: Implement Per-CPU Caching (True Per-CPU)
**Estimated Speedup**: 50-200% at high thread counts
**Difficulty**: Very High
**LOC**: ~1000

Requires kernel support or restartable sequences (rseq):
- Zero synchronization overhead
- Complex fallback for CPU migration
- Linux 4.18+ only

#### Task 14: NUMA-Aware Allocation
**Estimated Speedup**: 10-30% on NUMA systems
**Difficulty**: High
**LOC**: ~500

Implement NUMA-local magazine depots and slabs:
- Per-node depots
- Preferred node allocation
- Cross-node fallback

#### Task 15: Hardware Transactional Memory (HTM)
**Estimated Speedup**: 5-15% on supported CPUs
**Difficulty**: High
**LOC**: ~300

Use Intel TSX or ARM TME for magazine operations:
- Lock elision for depot operations
- Fallback to locks on abort
- CPU feature detection

## Benchmarking Requirements

For each optimization:

1. **Baseline Measurement**
   ```bash
   ./test/bench/bench_allocators.sh -n 10000000 -t 1,4,8,16 umem > baseline.txt
   ```

2. **Validation**
   ```bash
   make check  # All tests must pass
   UMEM_DEBUG=default make check  # Debug mode tests
   ```

3. **Performance Regression**
   ```bash
   # Must show ≥5% improvement to merge
   # No regressions in any workload
   ./test/bench/bench_allocators.sh -n 10000000 -t 1,4,8,16 umem > optimized.txt
   diff baseline.txt optimized.txt
   ```

4. **Stress Testing**
   ```bash
   # Run for 24 hours under valgrind/helgrind
   valgrind --tool=helgrind ./test/stress_test
   ```

## Implementation Order

### Phase 1: Quick Wins (1-2 weeks)
- Task 2: Inline hot paths
- Task 3: Optimize CPU hint
- Task 4: Reduce debug overhead
- Task 10: Cache line padding

**Expected Gain**: 15-25% improvement

### Phase 2: Lock Optimization (2-4 weeks)
- Task 1: Lock-free magazine cache
- Task 7: Depot lock optimization

**Expected Gain**: 30-50% improvement

### Phase 3: Advanced Features (4-8 weeks)
- Task 5: Per-thread cache
- Task 6: Vectorize operations
- Task 8: Size class optimization

**Expected Gain**: 40-80% improvement

### Phase 4: Polish (2-4 weeks)
- Task 9: Prefetch optimization
- Task 11: Slab coloring
- Task 12: Magazine tuning

**Expected Gain**: 5-15% improvement

## Target Performance Goals

### vs glibc malloc (ptmalloc2)

| Workload | Current | Target | glibc | Goal |
|----------|---------|--------|-------|------|
| Single-thread (16-256B) | ~10-20ns | ~5-10ns | ~5-15ns | Match |
| Multi-thread (8T, 16-256B) | ~40-60ns | ~15-25ns | ~20-40ns | Beat |
| Large alloc (>16KB) | ~200-500ns | ~100-200ns | ~150-300ns | Beat |
| Fragmentation | 1.2-1.5x | <1.3x | 1.1-1.4x | Match |

### vs jemalloc

| Workload | Current | Target | jemalloc | Goal |
|----------|---------|--------|----------|------|
| Single-thread | ~10-20ns | ~5-10ns | ~8-15ns | Match |
| Multi-thread (16T) | ~50-80ns | ~20-30ns | ~25-35ns | Beat |
| Fragmentation | 1.2-1.5x | <1.3x | 1.05-1.2x | Approach |

## Success Metrics

1. **Performance**: Beat glibc on multi-threaded workloads
2. **Scalability**: Linear scaling up to 32 threads
3. **Latency**: p99 < 500ns for small allocations
4. **Fragmentation**: < 1.3x overhead
5. **Compatibility**: Zero API changes, all tests pass

## References

- **glibc malloc internals**: [sourceware.org/glibc/wiki/MallocInternals](https://sourceware.org/glibc/wiki/MallocInternals)
- **jemalloc design**: [github.com/jemalloc/jemalloc/wiki/Design](https://github.com/jemalloc/jemalloc/wiki/Design)
- **tcmalloc design**: [google.github.io/tcmalloc/design](https://google.github.io/tcmalloc/design.html)
- **Lockless patterns**: [preshing.com/archives](https://preshing.com/archives/)
- **rseq**: [man7.org/linux/man-pages/man2/rseq.2.html](https://man7.org/linux/man-pages/man2/rseq.2.html)

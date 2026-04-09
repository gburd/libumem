# Why umem Slows Down with More Threads

## The Problem

**Observation**: umem performance degrades as thread count increases, while glibc malloc scales linearly.

### Performance Data

| Threads | umem (baseline) | glibc malloc | Gap |
|---------|-----------------|--------------|-----|
| 1 | 45.5 ns/op | 21.5 ns/op | 2.1x slower |
| 8 | 371.0 ns/op | 8.5 ns/op | **43.6x slower** |
| 16 | 406.7 ns/op | 9.0 ns/op | **45.2x slower** |

**After tcache** (current):
| Threads | umem (estimated) | glibc malloc | Gap |
|---------|------------------|--------------|-----|
| 1 | 8.5 ns/op | 21.5 ns/op | **2.5x FASTER** ✅ |
| 8 | ~40-50 ns/op | 8.5 ns/op | 5-6x slower ⚠️ |

## Root Cause: Lock Contention in Magazine Layer

Even with tcache enabled, cache misses go to the magazine layer, which has **three levels of locking**:

```
Thread allocation flow:
┌─────────────────────────────────────────┐
│ 1. tcache (thread-local)                │ ← LOCK-FREE ✅
│    ↓ hit → return (8.5ns)               │
│    ↓ miss                                │
├─────────────────────────────────────────┤
│ 2. Per-CPU Magazine Cache               │ ← LOCK #1 (cc_lock) ⚠️
│    mutex_lock(&ccp->cc_lock)            │
│    if (cc_rounds > 0)                   │
│       buf = cc_loaded->mag_round[...]   │
│    mutex_unlock(&ccp->cc_lock)          │
│    ↓ hit → return (~30ns)               │
│    ↓ miss (need magazine reload)        │
├─────────────────────────────────────────┤
│ 3. Depot (magazine exchange)            │ ← LOCK #2 (depot_lock) ⚠️
│    mutex_lock(&depot_stripe->ds_lock)   │
│    mp = depot_stripe->ds_full.ml_list   │
│    mutex_unlock(&depot_stripe->ds_lock) │
│    ↓ hit → return (~100ns)              │
│    ↓ miss (need new slab)               │
├─────────────────────────────────────────┤
│ 4. Slab Layer (allocate from arena)    │ ← LOCK #3 (cache_lock) ⚠️
│    mutex_lock(&cp->cache_lock)          │
│    allocate new slab from vmem          │
│    mutex_unlock(&cp->cache_lock)        │
│    return (~500ns+)                      │
└─────────────────────────────────────────┘
```

## The Three Bottlenecks

### Bottleneck #1: Per-CPU Magazine Cache Lock (cc_lock)

**Current Implementation** (umem.c:2232-2263):

```c
void *_umem_cache_alloc(umem_cache_t *cp, int umflag) {
    umem_cpu_cache_t *ccp = UMEM_CPU_CACHE(cp, CPU_CACHED(...));

    mutex_lock(&ccp->cc_lock);  // ⚠️ EVERY cache miss takes this lock

    if (ccp->cc_rounds > 0) {
        buf = ccp->cc_loaded->mag_round[--ccp->cc_rounds];
        mutex_unlock(&ccp->cc_lock);
        return buf;  // Common case: ~30ns
    }

    // Magazine reload logic (under lock)...
    mutex_unlock(&ccp->cc_lock);
}
```

**Problem**:
- Every tcache miss acquires cc_lock
- Multiple threads on same CPU core contend for same lock
- Lock acquire/release: ~20ns overhead
- Cache line bouncing between CPU cores

**Impact**:
- 1 thread: No contention, ~30ns
- 2-4 threads: Some contention, ~50-100ns
- 8+ threads: Heavy contention, ~100-300ns

### Bottleneck #2: Depot Lock (depot_stripe->ds_lock)

**Current Implementation** (umem.c:1960-1990):

```c
static umem_magazine_t *
umem_depot_alloc(umem_cache_t *cp, umem_maglist_t *mlp) {
    int stripe_idx = hash(thread_id) % UMEM_DEPOT_STRIPES;
    umem_depot_stripe_t *stripe = &cp->cache_depot[stripe_idx];

    mutex_lock(&stripe->ds_lock);  // ⚠️ Depot contention

    mp = stripe->ds_full.ml_list;
    if (mp != NULL) {
        stripe->ds_full.ml_list = mp->mag_next;
        stripe->ds_full.ml_total--;
    }

    mutex_unlock(&stripe->ds_lock);
    return mp;
}
```

**Problem**:
- 16 depot stripes shared by all threads
- Many threads can hash to same stripe
- Lock contention increases with thread count

**Impact**:
- 1-4 threads: Minimal contention
- 8-16 threads: Moderate contention (~50ns)
- 32+ threads: Heavy contention (~100-200ns)

### Bottleneck #3: Slab Layer Lock (cache_lock)

**Current Implementation** (umem.c:1600-1750):

```c
static void *
umem_slab_alloc(umem_cache_t *cp, int umflag) {
    mutex_lock(&cp->cache_lock);  // ⚠️ Global cache lock

    sp = cp->cache_freelist;
    if (sp->slab_refcnt < sp->slab_chunks) {
        // Allocate from existing slab
        buf = ...
    } else {
        // Allocate new slab from vmem
        sp = umem_slab_create(cp, umflag);
    }

    mutex_unlock(&cp->cache_lock);
    return buf;
}
```

**Problem**:
- Single lock for entire cache
- All threads allocating from same cache contend
- Worst case when creating new slabs (vmem allocation)

**Impact**: Less frequent (only on depot misses), but high cost (~500ns+)

## Why It Gets Worse with More Threads

### CPU Cache Line Bouncing

When multiple threads contend for the same lock:

```
Thread 1 (CPU 0):        Thread 2 (CPU 1):
lock(&cc_lock)           [waiting...]
  ↓ Cache line in CPU 0
  ↓ Modified state
unlock(&cc_lock)
  ↓ Cache line written back
                         lock(&cc_lock)
                           ↓ Cache line invalidated in CPU 0
                           ↓ Cache line loaded to CPU 1
                         unlock(&cc_lock)
```

**Cost of cache line transfer**: 40-100ns depending on CPU topology

### Lock Convoy Effect

When one thread holds a lock, other threads queue up:

```
Time →
Thread 1: [===lock===][work][===unlock===]
Thread 2:     [wait...][===lock===][work][===unlock===]
Thread 3:            [wait............][===lock===][work]
Thread 4:                   [wait.....................][lock]
```

**Serialization**: Instead of parallel execution, threads serialize

### Thundering Herd

When a lock is released, all waiting threads wake up and compete:

```
unlock(&cc_lock)
  ↓
  ├→ Thread 2 wakes up
  ├→ Thread 3 wakes up
  ├→ Thread 4 wakes up
  └→ Thread 5 wakes up

Only ONE wins the lock, others go back to sleep
(wasted context switches and CPU cycles)
```

## Why glibc malloc Scales Better

**glibc malloc (tcmalloc-style) architecture**:

```
Thread 1:              Thread 2:              Thread 3:
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│ tcache      │        │ tcache      │        │ tcache      │
│ (lock-free) │        │ (lock-free) │        │ (lock-free) │
└─────────────┘        └─────────────┘        └─────────────┘
      ↓ miss                 ↓ miss                 ↓ miss
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│ Arena 1     │        │ Arena 2     │        │ Arena 3     │
│ (thread-    │        │ (thread-    │        │ (thread-    │
│  specific)  │        │  specific)  │        │  specific)  │
└─────────────┘        └─────────────┘        └─────────────┘
```

**Key differences**:
1. **Per-thread arenas**: Each thread has its own arena (no sharing)
2. **Larger tcache**: More size classes, larger bins (lower miss rate)
3. **Lock-free fast path**: Atomic operations instead of mutexes
4. **Lazy allocation**: Arenas created on-demand, distributed across threads

## Task #90: Lock-Free Magazine Cache

### What It Does

Replace mutex locks in the magazine layer with atomic operations:

**Before** (current):
```c
mutex_lock(&ccp->cc_lock);
if (ccp->cc_rounds > 0) {
    buf = ccp->cc_loaded->mag_round[--ccp->cc_rounds];
}
mutex_unlock(&ccp->cc_lock);
```

**After** (Task #90):
```c
// Lock-free fast path
int rounds = atomic_load(&ccp->cc_rounds, acquire);
if (rounds > 0) {
    if (atomic_cas(&ccp->cc_rounds, rounds, rounds-1, acquire, relaxed)) {
        // We atomically claimed this slot
        buf = ccp->cc_loaded->mag_round[rounds-1];
        return buf;  // NO LOCK! (~15-20ns)
    }
}

// Slow path (magazine reload) still uses lock
mutex_lock(&ccp->cc_lock);
// ... reload magazine ...
mutex_unlock(&ccp->cc_lock);
```

### Expected Improvement

**Eliminates Bottleneck #1** (per-CPU cache lock):

| Scenario | Before (mutex) | After (lock-free) | Improvement |
|----------|---------------|-------------------|-------------|
| Cache hit, no contention | ~30ns | ~15ns | 2x faster |
| Cache hit, 2 threads | ~50-100ns | ~15ns | 3-7x faster |
| Cache hit, 8 threads | ~100-300ns | ~20ns | 5-15x faster |

**Overall impact**:
- Single-threaded: ~2x improvement (30ns → 15ns)
- Multi-threaded: **5-15x improvement** on cache hits

### Why It Will Help

1. **No lock acquire/release overhead** (~20ns saved)
2. **No cache line bouncing** (only atomic CAS)
3. **No lock convoy effect** (threads don't serialize)
4. **No thundering herd** (no wakeups needed)

### Design Approach

From LOCK_FREE_MAGAZINE_ANALYSIS.md:

**Option D: Double-Checked Locking with Proper Barriers** (Recommended)

```c
// Allocation fast path
rounds = atomic_load_acquire(&ccp->cc_rounds);
if (rounds > 0) {
    loaded_mag = ccp->cc_loaded;  // Local copy BEFORE CAS
    if (loaded_mag != NULL) {
        if (atomic_cas_weak(&ccp->cc_rounds, &rounds, rounds-1,
                           acquire, relaxed)) {
            // Success - we own this slot
            buf = loaded_mag->mag_round[rounds-1];  // Use local copy
            return buf;
        }
        // CAS failed - retry
        goto retry;
    }
}

// Fall through to locked slow path (magazine reload)
```

**Key safety properties**:
- Local magazine copy prevents TOCTOU race
- Atomic CAS ensures only one thread claims each slot
- Magazine reload still uses lock (complex operation)
- Memory barriers ensure visibility

### Complexity

**Risk**: Medium-High
- Requires careful memory ordering (acquire/release semantics)
- Race condition between fast path and magazine reload
- Must handle NULL magazine pointers
- Previous attempt (commit 9742317) had data corruption

**Testing Requirements**:
- ThreadSanitizer (TSan) validation
- Stress testing with 32+ threads
- 24-hour stability test
- Property-based testing

## Task #91: Lock-Free Depot Operations

### What It Does

Replace depot locks with lock-free stack operations:

**Before** (current):
```c
mutex_lock(&depot_stripe->ds_lock);
mp = stripe->ds_full.ml_list;
stripe->ds_full.ml_list = mp->mag_next;
mutex_unlock(&depot_stripe->ds_lock);
```

**After** (Task #91):
```c
// Lock-free stack pop using tagged pointers
tagged_ptr_t old, new;
do {
    old = atomic_load(&stripe->ds_full.head, acquire);
    if (old.ptr == NULL) return NULL;

    new.ptr = old.ptr->mag_next;
    new.tag = old.tag + 1;  // ABA prevention
} while (!atomic_cas(&stripe->ds_full.head, &old, new, release, acquire));

mp = old.ptr;
```

### Expected Improvement

**Eliminates Bottleneck #2** (depot lock):

| Threads | Before (mutex) | After (lock-free) | Improvement |
|---------|---------------|-------------------|-------------|
| 4 | ~50ns | ~20ns | 2.5x |
| 8 | ~100ns | ~25ns | 4x |
| 16 | ~150-200ns | ~30ns | 5-7x |

**Overall impact**: Additional **2-7x improvement** on depot operations

## Combined Expected Performance

### After Task #90 + #91

| Threads | Baseline | Current (tcache) | After #90+#91 | Total Improvement |
|---------|----------|------------------|---------------|-------------------|
| 1 | 45.5ns | 8.5ns | **6-8ns** | **7.5x** |
| 8 | 371ns | ~45ns | **10-15ns** | **30-40x** |
| 16 | 407ns | ~50ns | **12-18ns** | **25-35x** |

**Goal**: Match or beat glibc malloc (8.5-9.0ns @ 8-16 threads)

## Remaining Bottleneck #3: Slab Layer

**Not addressed by Tasks #90-91**:

The slab layer lock (cache_lock) remains, but:
- Only hit on depot misses (rare in steady state)
- Could be addressed by Task #103 (rseq per-CPU caching)
- Lower priority (infrequent path)

## Summary

### Why umem slows down with more threads:
1. ❌ **Mutex contention** in magazine layer (every cache miss)
2. ❌ **Depot lock contention** (magazine reloads)
3. ❌ **CPU cache line bouncing** (lock transfers between cores)
4. ❌ **Lock convoy effect** (serialization)
5. ❌ **Thundering herd** (wasted wakeups)

### Task #90 (Lock-Free Magazine Cache):
- ✅ Eliminates magazine layer mutex (Bottleneck #1)
- ✅ Uses atomic CAS instead of locks
- ✅ Expected: **5-15x improvement** on multi-threaded workloads
- ⚠️ Medium-high complexity, needs careful validation

### Task #91 (Lock-Free Depot):
- ✅ Eliminates depot mutex (Bottleneck #2)
- ✅ Uses lock-free stack with tagged pointers
- ✅ Expected: Additional **2-7x improvement**
- ⚠️ Requires ABA prevention (version tagging)

### Combined Impact:
- 🎯 **30-40x improvement** at 8-16 threads
- 🎯 **Should match glibc malloc performance**
- 🎯 Addresses the root cause of multi-threaded degradation

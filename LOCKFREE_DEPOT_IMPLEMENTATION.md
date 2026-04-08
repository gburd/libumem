# Lock-Free Depot Optimization Implementation Guide

## Overview

This document describes Task 7 from the performance optimization tasks: converting the umem depot magazine lists from lock-based to lock-free operations using atomic CAS.

## Current Implementation (Lock-Based)

The current depot uses mutexes to protect magazine lists:
- Location: `umem.c` lines 1905-2076
- Structure: `umem_depot_stripe_t` has `ds_lock` mutex
- Operations: `umem_depot_alloc()`, `umem_depot_free()` use mutex lock/unlock
- Problem: Mutex contention under high thread count (16+ threads)
- Overhead: ~50-200ns per depot access

## New Implementation (Lock-Free)

The lock-free implementation eliminates mutexes and uses atomic CAS operations.

### Data Structure Changes

File: `umem_impl.h` lines 277-300

#### Before:
```c
typedef struct umem_maglist {
    umem_magazine_t	*ml_list;	/* magazine list */
    long		ml_total;	/* number of magazines */
    long		ml_min;		/* min since last update */
    long		ml_reaplimit;	/* max reapable magazines */
    uint64_t	ml_alloc;	/* allocations from this list */
} umem_maglist_t;

typedef struct umem_depot_stripe {
    mutex_t		ds_lock;	/* protects this stripe */
    umem_maglist_t	ds_full;	/* full magazines */
    umem_maglist_t	ds_empty;	/* empty magazines */
    uint64_t	ds_contention;	/* contention count for this stripe */
} umem_depot_stripe_t;
```

#### After:
```c
typedef struct umem_maglist {
    _Atomic uint64_t ml_list;      /* tagged pointer: [counter:16][ptr:48] */
    _Atomic uint64_t ml_total;     /* number of magazines (atomic) */
    _Atomic uint64_t ml_min;       /* min since last update (atomic) */
    _Atomic uint64_t ml_reaplimit; /* max reapable magazines (atomic) */
    _Atomic uint64_t ml_alloc;     /* allocations from this list (atomic) */
} umem_maglist_t;

/* Tagged pointer operations for lock-free stack */
#define UMEM_TAG_SHIFT		48
#define UMEM_PTR_MASK		0x0000FFFFFFFFFFFFULL
#define UMEM_TAG_MASK		0xFFFF000000000000ULL

#define UMEM_TAGGED_PTR(ptr, tag) \
    ((((uint64_t)(tag) & 0xFFFF) << UMEM_TAG_SHIFT) | \
     ((uintptr_t)(ptr) & UMEM_PTR_MASK))

#define UMEM_GET_PTR(tagged) \
    ((umem_magazine_t *)(uintptr_t)((tagged) & UMEM_PTR_MASK))

#define UMEM_GET_TAG(tagged) \
    ((uint16_t)(((tagged) & UMEM_TAG_MASK) >> UMEM_TAG_SHIFT))

typedef struct umem_depot_stripe {
    umem_maglist_t	ds_full;      /* full magazines (lock-free) */
    umem_maglist_t	ds_empty;     /* empty magazines (lock-free) */
    _Atomic uint64_t	ds_contention;  /* contention count (CAS failures) */
} umem_depot_stripe_t;
```

### Algorithm Changes

#### Lock-Free Pop (umem_depot_alloc)

```c
do {
    // 1. Load current tagged pointer atomically
    old_tagged = atomic_load_explicit(&stripe_list->ml_list, memory_order_acquire);
    mp = UMEM_GET_PTR(old_tagged);

    if (mp == NULL) return NULL;  // Empty list

    // 2. Prepare new tagged pointer (next magazine, incremented tag)
    old_tag = UMEM_GET_TAG(old_tagged);
    new_tagged = UMEM_TAGGED_PTR(mp->mag_next, old_tag + 1);

    // 3. Track contention (CAS retries)
    if (cas_retries++ > 0) {
        atomic_fetch_add(&stripe->ds_contention, 1);
    }

// 4. CAS: if old_tagged still matches, swap to new_tagged
} while (!atomic_compare_exchange_weak(&stripe_list->ml_list,
    &old_tagged, new_tagged));

// 5. Update statistics atomically
atomic_fetch_add(&stripe_list->ml_alloc, 1);
atomic_fetch_sub(&stripe_list->ml_total, 1);
```

#### Lock-Free Push (umem_depot_free)

```c
do {
    // 1. Load current head
    old_tagged = atomic_load_explicit(&stripe_list->ml_list, memory_order_acquire);
    old_tag = UMEM_GET_TAG(old_tagged);

    // 2. Point new magazine to current head
    mp->mag_next = UMEM_GET_PTR(old_tagged);

    // 3. Create new tagged pointer
    new_tagged = UMEM_TAGGED_PTR(mp, old_tag + 1);

    // 4. Track contention
    if (cas_retries++ > 0) {
        atomic_fetch_add(&stripe->ds_contention, 1);
    }

// 5. CAS to swap head
} while (!atomic_compare_exchange_weak(&stripe_list->ml_list,
    &old_tagged, new_tagged));

// 6. Update count
atomic_fetch_add(&stripe_list->ml_total, 1);
```

## ABA Problem and Solution

### The ABA Problem

In a lock-free stack, the ABA problem occurs when:
1. Thread A reads head pointer as value "A"
2. Thread B pops "A", pops another node, pushes "A" back
3. Thread A's CAS succeeds (head is still "A") but list structure has changed

This can cause:
- Lost updates
- Memory corruption
- Use-after-free

### Tagged Pointer Solution

We use **tagged pointers** to prevent ABA:

```
64-bit value layout:
[63:48] = 16-bit counter (tag)
[47:0]  = 48-bit pointer

Counter increments on every CAS, even if pointer value repeats.
```

Example:
```
Initial: tag=0, ptr=A     → tagged_ptr = 0x0000_xxxx_xxxx_xxxxA
Pop A:   tag=1, ptr=B     → tagged_ptr = 0x0001_xxxx_xxxx_xxxxB
Push A:  tag=2, ptr=A     → tagged_ptr = 0x0002_xxxx_xxxx_xxxxA
```

Even though pointer is "A" again, tag differs (0 vs 2), so CAS fails correctly.

### Why 48-bit Pointers Are Safe

On x86-64:
- Virtual address space: 48 bits (256 TB)
- High bits [63:48] unused by hardware (sign-extended)
- Our heap allocations are in lower 48 bits
- Masking high bits is safe

On aarch64:
- Virtual address space: 48 bits (some systems use 52)
- Our masking is compatible with standard 48-bit VA
- If 52-bit VA is needed, adjust PTR_MASK to 0x000F_FFFF_FFFF_FFFF

### Tag Counter Wraparound

16-bit counter wraps after 65536 operations:
- For ABA to occur: same pointer must be reused within 65536 operations
- Magazine lifetime: typically hundreds to thousands of operations
- Probability of ABA despite tagging: negligible
- If concerned: use 32-bit tag + 32-bit pointer (requires architecture check)

## Memory Ordering

### Why Memory Order Matters

Without proper ordering:
- Thread A pushes magazine with data
- Thread B pops magazine
- Thread B might see stale data (memory reordering)

### Our Memory Ordering Strategy

```c
// Load: acquire semantics
// - Ensures subsequent loads see data written before the store
old_tagged = atomic_load_explicit(&ml_list, memory_order_acquire);

// Store: release semantics (via CAS)
// - Ensures all prior stores are visible to threads that acquire
atomic_compare_exchange_weak_explicit(&ml_list, &old, new,
    memory_order_release,  // success ordering
    memory_order_acquire); // failure ordering

// Statistics: relaxed ordering
// - Not critical for correctness, only for monitoring
atomic_fetch_add_explicit(&ml_alloc, 1, memory_order_relaxed);
```

### Happens-Before Relationships

1. Thread A writes magazine data (normal stores)
2. Thread A pushes magazine (release store via CAS)
3. Thread B pops magazine (acquire load + CAS)
4. Thread B reads magazine data (normal loads)

The release-acquire pair ensures: (1) happens-before (4)

## Implementation Steps

### Step 1: Update umem_impl.h

File: `/home/gburd/ws/libumem/umem_impl.h`

1. Add `#include <stdatomic.h>` if not already present
2. Modify `umem_maglist_t` structure (lines ~280-286)
3. Add tagged pointer macros after `umem_maglist_t` definition
4. Modify `umem_depot_stripe_t` to remove `ds_lock` (lines ~295-300)

### Step 2: Update umem.c

File: `/home/gburd/ws/libumem/umem.c`

Replace functions:

1. **umem_depot_alloc()** (lines ~1905-1952)
   - Remove mutex_trylock/lock/unlock calls
   - Add CAS loop for lock-free pop
   - Add atomic statistics updates

2. **umem_depot_free()** (lines ~1957-1986)
   - Remove mutex_lock/unlock calls
   - Add CAS loop for lock-free push
   - Add atomic total increment

3. **umem_depot_ws_update()** (lines ~1992-2007)
   - Remove mutex_lock/unlock calls
   - Use atomic_load, atomic_exchange, atomic_store

4. **umem_depot_ws_reap()** (lines ~2013-2076)
   - Remove mutex_lock/unlock calls
   - Use atomic_load for reading limits
   - Continue using umem_depot_alloc (now lock-free) for popping

### Step 3: Update Initialization Code

Search for cache depot initialization (grep for `cache_depot`):
- Remove mutex initialization calls for `ds_lock`
- Ensure atomic fields are initialized to 0 (default)

### Step 4: Testing

#### Compilation Test
```bash
make clean
make
```

Look for:
- No compiler errors
- No warnings about atomics
- Check that `_Atomic` is supported (requires C11 compiler)

#### Functional Test
```bash
make test
./test/unit/test_umem_advanced
./test/integration/test_multithreaded
```

Verify:
- All tests pass
- No crashes or assertion failures

#### Threading Stress Test
```bash
./test/integration/test_threading_stress -t 32 -i 100000
```

Verify:
- No data corruption
- No crashes
- Check contention counter values

#### Concurrent Correctness
```bash
valgrind --tool=helgrind ./test/integration/test_threading_stress -t 16 -i 10000
```

Look for:
- No data race warnings
- No lock order violations (there are no locks!)

#### Thread Sanitizer (if available)
```bash
make clean
CFLAGS="-fsanitize=thread -g" make
./test/integration/test_threading_stress -t 16 -i 10000
```

Verify:
- No data race reports
- No atomic operation warnings

### Step 5: Performance Measurement

#### Before Lock-Free (baseline)
```bash
# Measure depot contention
./test/integration/test_threading_stress -t 32 -i 100000 -p
```

Record:
- Total allocations
- Depot contention count
- Average latency

#### After Lock-Free
```bash
# Same test with lock-free depot
./test/integration/test_threading_stress -t 32 -i 100000 -p
```

Expected improvements:
- 10-20% lower depot contention (CAS failures vs mutex waits)
- 5-15% lower allocation latency under high thread count
- Better scaling: performance should be more linear with thread count

## Rollback Plan

If lock-free implementation has issues:

1. **Compilation Issues**
   - Check C11 support: `gcc --version` (need GCC 4.9+ or Clang 3.6+)
   - Fallback: Use GCC atomic builtins instead of stdatomic.h

2. **Correctness Issues**
   - Run `git diff umem_impl.h umem.c` to see changes
   - Revert: `git checkout umem_impl.h umem.c`
   - Restore from backup: `cp umem.c.before_lockfree umem.c`

3. **Performance Regression**
   - Unlikely, but possible if CAS contention is worse than mutex
   - Measure carefully before deciding
   - May need to tune UMEM_DEPOT_STRIPES (try 32 or 64)

## Expected Results

### Performance Gains

Under high contention (16+ threads):
- 10-20% reduction in depot access latency
- Linear scaling up to ~32 threads (vs plateau with locks)
- Lower CPU usage (no context switches for depot operations)

### Contention Metric Changes

Before: `ds_contention` counts mutex_trylock failures
After: `ds_contention` counts CAS retry loops

Both are valid contention metrics. If CAS retries are higher than mutex failures, that's expected - CAS is more "aggressive" but faster.

### Code Quality Improvements

- Simpler code: no lock/unlock calls
- Better scalability: no lock holder preemption problem
- Easier to reason about: atomic operations are more composable

## References

- **Tagged Pointers**: "Practical lock-free data structures" by Herlihy & Shavit
- **ABA Problem**: "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects" by Michael
- **Memory Ordering**: C++ Concurrency in Action by Anthony Williams
- **umem Design**: "Magazines and Vmem" by Bonwick & Adams (2001)

## Complete Implementation Files

- `depot_functions_lockfree.c` - Full implementation with comments
- `lockfree_depot.patch` - Summary of changes
- `LOCKFREE_DEPOT_IMPLEMENTATION.md` - This document

## Questions or Issues

If you encounter problems:

1. Check compiler version and C11 support
2. Verify stdatomic.h is available
3. Run under helgrind/tsan for correctness
4. Measure before/after performance carefully
5. Check ABA protection is working (counter increments)

Good luck with the implementation!

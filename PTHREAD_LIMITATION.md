# Known Limitation: pthread_create/malloc Interaction

## Summary

libumem currently has a known issue when used in applications that create threads via `pthread_create`. The root cause is a circular dependency between glibc's pthread implementation and malloc override.

## Root Cause

1. `pthread_create` internally calls `malloc` for thread stack/TLS allocation
2. libumem overrides `malloc` via weak symbols
3. During thread creation, this creates a potential deadlock:
   - `pthread_create` → `malloc` → umem initialization → `pthread_once` → deadlock

## Current Mitigation

A bootstrap allocator (using direct `mmap`) provides emergency allocation during early initialization, following patterns from jemalloc and tcmalloc. However, this only partially mitigates the issue.

## Affected Scenarios

- Multi-threaded test programs (`umem_test2`, `umem_ptc_test`)
- LD_PRELOAD usage with multi-threaded applications (`umem_test4`)
- Applications that create threads after loading libumem

## Working Scenarios

✅ Single-threaded applications
✅ Pre-initialized threads (threads created before libumem loads)
✅ Direct API usage (umem_alloc/umem_free instead of malloc/free)

## Recommended Usage

### Option 1: Direct API
Use umem_alloc/umem_free/umem_cache_* APIs directly instead of overriding malloc:

```c
#include <umem.h>

void *ptr = umem_alloc(size, UMEM_DEFAULT);
umem_free(ptr, size);
```

### Option 2: LD_PRELOAD (Single-threaded)
For single-threaded applications:
```bash
LD_PRELOAD=/usr/local/lib/libumem.so ./my_single_threaded_app
```

### Option 3: Careful Integration
Load libumem early, before any threads are created:
```c
__attribute__((constructor(101)))  // High priority
static void init_umem(void) {
    // Force umem initialization before main()
    void *p = malloc(16);
    free(p);
}
```

## Future Work

Further research into jemalloc and tcmalloc solutions:
- Platform-specific hooks (FreeBSD's `_pthread_mutex_init_calloc_cb`)
- Additional recursion detection mechanisms
- dlopen-based isolation strategies

## Related Research

- jemalloc: arena 0 bootstrap + TLS state machine
- tcmalloc: Arena allocator + reentrancy detection
- Both acknowledge similar limitations in certain configurations

## Status

This is a known limitation being actively researched. Contributions welcome.

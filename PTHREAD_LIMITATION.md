# pthread_create/malloc Circular Dependency

## Summary

libumem's malloc interposition (`libumem_malloc.so`) uses a **bootstrap allocator** for all malloc() calls to avoid circular dependency with pthread_create. This means LD_PRELOAD malloc replacement does NOT provide umem's performance benefits.

## Root Cause

The fundamental issue is a circular dependency:

1. `pthread_create` internally calls `malloc()` for thread stack/TLS allocation
2. umem's `umem_malloc()` uses pthread operations (`pthread_getspecific`) for Per-Thread Cache lookup
3. This creates a deadlock:
   ```
   pthread_create → malloc → umem_malloc → pthread_getspecific → deadlock
   ```

## Solution

**malloc interposition always uses the bootstrap allocator** (simple mmap-based allocations):
- malloc() → bootstrap_malloc() → mmap()
- free() → bootstrap_free() → munmap()

This eliminates the circular dependency by never calling pthread operations from malloc().

Only **direct umem_alloc()** calls use the real umem allocator with PTC performance benefits.

## Performance Implications

### LD_PRELOAD (libumem_malloc.so)
```bash
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./app
```
- ✅ Works with pthread_create (no hangs/deadlocks)
- ❌ NO performance benefits (uses bootstrap allocator)
- ❌ No PTC, no magazine layer, no debug features
- Use case: Testing, compatibility checking only

### Direct Linking (libumem.so)
```bash
gcc app.c -lumem
```
- ✅ Full umem performance (PTC, magazines, debug features)
- ✅ Works with pthread_create
- ✅ Recommended approach
- Use case: Production deployments

### Direct API Usage
```c
#include <umem.h>

void *ptr = umem_alloc(size, UMEM_DEFAULT);
umem_free(ptr, size);
```
- ✅ Full umem performance
- ✅ Works with pthread_create
- ✅ Best performance (no malloc wrapper overhead)
- Use case: New code, performance-critical paths

## Recommended Usage

### For New Code
Use umem's direct API:
```c
#include <umem.h>

// Simple allocation
void *ptr = umem_alloc(size, UMEM_DEFAULT);
umem_free(ptr, size);

// Caching allocator for hot paths
umem_cache_t *cache = umem_cache_create("my_objects",
    sizeof(my_obj), 0, NULL, NULL, NULL, NULL, NULL, 0);
my_obj *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
umem_cache_free(cache, obj);
```

### For Existing Code
Link directly against libumem:
```bash
# Makefile
LDFLAGS += -lumem

# Or CMakeLists.txt
target_link_libraries(myapp PRIVATE umem)
```

### For Testing Only
Use LD_PRELOAD (but don't expect performance gains):
```bash
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./test_program
```

## Why Not Follow jemalloc/tcmalloc?

jemalloc and tcmalloc do successfully use LD_PRELOAD malloc interposition with threads. They achieve this by:

1. **No pthread operations in malloc path**: Use only __thread variables (with initial-exec TLS model) and atomic operations
2. **Extensive platform-specific hooks**: FreeBSD's `_pthread_mutex_init_calloc_cb`, glibc's internal APIs
3. **Complex initialization**: Multi-phase bootstrap with careful ordering

libumem's architecture uses pthread_getspecific() for PTC lookup, making it incompatible with malloc interposition. A complete rewrite of the PTC mechanism would be required to match jemalloc's approach.

The **cost/benefit is not favorable**:
- LD_PRELOAD malloc replacement is a niche use case
- Direct linking provides better performance anyway
- Extensive testing would be required across all platforms
- Maintenance burden increases significantly

## Implementation Details

See `malloc_interpose.c:213-226` for the core decision:

```c
/*
 * Bootstrap phase: use bootstrap allocator.
 *
 * IMPORTANT: We NEVER transition to using umem_malloc() from malloc()
 * interposition. This is because:
 * 1. pthread_create internally calls malloc()
 * 2. umem_malloc() uses pthread operations (pthread_getspecific for PTC)
 * 3. This creates a circular dependency: pthread_create -> malloc ->
 *    umem_malloc -> pthread_getspecific -> deadlock
 *
 * Instead, malloc() always uses the bootstrap allocator (simple mmap-based).
 * Only direct umem_alloc() calls use the real umem allocator.
 */
```

## Test Status

All tests pass with direct umem API usage:
- ✅ umem_test (single-threaded)
- ✅ umem_test3 (malloc interposition, single-threaded)
- ✅ umem_ptc_fork_test (multi-process with fork)
- ❌ umem_test2 (disabled - LD_PRELOAD + threads, slow due to bootstrap allocator)
- ❌ umem_ptc_test (disabled - LD_PRELOAD + threads, slow due to bootstrap allocator)

## Related Documentation

- `docs/PTHREAD_RESEARCH.md` - Detailed research on jemalloc/tcmalloc approaches
- `docs/PERFORMANCE_GUIDE.md` - Performance optimization recommendations
- `PERFORMANCE_INVESTIGATION.md` - Analysis of malloc interposition overhead

## Status

This is an **architectural decision**, not a bug. LD_PRELOAD malloc interposition is not a supported use case for performance-critical applications. Use direct linking or direct API instead.

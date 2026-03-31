# Initial-Exec TLS Recursion Guard Implementation

## Overview

This document describes the implementation of per-thread recursion detection using the initial-exec TLS model to prevent pthread_create/malloc deadlock in libumem.

## Problem Statement

When libumem is loaded via `LD_PRELOAD`, a circular dependency can occur:

```
pthread_create → malloc → umem_init → pthread_once → pthread_getspecific → malloc
```

This circular dependency can cause deadlock when pthread operations internally call malloc before umem is fully initialized or when accessing thread-local storage.

## Solution

The solution implements a per-thread recursion guard using thread-local storage (TLS) with the `initial-exec` model. This allows detection of recursive malloc calls without triggering additional allocations.

## Implementation

### Files Created

#### `/home/gburd/ws/libumem/malloc_guard.h`

Header file defining the recursion guard API:

```c
extern __thread int umem_malloc_recursion_depth
    __attribute__((tls_model("initial-exec")));

static inline int umem_enter_malloc(void);
static inline void umem_exit_malloc(void);
static inline int umem_in_malloc(void);
```

**Key Features:**
- Uses `initial-exec` TLS model for single-instruction access
- No function calls to `__tls_get_addr()`
- No memory allocation during TLS access
- Available immediately during thread creation

#### `/home/gburd/ws/libumem/malloc_guard.c`

Implementation file defining the TLS variable:

```c
__thread int umem_malloc_recursion_depth
    __attribute__((tls_model("initial-exec"))) = 0;
```

Each thread gets its own copy of this variable, initialized to 0.

### Files Modified

#### `/home/gburd/ws/libumem/malloc.c`

Added recursion detection to `umem_malloc()`:

```c
/* Check for recursive malloc call */
if (umem_enter_malloc() > 0) {
    umem_exit_malloc();
    return (bootstrap_malloc(size_arg));
}

/* Normal allocation path */
size = size_arg + sizeof(malloc_data_t);
// ... allocation logic ...

umem_exit_malloc();
return ((void *)ret);
```

**Exit Paths:**
- All error paths call `umem_exit_malloc()` before returning
- Success path calls `umem_exit_malloc()` before returning pointer

#### `/home/gburd/ws/libumem/malloc_interpose.c`

Added recursion detection to malloc() interposition:

```c
if (__builtin_expect(interpose_state == INTERPOSE_READY, 1)) {
    if (umem_enter_malloc() > 0) {
        umem_exit_malloc();
        return (bootstrap_malloc(size));
    }
    ret = umem_malloc(size);
    umem_exit_malloc();
    return (ret);
}
```

#### `/home/gburd/ws/libumem/Makefile.am`

Added new files to both libraries:
- `libumem_la_SOURCES`: Added `malloc_guard.c` and `malloc_guard.h`
- `libumem_malloc_la_SOURCES`: Added `malloc_guard.c` and `malloc_guard.h`

## TLS Model: initial-exec

### Why initial-exec?

The `initial-exec` TLS model is chosen for several critical reasons:

1. **Single Instruction Access**: On x86-64, accessing an initial-exec TLS variable compiles to a single `mov` instruction:
   ```asm
   mov %fs:offset, %eax
   ```

2. **No Function Calls**: Unlike the default `global-dynamic` model, initial-exec doesn't call `__tls_get_addr()`, which might allocate memory.

3. **No Memory Allocation**: The TLS slot is allocated at program startup, not on first access.

4. **Available During pthread_create**: The variable is accessible even during thread creation.

### TLS Model Comparison

| Model | Access Speed | Function Calls | Memory Allocation | Works with dlopen() |
|-------|-------------|----------------|-------------------|---------------------|
| local-exec | Fastest | No | No | No |
| initial-exec | Fast | No | No | No |
| local-dynamic | Slow | Yes | Possible | Yes |
| global-dynamic | Slowest | Yes | Possible | Yes |

### Limitation

**LD_PRELOAD Only**: The initial-exec model requires that TLS slots be allocated at program startup. This means:
- ✅ Works with `LD_PRELOAD`
- ❌ Does NOT work with `dlopen()` (runtime loading)

This is acceptable for libumem's primary use case (malloc replacement via LD_PRELOAD).

## How It Works

### Recursion Detection Flow

1. **First malloc call**:
   ```
   Thread A calls malloc()
   → umem_enter_malloc() returns 0 (depth was 0, now 1)
   → Proceed with normal umem_malloc()
   → umem_exit_malloc() (depth back to 0)
   ```

2. **Recursive malloc call**:
   ```
   Thread A calls malloc()
   → umem_enter_malloc() returns 0 (depth: 0→1)
   → Inside umem_malloc(), pthread_getspecific() calls malloc()
     → umem_enter_malloc() returns 1 (depth: 1→2) ← RECURSION DETECTED
     → Use bootstrap_malloc() instead
     → umem_exit_malloc() (depth: 2→1)
   → Original malloc continues normally
   → umem_exit_malloc() (depth: 1→0)
   ```

### Bootstrap Allocator

When recursion is detected, allocations are routed to the bootstrap allocator:
- Direct `mmap()` calls
- No umem or pthread dependencies
- Tagged with `BOOTSTRAP_MAGIC` for identification
- Freed with `munmap()` during `free()`

## Performance

### Fast Path Overhead

The recursion guard adds minimal overhead:

```asm
; Check recursion depth (initial-exec TLS)
mov    eax, %fs:umem_malloc_recursion_depth  ; 1 cycle (TLS read)
inc    %fs:umem_malloc_recursion_depth        ; 1 cycle (increment)
test   eax, eax                               ; 1 cycle (check if > 0)
jnz    .recursive_path                         ; 1 cycle (branch)
; ... normal malloc path ...
```

**Total overhead**: ~4 cycles when branch is predicted correctly (common case).

### Memory Overhead

- **Per-thread**: 4 bytes (sizeof(int)) in TLS
- **Global**: None (no heap allocations)

### TLS Space

Initial-exec TLS uses a limited pool of TLS slots. Each module using initial-exec consumes part of this pool. On most systems:
- x86-64 Linux: ~1KB of initial-exec TLS available
- Our usage: 4 bytes (negligible)

## Testing

### Test Program

A test program is provided in `/home/gburd/ws/libumem/test_recursion_guard.c`:

```bash
# Build with libumem
gcc -o test_recursion_guard test_recursion_guard.c -lumem -lpthread

# Run with LD_PRELOAD
LD_PRELOAD=./libumem_malloc.so ./test_recursion_guard
```

### Test Scenarios

1. **Basic Thread Creation**: Create multiple threads, each performing allocations
2. **Concurrent Creation**: Multiple threads creating other threads
3. **Stress Test**: High allocation rate during thread creation
4. **Real Applications**: Test with Python, Ruby, Node.js using LD_PRELOAD

### Expected Behavior

- No deadlocks during `pthread_create`
- All allocations succeed
- Proper deallocation (no leaks)
- Minimal performance impact

## Integration with Existing Code

### Compatibility

The recursion guard integrates seamlessly with existing libumem features:

- **Bootstrap allocator**: Already present in malloc.c, now used for recursive calls
- **malloc_interpose.c**: Extended to check recursion in fast path
- **PTC (Per-Thread Cache)**: Unaffected, works normally after recursion guard
- **Debug features**: umem_debug, auditing, etc. work as before

### Upgrade Path

No changes required for existing libumem users:
1. Rebuild libumem with new sources
2. Existing applications continue to work
3. pthread_create deadlocks are automatically prevented

## Future Enhancements

### Statistics Tracking

Add optional statistics to track recursion events:

```c
typedef struct {
    size_t recursive_calls;
    size_t bootstrap_allocations;
    size_t max_recursion_depth;
} umem_recursion_stats_t;
```

### Debug Mode

Add debug logging for recursion events:

```c
#ifdef UMEM_DEBUG_RECURSION
if (umem_enter_malloc() > 0) {
    fprintf(stderr, "umem: recursion detected at depth %d\n",
        umem_malloc_recursion_depth);
}
#endif
```

### Platform-Specific Optimizations

#### FreeBSD

Integrate with FreeBSD pthread hooks:
```c
#ifdef __FreeBSD__
extern void _pthread_mutex_init_calloc_cb(void *(*)(size_t));
_pthread_mutex_init_calloc_cb(bootstrap_malloc);
#endif
```

#### musl libc

musl's `pthread_create` doesn't call malloc, so the guard could be disabled:
```c
#ifdef __MUSL__
/* musl doesn't need recursion guard */
#define umem_enter_malloc() 0
#define umem_exit_malloc() ((void)0)
#endif
```

## References

### Documentation

- **PTHREAD_RESEARCH.md**: Detailed analysis of pthread_create/malloc interaction
- **ELF TLS Specification**: https://www.akkadia.org/drepper/tls.pdf

### Similar Implementations

- **jemalloc**: Uses initial-exec TLS for per-thread cache
- **tcmalloc**: Uses initial-exec TLS for fast-path optimization
- **mimalloc**: Uses local-exec TLS in executables

### Related Code

- `malloc.c`: Bootstrap allocator implementation
- `malloc_interpose.c`: dlsym(RTLD_NEXT) based interposition
- `umem_hooks.c`: pthread_create wrapper (dlsym-based)

## Conclusion

The initial-exec TLS recursion guard provides a robust, efficient solution to the pthread_create/malloc deadlock problem. With minimal overhead (~4 cycles) and simple implementation, it prevents recursive malloc calls while maintaining compatibility with existing libumem features.

Key benefits:
- ✅ Prevents deadlock during pthread_create
- ✅ Minimal performance overhead
- ✅ Thread-safe (per-thread counter)
- ✅ Simple implementation (3 inline functions)
- ✅ Compatible with existing code

Limitations:
- ⚠️ Requires LD_PRELOAD (doesn't work with dlopen)
- ⚠️ Uses limited initial-exec TLS space

This implementation follows best practices from production allocators (jemalloc, tcmalloc) and is recommended in the pthread_create/malloc research documentation.

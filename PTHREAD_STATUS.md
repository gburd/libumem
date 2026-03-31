# pthread_create/malloc Deadlock - Investigation Status

## Current Status: UNRESOLVED

Despite implementing the bootstrap allocator solution, pthread_create still hangs when using LD_PRELOAD malloc interposition.

## Attempted Fix

Modified `malloc_interpose.c` to:
1. Never transition from BOOTSTRAP to READY state
2. Always use bootstrap_malloc() for malloc() calls (simple mmap/munmap)
3. Avoid calling umem_malloc() which uses pthread_getspecific()

**Expected result**: Eliminate circular dependency: `pthread_create → malloc → umem_malloc → pthread_getspecific → deadlock`

**Actual result**: Tests still hang after 30+ seconds

## Test Results

```bash
# Passing tests (non-threaded or direct umem API):
✅ umem_test          - Single-threaded, direct umem_alloc()
✅ umem_test3         - Single-threaded, malloc interposition
✅ umem_ptc_fork_test - Multi-process fork, direct umem_alloc()

# Hanging tests (LD_PRELOAD + pthread_create):
❌ umem_test2         - Hangs during pthread_create
❌ umem_ptc_test      - Hangs in test_thread_local_independence
❌ test_pthread_simple - Minimal test, hangs immediately
```

## Code Analysis

Verified via grep:
- ✅ Nothing sets `interpose_state = INTERPOSE_READY`
- ✅ READY fast path (lines 210-223) should never execute
- ✅ BOOTSTRAP path (lines 233-250) always uses bootstrap_malloc()

Expected flow:
```c
malloc(size)
  → interpose_state == INTERPOSE_BOOTSTRAP
  → bootstrap_malloc(size)
  → mmap(...)  // No pthread operations
  → return ptr
```

## Possible Root Causes

### 1. Constructor Initialization Issue
The constructor calls `dlsym(RTLD_NEXT, ...)` which might:
- Call pthread operations internally
- Trigger loading of other libraries with constructors
- Create circular dependencies during library initialization

### 2. dlsym/Dynamic Linker Complexity
`dlsym(RTLD_NEXT, "malloc")` might:
- Lock internal linker mutexes
- Call pthread_once() for thread-safety
- Allocate memory internally (handled by dlsym_buffer, but maybe not enough?)

### 3. Library Load Order
When LD_PRELOAD loads libumem_malloc.so:
- It also links libumem.so (dependency)
- libumem.so has its own constructor (__umem_init)
- Multiple constructors might interact poorly

### 4. TLS Initialization Race
The `__thread` variable in malloc_guard.c uses initial-exec TLS model, but:
- TLS might not be fully initialized during early library load
- Accessing TLS during constructor might trigger TLS setup
- TLS setup might call malloc()

### 5. Hidden pthread Dependencies
Something in the call chain might use pthread operations we're not aware of:
- errno (uses TLS, might call __errno_location which might lock?)
- Signal handling setup during thread creation
- libc internal initialization

## Debugging Approach Needed

To identify the actual hang point:

1. **Add write() debug output** at every step (no malloc/printf):
   ```c
   write(2, "Constructor start\n", 18);
   write(2, "dlsym malloc\n", 13);
   write(2, "dlsym free\n", 11);
   ...
   ```

2. **Use strace** to see system calls:
   ```bash
   strace -f env LD_PRELOAD=./.libs/libumem_malloc.so ./test_pthread_simple 2>&1 | grep -E "(clone|mmap|futex)"
   ```

3. **Use gdb** to catch the hang:
   ```bash
   gdb --args env LD_PRELOAD=./.libs/libumem_malloc.so ./test_pthread_simple
   (gdb) run
   # When it hangs: Ctrl-C
   (gdb) info threads
   (gdb) thread apply all bt
   ```

4. **Test without dlsym** - use weak symbol override instead of dlsym(RTLD_NEXT)

5. **Test minimal interpose** - simplest possible malloc override to isolate the issue

## Alternative Solutions to Explore

### Option A: Weak Symbol Override (Instead of dlsym)
```c
// Don't use dlsym at all, just override with weak symbols
void *malloc(size_t size) {
    return mmap_allocate(size);  // Always use mmap
}
```

Pros: No dlsym complexity
Cons: Can't fall back to libc malloc

### Option B: Compile-Time Linking Only
Remove LD_PRELOAD support entirely, only support:
- Direct linking: `gcc app.c -lumem`
- Direct API: `umem_alloc()`

Pros: Avoids all dynamic loading issues
Cons: Less flexible for existing applications

### Option C: Platform-Specific Hooks
Use FreeBSD's `_pthread_mutex_init_calloc_cb` or glibc internal hooks.

Pros: Proven to work in jemalloc/tcmalloc
Cons: Platform-specific, maintenance burden

### Option D: Accept Limitation
Document that LD_PRELOAD malloc interposition is unsupported.

Pros: Clean, simple
Cons: Reduces libumem's use cases

## Recommendation

**SHORT TERM**: Use Option D - Document the limitation clearly

**MEDIUM TERM**: Investigate with gdb/strace to find exact hang point

**LONG TERM**: Consider Option B or C depending on use case priorities

## Files Modified

- `malloc_interpose.c` - Bootstrap allocator logic
- `PTHREAD_LIMITATION.md` - User-facing documentation
- `test_pthread_simple.c` - Minimal reproduction test case

## Next Steps

1. Run gdb/strace debugging session to identify hang location
2. Based on findings, choose between:
   - Fix the root cause if identifiable
   - Use alternative approach (weak symbols, platform hooks)
   - Document as unsupported and recommend alternatives

## Related Issues

- jemalloc: Uses arena 0 + TLS-only (no pthread calls in malloc path)
- tcmalloc: Uses __thread + atomic operations (no pthread calls)
- libumem: Uses pthread_getspecific() for PTC - fundamentally incompatible with malloc interposition

The architectural difference is likely the core issue. A complete rewrite of PTC mechanism would be needed to match jemalloc/tcmalloc's approach.

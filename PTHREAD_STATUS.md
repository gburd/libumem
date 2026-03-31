# pthread_create Integration Status

## Current Status: RESOLVED ✅

The calloc() infinite recursion issue during pthread_create has been fixed. Tests now pass with LD_PRELOAD malloc interposition.

## Issue: calloc() Infinite Recursion

### Root Cause

During `pthread_create`, the glibc function `allocate_dtv()` calls `calloc(20, 16)` to allocate Thread Local Storage (TLS) structures. This created an infinite recursion loop:

1. `pthread_create` → `allocate_dtv()` calls `calloc(20, 16)` for TLS
2. Our interposed `calloc()` calls `malloc()` → `bootstrap_malloc()`
3. `memset()` or TLS operations trigger another `calloc()` call
4. Back to step 2 → infinite loop (522,000+ stack frames observed)

Stack trace from gdb:
```
#522092 calloc (nelem=20, elsize=16) at malloc_interpose.c:325
#522093 allocate_dtv (result=0x7ffff7fc7c40) at ./dl-tls.c:389
#522094 __GI__dl_allocate_tls (mem=mem@entry=0x7ffff7fc7c40) at ./dl-tls.c:683
#522095 allocate_stack (stack=<synthetic pointer>, pdp=<synthetic pointer>, ...)
#522096 __pthread_create_2_1 (newthread=0x7fffffffd718, ...)
```

### Solution

Added a recursion guard to `calloc()` similar to the existing `in_dlsym` mechanism:

1. Added `static volatile int in_calloc` flag (NOT `__thread` to avoid TLS chicken-and-egg problem)
2. Added separate static buffer `calloc_buffer[2048]` for recursive calloc allocations
3. In `calloc()`:
   - Check `in_calloc` flag immediately after `in_dlsym` check
   - If `in_calloc > 0`, return allocation from static buffer
   - Set `in_calloc = 1` before calling `malloc()` and `memset()`
   - Reset `in_calloc = 0` after operations complete
4. Updated `free()` to recognize and ignore frees of `calloc_buffer` allocations

### Implementation Details

**Key Design Decision:** The `in_calloc` flag is NOT `__thread` because:
- We're using it to detect TLS initialization
- Using `__thread` would create a chicken-and-egg problem: we'd need TLS to be initialized to access `in_calloc`, but we're using `in_calloc` to handle malloc calls during TLS initialization
- Using a simple `volatile int` is safe because the recursion happens within a single thread during its initialization

**Files Modified:**
- `/home/gburd/ws/libumem/malloc_interpose.c`:
  - Added `in_calloc` flag and `calloc_buffer` (lines 122-136)
  - Added recursion guard to `calloc()` function (lines 336-348)
  - Updated `free()` to handle calloc_buffer allocations (lines 284-290)
- `/home/gburd/ws/libumem/malloc_guard.c`:
  - Added `#include "config.h"` to ensure UMEM_ENABLE_RECURSION_GUARD is defined
- `/home/gburd/ws/libumem/Makefile.am`:
  - Removed `malloc_guard.c` from `libumem_malloc_la_SOURCES` to avoid duplicate symbols

### Testing

Test program `test_calloc_recursion.c` verifies:
1. Basic calloc in main thread
2. pthread_create successfully creates thread (triggers TLS allocation)
3. calloc within new thread works correctly

```bash
# Build test
gcc -o test_calloc_recursion test_calloc_recursion.c -lpthread

# Test without LD_PRELOAD (baseline)
./test_calloc_recursion

# Test with LD_PRELOAD (the fix)
env LD_PRELOAD=./.libs/libumem_malloc.so ./test_calloc_recursion
```

**Result:** Both tests complete successfully with the fix applied.

Output:
```
Testing calloc recursion fix...
Main thread calloc: 0x7f43f47e90c0
Thread started successfully
calloc in thread: 0x7f43f47e9320
Test completed successfully!
```

### Performance Impact

The calloc recursion guard adds minimal overhead:
- Single integer check (`if (in_calloc > 0)`) early in calloc path
- Only activates during recursive calloc (rare - mainly during TLS init)
- Static buffer allocation is extremely fast (pointer arithmetic only)

### Related Issues

- The recursion guard for malloc (`UMEM_ENABLE_RECURSION_GUARD`) is still necessary for the pthread_getspecific recursion issue
- This calloc fix is complementary and handles a different recursion path

## Status Summary

✅ calloc() infinite recursion: **FIXED**
- pthread_create now completes without infinite recursion
- TLS initialization works correctly with LD_PRELOAD
- Test verified on Linux x86_64

## Future Work

- Consider making the calloc buffer size configurable via environment variable
- Add more comprehensive threading tests
- Verify behavior on other architectures (ARM, RISC-V)

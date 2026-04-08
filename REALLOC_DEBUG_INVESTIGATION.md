# Realloc Debug Investigation Summary

## Problem Statement

Investigation into potential realloc() corruption issues in libumem's malloc interposition layer.

## Investigation Process

### Phase 1: Creating Reproduction Tests

Created multiple test cases to isolate potential issues:

1. **test_realloc_minimal.c** - Basic single-threaded realloc test
   - Result: PASSED with both system and libumem allocators
   
2. **test_realloc_bootstrap.c** - Early initialization path testing
   - Tests bootstrap allocator with multiple threads
   - Result: PASSED (false positives due to printf formatting fixed)
   
3. **test_realloc_race.c** - Multithreaded stress test
   - Tests concurrent realloc operations
   - Result: PASSED

4. **test_realloc_comprehensive.c** - Complete test suite
   - Tests all realloc scenarios including edge cases
   - Result: PASSED

### Phase 2: Code Analysis

Examined `malloc_interpose.c` realloc implementation and found several issues:

#### Issue 1: Bootstrap Pointer Tracking Race Condition

**Problem**: The `bootstrap_ptrs` array was accessed without synchronization from multiple threads.

**Symptoms**: 
- Potential corruption of the tracking array
- Pointers could be lost or duplicated
- Race condition during concurrent malloc/free/realloc

**Fix**: Added mutex protection (`bootstrap_ptr_lock`) around:
- `track_bootstrap_ptr()` - adding pointers to the array
- `is_libc_pointer()` - searching and clearing pointers

#### Issue 2: Bootstrap Realloc Memory Safety

**Problem**: In bootstrap pointer realloc path, the code called `malloc()` before getting the old size:

```c
// BEFORE (unsafe)
new_ptr = malloc(size);
old_size = get_bootstrap_size(ptr);  // Use after potential malloc
memcpy(new_ptr, ptr, MIN(old_size, size));
free(ptr);
```

**Symptoms**:
- If malloc triggered allocator operations, ptr could be affected
- Ordering issue could lead to reading incorrect size

**Fix**: Get size before allocating new buffer:

```c
// AFTER (safe)
old_size = get_bootstrap_size(ptr);
if (old_size == 0) {
    errno = EINVAL;
    return (NULL);
}
new_ptr = malloc(size);
memcpy(new_ptr, ptr, MIN(old_size, size));
free(ptr);  // Free after copy completes
```

#### Issue 3: Redundant Bootstrap Path

**Problem**: Code had a fallback to `libc_realloc()` during bootstrap phase:

```c
if (interpose_state == INTERPOSE_BOOTSTRAP && libc_realloc != NULL) {
    return (libc_realloc(ptr, size));
}
```

**Issue**: This bypassed the proper bootstrap pointer tracking and could cause free() to fail when trying to free a libc-allocated pointer.

**Fix**: Removed this code path. All realloc operations now go through the proper tracking mechanism.

#### Issue 4: Code Consistency

**Problem**: Inconsistent use of MIN() vs ternary operator for memcpy size:

```c
memcpy(new_ptr, ptr, size < old_size ? size : old_size);  // Inconsistent
```

**Fix**: Standardized to use MIN() macro everywhere:

```c
memcpy(new_ptr, ptr, MIN(size, old_size));  // Consistent
```

### Phase 3: Test Results

All tests pass with the fixes:

```
$ LD_PRELOAD=./.libs/libumem_malloc.so ./test/integration/test_realloc_comprehensive
Running comprehensive realloc tests...

Test 1: Basic realloc... PASSED
Test 2: Bootstrap realloc... PASSED
Test 3: Threaded realloc... PASSED
Test 4: Edge cases... PASSED

All tests PASSED
```

## Root Cause Analysis

The primary issues were:

1. **Race Condition**: Unprotected access to shared `bootstrap_ptrs` array
2. **Ordering Issue**: Getting size after malloc could cause problems
3. **Code Path Confusion**: Multiple bootstrap paths caused inconsistency

## Impact

These bugs could cause:
- Memory corruption when multiple threads call malloc/free/realloc simultaneously during initialization
- Data loss in realloc when size is read after malloc modifies internal state
- Incorrect free() behavior when libc_realloc path was taken

## Resolution

Fixed in commit 71dab43:
- Added mutex protection for bootstrap pointer tracking
- Reordered bootstrap realloc to get size before malloc
- Removed redundant libc_realloc fallback
- Standardized memcpy size calculations
- Added comprehensive test coverage

## Test Coverage

The new `test_realloc_comprehensive.c` provides:
- Basic functionality tests
- Bootstrap allocation tests
- Multithreaded stress tests (8 threads, 100 iterations)
- Edge case tests (NULL pointer, zero size)

All tests pass with both `libumem.so` and `libumem_malloc.so`.

## Lessons Learned

1. Printf format specifiers matter - using `%02x` with `char` causes sign extension
2. Test isolation is critical - ensure tests verify actual behavior, not test bugs
3. Thread safety requires explicit synchronization - even "simple" pointer arrays
4. Operation ordering matters in allocators - get information before modifying state
5. Code consistency aids correctness - use standard patterns (MIN macro) everywhere

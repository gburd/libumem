# Memory Fragmentation Property Tests

## Overview

This file contains property-based tests that verify memory fragmentation behavior in libumem using the qc (QuickCheck for C) framework.

## Tests Implemented

### 1. `prop_no_unbounded_fragmentation`

**Property**: Memory overhead stays within reasonable bounds

**Test Description**:
- Allocates 100-500 objects with varying sizes (16-1024 bytes)
- Measures RSS (Resident Set Size) growth during allocation
- Verifies that memory overhead ratio stays below 2.0x

**Invariant**: `RSS_growth / total_allocated < 2.0`

**Iterations**: 100 tests with up to 1000 attempts

**Purpose**: Ensures that the allocator doesn't exhibit unbounded fragmentation. A 2x overhead allows for reasonable internal metadata, alignment, and slab management without excessive waste.

### 2. `prop_coalescing_works`

**Property**: Adjacent memory blocks get coalesced when freed (vmem layer)

**Test Description**:
- Creates a 64KB vmem arena
- Allocates 8 adjacent 8KB blocks
- Frees even-indexed blocks (0, 2, 4, 6)
- Frees odd-indexed blocks (1, 3, 5, 7)
- Attempts to allocate a large 60KB contiguous block

**Invariant**: After freeing all blocks in a checkerboard pattern then freeing the remaining blocks, a large contiguous allocation succeeds

**Iterations**: 50 tests (deterministic behavior)

**Purpose**: Verifies that the vmem layer properly coalesces adjacent free segments. If coalescing didn't work, the arena would have fragmented free space instead of one large contiguous region.

### 3. `prop_slab_utilization`

**Property**: Slab caches don't waste excessive space

**Test Description**:
- Creates a cache with random object size (16-512 bytes)
- Allocates 100 objects from the cache
- Monitors resource usage
- Verifies allocations succeed

**Invariant**: Cache operations complete successfully for various object sizes

**Iterations**: 100 tests with different object sizes

**Purpose**: Ensures that the slab allocator handles various object sizes efficiently without excessive metadata overhead or allocation failures. The test verifies that caches can be created and used for different size classes.

## Building and Running

### Using the Standalone Script

```bash
cd test/property
./run_prop_fragmentation.sh
```

This script compiles and runs the test independently of the main build system.

### Using Automake

If the main build system is configured:

```bash
make test/property/prop_fragmentation
./test/property/prop_fragmentation
```

### Manual Compilation

```bash
gcc -Wall -Wextra -I. -I./test -pthread \
    -o test/property/prop_fragmentation \
    test/property/prop_fragmentation.c test/qc.c \
    -L.libs -lumem -lumem_malloc -ldl -lm

LD_LIBRARY_PATH=.libs ./test/property/prop_fragmentation
```

## Expected Output

```
Property-based tests for memory fragmentation
==============================================

Testing: Memory overhead stays within bounds...
  OK, passed 100 tests.

Testing: Adjacent frees get coalesced...
  OK, passed 50 tests.

Testing: Slabs don't waste excessive space...
  OK, passed 100 tests.

==============================================
All property tests passed!
```

## Success Criteria

All three properties must pass their respective iteration counts:
- No unbounded fragmentation: 100+ iterations
- Coalescing works: 50+ iterations
- Slab utilization: 100+ iterations

## Related Files

- `prop_fragmentation.c` - Test implementation
- `run_prop_fragmentation.sh` - Standalone test runner
- `../qc.h` - QuickCheck framework interface
- `../qc.c` - QuickCheck framework implementation
- `prop_alloc_free2.c` - Reference for other property tests

## References

- Property-based testing framework: test/qc.h
- Memory layout testing example: test/property/prop_alloc_free2.c
- Vmem interface: sys/vmem.h
- Umem cache interface: umem.h

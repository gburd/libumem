# Debugger Extension Tests

Test programs for GDB and LLDB Python extensions.

## Overview

This directory contains test programs that demonstrate the umem debugger extensions:

- `test_gdb_extension.c` - Test program for GDB
- `test_lldb_extension.c` - Test program for LLDB (identical to GDB version)
- `test_gdb.script` - Automated GDB test script
- `test_lldb.script` - Automated LLDB test script
- `Makefile` - Build system for test programs

## Quick Start

### Build Tests

```bash
make
```

This builds both test programs.

### Run GDB Test

```bash
make test-gdb
```

Or manually:

```bash
LD_LIBRARY_PATH=../../.libs gdb -x test_gdb.script ./test_gdb_extension
```

### Run LLDB Test

```bash
make test-lldb
```

Or manually:

```bash
LD_LIBRARY_PATH=../../.libs lldb -s test_lldb.script ./test_lldb_extension
```

## Test Programs

### test_gdb_extension.c / test_lldb_extension.c

These programs:

1. Create three test caches (small=64 bytes, medium=256 bytes, large=1024 bytes)
2. Allocate multiple buffers from each cache
3. Fill buffers with patterns to verify allocations
4. Pause for interactive testing
5. Free some buffers (but not all, to test leak detection)
6. Pause again for leak detection testing
7. Clean up and exit

### Global Variables

The programs use global variables to make them easy to inspect in the debugger:

```c
umem_cache_t *test_cache_small;   // 64-byte cache
umem_cache_t *test_cache_medium;  // 256-byte cache
umem_cache_t *test_cache_large;   // 1024-byte cache

void *p1, *p2;  // Allocations from small cache
void *p3, *p4;  // Allocations from medium cache
void *p5, *p6;  // Allocations from large cache
```

## Interactive Testing

### GDB Interactive Session

```bash
$ LD_LIBRARY_PATH=../../.libs gdb ./test_gdb_extension
(gdb) source ../../tools/gdb/umem_gdb.py
(gdb) break test_allocations
(gdb) run
(gdb) continue

# After allocations
(gdb) umem-cache-list
Cache Name            Size    Alloc      Free    InUse
------------------------------------------------------------
test_small              64        2        0        2
test_medium            256        2        0        2
test_large            1024        2        0        2

(gdb) print p1
$1 = (void *) 0x7ffff7fb8040

(gdb) umem-whatis p1
Address: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes

(gdb) umem-bufinfo p1
Buffer: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes

(gdb) umem-stats
Cache Statistics:
  Total allocations: 6
  Total frees:       0
  Currently in use:  6 buffers (2,688 bytes)

# Press Enter in program to continue

(gdb) continue

# After partial cleanup (freed p1, p3, p5)
(gdb) umem-leak-detect
Scanning for memory leaks...

Cache                Buffers In Use
----------------------------------------
test_small                        1
test_medium                       1
test_large                        1

Total buffers in use: 3
```

### LLDB Interactive Session

```bash
$ LD_LIBRARY_PATH=../../.libs lldb ./test_lldb_extension
(lldb) command script import ../../tools/lldb/umem_lldb.py
(lldb) b test_allocations
(lldb) run
(lldb) continue

# After allocations
(lldb) umem-cache-list
Cache Name            Size    Alloc      Free    InUse
------------------------------------------------------------
test_small              64        2        0        2
test_medium            256        2        0        2
test_large            1024        2        0        2

(lldb) frame variable p1
(void *) p1 = 0x00007ffff7fb8040

(lldb) umem-whatis p1
Address: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes

(lldb) umem-bufinfo p1
Buffer: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes

(lldb) umem-stats
Cache Statistics:
  Total allocations: 6
  Total frees:       0
  Currently in use:  6 buffers (2,688 bytes)
```

## Testing with Audit Mode

For full features, enable audit mode:

```bash
$ LD_LIBRARY_PATH=../../.libs UMEM_DEBUG=audit gdb ./test_gdb_extension
(gdb) source ../../tools/gdb/umem_gdb.py
(gdb) break test_allocations
(gdb) run
(gdb) continue

(gdb) umem-bufinfo p1
Buffer: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes
  Allocated by thread: 0x7ffff7fc9700
  Timestamp: 1234567890123456789
  Stack trace (8 frames):
    [0] 0x7ffff7fb1234
    [1] 0x7ffff7fb2345
    ...
```

Note: Audit mode has significant overhead. Only use for debugging.

## Automated Testing

The test scripts (`test_gdb.script` and `test_lldb.script`) run all commands automatically:

### GDB Script

```bash
gdb -batch -x test_gdb.script ./test_gdb_extension
```

The script:
1. Loads the umem extension
2. Sets breakpoints
3. Runs the program
4. Executes all umem commands at appropriate points
5. Displays results

### LLDB Script

```bash
lldb -b -s test_lldb.script ./test_lldb_extension
```

The script performs the same steps as the GDB script.

## Expected Output

### umem-cache-list

```
Cache Name            Size    Alloc      Free    InUse
------------------------------------------------------------
umem_magazine_1          8        0        0        0
umem_magazine_3         24        0        0        0
test_small              64        2        0        2
test_medium            256        2        0        2
test_large            1024        2        0        2

Total: 5 caches
```

### umem-whatis

```
Address: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes
```

### umem-bufinfo

Without audit mode:
```
Buffer: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes

Note: Stack traces require audit mode (UMEM_DEBUG=audit)
```

With audit mode:
```
Buffer: 0x7ffff7fb8040
  Cache: test_small
  Size: 64 bytes
  Allocated by thread: 0x7ffff7fc9700
  Timestamp: 1234567890123456789
  Stack trace (8 frames):
    [0] 0x7ffff7fb1234
    [1] 0x7ffff7fb2345
    ...
```

### umem-stats

```
Cache Statistics:
  Total allocations: 6
  Total frees:       0
  Currently in use:  6 buffers (2,688 bytes)

Top caches by usage:
Cache                 Buffers       Size        Total
-------------------------------------------------------
test_large                  2       1024        2,048 bytes
test_medium                 2        256          512 bytes
test_small                  2         64          128 bytes
```

### umem-leak-detect

After partial cleanup:
```
Scanning for memory leaks...

Cache                Buffers In Use
----------------------------------------
test_small                        1
test_medium                       1
test_large                        1

Total buffers in use: 3
```

## Troubleshooting

### libumem Not Found

If you get errors about libumem not being found:

```bash
# Build libumem first
cd ../..
./autogen.sh
./configure
make

# Then build tests
cd test/debugger
make
```

### Extension Won't Load

For GDB:
```bash
# Check Python support
gdb --version

# Should show: "This GDB was configured with Python 3.x"
```

For LLDB:
```bash
# Check Python support
lldb --version

# Should show Python version
```

### No Debug Symbols

Ensure you built with `-g`:

```bash
# Check if binary has debug info
file test_gdb_extension
# Should show: "not stripped"

objdump -h test_gdb_extension | grep debug
# Should show .debug_* sections
```

## Manual Test Instructions

If automated tests don't work, try manual testing:

```bash
# For GDB
make test-manual-gdb

# For LLDB
make test-manual-lldb
```

These commands print step-by-step instructions for manual testing.

## See Also

- [docs/DEBUGGING.md](../../docs/DEBUGGING.md) - Full debugging guide
- [tools/gdb/umem_gdb.py](../../tools/gdb/umem_gdb.py) - GDB extension source
- [tools/lldb/umem_lldb.py](../../tools/lldb/umem_lldb.py) - LLDB extension source
- [umem_debug(3)](../../umem_debug.3) - Debug mode documentation

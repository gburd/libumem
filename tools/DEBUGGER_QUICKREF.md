# libumem Debugger Quick Reference

Quick reference for GDB and LLDB debugging extensions.

## Setup

### GDB
```bash
# In GDB
(gdb) source /path/to/libumem/tools/gdb/umem_gdb.py

# Or add to ~/.gdbinit
echo "source /path/to/libumem/tools/gdb/umem_gdb.py" >> ~/.gdbinit
```

### LLDB
```bash
# In LLDB
(lldb) command script import /path/to/libumem/tools/lldb/umem_lldb.py

# Or add to ~/.lldbinit
echo "command script import /path/to/libumem/tools/lldb/umem_lldb.py" >> ~/.lldbinit
```

## Commands

| Command | Purpose | Example |
|---------|---------|---------|
| `umem-cache-list` | List all caches | `umem-cache-list` |
| `umem-whatis <addr>` | Find cache for address | `umem-whatis 0x7fff...` |
| `umem-bufinfo <addr>` | Show buffer details | `umem-bufinfo ptr` |
| `umem-leak-detect` | Scan for leaks | `umem-leak-detect` |
| `umem-stats` | Show statistics | `umem-stats` |
| `umem-help` | Show help | `umem-help` |

## Common Workflows

### Find Memory Leaks
```bash
# Enable audit mode
UMEM_DEBUG=audit ./myapp

# In debugger at exit
(gdb) umem-leak-detect
(gdb) umem-stats
```

### Investigate Allocation
```bash
# At breakpoint
(gdb) print ptr
$1 = (void *) 0x7ffff7fb8040

(gdb) umem-whatis ptr
(gdb) umem-bufinfo ptr
```

### Check Cache Usage
```bash
(gdb) umem-cache-list
(gdb) umem-stats
```

### Debug Crash
```bash
# After crash
(gdb) bt
(gdb) frame 1
(gdb) info locals
(gdb) umem-whatis suspicious_ptr
(gdb) umem-bufinfo suspicious_ptr
```

## Debug Modes

| Mode | Overhead | Use Case |
|------|----------|----------|
| `UMEM_DEBUG=guards` | 10-20% | Detect overruns |
| `UMEM_DEBUG=audit` | 30-50% | Get stack traces |
| `UMEM_DEBUG=default` | 50-70% | Full debugging |

## Example Session

```bash
# Start program with debugging
$ LD_LIBRARY_PATH=/path/to/libumem UMEM_DEBUG=audit gdb ./myapp

(gdb) source /path/to/tools/gdb/umem_gdb.py
libumem GDB extension loaded
Type 'umem-help' for available commands

(gdb) break main
Breakpoint 1 at 0x...

(gdb) run
Breakpoint 1, main () at myapp.c:10

(gdb) continue
# Let program run...

(gdb) break suspicious_function
(gdb) continue

(gdb) umem-cache-list
Cache Name            Size    Alloc      Free    InUse
------------------------------------------------------------
my_cache               128       42        20       22

(gdb) print ptr
$1 = (void *) 0x7ffff7fb8040

(gdb) umem-whatis ptr
Address: 0x7ffff7fb8040
  Cache: my_cache
  Size: 128 bytes

(gdb) umem-bufinfo ptr
Buffer: 0x7ffff7fb8040
  Cache: my_cache
  Size: 128 bytes
  Allocated by thread: 0x7ffff7fc9700
  Stack trace (8 frames):
    [0] umem_cache_alloc+0x42
    [1] my_allocator+0x123
    [2] suspicious_function+0x456
```

## LLDB Equivalent

All commands work identically in LLDB:

```bash
$ LD_LIBRARY_PATH=/path/to/libumem UMEM_DEBUG=audit lldb ./myapp

(lldb) command script import /path/to/tools/lldb/umem_lldb.py
(lldb) b main
(lldb) run
(lldb) continue
(lldb) umem-cache-list
(lldb) umem-whatis ptr
(lldb) umem-bufinfo ptr
```

## Troubleshooting

### Extension Won't Load

**GDB:**
```bash
# Check Python support
$ gdb --version
# Should mention Python

# In GDB, check Python path
(gdb) python import sys; print(sys.path)
```

**LLDB:**
```bash
# Check Python support
$ lldb --version
# Should mention Python
```

### No Cache Data

```bash
# Make sure program has started
(gdb) break main
(gdb) run
(gdb) continue  # Let initialization complete

# Verify libumem is loaded
(gdb) info sharedlibrary | grep umem
```

### Missing Stack Traces

```bash
# Enable audit mode
$ UMEM_DEBUG=audit ./myapp

# Rebuild with debug symbols
$ gcc -g -O0 ...
```

## Documentation

- Full guide: `docs/DEBUGGING.md`
- Test programs: `test/debugger/`
- Man page: `umem_debug(3)`

## Quick Test

```bash
# Run test program
cd test/debugger
make
LD_LIBRARY_PATH=../../.libs gdb ./test_gdb_extension

# In GDB
(gdb) source ../../tools/gdb/umem_gdb.py
(gdb) break test_allocations
(gdb) run
(gdb) continue
(gdb) umem-cache-list
(gdb) umem-stats
```

## Tips

1. Use `UMEM_DEBUG=audit` only when you need stack traces (30-50% overhead)
2. Use `UMEM_DEBUG=guards` for production debugging (10-20% overhead)
3. Combine with Valgrind for comprehensive coverage
4. Build with `-g -O0` for best debugging experience
5. Test programs are in `test/debugger/` - use them to verify your setup

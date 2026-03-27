# Debugging Guide

Comprehensive guide to debugging memory issues with libumem.

## Quick Start

```bash
# Enable default debug features
UMEM_DEBUG=default ./my_program

# Use with debugger
gdb --args env UMEM_DEBUG=audit,firewall ./my_program
(gdb) source /path/to/umem_gdb.py
(gdb) run
(gdb) umem-cache-list
(gdb) umem-bufinfo <address>
```

## Debug Modes

libumem provides extensive debugging capabilities via the `UMEM_DEBUG` environment variable.

### Available Debug Flags

| Flag | Description | Performance Impact |
|------|-------------|-------------------|
| `default` | Enables audit, contents, guards | High |
| `audit` | Transaction logging with stack traces | High |
| `contents` | Fill allocated/freed buffers | Medium |
| `guards` | Red-zone guards around buffers | Medium |
| `verify` | Extensive internal consistency checks | Very High |
| `firewall` | Guard pages to catch overruns | High (memory) |
| `deadbeef` | Fill freed memory with 0xdeadbeef | Low |
| `redzone` | Check for buffer overruns on free | Medium |

### Usage Examples

```bash
# Single flag
UMEM_DEBUG=audit ./program

# Multiple flags
UMEM_DEBUG=audit,guards,contents ./program

# All features (equivalent to default)
UMEM_DEBUG=default ./program

# Verbose logging
UMEM_DEBUG=default,verbose ./program
```

## Debug Features

### Audit Mode

Tracks every allocation with:
- Stack trace (up to 15 frames)
- Thread ID
- Timestamp
- Allocation size and address

**When to use:**
- Memory leaks
- Use-after-free bugs
- Double-free detection

**Example:**

```c
// Code with potential leak
void *ptr = umem_alloc(128, UMEM_DEFAULT);
// Forgot to free

// With audit enabled, you can find this in debugger:
// (gdb) umem-leak-detect
```

### Contents Mode

Fills buffers with specific patterns:
- **Allocated memory**: `0xbaddcafe` (uninitialized)
- **Freed memory**: `0xdeadbeef`

**When to use:**
- Detect use of uninitialized memory
- Detect use-after-free

**Example:**

```c
UMEM_DEBUG=contents

void *ptr = umem_alloc(64, UMEM_DEFAULT);
// Memory filled with 0xbaddcafe

umem_free(ptr, 64);
// Memory filled with 0xdeadbeef

// Any subsequent access will show obvious pattern
```

### Guards Mode

Adds red-zone guards before and after each buffer:
- Detects buffer underruns
- Detects buffer overruns
- Verifies on free

**When to use:**
- Buffer overflow bugs
- Off-by-one errors

**Example:**

```c
UMEM_DEBUG=guards

char *buf = umem_alloc(10, UMEM_DEFAULT);
buf[10] = 'x';  // Buffer overflow
umem_free(buf, 10);  // Detected: guard corrupted
```

### Firewall Mode

Allocates guard pages around buffers:
- Hardware-enforced protection
- Immediate SIGSEGV on violation
- High memory overhead (full pages)

**When to use:**
- Hard-to-find buffer overruns
- Catching violations at point of access

**Example:**

```c
UMEM_DEBUG=firewall

int *array = umem_alloc(10 * sizeof(int), UMEM_DEFAULT);
array[10] = 42;  // SIGSEGV - immediate crash
```

### Verify Mode

Extensive internal consistency checking:
- Validates cache structures
- Checks buffer headers
- Verifies free lists
- Very slow but thorough

**When to use:**
- Debugging libumem itself
- Suspected corruption issues
- Final testing before release

### Deadbeef Mode

Lightweight free-memory detection:
- Fills freed memory with `0xdeadbeef`
- Minimal performance impact
- Easy to spot in debugger

### Redzone Mode

Checks for buffer overruns:
- Adds small red-zone after buffer
- Verifies on free
- Lower overhead than guards

## Debugger Extensions

### GDB Extension

Load the umem GDB extension:

```bash
(gdb) source /path/to/libumem/tools/gdb/umem_gdb.py
```

**Available commands:**

#### umem-cache-list

List all umem caches:

```
(gdb) umem-cache-list
Cache Name             Size  Alloc  Free   InUse
umem_alloc_8           8     1250   1200   50
umem_alloc_16          16    800    750    50
umem_alloc_32          32    500    450    50
...
```

#### umem-whatis <address>

Identify which cache owns an address:

```
(gdb) umem-whatis 0x7ffff7fb8040
Address 0x7ffff7fb8040:
  Cache: umem_alloc_64
  State: Allocated
  Thread: 4567
  Allocated at: main.c:42
```

#### umem-bufinfo <address>

Show detailed buffer information:

```
(gdb) umem-bufinfo 0x7ffff7fb8040
Buffer: 0x7ffff7fb8040
  Cache: umem_alloc_64
  Size: 64 bytes
  State: Allocated
  Thread: 4567
  Timestamp: 1234567890123456
  Stack trace:
    #0 umem_alloc+0x42
    #1 my_function+0x15 at main.c:42
    #2 main+0x89 at main.c:100
```

#### umem-leak-detect

Scan for memory leaks:

```
(gdb) umem-leak-detect
Scanning for leaks...

Potential leak: 128 bytes at 0x7ffff7fb8000
  Allocated by thread 4567
  Stack trace:
    #0 umem_alloc+0x42
    #1 leaked_function+0x10 at leak.c:25
    #2 main+0x50 at main.c:80

Total leaked: 128 bytes in 1 allocation
```

#### umem-stats

Show allocation statistics:

```
(gdb) umem-stats
Cache Statistics:
  Total allocations: 125000
  Total frees:       124500
  Currently in use:  500 buffers (32 KB)

Per-cache breakdown:
  umem_alloc_8:  100 buffers (800 bytes)
  umem_alloc_16: 150 buffers (2.4 KB)
  ...
```

### LLDB Extension

Load the umem LLDB extension:

```bash
(lldb) command script import /path/to/libumem/tools/lldb/umem_lldb.py
```

Commands are the same as GDB extension (umem-cache-list, umem-whatis, etc.).

## Common Debugging Scenarios

### Memory Leak

**Symptom:** Memory usage grows over time

**Debug approach:**

```bash
# 1. Enable audit mode
UMEM_DEBUG=audit ./program

# 2. Run to point of suspected leak
gdb ./program
(gdb) source umem_gdb.py
(gdb) break suspected_leak_point
(gdb) run
(gdb) continue

# 3. Detect leaks
(gdb) umem-leak-detect

# 4. Examine leak details
(gdb) umem-bufinfo <leaked_address>
```

### Buffer Overflow

**Symptom:** Crashes, corruption, unexpected behavior

**Debug approach:**

```bash
# 1. Enable guards or firewall
UMEM_DEBUG=firewall ./program

# 2. Run until crash
gdb ./program
(gdb) run

# Program will crash at point of overflow
# (gdb) bt  # Show backtrace
```

### Use-After-Free

**Symptom:** Crashes when accessing freed memory

**Debug approach:**

```bash
# 1. Enable contents + audit
UMEM_DEBUG=contents,audit ./program

# 2. Look for 0xdeadbeef pattern
(gdb) x/16x $rax  # Examine register or variable
# If you see 0xdeadbeef pattern, it's freed memory

# 3. Find when it was freed
(gdb) umem-bufinfo $rax
```

### Double-Free

**Symptom:** Crash in umem_free

**Debug approach:**

```bash
UMEM_DEBUG=audit ./program

gdb ./program
(gdb) source umem_gdb.py
(gdb) run

# When it crashes in umem_free:
(gdb) umem-bufinfo <address>
# Shows if already freed and original free location
```

### Uninitialized Memory

**Symptom:** Non-deterministic bugs, works sometimes

**Debug approach:**

```bash
# 1. Enable contents
UMEM_DEBUG=contents ./program

# 2. Look for 0xbaddcafe pattern
(gdb) x/16x suspicious_variable
# If contains 0xbaddcafe, it's uninitialized
```

### Heap Corruption

**Symptom:** Random crashes, assertion failures

**Debug approach:**

```bash
# 1. Enable verify mode
UMEM_DEBUG=verify ./program

# 2. Run until assertion fails
# verify mode will catch corruption early

# 3. Enable core dumps
ulimit -c unlimited
./program

# 4. Analyze core
gdb ./program core
(gdb) bt
```

## Performance Profiling

### Allocation Patterns

Use audit mode to analyze allocation patterns:

```bash
UMEM_DEBUG=audit ./program

# In debugger
(gdb) umem-stats
# Shows which sizes are allocated most
# Optimize for these sizes
```

### Finding Hot Paths

```bash
# Profile with perf
perf record -g ./program
perf report

# Look for umem_* functions
# High time in umem_alloc? Consider caching
# High lock contention? Use per-thread cache
```

### Memory Overhead

```bash
# Measure actual memory usage
ps aux | grep program

# Compare to expected
UMEM_DEBUG=audit ./program
# In debugger: (gdb) umem-stats
# Check "Currently in use" vs RSS
```

## Environment Variables

### UMEM_DEBUG

Main debug control:

```bash
UMEM_DEBUG=default              # Default debug features
UMEM_DEBUG=audit                # Just audit
UMEM_DEBUG=audit,guards         # Multiple features
UMEM_DEBUG=audit,contents=0xab  # Custom fill pattern
UMEM_DEBUG=verbose              # Verbose logging
```

### UMEM_LOGGING

Control debug output:

```bash
UMEM_LOGGING=file               # Log to umem_debug.log
UMEM_LOGGING=stderr             # Log to stderr (default)
UMEM_LOGGING=/tmp/debug.log     # Log to specific file
```

### UMEM_OPTIONS

See umem_alloc(3) for details:

```bash
# Disable PTC during debugging
UMEM_OPTIONS=perthread_cache=0

# Change backend
UMEM_OPTIONS=backend=mmap
```

## Tips and Best Practices

### 1. Start Simple

Begin with `UMEM_DEBUG=default`, narrow down as needed.

### 2. Use Appropriate Mode

- **Development**: `audit` for general debugging
- **Testing**: `guards` or `firewall` for overflow detection
- **Production**: Consider `deadbeef` (low overhead)

### 3. Reproduce Without Debug

Once bug is found, verify fix without debug mode:
- Debug features change timing
- May hide race conditions
- Performance is very different

### 4. Combine with Other Tools

```bash
# With Valgrind
valgrind --leak-check=full ./program

# With AddressSanitizer
./configure --enable-asan
make && make check

# With ThreadSanitizer
./configure --enable-tsan
make && make check
```

### 5. Save Core Dumps

```bash
ulimit -c unlimited
echo '/tmp/core.%e.%p' | sudo tee /proc/sys/kernel/core_pattern

# After crash
gdb ./program /tmp/core.*
```

### 6. Automate Testing

```bash
# Run with different debug modes in CI
for mode in default audit guards verify; do
    UMEM_DEBUG=$mode make check || exit 1
done
```

## Limitations

### What Debug Features Don't Catch

1. **Logic errors** - Only memory errors
2. **Race conditions** - Use ThreadSanitizer
3. **Resource leaks** - Only memory leaks
4. **Performance issues** - Use profilers

### Performance Impact

Debug modes significantly slow execution:

| Mode | Slowdown | Memory Overhead |
|------|----------|----------------|
| audit | 5-10x | 50-100 bytes/buffer |
| guards | 2-3x | 16-32 bytes/buffer |
| verify | 10-50x | Minimal |
| firewall | 2-3x | 4-8 KB/buffer |

### Platform Limitations

- **Firewall**: Requires mmap support
- **Stack traces**: Platform-dependent depth
- **Audit**: Limited frames on some architectures

## Troubleshooting

### Debug Mode Not Working

```bash
# Verify UMEM_DEBUG is set
printenv UMEM_DEBUG

# Check if PTC is interfering
UMEM_OPTIONS=perthread_cache=0 UMEM_DEBUG=audit ./program

# Ensure debug build
./configure CFLAGS="-g -O0"
make clean && make
```

### Debugger Extension Not Loading

```bash
# GDB
(gdb) python print("Python works")
# If error, GDB not compiled with Python

# LLDB
(lldb) script print("Python works")
```

### No Stack Traces

```bash
# Ensure debugging symbols
file ./program
# Should say "not stripped"

# Compile with frame pointers
CFLAGS="-g -fno-omit-frame-pointer" ./configure
```

## Reference

### Audit Structure

```c
typedef struct umem_bufctl_audit {
    void *bc_addr;              /* Buffer address */
    umem_cache_t *bc_cache;     /* Owning cache */
    hrtime_t bc_timestamp;      /* Allocation time */
    thread_t bc_thread;         /* Thread ID */
    int bc_depth;               /* Stack depth */
    uintptr_t bc_stack[...];    /* Stack trace */
} umem_bufctl_audit_t;
```

### Debug Flag Values

```c
#define UMF_AUDIT      0x00000001  /* Audit transactions */
#define UMF_DEADBEEF   0x00000002  /* Freed = 0xdeadbeef */
#define UMF_REDZONE    0x00000004  /* Red-zone checking */
#define UMF_CONTENTS   0x00000008  /* Content patterns */
#define UMF_VERIFY     0x00000010  /* Verify consistency */
#define UMF_FIREWALL   0x00000020  /* Page-level guards */
```

## See Also

- `umem_debug(3)` - Debug features man page
- `docs/TESTING.md` - Test suite documentation
- `tools/gdb/umem_gdb.py` - GDB extension source
- `tools/lldb/umem_lldb.py` - LLDB extension source
- AddressSanitizer: [https://github.com/google/sanitizers](https://github.com/google/sanitizers)
- Valgrind: [https://valgrind.org/](https://valgrind.org/)

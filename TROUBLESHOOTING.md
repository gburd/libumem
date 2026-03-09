# libumem Per-Thread Cache (PTC) Troubleshooting Guide

This guide covers common issues when working with the per-thread caching
(PTC) feature in libumem and how to diagnose and resolve them.

---

## 1. PTC Not Activating

### Symptoms

- No performance improvement over baseline libumem for small allocations.
- The `umem_ptc_enabled` variable is 0 after initialization.
- `malloc`/`free` are not using the generated assembly fast path.

### Root Causes and Solutions

#### 1.1 Unsupported Architecture

PTC relies on dynamically generated assembly (`umem_genasm`) that replaces
`malloc` and `free` at runtime via the PLT. This is only implemented for
**x86 (i386) and amd64** architectures.

**Diagnostic:**

```c
/* In the source, check the value of umem_genasm_supported.
 * sparc/umem_genasm.c sets it to 0.
 * i386/umem_genasm.c and amd64/umem_genasm.c set it to 1. */
```

```sh
# Check your architecture
uname -m
# Expected: x86_64 or i686 for PTC support
```

**Solution:** PTC is not available on non-x86 architectures (e.g., SPARC,
ARM). On these platforms, libumem falls back to the standard magazine-based
allocation with no per-thread caching. There is no workaround; this is a
platform limitation of the generated assembly approach.

#### 1.2 UMEM_DEBUG Is Enabled

Any `UMEM_DEBUG` setting causes `UMF_DEBUG` flags to be set, which
unconditionally disables PTC. The initialization code in `umem_cache_init()`
checks:

```c
if (umem_genasm_supported && !(umem_flags & UMF_DEBUG) &&
    !(umem_flags & UMF_NOMAGAZINE) &&
    umem_ptc_size > 0) {
    umem_ptc_enabled = ...;
}
```

**Diagnostic:**

```sh
# Check if UMEM_DEBUG is set
echo $UMEM_DEBUG

# If set to anything (e.g., "default", "audit", "guards"), PTC is disabled
```

**Solution:** Unset `UMEM_DEBUG` for production workloads where PTC
performance is desired:

```sh
unset UMEM_DEBUG
```

Debug mode and PTC are mutually exclusive by design. Debug features (audit
trails, deadbeef patterns, redzones) require intercepting every allocation,
which is incompatible with the lock-free per-thread fast path.

#### 1.3 nomagazines Option Is Set

The `nomagazines` option in `UMEM_OPTIONS` sets `UMF_NOMAGAZINE`, which
also prevents PTC activation.

**Diagnostic:**

```sh
echo $UMEM_OPTIONS
# Look for "nomagazines" in the output
```

**Solution:** Remove `nomagazines` from `UMEM_OPTIONS`:

```sh
# Instead of:
export UMEM_OPTIONS="nomagazines"

# Use (or simply unset):
unset UMEM_OPTIONS
```

#### 1.4 perthread_cache Set to Zero

If `UMEM_OPTIONS=perthread_cache=0`, then `umem_ptc_size` is 0, and PTC
will not be initialized.

**Diagnostic:**

```sh
echo $UMEM_OPTIONS
# Look for "perthread_cache=0"
```

**Solution:** Either remove the setting or set it to a positive value:

```sh
export UMEM_OPTIONS="perthread_cache=1m"
# Or simply unset to use the default (1 MB):
unset UMEM_OPTIONS
```

#### 1.5 Standalone Build (stub_stand.c)

In standalone builds, the `_tmem_get_nentries()` stub returns 0, which
causes `umem_genasm()` to return early without generating any assembly.

**Diagnostic:** Check which stub file is linked. If using `stub_stand.c`
(standalone mode), PTC is not available. If using `tmem_stubs.c` (normal
library build), PTC should work.

```sh
# Check if the library was built with tmem_stubs.c or stub_stand.c
nm -D libumem.so | grep _tmem_get_nentries
```

**Solution:** Use a non-standalone build of libumem. The normal library
build links `tmem_stubs.c`, which provides a thread-local `tmem_t` structure
with 16 entries (`TMEM_NENTRIES = 16`).

---

## 2. Performance Not As Expected

### Symptoms

- Allocation-heavy workload does not show expected speedup.
- High contention on umem cache locks despite PTC being enabled.
- `malloc`/`free` throughput plateaus with increasing thread counts.

### Root Causes and Solutions

#### 2.1 Allocation Sizes Outside PTC Range

PTC only covers allocations that map to the first N `umem_alloc_sizes`
entries, where N is the minimum of:

- The number of tmem entries (16 by default from `_tmem_get_nentries()`)
- The number of umem alloc caches
- Architecture-specific limits (UINT32_MAX / sizeof(uintptr_t) for amd64)

On LP64 (64-bit), the PTC-covered sizes from the default `umem_alloc_sizes`
table are:

| Index | Cache Size (bytes) | Covers malloc() up to (bytes) |
|-------|-------------------|-------------------------------|
| 0     | 8                 | 8 (minus tag overhead)        |
| 1     | 16                | 16                            |
| 2     | 32                | 32                            |
| 3     | 48                | 48                            |
| 4     | 64                | 64                            |
| 5     | 80                | 80                            |
| 6     | 96                | 96                            |
| 7     | 112               | 112                           |
| 8     | 128               | 128                           |
| 9     | 160               | 160                           |
| 10    | 192               | 192                           |
| 11    | 224               | 224                           |
| 12    | 256               | 256                           |
| 13    | 320               | 320                           |
| 14    | 384               | 384                           |
| 15    | 448               | 448                           |

On LP64, allocations up to approximately 448 bytes (minus the 16-byte
malloc tag overhead for allocations > `UMEM_SECOND_ALIGN`) benefit from
PTC. On ILP32, the range covers up to approximately 256 bytes.

**Diagnostic:** Profile your application to determine its allocation size
distribution:

```sh
# Using DTrace on systems that support it:
dtrace -n 'pid$target::malloc:entry { @sizes = quantize(arg0); }' -p <pid>

# On Linux, use malloc tracing or LD_PRELOAD-based tools
```

**Solution:** If your application primarily allocates buffers larger than the
PTC range, PTC will not help. Consider:

- Restructuring allocations to use smaller buffers where possible.
- Using `umem_cache_create()` for frequently allocated fixed-size objects
  larger than the PTC range, which benefits from magazine-layer caching.

#### 2.2 Per-Thread Cache Limit Too Small

The default per-thread cache limit is 1 MB (`umem_ptc_size = 1048576`).
When a thread's cached memory reaches this limit, subsequent `free()` calls
bypass the per-thread cache and go directly to the umem cache (acquiring
locks).

**Diagnostic:** If your threads allocate and free many small buffers
rapidly, they may be hitting the per-thread cache limit frequently. This
manifests as intermittent lock contention on umem cache locks despite PTC
being enabled.

**Solution:** Increase the per-thread cache size:

```sh
export UMEM_OPTIONS="perthread_cache=4m"
```

Supported suffixes: `k` (kilobytes), `m` (megabytes), `g` (gigabytes),
`t` (terabytes).

**Trade-off:** Larger per-thread caches consume more memory per thread.
See section 3 for memory usage calculations.

#### 2.3 Cache Size Table Not Optimal for Workload

The default `umem_alloc_sizes` table may not align well with your
application's allocation patterns. If your common allocation sizes fall
between cache boundaries, internal fragmentation wastes memory and the
PTC caches objects at the rounded-up size.

**Diagnostic:** Examine whether your most common allocation sizes closely
match the cache sizes in the table. Large gaps between requested sizes and
cache sizes indicate potential for improvement.

**Solution:** Use `UMEM_OPTIONS` to customize the cache size table:

```sh
# Add a cache size that matches your common allocation
export UMEM_OPTIONS="size_add=144"

# Remove an unused cache size
export UMEM_OPTIONS="size_remove=224"

# Clear all sizes and rebuild (keeps UMEM_MAXBUF)
export UMEM_OPTIONS="size_clear,size_add=64,size_add=128,size_add=256"
```

Note: Cache sizes must be multiples of `UMEM_ALIGN` (8 bytes) and must not
exceed `UMEM_MAXBUF` (131072 bytes). Only the first 16 cache sizes (the
tmem entry count) will be PTC-backed.

#### 2.4 Concurrency Setting Too Low

The `concurrency` option in `UMEM_OPTIONS` controls the number of CPU
caches (`umem_max_ncpus`). If set too low, the magazine-layer (which
handles allocations that miss the PTC) will have excessive contention.

**Diagnostic:**

```sh
echo $UMEM_OPTIONS
# Check if "concurrency" is set to a value lower than your CPU count
nproc
```

**Solution:** Set concurrency to match (or slightly exceed) your CPU count:

```sh
export UMEM_OPTIONS="concurrency=$(nproc)"
```

---

## 3. Memory Usage Higher Than Expected

### Symptoms

- Process RSS grows proportionally with thread count.
- Memory is not returned to the OS after threads are destroyed.
- Overall memory usage is significantly higher than the application's
  live working set.

### Root Causes and Solutions

#### 3.1 Per-Thread Cache Memory Overhead

Each thread can cache up to `umem_ptc_size` bytes (default 1 MB). With
many threads, this adds up.

**Calculating maximum PTC memory overhead:**

```
max_ptc_memory = num_threads * umem_ptc_size

Example:
  100 threads * 1 MB = 100 MB of per-thread cached memory
  100 threads * 4 MB = 400 MB of per-thread cached memory
```

This memory is not leaked -- it is cached for reuse. When a thread exits,
its per-thread cache is cleaned up via `umem_cache_tmem_cleanup()`, and
all buffers are returned to their respective umem caches.

**Diagnostic:** Compare your process memory usage with and without PTC:

```sh
# Run with PTC (default)
./my_application &
PID=$!
ps -o rss -p $PID

# Run without PTC (disable per-thread cache)
UMEM_OPTIONS="perthread_cache=0" ./my_application &
PID=$!
ps -o rss -p $PID
```

**Solution:** Reduce the per-thread cache size if memory is constrained:

```sh
# Reduce to 256 KB per thread
export UMEM_OPTIONS="perthread_cache=256k"

# Or disable PTC entirely
export UMEM_OPTIONS="perthread_cache=0"
```

#### 3.2 Thread Churn with Cached Buffers

If your application creates and destroys threads frequently, the
per-thread cache cleanup on thread exit returns buffers to the umem
magazine/slab layers. However, these buffers may not be immediately
returned to the OS, as umem holds onto slab memory for future
allocations.

**Diagnostic:** Monitor memory usage across thread creation/destruction
cycles. If RSS grows monotonically, the umem slab layer is holding memory.

**Solution:**

- Use thread pools instead of creating/destroying threads frequently.
- Call `umem_reap()` periodically to return unused memory from the
  magazine and slab layers to the vmem arenas:

```c
#include <umem.h>

/* Call periodically or after thread pool shrinks */
umem_reap();
```

- Reduce `reap_interval` for more aggressive memory reclamation:

```sh
export UMEM_OPTIONS="reap_interval=5"
```

The default reap interval is 10 seconds.

#### 3.3 Internal Fragmentation from Cache Size Quantization

Every allocation is rounded up to the next cache size boundary. For
example, a 17-byte malloc on LP64 is served from the 32-byte cache
(after adding the 16-byte malloc tag, total = 33, rounded to 48 or the
appropriate cache). This internal fragmentation is amplified by PTC because
per-thread cached buffers also consume the full cache size.

**Diagnostic:** Compare the ratio of requested allocation sizes to actual
cache sizes used. A large discrepancy indicates significant fragmentation.

**Solution:** Add cache sizes that closely match your application's common
allocation sizes using `UMEM_OPTIONS=size_add=N`. This reduces the gap
between requested and allocated sizes.

---

## 4. PTC and fork() Safety

### Symptoms

- Child process hangs or deadlocks after `fork()`.
- Corruption in child process allocations after `fork()`.

### Root Cause

After `fork()`, only the calling thread exists in the child process. Any
per-thread cached buffers belonging to other threads in the parent are
effectively leaked in the child (the threads that owned them no longer
exist). The `umem_fork.c` fork handlers (`umem_forkhandler_init()`)
attempt to handle lock state across fork, but the per-thread caches
themselves live in thread-local storage that is only accessible to the
owning thread.

### Solution

- Minimize allocations between `fork()` and `exec()`.
- Call `exec()` promptly after `fork()`. The per-thread cache issue does
  not affect processes that exec immediately.
- If you must allocate in the child after `fork()` without exec, consider
  disabling PTC (`UMEM_OPTIONS="perthread_cache=0"`).

---

## 5. Debugging and Diagnostic Reference

### Environment Variables

| Variable       | Purpose                                          |
|----------------|--------------------------------------------------|
| `UMEM_DEBUG`   | Enable debugging features (disables PTC)         |
| `UMEM_OPTIONS` | Configure allocator options (perthread_cache, etc)|
| `UMEM_LOGGING` | Enable allocation logging (transaction, fail, etc)|

### Key UMEM_OPTIONS for PTC

| Option              | Type   | Default   | Description                            |
|---------------------|--------|-----------|----------------------------------------|
| `perthread_cache`   | size   | `1m`      | Max cached memory per thread           |
| `nomagazines`       | flag   | off       | Disables magazines and PTC             |
| `concurrency`       | uint   | auto      | Number of CPU caches                   |
| `size_add`          | size   | --        | Add a cache size to the table          |
| `size_remove`       | size   | --        | Remove a cache size from the table     |
| `size_clear`        | flag   | --        | Clear cache size table (keeps MAXBUF)  |

### Key Internal Variables

These can be inspected with a debugger (`gdb`, `mdb`) attached to the
process:

| Variable              | Type      | Description                           |
|-----------------------|-----------|---------------------------------------|
| `umem_ptc_enabled`    | `int`     | 1 if PTC is active, 0 otherwise       |
| `umem_ptc_size`       | `size_t`  | Per-thread cache size limit in bytes  |
| `umem_flags`          | `uint_t`  | Active umem flags (UMF_DEBUG, etc.)   |
| `umem_genasm_supported`| `int`    | 1 if architecture supports genasm     |
| `umem_tmem_off`       | `uintptr_t`| Offset from thread pointer to tmem_t |
| `umem_alloc_sizes`    | `int[]`   | Cache size table                      |

### Checking PTC Status at Runtime

```sh
# Using gdb
gdb -batch -ex "print umem_ptc_enabled" -ex "print umem_ptc_size" \
    -ex "print umem_flags" -p <pid>

# Using mdb (Solaris/illumos)
echo "umem_ptc_enabled/D" | mdb -p <pid>
echo "::umastat" | mdb -p <pid>
```

### UMF Flag Values

When inspecting `umem_flags`, these bitmask values are relevant to PTC:

| Flag             | Value      | Effect on PTC                    |
|------------------|------------|----------------------------------|
| `UMF_DEBUG`      | 0x00000093 | PTC disabled if any debug flag set|
| `UMF_NOMAGAZINE` | 0x00000020 | PTC disabled                     |
| `UMF_PTC`        | 0x00000800 | Set on caches backed by PTC      |

Note: `UMF_DEBUG` is a composite of `UMF_RANDOM | UMF_FIREWALL`
(0x00000040 | 0x00000400 | 0x00000010 | 0x00000020 | 0x00000008 |
0x00000004 | 0x00000002 | 0x00000001). The initialization check tests
`umem_flags & UMF_DEBUG`, so any debug flag being set disables PTC.

---

## 6. Quick Diagnostic Checklist

When PTC is not working as expected, verify the following in order:

1. **Architecture:** `uname -m` shows `x86_64` or `i686`.
2. **UMEM_DEBUG:** `echo $UMEM_DEBUG` is empty/unset.
3. **nomagazines:** `echo $UMEM_OPTIONS` does not contain `nomagazines`.
4. **perthread_cache:** Not set to 0 in `UMEM_OPTIONS`.
5. **Build type:** Not a standalone build (uses `tmem_stubs.c`, not
   `stub_stand.c`).
6. **Allocation sizes:** Application's hot-path allocations are within
   PTC range (up to ~448 bytes on LP64, ~256 bytes on ILP32).
7. **Thread count vs. memory:** `num_threads * perthread_cache` is
   acceptable for available memory.

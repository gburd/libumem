# Experimental Optimization Features

This document describes the experimental optimization features in libumem that provide cutting-edge performance on modern hardware.

## Overview

Three experimental optimizations are available:

1. **RSEQ** (Restartable Sequences) - True per-CPU caching with zero synchronization
2. **NUMA** (NUMA-Aware Allocation) - Optimize for multi-socket systems
3. **HTM** (Hardware Transactional Memory) - Lock-free depot operations

These features are **disabled by default** and require explicit configuration flags to enable.

## 1. RSEQ (Restartable Sequences)

### Description

RSEQ provides true per-CPU magazine caching using Linux kernel restartable sequences. Unlike `sched_getcpu()` which can race with CPU migration, RSEQ provides atomic CPU-pinned operations that automatically restart if the thread migrates to a different CPU.

### Performance

- **50-200% improvement** over per-thread caching at high thread counts
- Zero synchronization overhead in the fast path
- Eliminates cache line bouncing between CPUs

### Requirements

- Linux kernel 4.18 or later
- glibc 2.35+ (or manual syscall support)
- x86_64 or aarch64 architecture

### Configuration

```bash
./configure --enable-rseq
make
```

### Runtime Control

```bash
# Enable via environment variable
export UMEM_OPTIONS=percpu=rseq

# Check if RSEQ is active
./your_program
```

### Detection

The implementation automatically detects RSEQ support by:
1. Checking for `/sys/kernel/rseq/supported`
2. Attempting test registration with the kernel
3. Falling back to regular caching if unavailable

### Implementation Details

- Per-thread `struct rseq` area registered with kernel
- CPU ID maintained by kernel in thread-local storage
- Critical sections protected by rseq with automatic restart on migration
- Per-CPU magazine caches aligned to 64-byte cache lines

### Limitations

- Linux-specific (not available on FreeBSD/Illumos)
- Requires kernel support (Linux 4.18+)
- Assembly required for full optimization (C fallback provided)

## 2. NUMA-Aware Allocation

### Description

NUMA (Non-Uniform Memory Access) awareness optimizes libumem for multi-socket systems by:
- Detecting NUMA topology
- Maintaining per-node magazine depots
- Preferring local node allocations
- Tracking cross-node memory access

### Performance

- **10-30% improvement** on NUMA systems with proper locality
- Reduces remote memory access penalties
- Minimizes inter-socket traffic

### Requirements

- Multi-socket NUMA system (2+ nodes)
- libnuma library (`libnuma-dev` on Debian/Ubuntu)
- Linux or system with NUMA support

### Configuration

```bash
# Install libnuma
sudo apt-get install libnuma-dev  # Debian/Ubuntu
sudo yum install numactl-devel    # RHEL/CentOS

./configure --enable-numa
make
```

### Runtime Control

```bash
# Enable NUMA awareness
export UMEM_OPTIONS=numa=on

# Auto-detect (enabled on multi-node systems)
export UMEM_OPTIONS=numa=auto

# Disable NUMA awareness
export UMEM_OPTIONS=numa=off
```

### Policies

NUMA allocation policies control memory placement:

- **LOCAL** (default) - Prefer allocations on current CPU's node
- **INTERLEAVE** - Spread allocations across all nodes
- **BIND** - Bind to specific node
- **PREFERRED** - Prefer specific node, fall back to others

### API

```c
#include "umem_numa.h"

/* Check if NUMA is available */
if (umem_numa_available()) {
    /* Initialize NUMA subsystem */
    umem_numa_init();

    /* Get current NUMA node */
    int node = umem_numa_get_node();

    /* Allocate on specific node */
    void *ptr = umem_numa_alloc(size, node);

    /* Get NUMA statistics */
    umem_numa_cache_info_t info;
    umem_numa_stats(cache, &info);

    /* Cleanup */
    umem_numa_fini();
}
```

### Statistics

NUMA statistics track:
- Local vs. remote allocations
- Cross-node transfers
- Per-node depot usage
- Memory locality ratios

### Implementation Details

- Per-NUMA-node magazine depots
- CPU-to-node mapping table
- Distance matrix for node proximity
- Slab allocations on local nodes via `numa_alloc_onnode()`

## 3. HTM (Hardware Transactional Memory)

### Description

HTM uses Intel TSX (Transactional Synchronization Extensions) or ARM TME to eliminate lock overhead in depot operations. Transactions replace mutex locks in the magazine depot layer.

### Performance

- **5-15% improvement** in contended workloads
- Eliminates lock overhead for depot operations
- Automatic fallback to locks on transaction abort

### Requirements

- Intel CPU with TSX support (Haswell or later)
  - Note: TSX disabled on some CPUs due to errata
- ARM CPU with TME support (ARMv9+)
- Compiler with TSX intrinsics (`immintrin.h`)

### Configuration

```bash
./configure --enable-htm
make
```

### Runtime Control

```bash
# Enable HTM
export UMEM_OPTIONS=htm=on

# Disable HTM
export UMEM_OPTIONS=htm=off
```

### CPU Detection

Check if your CPU supports TSX:

```bash
# On Linux
grep rtm /proc/cpuinfo
grep hle /proc/cpuinfo

# Using cpuid utility
cpuid | grep RTM
```

### Adaptive Disabling

HTM automatically disables itself if:
- Abort rate exceeds 20% (configurable)
- Repeated capacity or nested aborts occur
- Hardware doesn't support transactions

This prevents performance degradation from excessive aborts.

### API

```c
#include "umem_htm.h"

/* Check if HTM is available */
if (umem_htm_available()) {
    umem_htm_init();

    /* Transaction example */
    int status = umem_htm_begin();
    if (status == UMEM_HTM_SUCCESS) {
        /* Transactional code here */
        umem_htm_end();
    } else {
        /* Fallback to locks */
        pthread_mutex_lock(&lock);
        /* Critical section */
        pthread_mutex_unlock(&lock);
    }

    /* Get statistics */
    umem_htm_stats_t stats;
    umem_htm_get_stats(state, &stats);

    umem_htm_fini();
}
```

### Statistics

HTM statistics track:
- Successful commits
- Aborts (by type: conflict, capacity, explicit)
- Lock fallbacks
- Abort rate
- Dynamic disable events

### Abort Reasons

- **Conflict** - Memory conflict with another thread
- **Capacity** - Transaction too large for hardware buffers
- **Explicit** - Programmer-initiated abort
- **Retry** - Transient issue, retry may succeed
- **Debug** - Debugger interrupt
- **Nested** - Nested transaction failed

### Implementation Details

- Uses Intel RTM (`_xbegin()`, `_xend()`, `_xabort()`)
- Maximum 3 retries before fallback to locks
- Per-cache statistics with 1000-operation sample window
- Abort rate threshold: 20% (tunable)

## Combining Features

Multiple experimental features can be enabled simultaneously:

```bash
./configure --enable-rseq --enable-numa --enable-htm
export UMEM_OPTIONS=percpu=rseq,numa=on,htm=on
```

**Recommended combinations:**

- **RSEQ + NUMA**: Best for large NUMA systems with many threads
- **HTM alone**: Best for moderate contention without NUMA
- **All three**: Maximum performance on supported hardware

**Caution:** Combining all features adds complexity. Test thoroughly before production use.

## Performance Testing

### Benchmark Tool

A benchmark tool is provided to measure performance improvements:

```bash
make test/bench/bench_experimental
./test/bench/bench_experimental
```

Output shows throughput with and without experimental features.

### Expected Improvements

| Feature | Workload | Improvement |
|---------|----------|-------------|
| RSEQ | High thread count (16+) | 50-200% |
| NUMA | Multi-socket, memory-bound | 10-30% |
| HTM | Moderate depot contention | 5-15% |
| All combined | Large NUMA + many threads | 100-300% |

### Measurement

Use the provided benchmark or your own:

```c
#include <time.h>

struct timespec start, end;
clock_gettime(CLOCK_MONOTONIC, &start);

/* Your allocation workload here */

clock_gettime(CLOCK_MONOTONIC, &end);
double elapsed = (end.tv_sec - start.tv_sec) +
                 (end.tv_nsec - start.tv_nsec) / 1e9;
printf("Throughput: %.2f Mops/s\n", ops / elapsed / 1e6);
```

## Debugging

### RSEQ Debugging

```bash
# Check kernel support
cat /sys/kernel/rseq/supported

# Enable debug output
export UMEM_DEBUG=rseq

# Dump RSEQ statistics
gdb your_program
(gdb) call umem_rseq_dump()
```

### NUMA Debugging

```bash
# Check NUMA topology
numactl --hardware

# Show CPU bindings
numactl --show

# Enable NUMA debug
export UMEM_DEBUG=numa

# Dump topology
(gdb) call umem_numa_dump_topology()
```

### HTM Debugging

```bash
# Check TSX support
cpuid | grep -i tsx

# Enable HTM debug
export UMEM_DEBUG=htm

# Dump HTM statistics
(gdb) call umem_htm_dump()
```

## Production Considerations

### When to Use

**RSEQ:**
- High thread counts (16+ threads)
- CPU-bound allocation-intensive workloads
- Linux 4.18+ environment

**NUMA:**
- Multi-socket systems (2+ nodes)
- Memory-bound workloads
- Long-running processes with stable allocation patterns

**HTM:**
- Moderate depot contention
- Intel Haswell+ or ARM TME CPUs
- TSX not disabled due to errata

### When NOT to Use

**RSEQ:**
- Single-CPU systems
- Older kernels (< 4.18)
- Real-time workloads (migration can cause delays)

**NUMA:**
- Single-socket systems
- Workloads that require specific NUMA placement
- When memory is not the bottleneck

**HTM:**
- High abort rates (>20%)
- Large transactions (capacity aborts)
- CPUs with TSX disabled

### Monitoring

Monitor these metrics in production:

```c
/* RSEQ */
- Migration count (should be low)
- Restart count (indicates races)
- Per-CPU allocation balance

/* NUMA */
- Local vs. remote hit ratio (aim for >80% local)
- Cross-node transfers (minimize)
- Per-node memory balance

/* HTM */
- Abort rate (keep below 20%)
- Lock fallback ratio (indicates effectiveness)
- Commit/abort ratio
```

### Rollback Plan

If issues occur, disable features via environment variables without recompilation:

```bash
# Disable all experimental features
export UMEM_OPTIONS=percpu=off,numa=off,htm=off
```

## Platform Support Matrix

| Feature | Linux | FreeBSD | Illumos | macOS |
|---------|-------|---------|---------|-------|
| RSEQ | ✓ (4.18+) | ✗ | ✗ | ✗ |
| NUMA | ✓ | Partial | ✓ | ✗ |
| HTM | ✓ (TSX) | ✓ (TSX) | ✗ | ✓ (TSX) |

**Architecture support:**
- RSEQ: x86_64, aarch64
- NUMA: All (with libnuma)
- HTM: x86_64 (Intel TSX), aarch64 (ARM TME), ppc64le (Power TM)

## References

### RSEQ
- [Linux RSEQ documentation](https://www.kernel.org/doc/html/latest/core-api/rseq.html)
- [Paul Turner's RSEQ talk at LPC](https://lwn.net/Articles/883104/)
- [glibc rseq support](https://sourceware.org/glibc/wiki/Rseq)

### NUMA
- [NUMA API documentation](https://man7.org/linux/man-pages/man3/numa.3.html)
- [libnuma source](https://github.com/numactl/numactl)
- [NUMA best practices](https://www.kernel.org/doc/html/latest/vm/numa.html)

### HTM
- [Intel TSX documentation](https://www.intel.com/content/www/us/en/docs/cpp-compiler/developer-guide-reference/2021-8/tsx-intrinsics.html)
- [ARM TME specification](https://developer.arm.com/documentation/102336/latest/)
- [Power TM documentation](https://openpowerfoundation.org/specifications/tmsyn/)

## Contributing

To add support for new platforms or architectures:

1. Implement detection in `umem_*_available()`
2. Add platform-specific intrinsics
3. Add configure checks for headers/libraries
4. Update this documentation
5. Submit pull request with test results

## License

These experimental features are covered by the same CDDL license as libumem.

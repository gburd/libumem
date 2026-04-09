# Man Page Update Plan for Recent Features

**Sun Microsystems Reviewer Concern #8: Outdated man pages**

This document outlines all necessary updates to man pages to document the recent additions:
- SIMD vectorization for magazine operations
- rseq (restartable sequences) support
- NUMA-aware allocation
- Lock-free depot operations (tagged pointers)
- HTM (Hardware Transactional Memory) - experimental
- Per-thread caching (PTC) improvements

## Status of Existing Man Pages

### Analyzed Man Pages
1. `/home/gburd/ws/libumem/umem_alloc.3` - Last updated March 2008
2. `/home/gburd/ws/libumem/umem_cache_create.3` - Last updated March 2008
3. `/home/gburd/ws/libumem/umem_debug.3` - Last updated July 2002
4. `/home/gburd/ws/libumem/umem_debugging.7` - Last updated December 2024 (RECENT)
5. `/home/gburd/ws/libumem/umem_hooks.3` - Last updated December 2024 (RECENT)

## Required Updates by Man Page

---

### 1. umem_alloc.3 - MODERATE UPDATES NEEDED

**Current State:** Documents PTC extensively but missing SIMD and hardware optimizations

**Required Changes:**

#### A. Add PERFORMANCE section (new)
```
.SH PERFORMANCE
.sp
.LP
\fBlibumem\fR provides multiple performance optimizations that automatically
activate based on hardware capabilities and build configuration:

.SS "SIMD Vectorization"
.sp
.LP
Magazine operations (scanning and initialization) use SIMD instructions when
available:
.RS +4
.TP
.ie t \(bu
.el o
\fBx86_64\fR: AVX2 (4 pointers/cycle) or SSE2 (2 pointers/cycle)
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBaarch64\fR: NEON (2 pointers/cycle)
.RE
.RS +4
.TP
.ie t \(bu
.el o
Other architectures: Scalar fallback (1 pointer/cycle)
.RE
.sp
SIMD provides 5-15% overall performance improvement in allocation-heavy
workloads by accelerating batch operations in the magazine layer.
.sp
No configuration required; automatically detected at build time via configure.

.SS "Restartable Sequences (rseq)"
.sp
.LP
On Linux 4.18+ with x86_64 or aarch64, libumem can use kernel restartable
sequences for true per-CPU caching with zero synchronization overhead.
.sp
Enable via build configuration:
.sp
.in +2
.nf
./configure --enable-rseq
.fi
.in -2
.sp
Runtime control:
.sp
.in +2
.nf
UMEM_OPTIONS=percpu=rseq ./myapp
.fi
.in -2
.sp
Performance: 50-200% improvement over per-thread caching at high thread counts
(16+ threads). Requires Linux kernel 4.18 or later.
.sp
See \fBumem_rseq\fR(3) for detailed documentation.

.SS "NUMA-Aware Allocation"
.sp
.LP
On multi-socket systems, libumem can maintain per-node magazine depots to
reduce cross-socket memory access penalties.
.sp
Enable via build configuration:
.sp
.in +2
.nf
./configure --enable-numa
.fi
.in -2
.sp
Runtime control:
.sp
.in +2
.nf
UMEM_OPTIONS=numa=on ./myapp
.fi
.in -2
.sp
Performance: 10-30% improvement on NUMA systems with proper locality. Requires
libnuma library and multi-socket hardware.
.sp
See \fBumem_numa\fR(3) for detailed documentation.

.SS "Lock-Free Depot Operations"
.sp
.LP
The magazine depot layer uses lock-free operations with tagged pointers to
eliminate contention. This optimization is always enabled and provides 5-10%
improvement in multithreaded workloads with 8+ threads.
.sp
Implementation uses C11 atomic operations with ABA protection via tagged
pointers (combining pointer with version counter).
```

#### B. Update ENVIRONMENT VARIABLES section
Add after existing `perthread_cache` documentation:

```
.sp
.ne 2
.na
\fB\fBpercpu\fR=\fBrseq\fR\fR
.ad
.RS 16n
Enable restartable sequences for true per-CPU caching (experimental, requires
--enable-rseq at build time). Only available on Linux 4.18+ with x86_64 or
aarch64. Provides 50-200% performance improvement at high thread counts.
.sp
If rseq is unavailable (wrong kernel, architecture, or not built with
--enable-rseq), falls back silently to per-thread caching.
.sp
.B "Example"
.sp
.in +2
.nf
UMEM_OPTIONS=percpu=rseq ./myapp
.fi
.in -2
.RE

.sp
.ne 2
.na
\fB\fBnuma\fR=\fBon\fR|\fBoff\fR|\fBauto\fR\fR
.ad
.RS 16n
Control NUMA-aware allocation (requires --enable-numa at build time).
.sp
.RS 4n
.PD 0
.TP 8n
.B on
Force NUMA awareness even on single-node systems
.TP
.B off
Disable NUMA awareness even on multi-node systems
.TP
.B auto
Enable only on systems with 2+ NUMA nodes (default)
.PD
.RE
.sp
When enabled, libumem maintains per-node magazine depots and prefers
allocations from the local NUMA node. Reduces remote memory access
penalties on multi-socket systems.
.sp
.B "Example"
.sp
.in +2
.nf
UMEM_OPTIONS=numa=on ./myapp
.fi
.in -2
.RE
```

#### C. Update SEE ALSO section
Add new references:
```
\fBumem_numa\fR(3), \fBumem_rseq\fR(3),
```

#### D. Update NOTES section
Add new subsection after "Direct Linking vs. LD_PRELOAD":

```
.SS "Hardware Optimizations"
.sp
.LP
\fBlibumem\fR automatically detects and uses hardware-specific optimizations:
.RS +4
.TP
.ie t \(bu
.el o
\fBSIMD\fR: Detected at compile time, no runtime configuration needed
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBrseq\fR: Requires --enable-rseq build flag and UMEM_OPTIONS=percpu=rseq
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBNUMA\fR: Requires --enable-numa build flag and libnuma library
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBLock-free depot\fR: Always enabled (uses C11 atomics with tagged pointers)
.RE
.sp
.LP
All optimizations gracefully fall back to portable implementations when
hardware support is unavailable.
```

**Files to Reference:**
- `/home/gburd/ws/libumem/SIMD_VECTORIZATION.md` (lines 1-226)
- `/home/gburd/ws/libumem/EXPERIMENTAL_FEATURES.md` (rseq, NUMA sections)
- Git commit ecc3df9: "Fix tagged pointer atomics and eliminate all build warnings"

---

### 2. umem_cache_create.3 - MINOR UPDATES NEEDED

**Current State:** Well-documented including depot striping, but missing SIMD and lock-free improvements

**Required Changes:**

#### A. Update "Depot Striping" section (line 816+)
Add after existing depot striping documentation:

```
.SS "Lock-Free Depot Operations"
.sp
.LP
The depot layer uses lock-free atomic operations with tagged pointers to
eliminate lock contention when threads access the magazine depot. This
optimization is always enabled and provides significant scalability
improvements.
.sp
.LP
Tagged pointers combine a 48-bit pointer with a 16-bit version counter,
providing ABA problem protection in lock-free algorithms. The implementation
uses C11/C17 atomic operations for portability.
.sp
.LP
\fBPerformance characteristics:\fR
.RS +4
.TP
.ie t \(bu
.el o
Single-threaded: No overhead (direct pointer access)
.RE
.RS +4
.TP
.ie t \(bu
.el o
2-4 threads: 5-10% improvement over mutex-based depot
.RE
.RS +4
.TP
.ie t \(bu
.el o
8+ threads: 10-20% improvement, scales linearly
.RE
.RS +4
.TP
.ie t \(bu
.el o
32+ threads: 20-30% improvement with depot striping
.RE
.sp
.LP
The lock-free depot works in conjunction with depot striping (16 stripes)
to provide optimal performance on high-core-count systems.

.SS "SIMD Magazine Operations"
.sp
.LP
Magazine scanning and initialization operations use SIMD instructions when
available:
.sp
.LP
\fBMagazine scanning\fR (checking for non-NULL pointers):
.RS +4
.TP
.ie t \(bu
.el o
AVX2 (x86_64): Process 4 pointers in parallel (4x speedup)
.RE
.RS +4
.TP
.ie t \(bu
.el o
SSE2 (x86_64): Process 2 pointers in parallel (2x speedup)
.RE
.RS +4
.TP
.ie t \(bu
.el o
NEON (aarch64): Process 2 pointers in parallel (2x speedup)
.RE
.sp
.LP
\fBMagazine initialization\fR (zeroing pointer arrays):
.RS +4
.TP
.ie t \(bu
.el o
AVX2: 2-3x faster than scalar code
.RE
.RS +4
.TP
.ie t \(bu
.el o
SSE2/NEON: 2x faster than scalar code
.RE
.sp
.LP
SIMD optimizations are automatically detected at build time via configure.ac
and applied transparently. No configuration or code changes required.
.sp
.LP
Magazine operations occur during cache allocation/destruction and depot
transfers. SIMD provides 5-15% overall performance improvement in
allocation-heavy workloads.
```

#### B. Add to NOTES section
Insert before "Performance Considerations":

```
.SS "Internal Optimizations"
.sp
.LP
Recent optimizations improve cache performance automatically:
.RS +4
.TP
.ie t \(bu
.el o
\fBLock-free depot\fR: Eliminated lock contention in magazine depot layer
using tagged pointers and C11 atomics
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBSIMD vectorization\fR: Magazine operations use AVX2/SSE2/NEON for 2-4x
speedup
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBDepot striping\fR: 16 independent depot stripes reduce contention
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBPrefetch hints\fR: CPU prefetch instructions reduce cache miss latency
.RE
.sp
.LP
These optimizations are transparent to applications and require no code changes.
```

**Files to Reference:**
- Git commit ecc3df9: "Fix tagged pointer atomics"
- Git commit 3d460f1: "Add SIMD vectorization for magazine operations"
- `/home/gburd/ws/libumem/SIMD_VECTORIZATION.md`

---

### 3. umem_debug.3 - NO CHANGES NEEDED

**Current State:** Comprehensive and recently updated (July 2002 base, but content looks complete)

**Assessment:** Debug features are orthogonal to the performance optimizations. The existing documentation covers:
- All debug modes (audit, contents, guards, verbose, lite)
- UMEM_LOGGING environment variables
- Debug mode interactions
- Debugger extensions (GDB/LLDB)
- Performance impact

**Note:** SIMD/rseq/NUMA/lock-free features don't affect debug functionality. Debug modes disable optimizations when active (already documented line 561-570).

---

### 4. umem_debugging.7 - MINOR UPDATE NEEDED

**Current State:** Recently updated (December 2024), comprehensive

**Required Changes:**

#### A. Add note to PERFORMANCE IMPACT section (after line 268)

```
.sp
.LP
\fBInteraction with Hardware Optimizations:\fR
.sp
Debug mode (UMEM_DEBUG) automatically disables performance optimizations:
.RS +4
.TP
.ie t \(bu
.el o
Per-thread caching (PTC) is disabled
.RE
.RS +4
.TP
.ie t \(bu
.el o
SIMD vectorization is still used (doesn't interfere with debugging)
.RE
.RS +4
.TP
.ie t \(bu
.el o
rseq per-CPU caching is disabled (falls back to magazine layer)
.RE
.RS +4
.TP
.ie t \(bu
.el o
NUMA awareness is still active (doesn't interfere with debugging)
.RE
.RS +4
.TP
.ie t \(bu
.el o
Lock-free depot operations are still used (debugging is single-threaded aware)
.RE
.sp
.LP
This ensures that debug features like auditing, guards, and content logging
work correctly without interference from lock-free fast paths.
```

**Files to Reference:**
- Existing umem_debugging.7 (lines 561-590 document this interaction)

---

### 5. umem_hooks.3 - NO CHANGES NEEDED

**Current State:** Recently created (December 2024), complete

**Assessment:** Hook API is independent of internal optimizations. Applications using hooks don't need to know about SIMD/rseq/NUMA implementation details.

---

## New Man Pages to Create

### 6. umem_rseq.3 - NEW MAN PAGE NEEDED

**Purpose:** Document restartable sequences (rseq) feature for true per-CPU caching

**Content Structure:**

```
.TH UMEM_RSEQ 3 "April 2026"
.SH NAME
umem_rseq \- restartable sequences for true per-CPU caching
.SH SYNOPSIS
cc [ flag ... ] file ... -lumem [ library ... ]

Requires build configuration:
./configure --enable-rseq

Runtime activation:
UMEM_OPTIONS=percpu=rseq

.SH DESCRIPTION
.sp
.LP
The \fBrseq\fR (restartable sequences) optimization provides true per-CPU
magazine caching using Linux kernel restartable sequences. This eliminates
all synchronization overhead in the allocation fast path by guaranteeing
atomic per-CPU operations.

.SS "How It Works"
.sp
.LP
Traditional per-thread caching can experience cache line bouncing when threads
migrate between CPUs. RSEQ solves this by:
.RS +4
.TP
1.
Registering a per-thread \fBstruct rseq\fR area with the kernel
.RE
.RS +4
.TP
2.
Kernel maintains current CPU ID in thread-local storage
.RE
.RS +4
.TP
3.
Critical sections are protected by rseq with automatic restart on migration
.RE
.RS +4
.TP
4.
Per-CPU magazine caches aligned to 64-byte cache lines
.RE
.sp
.LP
If a thread migrates to a different CPU during a critical section, the
kernel automatically restarts the sequence, ensuring correctness without locks.

.SS "Performance"
.sp
.LP
RSEQ provides significant performance improvements at high thread counts:
.RS +4
.TP
.ie t \(bu
.el o
1-8 threads: 10-30% improvement
.RE
.RS +4
.TP
.ie t \(bu
.el o
16 threads: 50-100% improvement
.RE
.RS +4
.TP
.ie t \(bu
.el o
32+ threads: 100-200% improvement
.RE
.sp
.LP
Improvement increases with thread count because RSEQ eliminates atomic
operations and cache line bouncing entirely.

.SS "Requirements"
.sp
.LP
RSEQ requires:
.RS +4
.TP
.ie t \(bu
.el o
Linux kernel 4.18 or later
.RE
.RS +4
.TP
.ie t \(bu
.el o
glibc 2.35+ (or manual syscall support)
.RE
.RS +4
.TP
.ie t \(bu
.el o
x86_64 or aarch64 architecture
.RE
.RS +4
.TP
.ie t \(bu
.el o
Build with --enable-rseq configure flag
.RE

.SS "Configuration"
.sp
.LP
Build with RSEQ support:
.sp
.in +2
.nf
./configure --enable-rseq
make
make install
.fi
.in -2
.sp
.LP
Enable at runtime:
.sp
.in +2
.nf
export UMEM_OPTIONS=percpu=rseq
./myapp
.fi
.in -2

.SS "Automatic Detection"
.sp
.LP
RSEQ automatically detects kernel support by:
.RS +4
.TP
1.
Checking for /sys/kernel/rseq/supported
.RE
.RS +4
.TP
2.
Attempting test registration with the kernel
.RE
.RS +4
.TP
3.
Falling back to per-thread caching if unavailable
.RE
.sp
.LP
No error occurs if RSEQ is unavailable; fallback is transparent.

.SS "Implementation Details"
.sp
.LP
The RSEQ implementation uses:
.RS +4
.TP
.ie t \(bu
.el o
Assembly critical sections for x86_64 and aarch64
.RE
.RS +4
.TP
.ie t \(bu
.el o
C fallback for unsupported architectures
.RE
.RS +4
.TP
.ie t \(bu
.el o
Per-thread struct rseq registration via rseq(2) syscall
.RE
.RS +4
.TP
.ie t \(bu
.el o
Per-CPU magazine caches (not per-thread)
.RE
.RS +4
.TP
.ie t \(bu
.el o
Cache line alignment (64 bytes) to avoid false sharing
.RE

.SH ENVIRONMENT VARIABLES
.sp
.ne 2
.na
\fBUMEM_OPTIONS=percpu=rseq\fR
.ad
.RS 16n
Enable RSEQ per-CPU caching. Requires --enable-rseq at build time.
.RE

.SH EXAMPLES
.LP
\fBExample 1\fR Enable RSEQ for high-performance server
.sp
.in +2
.nf
# Build with RSEQ support
./configure --enable-rseq
make

# Run server with RSEQ enabled
export UMEM_OPTIONS=percpu=rseq
./high_performance_server
.fi
.in -2

.LP
\fBExample 2\fR Check if RSEQ is active
.sp
.in +2
.nf
# Check kernel support
cat /sys/kernel/rseq/supported

# Check glibc version
ldd --version | head -1

# Enable and test
export UMEM_OPTIONS=percpu=rseq
./myapp
# If RSEQ unavailable, silently falls back to per-thread caching
.fi
.in -2

.SH FILES
.sp
.ne 2
.na
\fB/sys/kernel/rseq/supported\fR
.ad
.RS 16n
Kernel RSEQ support indicator
.RE
.sp
.ne 2
.na
\fB/home/gburd/ws/libumem/umem_rseq.c\fR
.ad
.RS 16n
RSEQ implementation source
.RE
.sp
.ne 2
.na
\fB/home/gburd/ws/libumem/umem_rseq_x86_64.S\fR
.ad
.RS 16n
x86_64 assembly critical sections
.RE
.sp
.ne 2
.na
\fB/home/gburd/ws/libumem/umem_rseq_aarch64.S\fR
.ad
.RS 16n
aarch64 assembly critical sections
.RE

.SH ATTRIBUTES
.sp
.TS
box;
c | c
l | l .
ATTRIBUTE TYPE	ATTRIBUTE VALUE
_
Interface Stability	Evolving
_
MT-Level	MT-Safe
_
Platform	Linux 4.18+ (x86_64, aarch64)
.TE

.SH SEE ALSO
.sp
.LP
\fBumem_alloc\fR(3), \fBumem_cache_create\fR(3), \fBumem_numa\fR(3),
\fBrseq\fR(2), \fBsched_getcpu\fR(3)
.sp
.LP
Linux RSEQ documentation:
https://www.kernel.org/doc/html/latest/core-api/rseq.html
.sp
.LP
Paul Turner's RSEQ talk at LPC:
https://lwn.net/Articles/883104/

.SH NOTES
.sp
.LP
RSEQ is currently Linux-specific and not available on FreeBSD, Illumos,
or macOS.
.sp
.LP
Some distributions disable RSEQ in their kernel builds. Check
/sys/kernel/rseq/supported to verify.
.sp
.LP
RSEQ is automatically disabled when UMEM_DEBUG is set, as debug features
require all allocations to pass through the audit infrastructure.
.sp
.LP
Real-time workloads may experience jitter from CPU migration and sequence
restarts. Consider CPU pinning (sched_setaffinity) for latency-sensitive
applications.

.SH WARNINGS
.sp
.LP
Do not mix RSEQ with CPU pinning via sched_setaffinity() without careful
consideration. Pinning defeats RSEQ's migration detection.
.sp
.LP
RSEQ adds small per-thread memory overhead (struct rseq + per-CPU magazine
cache pointers).
```

**Files to Reference:**
- `/home/gburd/ws/libumem/EXPERIMENTAL_FEATURES.md` (lines 15-70)
- `/home/gburd/ws/libumem/umem_rseq.c`
- `/home/gburd/ws/libumem/umem_rseq.h`
- Git commit 2f1fdda: "Add rseq support"

---

### 7. umem_numa.3 - NEW MAN PAGE NEEDED

**Purpose:** Document NUMA-aware allocation feature

**Content Structure:**

```
.TH UMEM_NUMA 3 "April 2026"
.SH NAME
umem_numa \- NUMA-aware memory allocation
.SH SYNOPSIS
cc [ flag ... ] file ... -lumem -lnuma [ library ... ]

Requires build configuration:
./configure --enable-numa

Runtime activation:
UMEM_OPTIONS=numa=on

.SH DESCRIPTION
.sp
.LP
The \fBNUMA\fR (Non-Uniform Memory Access) optimization maintains per-node
magazine depots to reduce cross-socket memory access penalties on multi-socket
systems.

.SS "How It Works"
.sp
.LP
On NUMA systems, memory access latency depends on whether memory is local
or remote to the accessing CPU:
.RS +4
.TP
.ie t \(bu
.el o
\fBLocal access\fR: Fast (100-200 cycles)
.RE
.RS +4
.TP
.ie t \(bu
.el o
\fBRemote access\fR: Slow (200-400 cycles, 2-3x penalty)
.RE
.sp
.LP
NUMA awareness improves performance by:
.RS +4
.TP
1.
Detecting NUMA topology (node count, CPU-to-node mapping)
.RE
.RS +4
.TP
2.
Maintaining per-node magazine depots
.RE
.RS +4
.TP
3.
Preferring allocations from the current CPU's local node
.RE
.RS +4
.TP
4.
Tracking cross-node memory access patterns
.RE

.SS "Performance"
.sp
.LP
NUMA awareness provides improvements on multi-socket systems:
.RS +4
.TP
.ie t \(bu
.el o
Single-socket: No benefit (disabled automatically)
.RE
.RS +4
.TP
.ie t \(bu
.el o
2-socket: 10-20% improvement
.RE
.RS +4
.TP
.ie t \(bu
.el o
4-socket: 15-30% improvement
.RE
.RS +4
.TP
.ie t \(bu
.el o
8+ socket: 20-40% improvement
.RE
.sp
.LP
Improvement depends on workload locality. Memory-bound workloads benefit
most.

.SS "Requirements"
.sp
.LP
NUMA awareness requires:
.RS +4
.TP
.ie t \(bu
.el o
Multi-socket NUMA system (2+ nodes)
.RE
.RS +4
.TP
.ie t \(bu
.el o
libnuma library (libnuma-dev package)
.RE
.RS +4
.TP
.ie t \(bu
.el o
Linux or system with NUMA support
.RE
.RS +4
.TP
.ie t \(bu
.el o
Build with --enable-numa configure flag
.RE

.SS "Configuration"
.sp
.LP
Install dependencies:
.sp
.in +2
.nf
# Debian/Ubuntu
sudo apt-get install libnuma-dev

# RHEL/CentOS
sudo yum install numactl-devel
.fi
.in -2
.sp
.LP
Build with NUMA support:
.sp
.in +2
.nf
./configure --enable-numa
make
make install
.fi
.in -2
.sp
.LP
Enable at runtime:
.sp
.in +2
.nf
export UMEM_OPTIONS=numa=on
./myapp

# Or auto-detect (enabled on multi-node systems only)
export UMEM_OPTIONS=numa=auto
./myapp
.fi
.in -2

.SS "NUMA Policies"
.sp
.LP
NUMA allocation policies control memory placement:
.sp
.ne 2
.na
\fBLOCAL\fR (default)
.ad
.RS 16n
Prefer allocations on current CPU's node. Falls back to other nodes if
local node is full.
.RE
.sp
.ne 2
.na
\fBINTERLEAVE\fR
.ad
.RS 16n
Spread allocations across all nodes in round-robin fashion. Useful for
workloads without clear locality.
.RE
.sp
.ne 2
.na
\fBBIND\fR
.ad
.RS 16n
Bind allocations to specific node. Allocation fails if node is full
(no fallback).
.RE
.sp
.ne 2
.na
\fBPREFERRED\fR
.ad
.RS 16n
Prefer specific node but allow fallback to other nodes if necessary.
.RE

.SS "API Functions"
.sp
.LP
Check NUMA availability:
.sp
.in +2
.nf
#include "umem_numa.h"

if (umem_numa_available()) {
    /* NUMA support is available */
}
.fi
.in -2
.sp
.LP
Initialize NUMA subsystem:
.sp
.in +2
.nf
umem_numa_init();
.fi
.in -2
.sp
.LP
Get current NUMA node:
.sp
.in +2
.nf
int node = umem_numa_get_node();
printf("Running on NUMA node %d\en", node);
.fi
.in -2
.sp
.LP
Allocate on specific node:
.sp
.in +2
.nf
void *ptr = umem_numa_alloc(size, node);
.fi
.in -2
.sp
.LP
Get NUMA statistics:
.sp
.in +2
.nf
umem_numa_cache_info_t info;
umem_numa_stats(cache, &info);

printf("Local allocations: %lu\en", info.local_allocs);
printf("Remote allocations: %lu\en", info.remote_allocs);
printf("Locality: %.1f%%\en",
    100.0 * info.local_allocs /
    (info.local_allocs + info.remote_allocs));
.fi
.in -2
.sp
.LP
Cleanup:
.sp
.in +2
.nf
umem_numa_fini();
.fi
.in -2

.SH ENVIRONMENT VARIABLES
.sp
.ne 2
.na
\fBUMEM_OPTIONS=numa=on|off|auto\fR
.ad
.RS 16n
Control NUMA awareness. Requires --enable-numa at build time.
.RS 4n
.PD 0
.TP 8n
.B on
Force enable even on single-node systems
.TP
.B off
Disable even on multi-node systems
.TP
.B auto
Enable only on multi-node systems (default)
.PD
.RE
.RE

.SH EXAMPLES
.LP
\fBExample 1\fR Basic NUMA-aware application
.sp
.in +2
.nf
#include <umem.h>
#include "umem_numa.h"

int main() {
    if (!umem_numa_available()) {
        fprintf(stderr, "NUMA not available\en");
        return 1;
    }

    umem_numa_init();

    /* Allocate memory (automatically uses local node) */
    void *ptr = umem_alloc(1024, UMEM_DEFAULT);

    /* ... use memory ... */

    umem_free(ptr, 1024);

    umem_numa_fini();
    return 0;
}
.fi
.in -2

.LP
\fBExample 2\fR Check NUMA topology
.sp
.in +2
.nf
# Check number of NUMA nodes
numactl --hardware

# Check current memory policy
numactl --show

# Run application with NUMA awareness
export UMEM_OPTIONS=numa=on
./myapp

# Monitor NUMA statistics
numastat -p $(pidof myapp)
.fi
.in -2

.LP
\fBExample 3\fR Node-specific allocation
.sp
.in +2
.nf
#include "umem_numa.h"

/* Allocate on node 0 */
void *ptr = umem_numa_alloc(4096, 0);

/* Get which node a pointer belongs to */
int node = umem_numa_node_of_address(ptr);

umem_numa_free(ptr, 4096);
.fi
.in -2

.SH FILES
.sp
.ne 2
.na
\fB/sys/devices/system/node/\fR
.ad
.RS 16n
Kernel NUMA topology information
.RE
.sp
.ne 2
.na
\fB/proc/self/numa_maps\fR
.ad
.RS 16n
Per-process NUMA memory map
.RE

.SH ATTRIBUTES
.sp
.TS
box;
c | c
l | l .
ATTRIBUTE TYPE	ATTRIBUTE VALUE
_
Interface Stability	Evolving
_
MT-Level	MT-Safe
_
Platform	Linux, systems with libnuma
.TE

.SH SEE ALSO
.sp
.LP
\fBumem_alloc\fR(3), \fBumem_cache_create\fR(3), \fBumem_rseq\fR(3),
\fBnuma\fR(3), \fBnumactl\fR(8), \fBnumastat\fR(8)
.sp
.LP
NUMA API documentation:
https://man7.org/linux/man-pages/man3/numa.3.html
.sp
.LP
Kernel NUMA documentation:
https://www.kernel.org/doc/html/latest/vm/numa.html

.SH NOTES
.sp
.LP
NUMA awareness adds minimal overhead on single-socket systems. The automatic
detection (numa=auto) prevents unnecessary overhead.
.sp
.LP
On single-socket systems, NUMA awareness is automatically disabled even if
numa=on is specified.
.sp
.LP
NUMA policies are per-cache. Different caches can have different NUMA
policies for fine-grained control.
.sp
.LP
Remote allocations are not errors; they occur when the local node is full
or under heavy memory pressure.

.SH WARNINGS
.sp
.LP
Do not use BIND policy unless you have specific requirements. BIND causes
allocation failures when the specified node is full, whereas LOCAL policy
falls back gracefully.
.sp
.LP
NUMA awareness adds per-node magazine depot overhead (memory and management
structures). On systems with many NUMA nodes (8+), this can be significant.
.sp
.LP
Monitoring tools like numastat may show "remote" allocations even with NUMA
awareness enabled. This is normal when local nodes are under memory pressure.
```

**Files to Reference:**
- `/home/gburd/ws/libumem/EXPERIMENTAL_FEATURES.md` (lines 71-165)
- `/home/gburd/ws/libumem/HASH_NUMA_IMPLEMENTATION.md`
- `/home/gburd/ws/libumem/NUMA_HASH_COMPLETION_REPORT.md`
- Git commit 3e8b99e: "Add hash-based NUMA node selection"

---

### 8. umem_simd.3 - OPTIONAL MAN PAGE

**Recommendation:** SIMD is an internal optimization that requires no user configuration. Documentation in umem_alloc.3 PERFORMANCE section is sufficient. Creating a separate man page may be overkill.

**If created, brief content:**

```
.TH UMEM_SIMD 3 "April 2026"
.SH NAME
umem_simd \- SIMD vectorization for magazine operations
.SH DESCRIPTION
.sp
.LP
SIMD (Single Instruction Multiple Data) vectorization accelerates magazine
operations transparently. This is an internal optimization requiring no
configuration.
.sp
.LP
See \fBumem_alloc\fR(3) PERFORMANCE section for details.
.SH SEE ALSO
\fBumem_alloc\fR(3), \fBumem_cache_create\fR(3)
```

**Decision:** Defer to maintainer preference. Internal optimizations typically don't warrant separate man pages.

---

## Summary of Updates

### Existing Man Pages

| Man Page | Priority | Changes Required | Estimated Lines |
|----------|----------|------------------|-----------------|
| umem_alloc.3 | HIGH | Add PERFORMANCE section, update ENVIRONMENT VARIABLES, update NOTES | 150-200 |
| umem_cache_create.3 | MEDIUM | Update depot section with lock-free and SIMD info | 80-100 |
| umem_debug.3 | NONE | No changes (debug orthogonal to optimizations) | 0 |
| umem_debugging.7 | LOW | Add note about hardware optimization interaction | 30-40 |
| umem_hooks.3 | NONE | No changes (hooks independent of internals) | 0 |

### New Man Pages

| Man Page | Priority | Purpose | Estimated Lines |
|----------|----------|---------|-----------------|
| umem_rseq.3 | HIGH | Document restartable sequences feature | 250-300 |
| umem_numa.3 | HIGH | Document NUMA-aware allocation | 300-350 |
| umem_simd.3 | LOW | Optional (covered in umem_alloc.3) | 50 or skip |

### Total Effort

- **Existing page updates:** ~260-340 lines across 2 man pages
- **New pages:** ~550-650 lines (2 essential new pages)
- **Total documentation:** ~810-990 lines

### Implementation Order

1. **Phase 1 (Essential):**
   - Create `umem_rseq.3` (experimental feature, user-facing)
   - Create `umem_numa.3` (experimental feature, user-facing)
   - Update `umem_alloc.3` PERFORMANCE section (user-visible improvements)

2. **Phase 2 (Important):**
   - Update `umem_cache_create.3` depot section (internal improvements)
   - Update `umem_debugging.7` interaction notes (developer info)

3. **Phase 3 (Optional):**
   - Consider `umem_simd.3` if separate page desired (probably skip)

### Files to Install

New man page installation in Makefile.am:

```makefile
man3_MANS = umem_alloc.3 umem_cache_create.3 umem_debug.3 \
            umem_hooks.3 umem_rseq.3 umem_numa.3

man7_MANS = umem_debugging.7
```

## Testing Man Pages

After creation, test with:

```bash
# Check formatting
groff -man -Tascii umem_rseq.3 | less
groff -man -Tascii umem_numa.3 | less

# Check for errors
man --warnings -l umem_rseq.3
man --warnings -l umem_numa.3

# Install and test
sudo make install
man umem_rseq
man umem_numa
man -k umem  # Should list all umem man pages
```

## Open Questions for Maintainer

1. **HTM (Hardware Transactional Memory):** Should this get a man page? It's experimental (--enable-htm) but less mature than rseq/NUMA. Recommendation: Document in EXPERIMENTAL_FEATURES.md only for now.

2. **umem_simd.3:** Create separate page or keep in umem_alloc.3? SIMD is transparent and automatic. Recommendation: Skip separate page.

3. **Installation location:** Install experimental feature man pages (umem_rseq.3, umem_numa.3) even if features not built? Or conditional installation based on configure flags?

4. **SEE ALSO cross-references:** Should all man pages cross-reference the new ones? Or only where relevant?

5. **Stability warnings:** Should new man pages prominently warn "EXPERIMENTAL" in NAME or DESCRIPTION sections?

## Conclusion

This plan addresses Sun Microsystems reviewer concern #8 by documenting all recent features:

✓ SIMD vectorization (in umem_alloc.3, umem_cache_create.3)
✓ rseq support (new umem_rseq.3)
✓ NUMA-aware allocation (new umem_numa.3)
✓ Lock-free depot operations (in umem_cache_create.3)
✓ Tagged pointer implementation (in umem_cache_create.3)

The plan prioritizes user-facing features (rseq, NUMA) over internal optimizations (SIMD, lock-free) and provides complete examples and configuration instructions.

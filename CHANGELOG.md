# Changelog

All notable changes to libumem are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [2.0.0] - Unreleased

### Rework Phases (0-5)

**Phase 0**: Fixed tcache wiring bug and added depot contention metric.

**Phase 1**: Replaced all `__sync_*` builtins with C11 `<stdatomic.h>`
operations.  Added spin hints (`_mm_pause` / `yield`) for lock loops.

**Phase 2.1**: Simplified depot from lock-free striped arrays to
straightforward mutex-protected lists.  Removed 279 lines of complexity.

**Phase 2.2**: Built lock-free magazine infrastructure using CAS on
`cc_rounds`.  Disabled due to a race condition that requires rseq
per-CPU isolation to fix correctly.

**Phase 3**: Introduced finer size classes (~1.25x geometric spacing)
and increased magazine capacity to 255 slots for better cache utilization.

**Phase 4**: Added slab state tracking (ACTIVE/DIRTY/CLEAN) and
madvise-based page reclamation for idle slabs.

**Phase 5**: Renamed tcache to PTC (Per-Thread Cache) throughout
the codebase.  Files are now `umem_ptc.c` / `umem_ptc.h`.

### New Features

- **Stack-Based Objects (SBO)**: Bump allocator and scoped arena for
  temporary allocations that auto-free on scope exit.
- **Ownership tracking** (`umem_own.h`): Rust-inspired ownership and
  borrowing system with lightweight (~2%) and full debug (~15%) modes.
  Detects use-after-free, double-free, borrow conflicts, thread violations.
- **Garbage collector** (`umem_gc.h`): Conservative mark-sweep GC with
  Boehm-compatible API (`gc.h`).  Concurrent marking, finalizers,
  sparsemap for O(1) pointer lookup.
- **Allocation profiling** (`umem_profile.h`): Record allocation patterns
  to binary profiles, replay to pre-warm caches.  Phase detection and
  predictive pre-allocation.
- **Budget contexts** (`umem_palloc.h`): PostgreSQL-style per-context
  memory management with budgets, backpressure, shared memory, and
  parent/child hierarchy.
- **Stack traces**: GDB-style stack trace formatting for error reporting
  in debug and ownership modes.
- **Transfer batching**: `umem_cache_alloc_batch` / `umem_cache_free_batch`
  for bulk operations on object caches.
- **vmem_xcreate()**: Extended arena creation API for custom vmem arenas.
- **NUMA-aware depot**: NUMA node selection for depot stealing and
  allocation statistics.
- **Per-thread magazines**: Reduce `cc_lock` contention by giving each
  thread its own magazine pair.
- **Experimental guards**: Headers for experimental APIs require
  `#define UMEM_ENABLE_EXPERIMENTAL` before inclusion.

### Performance

- PTC coverage expanded from 448 bytes to 2048 bytes.
- PTC fast path inlined into `umem.c` alloc/free (fewer branches, no
  stats overhead, lazy init).
- Per-CPU depot arrays to eliminate cross-CPU contention.
- RSEQ (restartable sequences) wired into x86_64 allocation hot path.
- Alignment audit: hot fields placed in first cache line with
  `_Static_assert` verification.
- Profiling is zero-cost when disabled (no atomic operations on hot path).

### Platform Fixes

- **FreeBSD**: Fixed W^X crash (removed PROT_EXEC from heap), MAP_ANON
  portability, removed broken `_pthread_mutex_init_calloc_cb` constructor.
- **Illumos/SPARC**: GAS syntax for assembly, alloca/pcstack fixes,
  guard pthread shims, `__EXTENSIONS__` for headers, 48-bit VA check
  skipped on SPARC.
- **RISC-V**: Fixed TLS static block exhaustion in benchmarks, added
  fallback dlopen paths.
- **Windows/MinGW**: Guarded mmap/munmap symbols, gettimeofday compat
  wrapper, general portability fixes.
- **Cross-compilation**: Fixed configure.ac for aarch64/riscv64 cross
  builds via Nix.
- **BSD platforms**: `PLATFORM_FEATURE_FLAGS=""` (no `_POSIX_C_SOURCE`).

### Code Quality

- Removed dead genasm/tmem code (-2,514 lines, 7 stale .md files).
- Removed 48 stale documentation files and tracked artifacts.
- Test coverage boosted from 33% to 80%+ line coverage (32 new tests).
- Property-based tests for allocation patterns, caches, fragmentation.
- Integration tests for signals, OOM, multithreading, debug features.
- Benchmark suite with comparison to libc/jemalloc, cross-platform
  results tracking (TOML output with OS/arch/compiler metadata).

### Breaking Changes

- `umem_genasm_supported` is kept as an ABI-compatible symbol (value 0)
  but genasm functionality is removed.
- Experimental headers now require `#define UMEM_ENABLE_EXPERIMENTAL`.

## [0.1.0] - Historical

Initial portable fork from Solaris libumem (circa 2008).

- Slab allocator with magazine layer
- Object caching with constructors/destructors
- vmem virtual memory management
- Debug features (guards, auditing, logging)
- Linux and Solaris support

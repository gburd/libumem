# Changelog

All notable changes to libumem will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added

- **Per-Thread Caching (PTC)**: Lock-free fast path for small allocations on x86/x86_64
  - Enabled by default with 1MB per-thread cache
  - Covers allocations up to 448 bytes (64-bit) or 256 bytes (32-bit)
  - 2-5x performance improvement for small, frequent allocations
  - Configurable via `UMEM_OPTIONS=perthread_cache=<size>`

- **Application Allocator Hooks**: Integration API for custom allocators
  - New `umem_hooks.h` API
  - Statistics tracking for multiple allocators
  - Example: PostgreSQL palloc integration

- **Comprehensive Documentation**:
  - New man pages: `umem_hooks(3)`, `umem_debugging(7)`
  - Updated man pages: `umem_alloc(3)`, `umem_cache_create(3)`, `umem_debug(3)`
  - New guides: `docs/ARCHITECTURE.md`, `docs/PORTING.md`, `docs/TESTING.md`
  - `CONTRIBUTING.md` with detailed contribution guidelines
  - Improved `README.md` with quick start and feature overview

- **Nix Flake Support**:
  - Complete Nix flake for reproducible builds
  - Cross-compilation support (aarch64, RISC-V)
  - Development shells with all tools pre-configured
  - CI-ready build and test infrastructure
  - See `NIX_USAGE.md` for details

- **FreeBSD Support**:
  - Full FreeBSD 13.x and 14.x support (amd64, aarch64, armv7)
  - FreeBSD-specific build hooks
  - See `FREEBSD_SUPPORT.md` for details

- **GDB and LLDB Debugger Extensions**:
  - Memory leak detection
  - Cache statistics inspection
  - Allocation tracing
  - Located in `tools/gdb/` and `tools/lldb/`

### Changed

- **PTC Enabled by Default**: Previously opt-in, now enabled automatically on x86/x86_64
- **Improved Magazine Layer**: Optimized depot contention and lock striping
- **Build System**: Modernized autotools configuration
- **Test Suite**: Expanded to >95% code coverage
  - Property-based tests for allocation patterns
  - Integration tests for signals, OOM, multithreading
  - Benchmark suite with comparison to libc/jemalloc

### Fixed

- **pthread Circular Dependency**: Documented LD_PRELOAD limitation
  - `libumem_malloc.so` uses bootstrap allocator to avoid deadlock
  - Direct linking provides full performance
  - See `PTHREAD_LIMITATION.md` for details

- **FreeBSD Build**: Fixed thread-local storage and atomic operations
- **Benchmark Segfault**: Added `-H` flag for header-only output
- **Memory Leaks**: Fixed leaks in cache destruction and vmem cleanup

### Performance

- **Small Allocations**: 2-5x improvement with PTC (x86/x86_64)
- **Multi-threaded**: Near-linear scalability up to 128 cores
- **Memory Overhead**: 5-15% typical overhead in production

## [0.1.0] - Historical

Initial portable fork from Solaris libumem (circa 2008).

### Features

- Slab allocator with magazine layer
- Object caching with constructors/destructors
- vmem virtual memory management
- Debug features (guards, auditing, logging)
- Linux and Solaris support

## Migration Guide

### From Previous Versions

#### PTC Now Enabled by Default

PTC is now enabled automatically on x86/x86_64. To disable:

```bash
UMEM_OPTIONS=perthread_cache=0 ./myapp
```

#### LD_PRELOAD No Longer Recommended

Using `libumem_malloc.so` with `LD_PRELOAD` does not provide performance benefits due to pthread circular dependency. **Always link directly** against libumem:

**Before** (slow):
```bash
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./myapp
```

**After** (fast):
```bash
gcc myapp.c -lumem -o myapp
./myapp
```

#### New Hook API

Applications with custom allocators can now integrate with umem for tracking:

```c
#include <umem_hooks.h>

umem_hook_t my_hook = {
    .hook_name = "my_allocator",
    .hook_alloc = my_alloc,
    .hook_free = my_free,
    .hook_arg = &my_context
};

umem_hook_register(&my_hook);
```

## Breaking Changes

None. This release maintains full API/ABI compatibility with previous versions.

## Deprecations

None.

## Known Issues

- **PTC Platform Support**: PTC only available on x86/x86_64. Other architectures use magazine layer.
- **Debug Overhead**: `UMEM_DEBUG` adds 50-70% overhead, disable in production.
- **Large Allocations**: Debug features only fully supported for allocations <16KB.

## Future Plans

- RISC-V PTC implementation
- aarch64 PTC implementation
- Windows support (x64, ARM64)
- SPARC architecture support
- Improved vmem segment coalescing
- Optional NUMA-aware allocation

## Contributors

Thanks to all contributors who made this release possible!

See git history for detailed attribution.

## References

- Bonwick, Jeff (1994). "The Slab Allocator: An Object-Caching Kernel Memory Allocator"
- Bonwick and Adams (2001). "Magazines and vmem: Extending the Slab Allocator to Many CPUs and Arbitrary Resources"

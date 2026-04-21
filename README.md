# libumem - Fast, Scalable Memory Allocator

Portable version of the Solaris slab allocator, originally designed by Jeff
Bonwick in 1994.  Provides high-performance memory allocation with built-in
debugging on Linux (x86_64, aarch64, RISC-V 64), FreeBSD, Illumos/SPARC,
and Windows (experimental).

## Quick Start

```bash
./autogen.sh
./configure
make -j$(nproc)
make check
sudo make install
```

Use as a drop-in allocator:

```bash
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./myapp
```

Or link directly (recommended for full performance):

```bash
gcc myapp.c -lumem -o myapp
```

## Platform Support

| Platform           | Arch              | Status       |
|--------------------|-------------------|--------------|
| Linux              | x86_64            | Production   |
| Linux              | aarch64, riscv64  | Production   |
| FreeBSD            | amd64             | Production   |
| Illumos            | SPARCv9           | Production   |
| Windows            | x64 (MSVC/MinGW) | Experimental |

## Stable Features

### Core Allocation

```c
#include <umem.h>

void *p = umem_alloc(1024, UMEM_DEFAULT);
umem_free(p, 1024);

void *z = umem_zalloc(1024, UMEM_DEFAULT);
umem_free(z, 1024);
```

### Object Caches

Type-stable caching with constructor/destructor support:

```c
umem_cache_t *c = umem_cache_create("objects",
    sizeof(obj_t), 0, ctor, dtor, NULL, NULL, NULL, 0);
obj_t *o = umem_cache_alloc(c, UMEM_DEFAULT);
umem_cache_free(c, o);
umem_cache_destroy(c);
```

### Virtual Memory (vmem)

Arena-based virtual address management with quantum caching,
best-fit/instant-fit allocation, and hierarchical sub-arenas.

### Debug Modes

All debug modes are controlled via environment variables (no recompile).

| Mode      | Variable                     | Overhead | Detects                          |
|-----------|------------------------------|----------|----------------------------------|
| Guards    | `UMEM_DEBUG=guards`          | ~10%     | Buffer overruns, use-after-free  |
| Audit     | `UMEM_DEBUG=audit`           | ~30%     | Leak detection, alloc history    |
| Contents  | `UMEM_DEBUG=default`         | ~50%     | Uninitialized reads, corruption  |
| Firewall  | `UMEM_DEBUG=firewall`        | High     | Guard page per allocation        |
| Logging   | `UMEM_LOGGING=transaction=1m`| ~5%      | Allocation transaction log       |

### Stack-Based Allocation (SBO)

Bump allocator and scoped arenas for temporary allocations that
automatically clean up when the scope ends.

### Per-Thread Caching (PTC)

Lock-free fast path for allocations up to 2048 bytes.  Enabled by
default on all architectures.  Configure via:

```bash
UMEM_OPTIONS=perthread_cache=2m ./myapp   # increase cache
UMEM_OPTIONS=perthread_cache=0 ./myapp    # disable
```

## Experimental Features

These APIs are under active development and may change without notice.
Include guards require `#define UMEM_ENABLE_EXPERIMENTAL` before the header.

### Ownership Tracking (umem_own.h)

Rust-inspired ownership/borrowing system.  Detects use-after-free,
double-free, borrow conflicts, and cross-thread violations at runtime.
Two modes: lightweight (~2% overhead) and full debug (~15% overhead).

### Garbage Collection (umem_gc.h, gc.h)

Conservative mark-sweep garbage collector with Boehm GC-compatible API.
Uses umem's slab allocator as backing store.  Supports finalizers,
atomic allocations, and per-thread root scanning.

### Allocation Profiling (umem_profile.h)

Record allocation patterns to a binary profile, then replay to pre-warm
caches on subsequent runs.  Phase detection identifies workload transitions
and pre-allocates for upcoming phases.

### Budget Contexts (examples/umem_palloc.h)

PostgreSQL-style per-context memory management with memory budgets,
backpressure, pre-allocation, shared memory support, and parent/child
hierarchy.

## Performance

Relative to glibc malloc on x86_64:

| Workload           | Single-thread | Multi-thread (8 cores) |
|--------------------|---------------|------------------------|
| Small alloc/free   | 85-93%        | 85-90%                 |
| Object cache       | 100-120%      | 110-150%               |
| Mixed sizes        | 90-95%        | 95-105%                |

PTC provides lock-free allocation for the common case.  The magazine
layer handles larger allocations with per-CPU caching to minimize
contention.

## Testing

```bash
make check                                         # autotools suite
LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork  # comprehensive suite
```

## Building with Nix

```bash
nix develop                          # dev shell
nix build                            # native build
nix build .#libumem-aarch64          # cross-compile
nix run .#test-native                # run tests
```

See [NIX_USAGE.md](NIX_USAGE.md) for details.

## Documentation

- [examples/](examples/) -- complete usage examples with user guide
- Man pages: umem_alloc(3), umem_cache_create(3), umem_debug(3)
- [CHANGELOG.md](CHANGELOG.md) -- version history

## License

CDDL 1.0 (Common Development and Distribution License).  See [LICENSE](LICENSE).

## References

- Bonwick (1994). "The Slab Allocator: An Object-Caching Kernel Memory Allocator". USENIX.
- Bonwick and Adams (2001). "Magazines and vmem: Extending the Slab Allocator". USENIX.

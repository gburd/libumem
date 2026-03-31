# libumem - Fast, Scalable Memory Allocator

Portable version of Solaris libumem, a fast, scalable, concurrent memory allocator with comprehensive debugging features.

## Features

- **High Performance**: Per-Thread Caching (PTC) provides lock-free fast path for small allocations
- **Experimental Per-CPU Caching**: Optional true per-CPU magazines for 100-200% throughput improvement at high thread counts
- **Scalable**: Magazine-layer caching minimizes lock contention across CPUs
- **NUMA Awareness**: Optional NUMA-aware allocation for multi-socket systems
- **Object Caches**: Type-stable caching with constructor/destructor support
- **Comprehensive Debugging**: Guards, auditing, stack traces, and transaction logging
- **Production Ready**: Battle-tested in Solaris kernel and userspace since 1994
- **Portable**: Supports x86_64, i386, with templates for aarch64, RISC-V, SPARC

## Quick Start

### Installation

```bash
# Clone and build
git clone https://github.com/yourusername/libumem.git
cd libumem
./autogen.sh
./configure
make
sudo make install
```

### Using with Nix

```bash
# Development shell
nix develop

# Build
nix build

# Run tests
nix run .#test-native
```

See [NIX_USAGE.md](NIX_USAGE.md) for comprehensive Nix documentation.

### Basic Usage

```c
#include <umem.h>

// Simple allocation
void *ptr = umem_alloc(1024, UMEM_DEFAULT);
if (ptr == NULL) {
    /* handle allocation failure */
}
umem_free(ptr, 1024);

// Zero-initialized allocation
void *zptr = umem_zalloc(1024, UMEM_DEFAULT);
umem_free(zptr, 1024);

// Aligned allocation (e.g., for DMA or cache-line alignment)
void *aptr = umem_alloc_align(4096, 64, UMEM_DEFAULT);
umem_free_align(aptr, 4096);
```

### Object Caches

For frequently allocated objects of the same type:

```c
#include <umem.h>

// Create cache
umem_cache_t *cache = umem_cache_create(
    "my_objects",           // name
    sizeof(my_object_t),    // size
    0,                      // alignment (0 = default)
    NULL,                   // constructor
    NULL,                   // destructor
    NULL,                   // reclaim
    NULL,                   // private data
    NULL,                   // source
    0                       // flags
);

// Allocate from cache
my_object_t *obj = umem_cache_alloc(cache, UMEM_DEFAULT);

// ... use object ...

// Free to cache
umem_cache_free(cache, obj);

// Destroy cache (when done)
umem_cache_destroy(cache);
```

## Configuration

### Performance Tuning

Per-Thread Caching (PTC) is enabled by default with 1MB per-thread cache:

```bash
# Default (1MB per-thread cache)
./myapp

# Increase per-thread cache
UMEM_OPTIONS=perthread_cache=2m ./myapp

# Disable PTC
UMEM_OPTIONS=perthread_cache=0 ./myapp

# Use mmap backend
UMEM_OPTIONS=backend=mmap ./myapp
```

### Debugging

Enable debugging features to detect memory issues:

```bash
# Full debugging (guards, auditing, contents logging)
UMEM_DEBUG=default ./myapp

# Guards only (pattern fill, redzone checking)
UMEM_DEBUG=guards ./myapp

# Auditing with transaction log
UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m ./myapp
```

See [umem_debugging(7)](umem_debugging.7) for comprehensive debugging guide.

## Performance Characteristics

### Small Allocations (≤448 bytes on x86_64)

With PTC enabled (default):
- **Lock-free fast path**: No locks acquired for cache hits
- **~10-20ns per allocation** on modern CPUs
- **Thread-local caching**: Minimal cache-line bouncing

### Medium Allocations (≤16KB)

- **Magazine-layer caching**: Per-CPU magazines reduce lock contention
- **Slab allocator**: Efficient memory usage with low fragmentation
- **~50-100ns per allocation**

### Large Allocations (>16KB)

- Direct vmem allocation with optional backend (sbrk or mmap)
- **~200-500ns per allocation**

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for detailed benchmarks.

## Platform Support

| Architecture | Status | PTC Support | Notes |
|--------------|--------|-------------|-------|
| x86_64 | ✓ Production | Yes | Full support |
| i386 | ✓ Production | Yes | Full support |
| aarch64 | ⚠ Template | No | Build infrastructure ready |
| RISC-V 64 | ⚠ Template | No | Build infrastructure ready |
| SPARC | ⚠ Planned | No | Port in progress |

PTC (Per-Thread Caching) requires architecture-specific assembly generation. On unsupported architectures, libumem operates with full functionality but uses the magazine layer instead of per-thread caching.

## Known Limitations

### LD_PRELOAD malloc interposition

`libumem_malloc.so` can be used with `LD_PRELOAD` for compatibility testing, but **does not provide performance benefits**:

```bash
# Works, but uses bootstrap allocator (no PTC, no magazine layer)
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./myapp
```

For production use, always **link directly** against libumem:

```bash
# Makefile
LDFLAGS += -lumem

# Or at link time
gcc myapp.c -lumem -o myapp
```

See [PTHREAD_LIMITATION.md](PTHREAD_LIMITATION.md) for technical details on why LD_PRELOAD is limited.

## Documentation

### Man Pages

- [umem_alloc(3)](umem_alloc.3) - Basic allocation functions
- [umem_cache_create(3)](umem_cache_create.3) - Object cache API
- [umem_debug(3)](umem_debug.3) - Debugging environment variables
- [umem_hooks(3)](umem_hooks.3) - Application allocator hooks
- [umem_debugging(7)](umem_debugging.7) - Comprehensive debugging guide

### Developer Documentation

- [NIX_USAGE.md](NIX_USAGE.md) - Nix flake usage and development
- [CONTRIBUTING.md](CONTRIBUTING.md) - How to contribute
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - High-level architecture
- [docs/PORTING.md](docs/PORTING.md) - Porting to new architectures
- [docs/TESTING.md](docs/TESTING.md) - Test suite documentation
- [docs/PER_CPU_PERFORMANCE.md](docs/PER_CPU_PERFORMANCE.md) - Per-CPU caching performance guide
- [CHANGELOG.md](CHANGELOG.md) - Version history and changes
- [PTHREAD_LIMITATION.md](PTHREAD_LIMITATION.md) - LD_PRELOAD technical details

### Implementation Documentation

- [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) - Feature implementation status
- [PERFORMANCE_SUMMARY.md](PERFORMANCE_SUMMARY.md) - Performance analysis
- [RECENT_CHANGES.md](RECENT_CHANGES.md) - Recent development activity

## Examples

Complete example programs in [examples/](examples/):

- `basic_usage.c` - Simple allocation examples
- `cache_usage.c` - Object cache patterns
- `debugging.c` - Using debug features
- `palloc_integration.c` - PostgreSQL palloc integration with hooks

## Building from Source

### Requirements

- C compiler (GCC 4.8+ or Clang 3.5+)
- autoconf, automake, libtool
- pthreads
- Optional: Python 3 (for debugger extensions)
- Optional: Nix (for reproducible builds)

### Standard Build

```bash
./autogen.sh
./configure
make
make check          # Run tests
sudo make install
```

### Build Options

```bash
# Enable experimental per-CPU caching (100-200% faster at high thread counts)
./configure --enable-percpu-caching
make

# Enable coverage measurement
./configure --enable-coverage
make coverage

# Enable sanitizers
./configure CFLAGS="-fsanitize=address,undefined"
make

# Disable recursion guard for maximum performance
./configure --disable-recursion-guard
make

# Cross-compile for aarch64
./configure --host=aarch64-unknown-linux-gnu
make
```

### Nix Build

```bash
# Build all architectures
nix build .#libumem          # native
nix build .#libumem-riscv64  # RISC-V
nix build .#libumem-aarch64  # aarch64

# Run tests
nix run .#test-native
nix run .#test-riscv64   # QEMU user-mode
nix run .#test-aarch64   # QEMU user-mode

# Run all checks
nix flake check
```

## Testing

```bash
# Run test suite
make check

# Run with debugging enabled
UMEM_DEBUG=default make check

# Run specific tests
./umem_test          # Basic functionality
./umem_ptc_test      # PTC functionality
./test/test_main     # Comprehensive suite

# Property-based tests
./test/property/prop_alloc_free2
./test/property/prop_cache
./test/property/prop_fragmentation

# Benchmarks
cd test/bench
make
./bench_allocators.sh umem libc jemalloc
```

## Contributing

Contributions welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for:

- Code standards
- Test requirements (>95% coverage for new code)
- PR process
- Performance regression testing

## License

CDDL 1.0 (Common Development and Distribution License)

See [LICENSE](LICENSE) for full text.

## History

libumem originated as the Solaris kernel's memory allocator, designed by Jeff Bonwick in 1994. The slab allocator design introduced:

- Object caching with constructed state
- Per-CPU magazine layer
- Comprehensive debugging features

This portable version brings libumem to Linux, FreeBSD, and other UNIX-like systems, adding Per-Thread Caching for even better performance on modern multi-core systems.

## References

- Bonwick, Jeff (1994). "The Slab Allocator: An Object-Caching Kernel Memory Allocator". USENIX Summer 1994.
- Bonwick, Jeff and Jonathan Adams (2001). "Magazines and vmem: Extending the Slab Allocator to Many CPUs and Arbitrary Resources". USENIX Summer 2001.

## Support

- Issues: https://github.com/yourusername/libumem/issues
- Discussions: https://github.com/yourusername/libumem/discussions
- Documentation: See man pages and docs/ directory

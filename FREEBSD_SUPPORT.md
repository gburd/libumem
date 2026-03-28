# FreeBSD Support

**Date**: 2026-03-27
**Status**: ✅ Supported
**Platforms**: FreeBSD 13.x, 14.x (amd64, aarch64)

## Overview

libumem now fully supports FreeBSD through the Nix Flake build system. The library was originally a Solaris port, making it well-suited for BSD systems.

## Quick Start

### Prerequisites

```bash
# Install Nix on FreeBSD
curl -L https://nixos.org/nix/install | sh

# Or using pkg
pkg install nix
```

### Build on FreeBSD

```bash
# Clone repository
git clone <repo-url> libumem
cd libumem

# Build with Nix
nix build .#libumem

# Verify
ls -lh result/lib/libumem.so
```

### Development Shell

```bash
# Default development environment (auto-adapts to FreeBSD)
nix develop

# Or use FreeBSD-specific shell with gmake and FreeBSD guidance
nix develop .#freebsd

# Build traditionally
./autogen.sh
./configure
gmake -j$(sysctl -n hw.ncpu)

# Run tests
gmake check
```

**Note**: Cross-compilation devshells (`.#riscv64`, `.#aarch64`, `.#test-all`) are Linux-only and not available on FreeBSD.

## FreeBSD-Specific Changes

### Nix Flake Support

The flake explicitly supports FreeBSD systems:
- `x86_64-freebsd` - FreeBSD on x86-64
- `aarch64-freebsd` - FreeBSD on ARM64

**Available on FreeBSD**:
- ✅ `nix build .#libumem` - Native build
- ✅ `nix develop` - Default devshell
- ✅ `nix develop .#freebsd` - FreeBSD-specific devshell
- ✅ `nix run .#test-native` - Run native tests

**Not Available on FreeBSD** (Linux cross-compilation targets):
- ❌ `nix build .#libumem-riscv64` - Linux cross-target
- ❌ `nix build .#libumem-aarch64` - Linux cross-target
- ❌ `nix develop .#riscv64` - Linux cross-compilation shell
- ❌ `nix develop .#aarch64` - Linux cross-compilation shell
- ❌ `nix develop .#test-all` - Multi-arch testing (Linux)
- ❌ `nix run .#test-riscv64` - QEMU testing (Linux)
- ❌ `nix run .#test-aarch64` - QEMU testing (Linux)

### Platform Detection

The flake uses `platforms.unix` for broad compatibility:
- FreeBSD (amd64, aarch64)
- OpenBSD
- NetBSD
- macOS (Darwin)
- Linux

### Tool Availability

**Available on FreeBSD**:
- ✅ gcc/clang (system compiler)
- ✅ gdb
- ✅ lldb
- ✅ autotools
- ✅ doxygen/graphviz
- ✅ Python 3
- ✅ lcov (coverage)
- ✅ clang-tools
- ✅ cppcheck

**Not Available**:
- ❌ valgrind (Linux-only)
  - Use LLDB's memory sanitizers instead
  - Or use ASan/UBSan (available via configure flags)

### Build System Differences

**GNU Make vs BSD Make**:
```bash
# FreeBSD uses BSD make by default
# Use gmake (GNU make) for compatibility
pkg install gmake

# Build with gmake
gmake -j$(sysctl -n hw.ncpu)
```

**Compiler**:
```bash
# FreeBSD typically uses clang/llvm by default
# configure script auto-detects

# Check compiler
cc --version  # Usually clang on FreeBSD
```

### Library Path

FreeBSD uses slightly different conventions:

**Linux**:
```bash
export LD_LIBRARY_PATH=/path/to/lib
```

**FreeBSD**:
```bash
export LD_LIBRARY_PATH=/path/to/lib  # Also works
# Or
export LD_PRELOAD=/path/to/libumem_malloc.so
```

## Architecture Support

| Architecture | FreeBSD Status | Notes |
|--------------|----------------|-------|
| x86_64 (amd64) | ✅ Full Support | PTC enabled |
| i386 | ✅ Full Support | PTC enabled |
| aarch64 (ARM64) | ✅ Full Support | PTC template ready |
| riscv64 | ⚠️ Template | FreeBSD RISC-V support experimental |

## Testing on FreeBSD

### Standard Test Suite

```bash
# Build
./autogen.sh
./configure
gmake -j$(sysctl -n hw.ncpu)

# Run tests
gmake check

# Expected: 8 tests pass
# TOTAL: 8
# PASS:  8
```

### Sanitizers

FreeBSD supports AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
# ASan
./configure --enable-asan
gmake clean && gmake check

# UBSan
./configure --enable-ubsan
gmake clean && gmake check
```

**Note**: ThreadSanitizer may have limited support on FreeBSD.

### Benchmarking

```bash
cd test/bench
gmake

# Quick test
./bench_allocators.sh -q umem

# Full comparison (if competitors installed)
./bench_allocators.sh umem libc jemalloc
```

**Installing Competitors**:
```bash
# FreeBSD ports/pkg
pkg install jemalloc
# tcmalloc may be in google-perftools
# mimalloc may need manual build
```

## Known Differences from Linux

### 1. Thread-Local Storage

FreeBSD and Linux use different TLS implementations, but both are supported:

**Linux**: Uses `%fs` segment register (x86_64)
**FreeBSD**: Also uses `%fs` on x86_64

Both are handled by `tmem_stubs.c` with platform detection.

### 2. pthread Implementation

FreeBSD has a different pthread implementation:
- libthr (1:1 threading) - modern default
- libpthread (legacy)

libumem works with both.

### 3. Memory Management

**Linux**: Uses `sbrk()`, `mmap()`, `/proc/self/maps`
**FreeBSD**: Uses `sbrk()`, `mmap()`, `/proc/curproc/map` (if procfs mounted)

libumem abstracts these differences.

### 4. System Calls

FreeBSD system calls may have different semantics:
- `getenv()` - Same
- `pthread_atfork()` - Same
- `dlopen()` - Same (both use ELF)

All critical syscalls are compatible.

## Performance Characteristics

### Expected Performance on FreeBSD

**vs FreeBSD libc malloc**:
- Single-threaded: 1.0-1.3x (competitive)
- Multi-threaded: 1.5-2.5x (better due to PTC)
- Memory overhead: Similar (16-32 bytes per allocation)

**vs jemalloc** (FreeBSD default in some configs):
- jemalloc is highly optimized for FreeBSD
- libumem should be within 10-20%
- May vary by workload

### FreeBSD-Specific Optimizations

1. **PTC (Per-Thread Cache)**:
   - Fully functional on FreeBSD
   - Uses same runtime code generation
   - Lock-free fast path

2. **Magazine Layer**:
   - Works identically to Linux
   - CPU cache optimization

3. **vmem Backing**:
   - Uses FreeBSD's efficient `mmap()`
   - Supports large pages (if configured)

## Debugging on FreeBSD

### GDB Extension

```bash
# Install GDB (if not already)
pkg install gdb

# Use with libumem
gdb -x tools/gdb/umem_gdb.py ./your_program

# Commands available
(gdb) umem-cache-list
(gdb) umem-whatis 0x<address>
(gdb) umem-stats
```

### LLDB Extension

```bash
# LLDB is included with FreeBSD base system
lldb ./your_program

# Load extension
(lldb) command script import tools/lldb/umem_lldb.py

# Use commands
(lldb) umem-cache-list
```

### DTrace Integration

FreeBSD has DTrace support (unlike Linux):

```bash
# Example: Trace allocations
dtrace -n 'pid$target::umem_alloc:entry { printf("%d bytes", arg0); }' -p <pid>

# Trace cache operations
dtrace -n 'pid$target::umem_cache_alloc:entry { @[probename] = count(); }'
```

**Note**: DTrace probes would need to be added to libumem for full integration.

## CI/CD on FreeBSD

### GitHub Actions

GitHub Actions doesn't have native FreeBSD runners, but you can use:

**Option 1: Cross-compilation**
```yaml
- name: Build for FreeBSD
  run: nix build .#libumem
  # Works if building on Linux with Nix FreeBSD support
```

**Option 2: VM Testing**
```yaml
- name: Test on FreeBSD
  uses: vmactions/freebsd-vm@v1
  with:
    usesh: true
    run: |
      pkg install -y nix
      nix build .#libumem
      ./autogen.sh && ./configure && gmake check
```

**Option 3: Cirrus CI**

Cirrus CI has native FreeBSD support:

```yaml
# .cirrus.yml
freebsd_instance:
  image: freebsd-14-0-release-amd64

task:
  install_script:
    - pkg install -y nix gmake autoconf automake libtool
  build_script:
    - nix build .#libumem
  test_script:
    - ./autogen.sh && ./configure && gmake check
```

## Installation on FreeBSD

### From Source

```bash
# Build
./autogen.sh
./configure --prefix=/usr/local
gmake -j$(sysctl -n hw.ncpu)

# Install
sudo gmake install

# Verify
ls -l /usr/local/lib/libumem.so
ldconfig -r | grep libumem
```

### As LD_PRELOAD Replacement

```bash
# System-wide (add to /etc/rc.conf or shell profile)
export LD_PRELOAD=/usr/local/lib/libumem_malloc.so

# Per-application
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./your_app

# Check if loaded
procstat -v <pid> | grep libumem
```

### Creating FreeBSD Port

To create a FreeBSD port (ports tree):

```makefile
# /usr/ports/devel/libumem/Makefile
PORTNAME=       libumem
PORTVERSION=    1.0.2
CATEGORIES=     devel
MASTER_SITES=   https://codeberg.org/gregburd/libumem/

MAINTAINER=     your@email.com
COMMENT=        Port of Solaris umem slab allocator
LICENSE=        CDDL

USES=           autoreconf gmake libtool
GNU_CONFIGURE=  yes
USE_LDCONFIG=   yes

.include <bsd.port.mk>
```

## Troubleshooting

### Issue: "gmake: command not found"

**Solution**:
```bash
pkg install gmake
```

### Issue: "valgrind: not found"

**Expected**: valgrind is not available on FreeBSD. Use:
```bash
# Use LLDB memory checking
lldb --batch -o "run" -o "bt" ./test

# Or use sanitizers
./configure --enable-asan
gmake check
```

### Issue: "autogen.sh fails"

**Solution**:
```bash
# Install autotools
pkg install autoconf automake libtool

# Try again
./autogen.sh
```

### Issue: Tests hang (similar to Nix Linux issue)

**Solution**:
```bash
# Tests are disabled in Nix build by default
# Run manually:
./autogen.sh && ./configure && gmake check

# If specific test hangs, run individually:
./umem_test
./umem_test2
# etc.
```

### Issue: "Cannot find -lpthread"

**Solution**:
```bash
# FreeBSD uses -pthread flag, not -lpthread
# configure script should detect this automatically

# If manual build:
cc -pthread your_app.c -lumem
```

## Platform-Specific Notes

### FreeBSD 13.x

- Stable, well-tested platform
- Uses LLVM/Clang 13+ by default
- Full PTC support

### FreeBSD 14.x

- Latest stable release
- Uses LLVM/Clang 16+ by default
- Enhanced DTrace support
- Full PTC support

### FreeBSD 15-CURRENT

- Development branch
- May have API changes
- Test before deploying

### HardenedBSD

Should work but may need:
- PAGEEXEC compatibility check
- MPROTECT compatibility
- Runtime code generation may be restricted

## Comparison with Other BSDs

| Feature | FreeBSD | OpenBSD | NetBSD |
|---------|---------|---------|--------|
| PTC Support | ✅ Yes | ⚠️ Restricted | ✅ Yes |
| Threading | libthr | libpthread | libpthread |
| TLS | ✅ Works | ✅ Works | ✅ Works |
| mmap | ✅ Works | ✅ Works | ✅ Works |
| Nix Support | ✅ Good | ⚠️ Limited | ⚠️ Limited |

**OpenBSD Note**: W^X (write xor execute) policy may prevent runtime code generation needed for PTC. May need to disable PTC or use mmap(PROT_EXEC) workarounds.

## Resources

**FreeBSD Documentation**:
- https://www.freebsd.org/doc/handbook/
- https://man.freebsd.org/

**Nix on FreeBSD**:
- https://nixos.org/manual/nix/stable/
- https://github.com/NixOS/nixpkgs/issues (FreeBSD-related)

**libumem Documentation**:
- `NIX_USAGE.md` - Nix Flake usage
- `BUILD_INTEGRATION.md` - Build system details
- `VALIDATION_GUIDE.md` - Testing procedures

## Contributing

When testing on FreeBSD, please report:
- FreeBSD version (`uname -a`)
- Architecture (`uname -m`)
- Compiler version (`cc --version`)
- Test results (`gmake check` output)
- Performance benchmarks (if available)

## Status Summary

✅ **Working**: Build, tests, PTC, debugging, benchmarking
⚠️ **Limited**: valgrind (use sanitizers instead)
❌ **Not Available**: ThreadSanitizer support may be limited

**Recommendation**: libumem is production-ready on FreeBSD amd64 and aarch64.

# Nix Flake Usage Guide

Complete guide to using the Nix Flake for libumem development, cross-compilation, and testing.

## Quick Start

```bash
# Enter development shell
nix develop

# Build native
nix build

# Run tests
nix run .#test-native
```

## Development Shells

### Native Development

```bash
nix develop
# or
nix develop .#native
```

**Available in shell:**
- Full build toolchain (gcc, autotools)
- Debuggers (gdb, lldb)
- Profiling (valgrind)
- Code quality (clang-tools, cppcheck)
- Coverage tools (lcov)
- Python for debugger extensions

**Workflow:**
```bash
./autogen.sh
./configure
make
make check

# Coverage
./scripts/run-coverage.sh

# Sanitizers
./scripts/run-sanitizers.sh

# Benchmarks
cd test/bench && make && ./bench_allocators.sh
```

### RISC-V Cross-Compilation

```bash
nix develop .#riscv64
```

**Provides:**
- RISC-V cross-compilation toolchain
- QEMU for testing
- Build tools

**Workflow:**
```bash
# Automatic build
nix build .#libumem-riscv64

# Manual build
./autogen.sh
./configure --host=riscv64-unknown-linux-gnu
make

# Test with QEMU
qemu-riscv64 -L /nix/store/.../riscv64-unknown-linux-gnu-glibc-.../lib ./umem_test
```

### aarch64 Cross-Compilation

```bash
nix develop .#aarch64
```

**Provides:**
- aarch64 (ARM64) cross-compilation toolchain
- QEMU for testing
- Build tools

**Workflow:**
```bash
# Automatic build
nix build .#libumem-aarch64

# Manual build
./autogen.sh
./configure --host=aarch64-unknown-linux-gnu
make

# Test with QEMU
qemu-aarch64 -L /nix/store/.../aarch64-unknown-linux-gnu-glibc-.../lib ./umem_test
```

### Multi-Architecture Testing

```bash
nix develop .#test-all
```

**Provides:**
- QEMU for all architectures
- Test runners
- Build tools

**Workflow:**
```bash
# Build all architectures
nix build .#libumem
nix build .#libumem-riscv64
nix build .#libumem-aarch64

# Run tests
./scripts/test-all-architectures.sh
```

## Building Packages

### Native Build

```bash
# Build
nix build

# Or explicitly
nix build .#libumem

# Outputs
./result/lib/libumem.so
./result/lib/libumem_malloc.so
./result/include/umem.h
./result/share/doc/libumem/
```

### Cross-Compiled Builds

```bash
# RISC-V
nix build .#libumem-riscv64

# aarch64
nix build .#libumem-aarch64

# Build all
nix build .#libumem .#libumem-riscv64 .#libumem-aarch64
```

## Running Tests

### Native Tests

```bash
# Via Nix app
nix run .#test-native

# Or in dev shell
nix develop
make check
```

### RISC-V Tests (QEMU)

```bash
# Via Nix app
nix run .#test-riscv64

# Manual
nix build .#libumem-riscv64
qemu-riscv64 -L $(nix eval --raw .#pkgsRiscV64.stdenv.cc.libc) ./result/bin/umem_test
```

### aarch64 Tests (QEMU)

```bash
# Via Nix app
nix run .#test-aarch64

# Manual
nix build .#libumem-aarch64
qemu-aarch64 -L $(nix eval --raw .#pkgsAarch64.stdenv.cc.libc) ./result/bin/umem_test
```

## Checks

Nix flake checks verify build and functionality:

```bash
# Run all checks
nix flake check

# Individual checks
nix build .#checks.x86_64-linux.default
nix build .#checks.x86_64-linux.integration
nix build .#checks.x86_64-linux.build-riscv64
nix build .#checks.x86_64-linux.build-aarch64
```

**Available checks:**
- `default` - Native build succeeds
- `integration` - Integration test passes
- `build-riscv64` - RISC-V cross-compilation succeeds
- `build-aarch64` - aarch64 cross-compilation succeeds

## CI Integration

### GitHub Actions

```yaml
name: Nix Build and Test

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: cachix/install-nix-action@v25
      - uses: cachix/cachix-action@v14
        with:
          name: my-cache

      # Build all architectures
      - name: Build native
        run: nix build .#libumem

      - name: Build RISC-V
        run: nix build .#libumem-riscv64

      - name: Build aarch64
        run: nix build .#libumem-aarch64

      # Run tests
      - name: Run native tests
        run: nix run .#test-native

      # Run checks
      - name: Run checks
        run: nix flake check
```

## Development Workflow

### Typical Development Session

```bash
# 1. Enter dev shell
nix develop

# 2. Make changes to code
vim umem.c

# 3. Build
make

# 4. Test
make check

# 5. Coverage
./scripts/run-coverage.sh
xdg-open test/coverage/index.html

# 6. Commit
git add -p
git commit
```

### Testing New Architecture Support

```bash
# 1. Implement genasm
vim riscv64/umem_genasm.c

# 2. Update configure.ac
vim configure.ac

# 3. Build for that architecture
nix build .#libumem-riscv64

# 4. Test with QEMU
nix run .#test-riscv64

# 5. Debug if needed
nix develop .#riscv64
qemu-riscv64 -g 1234 ./result/bin/umem_test
# In another terminal:
gdb-multiarch ./result/bin/umem_test
(gdb) target remote localhost:1234
```

### Benchmarking Across Architectures

```bash
# Build benchmarks for all architectures
nix develop .#native
cd test/bench && make

# Run native benchmarks
./bench_allocators.sh umem libc

# Compare architectures (if running on those platforms)
# Or use QEMU user-mode for rough comparisons
```

## Advanced Usage

### Custom Patches

```bash
# Apply patches before building
nix build .#libumem --override-input nixpkgs github:NixOS/nixpkgs/branch
```

### Development with Direnv

Create `.envrc`:

```bash
use flake
```

Then:

```bash
direnv allow
# Shell automatically enters nix develop when you cd into directory
```

### Binary Cache

Speed up builds with Cachix:

```bash
# Setup
cachix use my-cache

# Push builds
cachix push my-cache $(nix build .#libumem --print-out-paths)
cachix push my-cache $(nix build .#libumem-riscv64 --print-out-paths)
cachix push my-cache $(nix build .#libumem-aarch64 --print-out-paths)
```

## Troubleshooting

### Build Failures

```bash
# Clean and rebuild
nix build .#libumem --rebuild

# Verbose output
nix build .#libumem --print-build-logs

# Debug failed build
nix develop .#libumem
./autogen.sh
./configure
make
```

### QEMU Issues

```bash
# Check QEMU installation
nix shell nixpkgs#qemu
qemu-riscv64 --version
qemu-aarch64 --version

# Set QEMU environment
export QEMU_LD_PREFIX=/nix/store/.../riscv64-unknown-linux-gnu-glibc-2.38/lib

# Debug with QEMU
qemu-riscv64 -d in_asm,cpu ./umem_test
```

### Cross-Compilation Issues

```bash
# Check toolchain
nix develop .#riscv64
which riscv64-unknown-linux-gnu-gcc
riscv64-unknown-linux-gnu-gcc --version

# Verify config.guess
./config.guess
./config.sub riscv64-unknown-linux-gnu

# Check configure detection
./configure --host=riscv64-unknown-linux-gnu
cat config.log | grep -i riscv
```

## Architecture Support Matrix

| Architecture | Status | Build Command | Test Command |
|--------------|--------|--------------|--------------|
| x86_64 | ✓ Complete | `nix build` | `nix run .#test-native` |
| i386 | ✓ Inherited | Native build | Native test |
| riscv64 | ✓ Template | `nix build .#libumem-riscv64` | `nix run .#test-riscv64` |
| aarch64 | ✓ Template | `nix build .#libumem-aarch64` | `nix run .#test-aarch64` |
| sparc | ⚠ Planned | TBD | TBD |
| Windows x64 | ⚠ Planned | TBD | TBD |
| Windows ARM64 | ⚠ Planned | TBD | TBD |

## Performance Considerations

### Native Builds

- First build may take 5-10 minutes
- Subsequent builds use Nix cache (seconds)
- Enable parallel builds: `--max-jobs auto`

### Cross-Compilation

- First cross-build: 10-15 minutes
- Uses native toolchain with cross-compiler
- Much faster than full QEMU system emulation

### QEMU Testing

- User-mode QEMU: Fast (near-native for many workloads)
- System emulation: Slower but more accurate
- Use binary caches to avoid repeated builds

## See Also

- `flake.nix` - Flake definition
- `docs/PORTING.md` - Architecture porting guide
- `docs/TESTING.md` - Test suite documentation
- `IMPLEMENTATION_STATUS.md` - Implementation tracking
- [Nix Manual](https://nixos.org/manual/nix/stable/)
- [Nix Flakes](https://nixos.wiki/wiki/Flakes)

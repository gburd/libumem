# libumem Validation and Benchmarking Guide

**Date**: 2026-03-27
**Phase**: Validation & Performance Analysis
**Status**: Ready to Execute

## Quick Start

```bash
# 1. Push committed changes
git push origin master

# 2. Verify integration
bash scripts/verify-integration.sh

# 3. Build and test
nix build .#libumem
nix run .#test-native

# 4. Run benchmarks
cd test/bench && make && ./bench_allocators.sh
```

---

## Phase 1: Build Validation

### 1.1 Verify Integration

```bash
bash scripts/verify-integration.sh
```

**Expected Output**: All checks should pass ✓

### 1.2 Native Build (Autotools)

```bash
# Generate configure script
./autogen.sh

# Configure
./configure

# Build
make -j$(nproc)

# Expected: Clean build with no errors
```

### 1.3 Run Test Suite

```bash
make check
```

**Expected Tests**:
- ✓ umem_test (basic functionality)
- ✓ umem_test2 (multithreaded)
- ✓ umem_test3 (malloc replacement)
- ✓ umem_test4 (shell script)
- ✓ umem_ptc_test (PTC functionality)
- ✓ umem_ptc_fork_test (fork safety)
- ✓ test/test_main (5 unit test suites)
- ✓ test/property/prop_alloc_free2 (property tests)

**Success Criteria**: All tests pass

### 1.4 Nix Builds

```bash
# Native build
nix build .#libumem
ls -lh result/lib/libumem.so

# RISC-V cross-compilation
nix build .#libumem-riscv64
ls -lh result/lib/libumem.so

# ARM64 cross-compilation
nix build .#libumem-aarch64
ls -lh result/lib/libumem.so

# Run all checks
nix flake check
```

**Success Criteria**: All builds complete successfully

---

## Phase 2: Coverage Analysis

### 2.1 Build with Coverage

```bash
# Clean previous build
make clean

# Configure with coverage
./configure --enable-coverage

# Build and test
make -j$(nproc)
make check
```

### 2.2 Generate Coverage Report

```bash
bash scripts/run-coverage.sh
```

**This script will**:
1. Collect coverage data with lcov
2. Filter out system and test files
3. Generate HTML report in `test/coverage/`
4. Display summary statistics

### 2.3 View Coverage

```bash
# Open in browser
xdg-open test/coverage/index.html
# Or on macOS: open test/coverage/index.html
```

### 2.4 Coverage Targets

**Overall Target**: >95% line coverage

**Critical Files** (must have >90%):
- `umem.c` - Core allocator
- `umem_hooks.c` - Hook API
- `vmem.c` - Virtual memory layer
- `umem_fork.c` - Fork safety
- Architecture genasm files

**Expected Coverage**:
- Unit-tested components: >95%
- Integration components: >85%
- Error paths: >75%

---

## Phase 3: Sanitizer Testing

### 3.1 Run All Sanitizers

```bash
bash scripts/run-sanitizers.sh
```

This script tests with:
- **AddressSanitizer (ASan)**: Memory errors, leaks, buffer overflows
- **UndefinedBehaviorSanitizer (UBSan)**: Undefined behavior
- **ThreadSanitizer (TSan)**: Data races (if supported)

### 3.2 Manual Sanitizer Testing

**AddressSanitizer**:
```bash
make clean
./configure --enable-asan
make -j$(nproc)
ASAN_OPTIONS=detect_leaks=1:check_initialization_order=1 make check
```

**UndefinedBehaviorSanitizer**:
```bash
make clean
./configure --enable-ubsan
make -j$(nproc)
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 make check
```

**ThreadSanitizer** (separate run, conflicts with ASan):
```bash
make clean
./configure --enable-tsan
make -j$(nproc)
make check
```

### 3.3 Success Criteria

- Zero ASan errors or leaks
- Zero UBSan warnings
- Zero TSan data races (or document known/acceptable races)

---

## Phase 4: Performance Benchmarking

### 4.1 Install Competitor Allocators

```bash
# Debian/Ubuntu
sudo apt-get install libjemalloc-dev libtcmalloc-minimal4 libmimalloc-dev

# Or build from source for latest versions
```

### 4.2 Build Benchmark Suite

```bash
cd test/bench
make clean
make -j$(nproc)
```

### 4.3 Quick Benchmark (Smoke Test)

```bash
# Test umem only (fast, ~30 seconds)
./bench_allocators.sh -q umem
```

**Expected Output**:
```
=== Benchmarking: umem ===
Workload: single-thread
  Throughput: XXX ops/sec
  Latency p50: XX ns
  Latency p99: XXX ns

Workload: multi-thread
  ...
```

### 4.4 Full Benchmark Suite

```bash
# Compare all allocators (takes 5-15 minutes)
./bench_allocators.sh umem libc jemalloc tcmalloc mimalloc
```

**This benchmarks**:
- **Single-threaded**: Sequential alloc/free
- **Multi-threaded**: Concurrent allocation (1, 2, 4, 8, 16 threads)
- **Producer-consumer**: Cross-thread allocation/deallocation
- **Fragmentation**: Allocate-free-allocate patterns

### 4.5 Analyze Results

```bash
# Results saved in test/bench/results/
ls -lh results/*.csv

# View summary
cat results/summary.txt
```

### 4.6 Performance Targets

Based on Phase 0 research, libumem should achieve:

**Throughput** (relative to libc malloc):
- Single-threaded: 1.0-1.5x (at par or better)
- Multi-threaded: 1.5-3.0x (better due to PTC)
- Producer-consumer: 1.2-2.0x

**Latency p99** (target <500ns for small allocations):
- Small allocations (≤128B): <500ns
- Medium allocations (129B-4KB): <1µs
- Large allocations (>4KB): <10µs

**Memory Overhead**:
- Per-allocation metadata: 16-32 bytes (competitive)
- Fragmentation ratio: <1.2 (good)

**Comparison to Competitors**:
- vs libc malloc: Better multi-threaded, comparable single-threaded
- vs jemalloc: Competitive within 10-20%
- vs tcmalloc: Similar performance profile
- vs mimalloc: Within 20-30% (mimalloc is highly optimized)

### 4.7 Benchmark Report

Create a summary report:

```bash
cd test/bench
cat > BENCHMARK_REPORT.md <<EOF
# libumem Performance Benchmark Results

**Date**: $(date +%Y-%m-%d)
**System**: $(uname -m) / $(uname -s)
**CPU**: $(lscpu | grep "Model name" | cut -d: -f2 | xargs)

## Results Summary

[Paste throughput comparison]
[Paste latency comparison]
[Paste memory overhead comparison]

## Observations

- libumem shows <X>x speedup in multi-threaded workloads
- Latency p99 is within target (<500ns for small allocations)
- Memory overhead is competitive at X bytes per allocation
- PTC (Per-Thread Cache) provides significant benefits for concurrent workloads

## Conclusions

[Summarize findings]
[Note any regressions or surprises]
[Recommendations for optimization]
EOF
```

---

## Phase 5: Cross-Platform Testing

### 5.1 QEMU Testing (RISC-V)

```bash
# Build for RISC-V
nix build .#libumem-riscv64

# Run tests with QEMU
nix run .#test-riscv64
```

**Expected Output**: Tests pass under QEMU emulation

### 5.2 QEMU Testing (ARM64)

```bash
# Build for ARM64
nix build .#libumem-aarch64

# Run tests with QEMU
nix run .#test-aarch64
```

### 5.3 Native ARM64 Testing (if available)

If you have access to ARM64 hardware (Raspberry Pi 4, AWS Graviton, etc.):

```bash
# On ARM64 system
./autogen.sh
./configure
make -j$(nproc)
make check

# Run benchmarks
cd test/bench && make && ./bench_allocators.sh
```

### 5.4 Native RISC-V Testing (if available)

If you have RISC-V hardware:

```bash
# Same as ARM64
./autogen.sh
./configure
make check
```

---

## Phase 6: CI Validation

### 6.1 Push to GitHub

```bash
git push origin master
```

### 6.2 Monitor GitHub Actions

Navigate to: `https://github.com/<your-org>/libumem/actions`

**Expected Jobs**:
- ✓ test-matrix (Ubuntu, normal/coverage/asan/ubsan)
- ✓ benchmark (performance tests)
- ✓ valgrind (memory leak detection)
- ✓ nix-build (native, riscv64, aarch64)

All jobs should pass.

### 6.3 Review Coverage Report

If using Codecov, check the coverage report from CI.

---

## Phase 7: Debugger Extension Testing

### 7.1 GDB Extension

```bash
# Build with debug symbols
./configure CFLAGS="-g -O0"
make

# Start GDB with extension
gdb -x tools/gdb/umem_gdb.py ./umem_test

# In GDB:
(gdb) break main
(gdb) run
(gdb) umem-cache-list
(gdb) umem-stats
```

**Expected**: Commands work and display cache/allocation info

### 7.2 LLDB Extension

```bash
# Start LLDB with extension
lldb ./umem_test

# In LLDB:
(lldb) command script import tools/lldb/umem_lldb.py
(lldb) breakpoint set -n main
(lldb) run
(lldb) umem-cache-list
```

### 7.3 Test with Real Application

```bash
# Example: Debug PostgreSQL with libumem
gdb --args postgres -D /path/to/data
(gdb) source tools/gdb/umem_gdb.py
(gdb) break palloc
(gdb) run
(gdb) umem-whatis 0x<address>
```

---

## Phase 8: Documentation Review

### 8.1 Verify Man Pages

```bash
man ./umem_alloc.3
man ./umem_cache_create.3
man ./umem_debug.3
```

**Check**: Complete, accurate, no formatting errors

### 8.2 Review Guides

```bash
# Check all documentation files
cat NIX_USAGE.md
cat BUILD_INTEGRATION.md
cat IMPLEMENTATION_STATUS.md
cat docs/BENCHMARKING.md
cat docs/DEBUGGING.md
cat docs/PORTING.md
cat docs/TESTING.md
```

**Verify**: Accurate, up-to-date, complete

### 8.3 Test Examples

```bash
cd examples

# Build palloc integration example
gcc -o palloc_test palloc_integration.c \
    -I../include -L../lib -lumem -pthread

# Run
LD_LIBRARY_PATH=../lib ./palloc_test
```

---

## Validation Checklist

Use this checklist to track validation progress:

### Build & Integration
- [ ] Integration verification script passes
- [ ] Autotools build succeeds
- [ ] Nix build succeeds (native)
- [ ] Nix build succeeds (riscv64)
- [ ] Nix build succeeds (aarch64)
- [ ] All tests pass with `make check`
- [ ] Distribution tarball creates successfully

### Coverage
- [ ] Coverage analysis completes
- [ ] Overall coverage >95%
- [ ] Critical files >90%
- [ ] Coverage report generated

### Sanitizers
- [ ] ASan: Zero errors/leaks
- [ ] UBSan: Zero warnings
- [ ] TSan: Zero data races (or documented)
- [ ] Valgrind: Acceptable results for allocator

### Performance
- [ ] Benchmark framework builds
- [ ] Smoke test passes
- [ ] Full benchmark suite completes
- [ ] Results within target ranges
- [ ] No significant regressions vs baseline
- [ ] Benchmark report created

### Cross-Platform
- [ ] RISC-V build succeeds
- [ ] RISC-V QEMU tests pass
- [ ] ARM64 build succeeds
- [ ] ARM64 QEMU tests pass
- [ ] Native ARM64 testing (if available)

### CI/CD
- [ ] All GitHub Actions jobs pass
- [ ] Coverage uploaded to Codecov
- [ ] No build warnings
- [ ] Cross-compilation successful

### Tools & Docs
- [ ] GDB extension works
- [ ] LLDB extension works
- [ ] Man pages render correctly
- [ ] Documentation accurate
- [ ] Examples compile and run

---

## Troubleshooting

### Build Failures

```bash
# Clean everything
make distclean
git clean -fdx

# Regenerate build files
./autogen.sh

# Try again
./configure && make
```

### Test Failures

```bash
# Run individual test with verbose output
./test/test_main --verbose

# Check test logs
cat test-suite.log
cat test/unit/*.log
```

### Coverage Issues

```bash
# Ensure gcov/lcov installed
which gcov lcov

# Check coverage flags
./configure --enable-coverage
make clean && make check

# Manual coverage collection
lcov --capture --directory . --output-file coverage.info
lcov --list coverage.info
```

### Benchmark Issues

```bash
# Check required allocators installed
ldconfig -p | grep -E 'jemalloc|tcmalloc|mimalloc'

# Run with single allocator
cd test/bench
./bench_allocators.sh umem

# Adjust iterations for faster testing
./bench_allocators.sh -i 10000 umem
```

---

## Expected Time Investment

| Phase | Duration | Priority |
|-------|----------|----------|
| Build Validation | 30-60 min | Critical |
| Coverage Analysis | 15-30 min | High |
| Sanitizer Testing | 30-60 min | High |
| Performance Benchmarking | 1-2 hours | Critical |
| Cross-Platform Testing | 30-60 min | Medium |
| CI Validation | 15-30 min | High |
| Debugger Testing | 30-60 min | Medium |
| Documentation Review | 15-30 min | Low |

**Total**: 4-7 hours for comprehensive validation

---

## Success Criteria Summary

**Minimum for Release**:
- ✓ All builds succeed
- ✓ All tests pass
- ✓ Coverage >80%
- ✓ Zero sanitizer errors
- ✓ CI passes
- ✓ No performance regressions

**Recommended for Production**:
- ✓ Coverage >95%
- ✓ Full benchmark suite run
- ✓ Cross-platform testing complete
- ✓ Debugger extensions validated
- ✓ Documentation reviewed

---

## Next Steps After Validation

1. **Tag Release**: `git tag -a v1.0.2 -m "Comprehensive enhancements release"`
2. **Push Tags**: `git push origin v1.0.2`
3. **Create GitHub Release**: With benchmark results and changelog
4. **Update Package Registries**: Submit to Nix, Homebrew, etc.
5. **Announce**: Blog post, mailing lists, social media

---

## Contact & Support

For issues or questions during validation:
- Check `IMPLEMENTATION_STATUS.md` for component status
- Review `docs/TESTING.md` for test suite details
- See `docs/BENCHMARKING.md` for performance analysis guide
- Refer to `BUILD_INTEGRATION.md` for build system info

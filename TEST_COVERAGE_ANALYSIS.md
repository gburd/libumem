# Test Coverage Analysis

**Document Purpose**: Address Sun Microsystems Reviewer Concern #9
**Goal**: Achieve >95% test coverage
**Current Status**: 46.2% line coverage, 56.2% function coverage
**Generated**: 2026-04-09

## Executive Summary

The libumem project has comprehensive testing infrastructure but currently achieves only **46.2% line coverage** and **56.2% function coverage**. This analysis identifies critical untested code sections and provides a roadmap to achieve >95% coverage.

## Current Coverage Status

Based on existing coverage reports in `test/coverage/`:

### Overall Metrics
- **Line Coverage**: 46.2% (1912/4141 lines)
- **Function Coverage**: 56.2% (117/208 functions)
- **Source Files**: 20 main source files + architecture-specific code

### Coverage by Component

| Component | Line Coverage | Function Coverage | Status |
|-----------|---------------|-------------------|--------|
| umem.c (core allocator) | 46.0% | 53.8% | **Critical gap** |
| umem_audit.c | 0.0% | 0.0% | **Untested** |
| umem_fail.c | 0.0% | 0.0% | **Untested** |
| umem_hooks.c | 0.0% | 0.0% | **Untested** |
| umem_update_thread.c | 0.0% | 0.0% | **Untested** |
| vmem_sbrk.c | 5.7% | 28.6% | **Critical gap** |
| malloc.c | 10.8% | 33.3% | **Critical gap** |
| envvar.c | 14.1% | 25.0% | **Critical gap** |
| misc.c | 28.1% | 18.2% | **Critical gap** |
| malloc_interpose.c | 32.3% | 40.0% | Needs improvement |
| vmem.c | 64.5% | 80.0% | Good |
| vmem_fork.c | 68.6% | 88.9% | Good |
| vmem_mmap.c | 68.1% | 100.0% | Good |
| init_lib.c | 93.5% | 100.0% | Excellent |
| vmem_base.c | 92.3% | 100.0% | Excellent |
| getpcstack.c | 100.0% | 100.0% | Complete |
| tmem_stubs.c | 100.0% | 100.0% | Complete |

### Experimental/Optional Features (Not in Coverage Report)

These features are conditionally compiled and not measured in the current report:

- **umem_percpu.c** - Per-CPU caching (--enable-percpu-caching)
- **umem_rseq.c** - Restartable sequences (--enable-rseq)
- **umem_rseq_x86_64.S** - rseq x86_64 assembly
- **umem_rseq_aarch64.S** - rseq aarch64 assembly
- **umem_hash_partition.c** - NUMA hash partitioning (--enable-numa)
- **umem_numa.c** - NUMA allocation (--enable-numa)
- **umem_htm.c** - Hardware transactional memory (--enable-htm)
- **umem_simd.h** - SIMD vectorization (inline functions)

## Critical Untested Code Sections

### 1. Tagged Pointer Atomic Operations

**Location**: `umem.c` (depot operations)
**Current Coverage**: 46.0%
**Issue**: Lock-free depot operations using tagged pointers with C11 atomics

**Untested scenarios**:
- ABA problem prevention via tagged pointers
- Concurrent depot full/empty transitions
- Race conditions between multiple allocators
- Memory ordering guarantees (acquire/release semantics)

**Required tests**:
```c
// Test: Concurrent depot push/pop with ABA prevention
// Test: Tagged pointer atomic CAS under contention
// Test: Memory ordering in lock-free paths
// Test: Depot wraparound with tag overflow
```

### 2. SIMD Code Paths

**Location**: `umem_simd.h` (inline functions in header)
**Current Coverage**: Not measured (inline functions)
**Issue**: Three SIMD implementations + fallback not tested

**Untested scenarios**:
- SSE2 vector operations (x86_64)
- AVX2 vector operations (x86_64 with -mavx2)
- NEON vector operations (aarch64)
- Fallback scalar implementation
- Magazine scanning for non-NULL pointers
- Fast magazine initialization

**Required tests**:
```c
// Test: umem_mag_scan_notnull() with SSE2/AVX2/NEON
// Test: umem_mag_init_fast() with all vector paths
// Test: SIMD operations on unaligned arrays
// Test: Fallback path when SIMD unavailable
// Test: Performance validation (2-4x speedup)
```

### 3. NUMA Hash Selection

**Location**: `umem_hash_partition.c`, `umem_numa.c`
**Current Coverage**: 0% (conditional compilation)
**Issue**: Hash-based NUMA node selection untested

**Untested scenarios**:
- FNV-1a hash distribution across NUMA nodes
- Hash partition creation and lookup
- NUMA node affinity selection
- Multi-socket allocation patterns
- Hash collision handling

**Required tests**:
```c
// Test: Hash distribution uniformity (chi-squared test)
// Test: NUMA node selection for different allocation patterns
// Test: Performance on multi-socket systems
// Test: Fallback when NUMA unavailable
```

### 4. Restartable Sequences (rseq)

**Location**: `umem_rseq.c`, `umem_rseq_x86_64.S`, `umem_rseq_aarch64.S`
**Current Coverage**: 0% (conditional compilation)
**Issue**: Assembly code and kernel interactions untested

**Untested scenarios**:
- rseq registration and unregistration
- Critical section restart handling
- CPU migration during rseq execution
- Signature verification
- x86_64 vs aarch64 assembly paths
- Fallback when kernel doesn't support rseq

**Required tests**:
```c
// Test: rseq registration success/failure
// Test: CPU migration triggers restart
// Test: Invalid signature detection
// Test: Per-CPU cache access via rseq
// Test: Thread cleanup on exit
```

### 5. Lock-Free Depot Operations

**Location**: `umem.c` (depot_t structure)
**Current Coverage**: Partial (46.0%)
**Issue**: Complex lock-free algorithms with subtle race conditions

**Untested scenarios**:
- Depot contention under heavy load
- Empty depot allocation
- Full depot free operations
- Magazine exchange races
- Depot lock acquisition failure paths

**Required tests**:
```c
// Test: High-contention depot operations (100+ threads)
// Test: Depot full/empty boundary conditions
// Test: Magazine exchange correctness under race
// Test: Lock acquisition timeout and retry
```

### 6. Error Paths and Edge Cases

**Location**: Multiple files
**Current Coverage**: Varies widely
**Issue**: Error handling paths rarely exercised

**Untested scenarios**:
- Out-of-memory handling (umem_fail.c - 0% coverage)
- Memory exhaustion with UMEM_NOFAIL
- Allocation failure callbacks (umem_hooks.c - 0% coverage)
- Signal handler safety (UMF_CHECKSIGNAL)
- Fork safety edge cases
- Update thread failure modes (umem_update_thread.c - 0% coverage)

**Required tests**:
```c
// Test: OOM injection and recovery
// Test: umem_nofail_callback invocation
// Test: Allocation during signal handler
// Test: Fork with active allocations
// Test: Update thread crash recovery
```

### 7. Debug and Audit Features

**Location**: `umem_audit.c`, `umem_debug.c`
**Current Coverage**: 0% (umem_audit.c)
**Issue**: Debugging features completely untested

**Untested scenarios**:
- Transaction auditing (UMF_AUDIT)
- Deadbeef pattern checking (UMF_DEADBEEF)
- Redzone validation (UMF_REDZONE)
- Content logging (UMF_CONTENTS)
- Firewall mode (UMF_FIREWALL)
- Buffer tag verification

**Required tests**:
```c
// Test: UMEM_DEBUG=audit detection
// Test: Deadbeef pattern on free
// Test: Redzone overflow detection
// Test: Buffer-after-free detection
// Test: Memory leak auditing
```

### 8. Environment Variable Parsing

**Location**: `envvar.c`
**Current Coverage**: 14.1%
**Issue**: Complex parsing logic untested

**Untested scenarios**:
- UMEM_DEBUG flag parsing
- UMEM_LOGGING configuration
- UMEM_OPTIONS parsing
- Invalid environment variable handling
- Flag combination validation

**Required tests**:
```c
// Test: All UMEM_DEBUG flag combinations
// Test: UMEM_LOGGING levels
// Test: Invalid flag detection
// Test: Conflicting flag combinations
```

### 9. vmem Allocator Edge Cases

**Location**: `vmem_sbrk.c`, `vmem.c`
**Current Coverage**: 5.7% (sbrk), 64.5% (vmem)
**Issue**: Low-level memory management untested

**Untested scenarios**:
- sbrk() failure and fallback
- vmem arena boundary conditions
- Large allocation (>UMEM_MAXBUF)
- vmem_alloc() with VM_NOSLEEP
- Arena exhaustion
- Segment coalescing

**Required tests**:
```c
// Test: sbrk() failure simulation
// Test: vmem arena exhaustion
// Test: Large allocation stress test
// Test: VM_NOSLEEP failure paths
// Test: Segment free and coalesce
```

### 10. Per-Thread Caching

**Location**: `umem_tcache.c`, `malloc_guard.c`
**Current Coverage**: Not in report (conditional)
**Issue**: PTC implementation not measured

**Untested scenarios**:
- Thread-local cache allocation/free
- Cache overflow and flush
- Thread exit cleanup
- malloc recursion guard
- pthread integration

**Required tests**:
```c
// Test: PTC allocation and free
// Test: Cache overflow triggers flush
// Test: Thread cleanup on exit
// Test: Recursion guard prevents deadlock
```

## Strategy to Reach >95% Coverage

### Phase 1: Critical Gap Closure (Target: 70% coverage)

**Priority 1 - Zero Coverage Files** (2 weeks):
1. `umem_audit.c` - Add debug feature tests
2. `umem_fail.c` - Add OOM injection tests
3. `umem_hooks.c` - Add callback tests
4. `umem_update_thread.c` - Add background thread tests

**Priority 2 - Low Coverage Core** (2 weeks):
5. `vmem_sbrk.c` (5.7% → 80%)
6. `malloc.c` (10.8% → 80%)
7. `envvar.c` (14.1% → 90%)
8. `umem.c` (46.0% → 75%)

### Phase 2: Experimental Feature Testing (Target: 85% coverage)

**Conditional Features** (3 weeks):
1. Per-CPU caching tests (--enable-percpu-caching)
2. NUMA tests (--enable-numa)
3. rseq tests (--enable-rseq)
4. HTM tests (--enable-htm)
5. SIMD vector tests (all architectures)

### Phase 3: Edge Case and Stress Testing (Target: 95% coverage)

**Comprehensive Coverage** (3 weeks):
1. Lock-free algorithm stress tests
2. Tagged pointer atomic races
3. Memory ordering validation
4. Signal handler safety
5. Fork/exec combinations
6. Multi-threaded stress (1000+ threads)
7. Error injection framework

### Phase 4: Assembly and Platform-Specific (Target: >95% coverage)

**Architecture Coverage** (2 weeks):
1. x86_64 assembly paths
2. aarch64 assembly paths
3. rseq critical sections
4. SIMD vector operations
5. Platform-specific hooks

## Recommended Tests to Add

### New Test Files

#### `test/unit/test_umem_audit_features.c`
- Test transaction auditing
- Test deadbeef pattern checking
- Test redzone validation
- Test content logging
- Test firewall mode

#### `test/unit/test_umem_fail_injection.c`
- Test OOM handling
- Test allocation failure callbacks
- Test UMEM_NOFAIL behavior
- Test memory exhaustion scenarios

#### `test/unit/test_umem_hooks_callbacks.c`
- Test hook registration
- Test allocation callbacks
- Test free callbacks
- Test error callbacks

#### `test/unit/test_umem_update_thread.c`
- Test background thread startup
- Test periodic maintenance
- Test thread termination
- Test cache reaping

#### `test/unit/test_vmem_sbrk_edge_cases.c`
- Test sbrk() failure handling
- Test large allocations
- Test boundary conditions
- Test fallback mechanisms

#### `test/unit/test_envvar_parsing.c`
- Test all UMEM_DEBUG flags
- Test UMEM_LOGGING levels
- Test UMEM_OPTIONS parsing
- Test invalid input handling

#### `test/unit/test_simd_operations.c`
- Test SSE2 vector paths
- Test AVX2 vector paths
- Test NEON vector paths
- Test fallback scalar paths
- Verify 2-4x performance improvement

#### `test/integration/test_tagged_pointer_atomics.c`
- Test concurrent depot operations
- Test ABA prevention
- Test memory ordering
- Test high-contention scenarios

#### `test/integration/test_rseq_operations.c`
- Test rseq registration
- Test CPU migration handling
- Test critical section restarts
- Test per-CPU cache access

#### `test/integration/test_numa_allocation.c`
- Test hash-based node selection
- Test NUMA-aware allocation
- Test multi-socket performance
- Test hash distribution

#### `test/stress/test_lockfree_depot.c`
- 100+ threads concurrent operations
- Depot contention stress test
- Magazine exchange races
- Long-duration stability test

### Enhanced Existing Tests

Enhance existing test files to cover untested branches:

1. **test/unit/test_umem_alloc.c** - Add edge cases, error paths
2. **test/unit/test_umem_cache.c** - Add magazine operations
3. **test/integration/test_multithreaded.c** - Increase thread count
4. **test/property/prop_alloc_free.c** - Add concurrent properties

## Instructions for Running Coverage Analysis

### Prerequisites

Install coverage tools:
```bash
# Debian/Ubuntu
sudo apt-get install lcov gcov

# Fedora/RHEL
sudo dnf install lcov gcc

# macOS (using Homebrew)
brew install lcov
```

### Generate Coverage Report

#### Method 1: Using Make Target (Recommended)

```bash
# Configure with coverage enabled
./configure --enable-coverage

# Build and generate coverage
make clean
make all
make check
make coverage

# View report
xdg-open test/coverage/index.html
```

#### Method 2: Using Coverage Script

```bash
# Run comprehensive coverage script
./scripts/generate-coverage-report.sh

# View report
xdg-open test/coverage/index.html
```

#### Method 3: Manual Steps

```bash
# 1. Configure with coverage flags
./configure --enable-coverage CFLAGS="--coverage -fprofile-arcs -ftest-coverage"

# 2. Clean previous coverage data
find . -name "*.gcda" -delete
find . -name "*.gcno" -delete

# 3. Build
make clean
make -j$(nproc)

# 4. Run all tests
make check
./test/test_main
./test/property/prop_alloc_free2
./test/property/prop_cache
./test/integration/test_multithreaded
./test/integration/test_signals
./test/integration/test_oom

# 5. Capture coverage data
lcov --capture --directory . --output-file coverage.info

# 6. Remove system and test files
lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage.info.cleaned

# 7. Generate HTML report
genhtml coverage.info.cleaned --output-directory test/coverage

# 8. View summary
lcov --summary coverage.info.cleaned
```

### Coverage for Experimental Features

Test experimental features separately with conditional compilation:

```bash
# Per-CPU caching
./configure --enable-coverage --enable-percpu-caching
make clean && make check && make coverage

# NUMA support
./configure --enable-coverage --enable-numa
make clean && make check && make coverage

# rseq support
./configure --enable-coverage --enable-rseq
make clean && make check && make coverage

# All experimental features
./configure --enable-coverage --enable-percpu-caching --enable-numa --enable-rseq --enable-htm
make clean && make check && make coverage
```

## HTML Report Generation Commands

### Basic Report

```bash
genhtml coverage.info.cleaned --output-directory test/coverage
```

### Report with Branch Coverage

```bash
genhtml coverage.info.cleaned \
  --output-directory test/coverage \
  --branch-coverage \
  --function-coverage \
  --demangle-cpp
```

### Report with Dark Theme

```bash
genhtml coverage.info.cleaned \
  --output-directory test/coverage \
  --dark-mode \
  --legend
```

### Report with File Filters

```bash
# Only show files with <80% coverage
genhtml coverage.info.cleaned \
  --output-directory test/coverage \
  --missed \
  --missed-threshold 80
```

## Coverage Metrics Tracking

Track coverage progress over time:

```bash
# Extract coverage percentage
COVERAGE=$(lcov --summary coverage.info.cleaned 2>&1 | \
  grep "lines" | awk '{print $2}')

# Log to file
echo "$(date): ${COVERAGE}" >> coverage_history.log

# Fail CI if below threshold
if (( $(echo "$COVERAGE < 95.0" | bc -l) )); then
  echo "Coverage ${COVERAGE}% below 95% threshold"
  exit 1
fi
```

## Continuous Integration

Add to `.github/workflows/coverage.yml`:

```yaml
name: Coverage

on: [push, pull_request]

jobs:
  coverage:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y lcov

      - name: Configure with coverage
        run: ./configure --enable-coverage

      - name: Build
        run: make -j$(nproc)

      - name: Run tests
        run: make check

      - name: Generate coverage
        run: make coverage

      - name: Check coverage threshold
        run: |
          COVERAGE=$(lcov --summary coverage.info.cleaned 2>&1 | \
            grep "lines" | awk '{print $2}' | sed 's/%//')
          echo "Coverage: ${COVERAGE}%"
          if (( $(echo "$COVERAGE < 95.0" | bc -l) )); then
            echo "::error::Coverage ${COVERAGE}% below 95% threshold"
            exit 1
          fi

      - name: Upload coverage report
        uses: actions/upload-artifact@v4
        with:
          name: coverage-report
          path: test/coverage/
```

## Coverage Report Interpretation

### Understanding Metrics

- **Line Coverage**: Percentage of executable lines executed
- **Function Coverage**: Percentage of functions called
- **Branch Coverage**: Percentage of conditional branches taken

### Color Coding in HTML Report

- **Green (>90%)**: Excellent coverage
- **Yellow (75-90%)**: Good coverage, minor gaps
- **Orange (50-75%)**: Moderate coverage, significant gaps
- **Red (<50%)**: Poor coverage, critical gaps

### Focus Areas

1. **Zero Coverage** - Immediate priority
2. **Low Coverage (<50%)** - Critical gap
3. **Moderate Coverage (50-75%)** - Needs improvement
4. **Good Coverage (75-90%)** - Minor gaps
5. **Excellent Coverage (>90%)** - Maintain

## Timeline and Effort Estimate

| Phase | Duration | Target Coverage | Team Size |
|-------|----------|-----------------|-----------|
| Phase 1: Critical Gaps | 4 weeks | 70% | 2 engineers |
| Phase 2: Experimental | 3 weeks | 85% | 2 engineers |
| Phase 3: Edge Cases | 3 weeks | 95% | 2 engineers |
| Phase 4: Platform-Specific | 2 weeks | >95% | 1 engineer |
| **Total** | **12 weeks** | **>95%** | **2-3 engineers** |

## Success Criteria

1. **Line Coverage** ≥ 95%
2. **Function Coverage** ≥ 95%
3. **Branch Coverage** ≥ 90%
4. All experimental features tested with coverage
5. All assembly code paths validated
6. All error paths exercised
7. Stress tests pass (1000+ threads)
8. No undefined behavior (UBSAN clean)
9. No memory leaks (ASAN clean)
10. No data races (TSAN clean)

## Next Steps

1. Run `./scripts/generate-coverage-report.sh` to generate current report
2. Review HTML report to identify specific untested lines
3. Create GitHub issues for each uncovered component
4. Assign priority and owners
5. Implement tests following test naming conventions
6. Run coverage after each test addition
7. Track progress weekly
8. Aim for >95% coverage before v1.1.0 release

## References

- Coverage reports: `test/coverage/index.html`
- Existing tests: `test/unit/`, `test/integration/`, `test/property/`
- Test framework: munit (test/munit.h)
- Property testing: qc.h (QuickCheck-style)
- Build configuration: `configure.ac`, `Makefile.am`
- Coverage flags: `--enable-coverage` in configure

## Document Maintenance

This document should be updated:
- After each test addition (update coverage percentages)
- When new features are added (add to untested sections)
- Weekly during coverage improvement project
- Before major releases (verify >95% achieved)

---

**Last Updated**: 2026-04-09
**Coverage Goal**: >95% line and function coverage
**Current Status**: 46.2% line, 56.2% function (53.8% gap remaining)

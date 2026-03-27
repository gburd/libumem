# Testing Guide

Comprehensive guide to testing libumem.

## Quick Start

```bash
# Build and run all tests
./configure
make check

# With coverage
./configure --enable-coverage
make check
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory test/coverage

# With sanitizers
./configure --enable-asan --enable-ubsan
make check
```

## Test Suite Overview

libumem has multiple levels of testing:

### 1. Unit Tests (`test/unit/`)

Test individual functions and components in isolation.

- `test_umem_alloc.c` - umem_alloc/free/zalloc
- `test_umem_cache.c` - umem_cache API
- `test_umem_align.c` - Alignment functions
- `test_umem_debug.c` - Debug modes
- `test_vmem.c` - vmem layer

### 2. Property-Based Tests (`test/property/`)

Test invariants and properties using the qc framework.

- `prop_alloc_free.c` - Allocation/deallocation properties
- `prop_cache.c` - Cache behavior
- `prop_fragmentation.c` - Memory layout properties

### 3. Integration Tests (`test/integration/`)

Test complete workflows and interactions.

- `test_ptc.c` - Per-thread cache functionality
- `test_ptc_fork.c` - Fork safety
- `test_multithreaded.c` - Concurrent allocation
- `test_signals.c` - Signal handler safety
- `test_oom.c` - Out-of-memory handling

### 4. Benchmarks (`test/bench/`)

Performance testing and comparison.

See `test/bench/README.md` for details.

## Running Tests

### All Tests

```bash
make check
```

### Specific Test

```bash
./umem_ptc_test
./test/integration/test_multithreaded
```

### With Verbose Output

```bash
make check VERBOSE=1
```

### Test Framework Options

Tests use the µnit framework. Run with:

```bash
./test_umem_cache --help
./test_umem_cache --seed 12345  # Reproducible random tests
./test_umem_cache --iterations 1000  # More iterations
```

## Coverage Testing

### Generate Coverage Report

```bash
# Using the script
./scripts/run-coverage.sh

# Manual process
./configure --enable-coverage
make clean && make
make check
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory test/coverage
```

### View Coverage

```bash
xdg-open test/coverage/index.html
```

### Coverage Goals

- **Overall**: >95%
- **Core files** (umem.c, vmem.c): >98%
- **Critical paths**: 100% (alloc/free fast paths)

### Finding Uncovered Code

```bash
# List files by coverage
lcov --list coverage_filtered.info | sort -k2 -n

# Find uncovered lines in specific file
lcov --list coverage_filtered.info | grep umem.c
```

## Sanitizer Testing

### AddressSanitizer (ASan)

Detects:
- Use-after-free
- Heap buffer overflow
- Stack buffer overflow
- Memory leaks

```bash
./configure --enable-asan
make clean && make
ASAN_OPTIONS="detect_leaks=1:check_initialization_order=1" make check
```

### UndefinedBehaviorSanitizer (UBSan)

Detects:
- Integer overflow
- NULL pointer dereference
- Misaligned access
- Division by zero

```bash
./configure --enable-ubsan
make clean && make
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" make check
```

### ThreadSanitizer (TSan)

Detects:
- Data races
- Deadlocks
- Thread leaks

```bash
./configure --enable-tsan
make clean && make
TSAN_OPTIONS="history_size=7:second_deadlock_stack=1" make check
```

### Combined Sanitizers

```bash
# ASan + UBSan (recommended)
./configure --enable-asan --enable-ubsan
make clean && make check

# Run all sanitizers
./scripts/run-sanitizers.sh
```

Note: TSan cannot be combined with ASan/UBSan.

## Valgrind Testing

```bash
# Build with debug symbols
./configure CFLAGS="-g -O0"
make clean && make

# Run under valgrind
valgrind --leak-check=full --show-leak-kinds=all ./umem_ptc_test

# Full test suite
make check-valgrind
```

Note: Valgrind will report false positives for allocators due to internal caching.

## Stress Testing

### Long-Running Tests

```bash
# 24-hour stress test
for i in {1..1000}; do
    echo "Iteration $i"
    ./umem_ptc_test
    ./test/integration/test_multithreaded
done
```

### High Load

```bash
# Many concurrent threads
./test/integration/test_multithreaded --threads 128 --iterations 10000000

# Varied allocation sizes
./test/integration/test_stress --size-range 16:1048576 --operations 100000000
```

### Fork Stress

```bash
# Rapid fork/exit cycles
for i in {1..10000}; do
    ./umem_ptc_fork_test
done
```

## Debugging Test Failures

### Get Stack Trace

```bash
# With gdb
gdb --args ./umem_ptc_test
(gdb) run
(gdb) bt

# With core dump
ulimit -c unlimited
./umem_ptc_test
gdb ./umem_ptc_test core
(gdb) bt
```

### Enable Debug Output

```bash
# Set UMEM_DEBUG environment variable
UMEM_DEBUG=default ./umem_ptc_test

# More verbose
UMEM_DEBUG=audit,firewall,redzone ./umem_ptc_test
```

### Reproduce Specific Seed

```bash
# If test failure shows seed=12345
./test_umem_cache --seed 12345
```

### Isolate Failing Test

```bash
# Run single test case
./test_umem_cache --filter "test_cache_alloc_free"

# List all tests
./test_umem_cache --list
```

## Writing Tests

### Unit Test Template

```c
#include "munit.h"
#include <umem.h>

static MunitResult test_my_function(const MunitParameter params[], void* data) {
    // Setup
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(ptr);

    // Test
    memset(ptr, 0x42, 64);
    munit_assert_uint8(((uint8_t*)ptr)[0], ==, 0x42);

    // Teardown
    umem_free(ptr, 64);

    return MUNIT_OK;
}

static MunitTest tests[] = {
    { "/my_function", test_my_function, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/umem", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[]) {
    return munit_suite_main(&suite, NULL, argc, argv);
}
```

### Property-Based Test Template

```c
#include "qc.h"
#include <umem.h>

static bool prop_alloc_returns_aligned(qc_t *qc) {
    size_t size = qc_gen_size_t(qc, 1, 4096);

    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    if (!ptr) return false;

    // Property: pointer should be aligned
    bool aligned = ((uintptr_t)ptr % sizeof(void*)) == 0;

    umem_free(ptr, size);
    return aligned;
}

int main(void) {
    qc_init();
    qc_check(prop_alloc_returns_aligned, "alloc returns aligned pointer");
    return qc_summary();
}
```

## Test Categories

### Smoke Tests

Quick tests to verify basic functionality (< 1 second):

```bash
./umem_test
./umem_test2
```

### Regression Tests

Tests for previously fixed bugs:

```c
// test/regression/test_issue_123.c
static MunitResult test_double_free_crash(const MunitParameter params[], void* data) {
    // This used to crash (issue #123)
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    umem_free(ptr, 64);
    // Second free should be detected in debug mode
    return MUNIT_OK;
}
```

### Platform-Specific Tests

```c
#ifdef __linux__
static MunitResult test_linux_specific(const MunitParameter params[], void* data) {
    // Linux-specific functionality
}
#endif
```

## Continuous Integration

Tests run automatically on:

- Every push to main/master
- Every pull request
- Nightly builds

See `.github/workflows/test.yml` for CI configuration.

### CI Test Matrix

- **OS**: Ubuntu, macOS, Windows
- **Architecture**: AMD64, ARM64, i386
- **Build modes**: Normal, Coverage, ASan, UBSan
- **Compilers**: GCC, Clang

## Performance Testing

### Benchmark Suite

```bash
cd test/bench
make
./bench_allocators.sh
```

See `docs/BENCHMARKING.md` for details.

### Regression Thresholds

- Throughput decrease >5%: Warning
- Throughput decrease >10%: Failure
- p99 latency increase >10%: Warning
- p99 latency increase >20%: Failure

## Test Coverage Gaps

Current focus areas for improvement:

1. **vmem layer** - Currently minimal testing
2. **Error paths** - OOM, invalid arguments
3. **Edge cases** - Boundary conditions, alignment edge cases
4. **Cross-thread scenarios** - Producer-consumer patterns
5. **Signal safety** - Allocation in signal handlers
6. **Fork safety** - Complex fork scenarios

## References

- **µnit framework**: [nemequ.github.io/munit](https://nemequ.github.io/munit/)
- **qc framework**: Property-based testing for C
- **lcov**: [github.com/linux-test-project/lcov](https://github.com/linux-test-project/lcov)
- **AddressSanitizer**: [github.com/google/sanitizers](https://github.com/google/sanitizers)

## See Also

- `docs/DEBUGGING.md` - Debugging tools and techniques
- `docs/BENCHMARKING.md` - Performance analysis
- `test/bench/README.md` - Benchmark suite documentation

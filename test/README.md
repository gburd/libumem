# libumem Test Suite

Comprehensive test suite for libumem with >90% code coverage.

## Quick Start

```bash
# Run all tests
make check

# Run specific test suites
./test/test_main                           # Unit tests
./test/property/prop_alloc_free2           # Property-based tests
./test/integration/test_multithreaded      # Integration tests

# Generate coverage report
./scripts/generate-coverage.sh
xdg-open test/coverage/index.html
```

## Test Organization

```
test/
├── unit/                    # Unit tests (62 tests)
│   ├── test_umem_alloc.c           # Basic allocation APIs
│   ├── test_umem_cache.c           # Cache management
│   ├── test_umem_align.c           # Aligned allocations
│   ├── test_umem_debug.c           # Debug modes
│   ├── test_vmem.c                 # Virtual memory layer
│   ├── test_error_paths.c          # Error handling
│   ├── test_rare_flags.c           # Uncommon flags
│   └── test_boundary_conditions.c  # Edge cases
│
├── property/                # Property-based tests (12 properties)
│   ├── prop_alloc_free2.c          # Allocation invariants
│   ├── prop_cache.c                # Cache behavior
│   └── prop_fragmentation.c        # Fragmentation analysis
│
├── integration/             # Integration tests (15 tests)
│   ├── test_multithreaded.c        # Concurrency
│   ├── test_signals.c              # Signal safety
│   └── test_oom.c                  # OOM handling
│
├── bench/                   # Performance benchmarks
│   ├── bench_main.c                # Benchmark framework
│   ├── allocators.c                # Allocator comparison
│   └── bench_allocators.sh         # Benchmark runner
│
├── munit.c/h                # Test framework
├── qc.c/h                   # QuickCheck-style property testing
└── tdigest.c/h              # Statistical analysis
```

## Test Categories

### Unit Tests

Focus on individual API functions:

- **test_umem_alloc.c**: umem_alloc(), umem_free(), umem_zalloc()
- **test_umem_cache.c**: umem_cache_create(), umem_cache_alloc(), etc.
- **test_umem_align.c**: Aligned allocation variants
- **test_umem_debug.c**: Debug modes (AUDIT, REDZONE, FIREWALL)
- **test_vmem.c**: vmem_create(), vmem_alloc(), vmem_xalloc()
- **test_error_paths.c**: Error handling, invalid inputs
- **test_rare_flags.c**: NOFAIL, NOMAGAZINE, cache flags
- **test_boundary_conditions.c**: Min/max sizes, alignment boundaries

### Property-Based Tests

Test invariants that should always hold:

- **prop_alloc_free2.c**:
  - Allocated memory is writable
  - Free doesn't crash
  - Multiple alloc/free cycles work
  - Alignment is correct

- **prop_cache.c**:
  - Cache allocations return valid pointers
  - Cache objects maintain alignment
  - Constructor/destructor are called

- **prop_fragmentation.c**:
  - Fragmentation patterns
  - Memory efficiency
  - Working set behavior

### Integration Tests

Test system-level behavior:

- **test_multithreaded.c**: Concurrent allocations, cache contention
- **test_signals.c**: Allocation in signal handlers, re-entrancy
- **test_oom.c**: Out-of-memory recovery, NOFAIL behavior

### Benchmarks

Performance testing:

- **bench_allocators.sh**: Compare umem vs glibc, jemalloc, tcmalloc
- Various workloads: sequential, random, multithreaded

## Running Tests

### All Tests

```bash
make check
```

### Individual Test Suites

```bash
# Unit tests
./test/test_main

# With specific test filter
./test/test_main --filter="/umem_alloc/*"

# Property tests
./test/property/prop_alloc_free2
./test/property/prop_cache
./test/property/prop_fragmentation

# Integration tests
./test/integration/test_multithreaded
./test/integration/test_signals
./test/integration/test_oom

# Benchmarks
cd test/bench
./bench_allocators.sh umem
```

### With Valgrind

```bash
valgrind --leak-check=full ./test/test_main
```

### With Sanitizers

```bash
# Address Sanitizer
./configure --enable-asan
make clean && make && make check

# UndefinedBehavior Sanitizer
./configure --enable-ubsan
make clean && make && make check

# Thread Sanitizer
./configure --enable-tsan
make clean && make && make check
```

## Coverage Analysis

### Generate Coverage Report

```bash
# Automated (recommended)
./scripts/generate-coverage.sh --min-coverage 85

# Manual
./configure --enable-coverage
make clean && make
make check
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory test/coverage
xdg-open test/coverage/index.html
```

### Coverage Targets

| File | Target | Status |
|------|--------|--------|
| umem.c | >95% | On track |
| vmem.c | >95% | On track |
| malloc.c | >95% | On track |
| umem_fork.c | >90% | On track |
| Overall | >90% | On track |

See [COVERAGE.md](COVERAGE.md) for detailed status.

## Writing Tests

### Unit Test Template

```c
#include <umem.h>
#include "../munit.h"

static MunitResult
test_my_feature(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;

    // Arrange
    void *p = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(p);

    // Act
    memset(p, 0xAA, 64);

    // Assert
    munit_assert_uint8(*(uint8_t*)p, ==, 0xAA);

    // Cleanup
    umem_free(p, 64);

    return MUNIT_OK;
}

static MunitTest my_tests[] = {
    {"/my_feature", test_my_feature, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

const MunitSuite my_suite = {
    "/my_suite",
    my_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};
```

### Property Test Template

```c
#include <umem.h>
#include "../qc.h"

static bool
prop_allocation_valid(qc_value_t v)
{
    size_t size = v.u64 % 8192 + 1;
    void *p = umem_alloc(size, UMEM_DEFAULT);

    if (p == NULL) return false;

    // Property: allocated memory is writable
    memset(p, 0xFF, size);

    umem_free(p, size);
    return true;
}

int main(void)
{
    qc_init();
    qc_check(prop_allocation_valid, QC_GEN_U64, 1000);
    return qc_report();
}
```

### Adding Tests to Build

Edit `Makefile.am`:

```makefile
test_test_main_SOURCES = test/test_main.c test/munit.c \
    test/unit/test_new_feature.c  # Add here
```

Update `test/test_main.c`:

```c
extern MunitSuite new_feature_suite;

static MunitSuite* test_suites[] = {
    // existing suites...
    &new_feature_suite,
    NULL
};
```

## Test Guidelines

### Do's

✅ Test one concept per test function
✅ Use descriptive test names (`test_cache_aligned_allocation`)
✅ Clean up all allocations
✅ Check for NULL before dereferencing
✅ Test both success and failure cases
✅ Use munit_assert_* macros for clear failures

### Don'ts

❌ Don't test multiple unrelated things in one test
❌ Don't skip cleanup on assertion failure
❌ Don't depend on test execution order
❌ Don't use global state between tests
❌ Don't ignore valgrind/sanitizer warnings

## Continuous Integration

Tests run automatically on:
- Every push to master/main
- Every pull request
- Nightly builds

CI includes:
- Unit tests
- Property tests
- Integration tests
- Coverage analysis (target: >85%)
- ASan, UBSan sanitizers
- Valgrind memory check
- Cross-compilation (riscv64, aarch64)
- Benchmarks

See [../.github/workflows/test.yml](../.github/workflows/test.yml)

## Debugging Test Failures

### Verbose Output

```bash
./test/test_main --verbose
```

### Single Test

```bash
./test/test_main --filter="/umem_alloc/basic"
```

### With GDB

```bash
gdb --args ./test/test_main --filter="/failing_test"
(gdb) run
(gdb) bt  # backtrace on failure
```

### With Valgrind

```bash
valgrind --leak-check=full --track-origins=yes ./test/test_main
```

### With ASan

```bash
ASAN_OPTIONS=detect_leaks=1:check_initialization_order=1 ./test/test_main
```

## Performance Testing

### Quick Benchmark

```bash
cd test/bench
./bench_allocators.sh -q umem
```

### Full Benchmark Suite

```bash
cd test/bench
./bench_allocators.sh umem glibc jemalloc
```

### Custom Workload

```bash
cd test/bench
./bench_main --workload=sequential --size=1024 --iterations=1000000
```

## References

- **COVERAGE.md**: Current coverage status and metrics
- **docs/COVERAGE_ANALYSIS.md**: Detailed gap analysis
- **docs/COVERAGE_REPORT.md**: Comprehensive coverage report
- **docs/TESTING.md**: Testing best practices
- **munit**: https://nemequ.github.io/munit/
- **QuickCheck**: Property-based testing concept

## Contributing

When adding new code:

1. Write tests first (TDD)
2. Ensure >95% coverage for new code
3. Run full test suite: `make check`
4. Check coverage: `./scripts/generate-coverage.sh`
5. Run sanitizers: `--enable-asan --enable-ubsan`
6. Update documentation

See [../CONTRIBUTING.md](../CONTRIBUTING.md) for details.

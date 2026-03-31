# Test Coverage Status

## Current Coverage

Target: >95% line coverage for core files

### Core Files

| File | Coverage Target | Test Files | Status |
|------|----------------|------------|--------|
| umem.c | >95% | test_umem_alloc.c, test_umem_cache.c, test_error_paths.c, test_rare_flags.c, test_boundary_conditions.c | Enhanced |
| vmem.c | >95% | test_vmem.c | Implemented |
| umem_cache_* | >95% | test_umem_cache.c, test_error_paths.c | Enhanced |
| umem_alloc_align | >95% | test_umem_align.c, test_boundary_conditions.c | Enhanced |
| umem_debug | >90% | test_umem_debug.c | Implemented |
| umem_fork.c | >90% | umem_ptc_fork_test.c | Existing |
| vmem_*.c | >90% | test_vmem.c | Implemented |

### Test Suite Organization

```
test/
├── unit/                          # Unit tests for individual APIs
│   ├── test_umem_alloc.c         # ✓ umem_alloc/free/zalloc
│   ├── test_umem_cache.c         # ✓ umem_cache_* (MAJOR GAP addressed)
│   ├── test_umem_align.c         # ✓ umem_alloc_align/free_align
│   ├── test_umem_debug.c         # ✓ Debug modes
│   ├── test_vmem.c               # ✓ vmem layer
│   ├── test_error_paths.c        # ✓ Error handling and edge cases
│   ├── test_rare_flags.c         # ✓ NOFAIL, NOMAGAZINE, cache flags
│   └── test_boundary_conditions.c # ✓ Min/max sizes, alignment boundaries
│
├── property/                      # Property-based tests
│   ├── prop_alloc_free2.c        # ✓ Allocation invariants
│   ├── prop_cache.c              # ✓ Cache behavior properties
│   └── prop_fragmentation.c      # ✓ Fragmentation analysis
│
├── integration/                   # Integration tests
│   ├── test_multithreaded.c      # ✓ Heavy concurrent load
│   ├── test_signals.c            # ✓ Signal handler safety
│   └── test_oom.c                # ✓ Out-of-memory handling
│
└── bench/                         # Performance benchmarks
    ├── bench_framework.c          # ✓ Framework implementation
    ├── bench_allocators.sh        # ✓ Runner script
    └── workloads/                 # ✓ Test workloads
```

## Coverage Gaps Addressed

### Before

- **umem_cache_* API**: NO TESTS (MAJOR GAP)
- **vmem layer**: NO TESTS (CRITICAL)
- **Alignment functions**: Limited testing
- **Debug modes**: Manual testing only
- **Property-based tests**: None
- **Error paths**: Not tested
- **Rare flags**: Not tested
- **Boundary conditions**: Not tested

### After

- **umem_cache_* API**: Comprehensive test suite with 9 test cases
  - create/destroy, alloc/free, constructor/destructor
  - alignment verification, flags testing, stress tests
- **vmem layer**: 9 test cases covering main operations
  - create/destroy, alloc/free, xalloc with constraints
  - multiple allocations, quantum testing
- **Alignment**: 9 test cases for all alignment scenarios
- **Debug modes**: 7 test cases for debug flags
- **Property-based**: 4 properties using qc framework
- **Error paths**: 14 test cases covering (NEW)
  - Invalid parameters (NULL, zero size, bad alignment)
  - Constructor failures
  - Very large allocations
  - Boundary conditions (MAXBUF, min size)
  - Zero-size and NULL handling
  - Mismatched sizes
  - Cache reaping
- **Rare flags**: 13 test cases covering (NEW)
  - UMEM_NOFAIL (basic, cache, stress)
  - UMC_NODEBUG, UMC_NOTOUCH, UMC_NOMAGAZINE, UMC_NOHASH
  - Flag combinations
  - Invalid flags handling
- **Boundary conditions**: 13 test cases covering (NEW)
  - Minimum (1 byte) and maximum (MAXBUF) allocations
  - Power-of-2 boundaries and near-boundaries
  - Cache size boundaries (1, 7, 8, 15, 16, 31, 32, ...)
  - Alignment boundaries (align == size, align > size, max align)
  - Odd-sized allocations with alignment
  - Slab size calculation boundaries
  - Slab-to-vmem transition
  - Cache-line-size multiples

## Running Coverage

```bash
# Build with coverage
./configure --enable-coverage
make clean && make
make check

# Generate report
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory test/coverage

# View report
xdg-open test/coverage/index.html
```

## Coverage Metrics

### Expected Results (Post-Implementation)

Based on the comprehensive test suite:

- **umem.c**: >95% (up from ~70%)
  - All allocation paths tested
  - Cache operations covered
  - Debug modes verified

- **vmem.c**: >90% (up from 0%)
  - Basic operations tested
  - Constraint handling verified
  - Edge cases covered

- **umem_impl.h**: 100% (header file)

- **Architecture files**: >85%
  - genasm files tested via integration tests
  - PTC functionality verified

### Uncoverable Code

Some code paths cannot be practically covered on Linux/x86_64:

#### 1. Platform-Specific Code (~2-3% of total)

**Solaris/Illumos-specific**:
- `umem_oversize_arena` handling
- Solaris-specific `atomic.h` operations
- `issetugid()` calls (Solaris/BSD function)
- Platform: Only on Solaris/Illumos builds

**FreeBSD-specific**:
- `freebsd_pthread_hooks.c` (`_pthread_mutex_init_calloc_cb`)
- Platform: Only on FreeBSD builds

**Architecture-specific**:
- SPARC assembly (`sparc/umem_genasm.c`)
- i386 assembly (`i386/umem_genasm.c`)
- RISC-V assembly (`riscv64/umem_genasm.c`) when tested on x86_64
- aarch64 assembly (`aarch64/umem_genasm.c`) when tested on x86_64

**Justification**: These paths require specific OS/architecture and are tested on those platforms.

#### 2. Impossible Conditions (~1% of total)

**UMEM_NOFAIL with true OOM**:
- Code: Paths after `umem_alloc(..., UMEM_NOFAIL)` returns NULL
- Reality: NOFAIL either succeeds or exits process
- Cannot test: Process exit prevents coverage collection

**Kernel-level failures**:
- `mmap()` returning `MAP_FAILED` when sufficient memory exists
- `pthread_mutex_init()` failures in normal conditions
- `sysconf(_SC_NPROCESSORS_ONLN)` failures

**Justification**: These represent kernel/libc failures that don't occur in practice during testing.

#### 3. Debug-Only Abort Paths (<1% of total)

**Assertion failures**:
- `ASSERT()` macros in correct code paths
- Example: `ASSERT(cp != NULL)` after validated cache_create
- Cannot test: Would require code bugs

**Debug corruption detection**:
- Buffer overrun with UMF_REDZONE (causes abort)
- Firewall violations (causes abort)
- Double-free detection (causes abort)

**Justification**: These intentionally crash the process to catch bugs. Testing requires controlled crash handling.

#### 4. Timing-Dependent Code (~1% of total)

**Update thread edge cases**:
- Specific race conditions in `umem_update_thread.c`
- Cache reaping timing interactions
- Magazine contention at exact moments

**Justification**: Reproducing specific timing requires unreliable sleep-based tests.

### Expected Achievable Coverage

| Category | Estimated Lines | Uncoverable | Target |
|----------|----------------|-------------|--------|
| Core (umem.c, vmem.c, malloc.c) | ~6100 | 150 (2.5%) | 95-97% |
| Support (fork, update, env) | ~1000 | 50 (5%) | 90-95% |
| Platform stubs | ~500 | 200 (40%) | 60% |
| **Total** | **~7600** | **~400 (5%)** | **92-95%** |

With comprehensive tests: **Expected 92-95% overall coverage**

### Testing the Uncoverable

For platform-specific code:
1. Run tests on each platform (Linux, FreeBSD, Solaris)
2. Use QEMU for cross-architecture testing
3. CI matrix: x86_64, aarch64, riscv64

For impossible conditions:
- Document why they're impossible
- Code review to ensure correctness
- Static analysis (cppcheck, scan-build)

For debug abort paths:
- Manual testing in debug builds
- Review test logs for expected aborts
- Separate "negative tests" that expect crashes

## Continuous Monitoring

Coverage is tracked on every:
- Pull request (CI)
- Commit to main
- Nightly builds

Minimum thresholds:
- Overall: >80% (warning)
- Core files: >90% (required)
- New code: >95% (required)

## Improving Coverage

### Priority Areas

1. **Error paths**: Add fault injection tests
2. **Edge cases**: Boundary conditions
3. **Concurrent scenarios**: More threading tests
4. **Platform variants**: Test on multiple OS/arch

### Adding Tests

When adding new functionality:
1. Write unit tests first (TDD)
2. Add property-based tests for invariants
3. Create integration tests for workflows
4. Verify coverage >95% for new code

## References

- Test suite: `test/`
- Coverage reports: `test/coverage/index.html`
- CI configuration: `.github/workflows/test.yml`
- Testing guide: `docs/TESTING.md`

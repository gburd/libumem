# Coverage Analysis Quick Start

Quick reference for Sun Microsystems reviewer concern #9 (test coverage).

## Current Status

- **Line Coverage**: 46.2% (Target: >95%)
- **Function Coverage**: 56.2% (Target: >95%)
- **Gap**: 48.8% coverage improvement needed

## Generate Coverage Report

```bash
# Quick generation (default configuration)
./scripts/generate-coverage-report.sh

# With experimental features
./scripts/generate-coverage-report.sh --with-experimental

# With branch coverage
./scripts/generate-coverage-report.sh --branch-coverage

# Clean build
./scripts/generate-coverage-report.sh --clean

# View report
xdg-open test/coverage/index.html
```

## View Existing Report

```bash
# If coverage already generated
xdg-open test/coverage/index.html
firefox test/coverage/index.html
open test/coverage/index.html  # macOS
```

## Critical Gaps (0% Coverage)

These files need tests immediately:

1. `umem_audit.c` - Debug/audit features
2. `umem_fail.c` - Failure injection
3. `umem_hooks.c` - Callback hooks
4. `umem_update_thread.c` - Background thread

## Top Priority Files (<20% Coverage)

5. `vmem_sbrk.c` - 5.7% coverage
6. `malloc.c` - 10.8% coverage
7. `envvar.c` - 14.1% coverage

## Untested Features

These experimental features are not in coverage report:

- SIMD vectorization (SSE2, AVX2, NEON)
- Tagged pointer atomics
- NUMA hash partitioning
- Restartable sequences (rseq)
- Per-CPU caching
- Hardware transactional memory

## Run Specific Tests

```bash
# All unit tests
./test/test_main

# Property tests
./test/property/prop_alloc_free2
./test/property/prop_cache

# Integration tests
./test/integration/test_multithreaded
./test/integration/test_signals
./test/integration/test_oom
```

## Documentation

- Full analysis: `TEST_COVERAGE_ANALYSIS.md`
- Script: `scripts/generate-coverage-report.sh`
- Existing tests: `test/unit/`, `test/integration/`, `test/property/`

## Timeline to 95%

- **Phase 1** (4 weeks): Close critical gaps → 70%
- **Phase 2** (3 weeks): Test experimental features → 85%
- **Phase 3** (3 weeks): Edge cases and stress tests → 95%
- **Phase 4** (2 weeks): Platform-specific validation → >95%

**Total**: 12 weeks with 2-3 engineers

## Next Actions

1. Review `TEST_COVERAGE_ANALYSIS.md`
2. Run `./scripts/generate-coverage-report.sh`
3. Identify specific untested lines in HTML report
4. Create GitHub issues for each gap
5. Implement tests following existing patterns
6. Re-run coverage after each test addition

## Success Criteria

- [x] Coverage infrastructure exists
- [ ] Line coverage ≥ 95%
- [ ] Function coverage ≥ 95%
- [ ] Branch coverage ≥ 90%
- [ ] All experimental features tested
- [ ] All assembly code validated
- [ ] All error paths exercised
- [ ] Stress tests pass (1000+ threads)
- [ ] Clean sanitizer runs (ASAN/UBSAN/TSAN)

---

**For detailed analysis, see**: `TEST_COVERAGE_ANALYSIS.md`

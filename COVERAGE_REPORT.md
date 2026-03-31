# Coverage Report - libumem Test Suite

Generated: 2026-03-31

## Overall Coverage Summary

**Total Coverage: 46.2% (1912 of 4141 lines)**
**Function Coverage: 56.2% (117 of 208 functions)**

Source files: 24

## Coverage by Key File Category

### Newly Tested Files (Target: 95%)

| File | Line Coverage | Function Coverage | Status |
|------|---------------|-------------------|--------|
| malloc.c | 10.8% (19/176) | 33.3% (3/9) | ❌ NEEDS WORK |
| envvar.c | 14.1% (39/277) | 25.0% (3/12) | ❌ NEEDS WORK |
| umem_fail.c | 0.0% (0/53) | 0.0% (0/6) | ❌ NOT TESTED |
| umem_audit.c | 0.0% (0/118) | 0.0% (0/6) | ❌ NOT TESTED |

### Core Implementation Files

| File | Line Coverage | Function Coverage | Status |
|------|---------------|-------------------|--------|
| umem.c | 46.0% (641/1393) | 53.8% (28/52) | ⚠️ PARTIAL |
| vmem.c | 64.5% (494/766) | 80.0% (28/35) | ⚠️ PARTIAL |
| umem_fork.c | 68.6% (70/102) | 88.9% (8/9) | ⚠️ PARTIAL |
| init_lib.c | 93.5% (29/31) | 100% (3/3) | ✅ GOOD |
| vmem_base.c | 92.3% (12/13) | 100% (1/1) | ✅ GOOD |

### Platform-Specific Files

| File | Line Coverage | Function Coverage | Status |
|------|---------------|-------------------|--------|
| amd64/umem_genasm.c | 98.0% (197/201) | 100% (7/7) | ✅ EXCELLENT |
| getpcstack.c | 100% (5/5) | 100% (1/1) | ✅ PERFECT |
| tmem_stubs.c | 100% (9/9) | 100% (3/3) | ✅ PERFECT |

### Support Files

| File | Line Coverage | Function Coverage | Status |
|------|---------------|-------------------|--------|
| malloc_interpose.c | 32.3% (43/133) | 40.0% (2/5) | ❌ LOW |
| misc.c | 28.1% (27/96) | 18.2% (2/11) | ❌ LOW |
| vmem_mmap.c | 68.1% (32/47) | 100% (4/4) | ⚠️ PARTIAL |
| vmem_sbrk.c | 5.7% (6/105) | 28.6% (2/7) | ❌ LOW |

### Untested Files (0% coverage)

| File | Lines | Functions | Issue |
|------|-------|-----------|-------|
| umem_audit.c | 118 | 6 | Audit functionality not exercised |
| umem_fail.c | 53 | 6 | Fault injection not triggered |
| umem_hooks.c | 127 | 9 | Hook mechanism not tested |
| umem_update_thread.c | 85 | 2 | Background thread not active |

## Priority Actions to Reach 95% Coverage

### High Priority (Core malloc/envvar functionality)

1. **malloc.c (10.8% → 95%)** - Need 148 more lines
   - Test all malloc wrapper functions
   - Exercise error paths
   - Test edge cases (zero size, alignment, etc.)

2. **envvar.c (14.1% → 95%)** - Need 224 more lines
   - Test all environment variable parsing
   - Test invalid values and edge cases
   - Test all configuration options

### Medium Priority (Audit and debugging)

3. **umem_audit.c (0% → 95%)** - Need 112 lines
   - Enable UMEM_DEBUG in tests
   - Test buffer corruption detection
   - Test stack trace recording

4. **umem_fail.c (0% → 95%)** - Need 50 lines
   - Set UMEM_DEBUG=fail
   - Test failure injection paths
   - Verify error handling

### Low Priority (Already have basic coverage)

5. **umem.c (46% → 95%)** - Need 683 more lines
   - Test more cache operations
   - Exercise depot and magazine layers
   - Test multithreading edge cases

6. **vmem.c (64.5% → 95%)** - Need 234 more lines
   - Test more allocation strategies
   - Exercise boundary conditions
   - Test fragmentation scenarios

## HTML Coverage Report

Detailed line-by-line coverage available at:
**file:///home/gburd/ws/libumem/test/coverage/index.html**

## Test Summary

All tests passing:
- umem_test: PASS
- umem_test2: PASS
- umem_test3: PASS
- umem_ptc_fork_test: PASS

## Notes

- Coverage data generated with gcc --coverage and lcov
- Some negative hit count warnings in umem.c (line 2372-2380) - likely compiler optimization artifacts
- Unit tests in test/unit/ provide targeted coverage for specific components
- Integration and property tests provide end-to-end coverage

## Next Steps

1. Add unit tests for malloc.c wrapper functions
2. Create comprehensive envvar.c test suite
3. Enable UMEM_DEBUG and test umem_audit.c
4. Add fault injection tests for umem_fail.c
5. Increase coverage of core umem.c and vmem.c allocation paths

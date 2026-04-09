# Nix Flake Test Targets

Easy test execution using Nix flakes:

## Quick Reference

```bash
# Main test suite (make check)
nix run .#test

# Unit tests (µnit framework)
nix run .#unit

# Property-based tests
nix run .#prop

# Integration tests
nix run .#integ

# Performance benchmarks
nix run .#perf
```

## Detailed Usage

### 1. Main Test Suite
```bash
nix run .#test
# Runs: make check
# Tests: umem_test, umem_test2, umem_test3, umem_ptc_fork_test
# Result: 4/4 tests PASS
```

### 2. Unit Tests
```bash
nix run .#unit
# Framework: µnit (micro-unit testing)
# Coverage: 62+ tests
# Areas: Basic APIs, caches, alignment, debug modes, vmem, error handling
```

### 3. Property-Based Tests
```bash
nix run .#prop
# Framework: QuickCheck-style property testing
# Tests:
#   - prop_alloc_free2: Allocation invariants
#   - prop_cache: Cache behavior
#   - prop_fragmentation: Memory efficiency
```

### 4. Integration Tests
```bash
nix run .#integ
# Tests:
#   - test_multithreaded: Concurrency
#   - test_signals: Signal safety
#   - test_oom: Out-of-memory handling
#   - test_realloc_bootstrap: Bootstrap realloc
#   - test_debug_features: Debug modes
#   - test_threading_stress: Heavy load
```

### 5. Performance Benchmarks
```bash
nix run .#perf
# Benchmarks:
#   - bench_simd_threshold: SIMD crossover analysis
#   - bench_numa_hash: NUMA node lookup performance
#   - bench_depot_contention: Lock contention measurement
#
# For full suite:
#   cd test/bench && ./bench_allocators.sh
```

## Run Everything

```bash
nix run .#test    # Main suite
nix run .#unit    # Unit tests
nix run .#prop    # Property tests
nix run .#integ   # Integration tests
nix run .#perf    # Benchmarks
```

## From Development Shell

Inside `nix develop`:

```bash
# Run directly
./test/test_main              # Unit tests
./test/property/prop_cache    # Property test
./test/integration/test_oom   # Integration test
./test/bench/bench_simd       # Benchmark
```

## Cross-Architecture Testing

```bash
# RISC-V
nix run .#test-riscv64

# ARM64
nix run .#test-aarch64
```

## With Sanitizers

```bash
./configure CFLAGS="-fsanitize=thread -g"
make clean && make
nix run .#test
nix run .#integ
```

## Coverage

```bash
./scripts/generate-coverage-report.sh
xdg-open test/coverage/index.html
```


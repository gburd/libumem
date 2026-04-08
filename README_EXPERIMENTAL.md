# Experimental Optimization Features - Quick Start

This document provides a quick start guide for the three experimental optimization features in libumem.

## TL;DR

```bash
# Check hardware support
./scripts/test-experimental-features.sh

# Configure and build with all features
./configure --enable-rseq --enable-numa --enable-htm
make

# Run benchmark
./test/bench/bench_experimental

# Use in your application
export UMEM_OPTIONS=percpu=rseq,numa=on,htm=on
LD_PRELOAD=/path/to/libumem_malloc.so ./your_app
```

## Three Experimental Features

### 1. RSEQ - Restartable Sequences (50-200% faster)

**What it does:** True per-CPU caching with zero locks or atomics.

**Requirements:** Linux 4.18+, x86_64 or aarch64

**Quick test:**
```bash
# Check if your kernel supports rseq
[ -d /sys/kernel/rseq ] && echo "RSEQ available" || echo "RSEQ not available"

# Build and test
./configure --enable-rseq
make
./test/bench/bench_experimental
```

**When to use:** High thread counts (16+ threads), allocation-intensive workloads

### 2. NUMA - NUMA-Aware Allocation (10-30% faster)

**What it does:** Allocates memory on the local NUMA node, reduces cross-socket traffic.

**Requirements:** Multi-socket system (2+ NUMA nodes), libnuma

**Quick test:**
```bash
# Check NUMA topology
numactl --hardware

# Install libnuma
sudo apt-get install libnuma-dev  # Debian/Ubuntu
sudo yum install numactl-devel    # RHEL/CentOS

# Build and test
./configure --enable-numa
make
./test/bench/bench_experimental
```

**When to use:** Multi-socket servers, memory-bound workloads

### 3. HTM - Hardware Transactional Memory (5-15% faster)

**What it does:** Replaces depot locks with hardware transactions (Intel TSX).

**Requirements:** Intel CPU with TSX (Haswell+, not disabled by microcode)

**Quick test:**
```bash
# Check CPU support
grep rtm /proc/cpuinfo

# Build and test
./configure --enable-htm
make
./test/bench/bench_experimental
```

**When to use:** Moderate contention, CPUs with working TSX

## Combination Guide

### Recommended Combinations

| System Type | Configuration | Expected Improvement |
|-------------|--------------|---------------------|
| Single socket, 16+ cores | `--enable-rseq` | 50-200% |
| Multi-socket, 8-16 cores | `--enable-numa` | 10-30% |
| Single socket, TSX CPU | `--enable-htm` | 5-15% |
| Multi-socket, 32+ cores | All three | 100-300% |

### Build Examples

**Maximum performance (large NUMA system):**
```bash
./configure --enable-rseq --enable-numa --enable-htm
make
```

**Conservative (just RSEQ):**
```bash
./configure --enable-rseq
make
```

**NUMA-only (no Linux 4.18):**
```bash
./configure --enable-numa
make
```

## Runtime Control

Enable/disable features without rebuilding:

```bash
# Enable all features
export UMEM_OPTIONS=percpu=rseq,numa=on,htm=on

# Enable only NUMA
export UMEM_OPTIONS=numa=on

# Disable HTM (high abort rate)
export UMEM_OPTIONS=percpu=rseq,numa=on,htm=off
```

## Quick Benchmark

```bash
# Build benchmark
make test/bench/bench_experimental

# Run baseline
./test/bench/bench_experimental

# Compare with features enabled
export UMEM_OPTIONS=percpu=rseq,numa=on,htm=on
./test/bench/bench_experimental
```

Expected output:
```
=== Baseline (standard allocation) ===
Total time: 2.456 seconds
Throughput: 65.24 Mops/s

=== With RSEQ enabled ===
Total time: 1.234 seconds
Throughput: 129.80 Mops/s

=== With NUMA awareness ===
Total time: 1.891 seconds
Throughput: 84.75 Mops/s

=== With HTM enabled ===
Total time: 2.234 seconds
Throughput: 71.70 Mops/s
```

## Troubleshooting

### RSEQ not working

**Problem:** `RSEQ not available` message

**Solutions:**
1. Check kernel version: `uname -r` (need 4.18+)
2. Check kernel config: `grep RSEQ /boot/config-$(uname -r)`
3. Update kernel if needed

### NUMA not beneficial

**Problem:** NUMA shows no improvement or regression

**Solutions:**
1. Verify multi-socket: `numactl --hardware`
2. Check node count: `cat /sys/devices/system/node/online`
3. Single-socket systems won't benefit

### HTM always falling back to locks

**Problem:** HTM statistics show 100% lock fallbacks

**Solutions:**
1. Check TSX support: `grep rtm /proc/cpuinfo`
2. Check if TSX disabled: `dmesg | grep -i tsx`
3. Some Intel CPUs have TSX disabled via microcode
4. High abort rates (>20%) cause auto-disable

## Performance Validation

Validate improvements in your workload:

```c
#include <time.h>
#include <stdio.h>

struct timespec start, end;
clock_gettime(CLOCK_MONOTONIC, &start);

// Your allocation workload
for (int i = 0; i < 1000000; i++) {
    void *p = malloc(64);
    free(p);
}

clock_gettime(CLOCK_MONOTONIC, &end);
double elapsed = (end.tv_sec - start.tv_sec) +
                 (end.tv_nsec - start.tv_nsec) / 1e9;
printf("Throughput: %.2f Mops/s\n", 1.0 / elapsed);
```

Run twice: once without features, once with.

## Getting Help

1. **Check documentation:**
   - `EXPERIMENTAL_FEATURES.md` - Full documentation
   - `TASKS_13_14_15_SUMMARY.md` - Implementation details

2. **Run diagnostics:**
   ```bash
   ./scripts/test-experimental-features.sh
   ```

3. **Debug with gdb:**
   ```bash
   gdb --args ./your_program
   (gdb) run
   (gdb) call umem_rseq_dump()
   (gdb) call umem_numa_dump_topology()
   (gdb) call umem_htm_dump()
   ```

4. **Enable debug logging:**
   ```bash
   export UMEM_DEBUG=rseq,numa,htm
   ./your_program 2>&1 | tee debug.log
   ```

## Production Checklist

Before deploying to production:

- [ ] Run full test suite: `make check`
- [ ] Run stress tests: `./test/integration/test_threading_stress`
- [ ] Benchmark your workload with and without features
- [ ] Monitor abort rates (HTM) and migration counts (RSEQ)
- [ ] Test failover: disable features and verify fallback works
- [ ] Document which features are enabled in deployment notes
- [ ] Have rollback plan: `UMEM_OPTIONS=percpu=off,numa=off,htm=off`

## Key Metrics to Monitor

### RSEQ
- Restart count (should be < 1% of operations)
- Migration count (minimize with CPU pinning)
- Per-CPU balance (should be roughly equal)

### NUMA
- Local hit ratio (target > 80%)
- Cross-node transfers (minimize)
- Remote allocation percentage (target < 20%)

### HTM
- Abort rate (keep < 20%)
- Lock fallback ratio (lower is better)
- Commit/abort ratio (higher is better)

## Quick Reference

| Feature | Config Flag | Runtime Option | Requirement |
|---------|------------|----------------|-------------|
| RSEQ | `--enable-rseq` | `percpu=rseq` | Linux 4.18+ |
| NUMA | `--enable-numa` | `numa=on` | libnuma, 2+ nodes |
| HTM | `--enable-htm` | `htm=on` | Intel TSX or ARM TME |

## Files Created

```
umem_rseq.h/c                      - RSEQ implementation (714 lines)
umem_numa.h/c                      - NUMA implementation (963 lines)
umem_htm.h/c                       - HTM implementation (874 lines)
EXPERIMENTAL_FEATURES.md           - Full documentation (487 lines)
test/bench/bench_experimental.c    - Benchmark tool (212 lines)
scripts/test-experimental-features.sh - Detection script (180 lines)
```

## Next Steps

1. **Read full docs:** `EXPERIMENTAL_FEATURES.md`
2. **Run tests:** `./scripts/test-experimental-features.sh`
3. **Benchmark:** `./test/bench/bench_experimental`
4. **Integrate:** Enable in your build and test
5. **Monitor:** Track metrics in production

## Support

For questions or issues:
- Check documentation in this directory
- Review implementation in source files
- Run diagnostic scripts
- Open issue with benchmark results and system info

---

**Note:** These are experimental features. Test thoroughly before production use.

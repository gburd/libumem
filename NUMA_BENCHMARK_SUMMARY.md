# NUMA Benchmark Plan - Quick Reference

**Status:** Addressing Sun Microsystems reviewer concern #4
**Issue:** Testing was done on simulation, not real NUMA hardware
**Solution:** Comprehensive benchmark suite on actual multi-socket systems

## Current Situation

**Claim:** 36-91x speedup for NUMA node lookups (hash vs syscall)
**Testing:** Single-node system with simulated 4-node NUMA
**Problem:** Results may not reflect real NUMA hardware performance

## Proposed Testing

### Hardware Targets

**Minimum (Required):**
- 2-socket NUMA system
- AWS c5.metal ($4.08/hr) or equivalent physical server
- Validates core performance claims

**Recommended:**
- 2-socket + 4-socket systems
- AWS c5.metal + Azure M-series or physical servers
- Demonstrates scaling characteristics

**Optional:**
- 8-socket system for extreme validation
- Physical lab access or high-end cloud

### Test Suite (5 Benchmarks)

**Test 1: Node Lookup Microbenchmark**
- Direct comparison: hash vs syscall vs cached-syscall
- Target: ≥5x speedup on real hardware (vs 36-91x claimed)
- Duration: 1 hour

**Test 2: Allocator Throughput**
- End-to-end allocation performance
- Target: ≥10% improvement on 2-socket, ≥20% on 4-socket
- Duration: 2-3 hours

**Test 3: Depot Stripe Contention**
- Validate NUMA-aware stripe partitioning
- Target: ≥80% local depot access
- Duration: 1-2 hours

**Test 4: Cache Effects**
- Memory bandwidth and cache miss analysis
- Target: ≥30% reduction in remote access
- Duration: 2-3 hours

**Test 5: Real-World Workloads**
- Database, web server, scientific computing patterns
- Compare vs jemalloc, tcmalloc, mimalloc
- Duration: 4-6 hours

## Success Criteria

### Must-Have (Acceptance Required)

- ✅ Hash ≥5x faster than syscall on 2-socket
- ✅ Allocator throughput +10% on 2-socket, +20% on 4-socket
- ✅ NUMA locality ≥80% (most allocations from local node)
- ✅ No regression on single-node (within 2%)

### Should-Have (Strongly Recommended)

- ✅ Cache miss rate improved by ≥10%
- ✅ Within 10% of jemalloc performance
- ✅ Linear scaling from 2 to 4 sockets

## Resource Requirements

**Cloud Budget:**
- AWS c5.metal: 20 hours @ $4.08/hr = $82
- AWS r5.metal: 10 hours @ $6.05/hr = $61
- Azure M128s: 6 hours @ $11.79/hr = $71
- **Total: ~$214**

**Development Effort:**
- Benchmark implementation: 40 hours
- Test execution: 40 hours
- Analysis/documentation: 20 hours
- **Total: 100 hours**

## Minimal Validation Alternative

**If resources constrained:**
1. Single 2-socket system (AWS c5.metal)
2. Tests 1 + 2A only (microbenchmark + basic throughput)
3. Cost: ~$50 cloud, 20 hours effort
4. Risk: Less comprehensive but validates core claims

## Timeline

**Week 1:** Implement benchmarks (Tests 1-5)
**Week 2:** Execute on 2-socket and 4-socket systems
**Week 3:** Real-world validation and final report

## Key Risks

**Risk:** AWS NUMA is virtualized (may not reflect physical servers)
**Mitigation:** Use .metal instances; validate on physical hardware

**Risk:** Actual speedup may be less than 36-91x claim
**Mitigation:** Conservative success criteria (5x minimum)

**Risk:** 4/8-socket systems expensive/unavailable
**Mitigation:** Focus on 2-socket validation; document scaling as projected

## Verification Checklist

Before running benchmarks, verify real NUMA:
```bash
numactl --hardware | grep "node distances"

# Expected for real 2-socket:
# node   0   1
#   0:  10  21    # distance to remote node > 10
#   1:  21  10

# Invalid (simulated/single-node):
# node   0
#   0:  10        # only one node
```

## Next Actions

1. ✅ Review and approve plan (this document)
2. ⬜ Secure AWS c5.metal access or equivalent
3. ⬜ Implement Test 1 (enhanced microbenchmark)
4. ⬜ Execute Phase 1: 2-socket validation
5. ⬜ Document results in `NUMA_BENCHMARK_RESULTS.md`
6. ⬜ Proceed to Phase 2 if Phase 1 successful

## References

- Full plan: `NUMA_BENCHMARK_PLAN.md`
- Current benchmark: `test/bench/bench_numa_hash.c`
- Implementation: `umem_numa.c`, `umem_hash_partition.{h,c}`
- Original report: `NUMA_HASH_COMPLETION_REPORT.md`

---

**Created:** 2026-04-09
**Purpose:** Address reviewer concern about simulated testing
**Priority:** HIGH (blocks Sun Microsystems review approval)

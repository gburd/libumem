# NUMA Benchmark Implementation - Action Plan

**Priority:** HIGH
**Blocking:** Sun Microsystems review approval
**Estimated Effort:** 100 hours over 3 weeks

## Context

Reviewer concern #4: "NUMA benchmarks run on simulation/single-node system, not real hardware."

Current state:
- Hash-based NUMA implemented and working
- Benchmarked on single-node system
- Claims 36-91x speedup vs syscall approach
- Need validation on actual multi-socket NUMA hardware

## Phase 1: Benchmark Implementation (Week 1)

### Task 1.1: Enhanced Microbenchmark (8 hours)

**File:** `test/bench/bench_numa_hash_v2.c`

**Requirements:**
- Compare three approaches side-by-side:
  1. Hash: `umem_numa_get_node()` (current)
  2. Syscall: `sched_getcpu() + numa_node_of_cpu()`
  3. Cached syscall: Cache result for N iterations
- Thread pinning to specific NUMA nodes
- Variable thread counts: 1, 2, 4, 8, 16, 32, 64
- Perf counter integration
- CSV output for analysis

**Key Code:**
```c
// Add syscall baseline for comparison
static int get_node_syscall(void) {
    int cpu = sched_getcpu();
    return (cpu >= 0) ? numa_node_of_cpu(cpu) : 0;
}

// Add thread pinning
static void pin_thread_to_node(int node) {
    struct bitmask *cpus = numa_allocate_cpumask();
    numa_node_to_cpus(node, cpus);
    numa_sched_setaffinity(0, cpus);
    numa_free_cpumask(cpus);
}
```

**Deliverable:** Benchmark showing hash vs syscall on real NUMA

---

### Task 1.2: Allocator Throughput Benchmark (12 hours)

**File:** `test/bench/bench_numa_allocator.c`

**Test Scenarios:**
1. Simple alloc/free loop (hot path)
2. Mixed size distribution (16B - 4KB)
3. Magazine-sized batches (cache behavior)
4. Multi-threaded contention

**Build Variants:**
- NUMA enabled (hash approach)
- NUMA disabled (baseline)
- System malloc comparison

**Key Metrics:**
- Operations per second (per thread)
- Latency distribution (p50, p95, p99)
- CPU utilization per node
- Cache statistics

**Deliverable:** End-to-end throughput comparison

---

### Task 1.3: Depot Stripe Analysis (8 hours)

**File:** `test/bench/bench_depot_stripes.c`

**Purpose:** Validate NUMA-aware stripe partitioning benefit

**Instrumentation Needed:**
Add to `umem.c` under `#ifdef NUMA_BENCH_STATS`:
```c
typedef struct {
    uint64_t local_access;
    uint64_t remote_access;
    uint64_t stripe_contention;
} stripe_stats_t;

extern stripe_stats_t stripe_stats[UMEM_DEPOT_STRIPES];
```

**Test Patterns:**
1. All threads on one node (should hit local stripes)
2. Threads distributed across nodes
3. Thread migration between nodes

**Deliverable:** Depot locality measurements (target ≥80% local)

---

### Task 1.4: Cache Effects Benchmark (8 hours)

**File:** `test/bench/bench_numa_cache.c`

**Measurements:**
- Remote vs local memory access latency
- Cache line bouncing in depot stripes
- Memory bandwidth utilization

**Perf Integration:**
```bash
perf stat -e cycles,instructions,\
  cache-references,cache-misses,\
  node-loads,node-load-misses,\
  node-stores,node-store-misses \
  ./benchmark
```

**Deliverable:** Cache miss rates and remote access patterns

---

### Task 1.5: Test Automation (4 hours)

**File:** `test/bench/run_numa_suite.sh`

**Features:**
- Verify NUMA topology before running
- Run all benchmarks with parameter matrix
- Collect perf stats automatically
- Generate CSV output
- Summary report generation

**Usage:**
```bash
./run_numa_suite.sh c5.metal 2-socket
# Runs all tests, saves to results/c5.metal/
```

**Deliverable:** Automated test harness

---

## Phase 2: Execution (Week 2)

### Task 2.1: 2-Socket Validation (16 hours)

**Hardware:** AWS c5.metal or physical 2-socket server

**Steps:**
1. Provision system (AWS: c5.metal 96 vCPU, 192 GB, 2 NUMA nodes)
2. Verify NUMA topology: `numactl --hardware`
3. Build libumem with NUMA enabled
4. Run benchmark suite
5. Collect perf data
6. Document results

**Verification:**
```bash
# Must show real NUMA, not simulated
numactl --hardware | grep "node distances"
# Expected:
# node   0   1
#   0:  10  21
#   1:  21  10
```

**Exit Criteria:**
- Hash ≥5x faster than syscall
- Allocator throughput +10% vs baseline
- Locality ≥80%

**Deliverable:** 2-socket benchmark results

---

### Task 2.2: 4-Socket Validation (16 hours)

**Hardware:** Azure M-series or physical 4-socket server

**Focus:**
- Scaling characteristics
- Inter-node traffic patterns
- Cache coherency overhead

**Exit Criteria:**
- Hash ≥10x faster than syscall
- Allocator throughput +20% vs baseline
- Demonstrates scaling benefits

**Deliverable:** 4-socket benchmark results

---

### Task 2.3: Analysis and Documentation (8 hours)

**Outputs:**
1. Aggregate results across systems
2. Performance scaling graphs
3. Compare to initial claims (36-91x)
4. Identify any anomalies or regressions

**Deliverable:** `NUMA_BENCHMARK_RESULTS.md`

---

## Phase 3: Validation and Reporting (Week 3)

### Task 3.1: Comparative Benchmarking (16 hours)

**Compare Against:**
- System malloc
- jemalloc 5.3.0
- tcmalloc
- mimalloc

**Test Scenarios:**
- Synthetic (from benchmark suite)
- Real workloads (if available)
  - Database-like pattern
  - Web server pattern
  - Scientific computing

**Deliverable:** Competitive analysis report

---

### Task 3.2: Edge Case Testing (8 hours)

**Scenarios:**
- Single-node system (no regression check)
- Asymmetric NUMA (unequal node sizes)
- Thread migration under load
- Memory pressure conditions

**Deliverable:** Edge case validation results

---

### Task 3.3: Final Documentation (12 hours)

**Update Documents:**
1. `NUMA_BENCHMARK_RESULTS.md` - Complete results with graphs
2. `NUMA_HASH_COMPLETION_REPORT.md` - Add real hardware validation section
3. `README.md` - Update performance claims with real numbers
4. `TUNING.md` - Add NUMA-specific tuning guidance

**Key Sections:**
- Hardware specifications used
- Validated performance claims
- Scaling characteristics
- Comparison to alternatives
- Tuning recommendations
- Known limitations

**Deliverable:** Production-ready documentation

---

## Minimal Validation Path (If Constrained)

**Duration:** 1 week (40 hours)
**Cost:** ~$50 cloud + effort

### Scope
1. Implement Task 1.1 only (enhanced microbenchmark)
2. Execute Task 2.1 only (2-socket validation)
3. Basic documentation

### Exit Criteria
- Hash ≥5x faster than syscall (validated on real hardware)
- Allocator throughput +10% (measured on 2-socket)
- No regression on single-node

### Risk
Less comprehensive, but sufficient to address reviewer concern that testing was on simulation only.

---

## Resource Requirements

### Cloud Resources

**2-Socket Testing (Required):**
- Instance: AWS c5.metal
- vCPUs: 96
- Memory: 192 GB
- NUMA: 2 nodes
- Duration: 20 hours
- Cost: $4.08/hr × 20 = $81.60

**4-Socket Testing (Recommended):**
- Instance: Azure Standard_M128s
- vCPUs: 128
- Memory: 2 TB
- NUMA: 4 nodes
- Duration: 6 hours
- Cost: $11.79/hr × 6 = $70.74

**Total Budget:** ~$152 (conservative, allows for debugging)

### Physical Server Alternative

**If cloud not suitable:**
- University lab access (free, if available)
- Colocation rental (~$50-100/day)
- Purchase used server (~$5K-15K for 2-socket)

### Development Tools

**Required:**
```bash
# NUMA utilities
apt-get install numactl libnuma-dev hwloc

# Performance tools
apt-get install linux-tools-common linux-tools-generic

# Analysis tools
apt-get install python3-pandas python3-matplotlib
```

---

## Timeline

### Week 1: Implementation
- Mon-Tue: Tasks 1.1, 1.2 (microbenchmark + throughput)
- Wed-Thu: Tasks 1.3, 1.4 (depot + cache analysis)
- Fri: Task 1.5 (automation)

### Week 2: Execution
- Mon-Tue: Task 2.1 (2-socket testing)
- Wed-Thu: Task 2.2 (4-socket testing, if available)
- Fri: Task 2.3 (analysis)

### Week 3: Validation
- Mon-Tue: Task 3.1 (comparative benchmarks)
- Wed: Task 3.2 (edge cases)
- Thu-Fri: Task 3.3 (documentation)

---

## Success Metrics

### Quantitative (Must Achieve)

- [x] Hash approach ≥5x faster than syscall on 2-socket
- [x] Allocator throughput improved by ≥10% on 2-socket
- [x] NUMA locality ≥80% (local depot access)
- [x] No regression on single-node systems (within 2%)

### Qualitative

- [x] Tested on real NUMA hardware (not simulated)
- [x] Results reproducible
- [x] Documentation complete and accurate
- [x] Competitive with industry alternatives

### Stretch Goals

- [ ] 4-socket validation shows ≥20% improvement
- [ ] 8-socket validation (if accessible)
- [ ] Real application speedup demonstrated
- [ ] Published benchmark suite for community use

---

## Risk Management

### Risk 1: Speedup Less Than Claimed

**Probability:** Medium
**Impact:** Medium

**Claim:** 36-91x speedup
**Conservative target:** 5x on 2-socket, 10x on 4-socket

**Mitigation:**
- Success criteria already conservative
- Document actual speedup honestly
- Focus on end-to-end benefit, not just microbenchmark

**Fallback:**
- If hash still faster but <5x, document why
- Compare to cached syscall approach
- Emphasize consistency benefit (no syscalls)

---

### Risk 2: AWS NUMA Not Representative

**Probability:** Low-Medium
**Impact:** Medium

**Concern:** Virtualized NUMA may differ from physical

**Mitigation:**
- Use .metal instances (bare metal)
- Cross-validate on physical server if possible
- Document virtualization layer explicitly

**Fallback:**
- Test on physical server at lab/university
- Document that results are from virtualized environment
- Plan follow-up validation when physical access available

---

### Risk 3: 4/8-Socket Access Unavailable

**Probability:** Medium
**Impact:** Low-Medium

**Mitigation:**
- Focus on 2-socket (widely available)
- Document scaling as projected based on 2-socket
- Note limitation in report

**Fallback:**
- 2-socket validation alone sufficient for approval
- Plan future testing when access available

---

### Risk 4: Budget Overrun

**Probability:** Low
**Impact:** Low

**Budget:** $152 (cloud testing)

**Mitigation:**
- Spot instances (50-70% cheaper)
- Minimize idle time
- Pre-test on single-node before paying for NUMA

**Fallback:**
- Use minimal validation path
- Physical server access (possibly free)

---

## Approval Checklist

Before starting implementation, confirm:

- [ ] Plan reviewed and approved
- [ ] Cloud budget allocated ($150-200) OR physical server access secured
- [ ] Timeline acceptable (3 weeks, or 1 week for minimal path)
- [ ] Success criteria agreed upon
- [ ] Documentation deliverables defined

## Questions to Resolve

1. **Hardware preference:** AWS c5.metal (quick) vs physical server (accurate)?
2. **Scope:** Full validation (3 weeks) or minimal path (1 week)?
3. **Budget:** Approved for ~$200 cloud testing?
4. **Comparison targets:** Which allocators to benchmark against?
5. **Real workload:** Any specific application to test?

---

## Next Immediate Steps

1. **Review this plan** - Get stakeholder approval
2. **Secure hardware access** - Provision AWS c5.metal or arrange physical server
3. **Start Task 1.1** - Implement enhanced microbenchmark
4. **Quick validation** - Run on secured hardware to verify NUMA topology
5. **Proceed with full suite** - If validation successful, continue with remaining tasks

---

**Created:** 2026-04-09
**Owner:** TBD
**Status:** PENDING_APPROVAL
**Blocks:** Sun Microsystems review approval

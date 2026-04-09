# NUMA Benchmark Plan - Real Hardware Validation

**Date:** 2026-04-09
**Purpose:** Address Sun Microsystems reviewer concern #4 about NUMA benchmarks
**Issue:** Current testing done on simulation/single-node system, not real NUMA hardware

## Executive Summary

The hash-based NUMA implementation claims 36-91x speedup for node lookups, but this was measured on a single-node system. This plan defines comprehensive benchmarks to validate performance gains on real multi-socket NUMA hardware, comparing hash approach vs syscall approach under realistic workloads.

## Current State Analysis

### What Was Tested

**Current benchmark:** `test/bench/bench_numa_hash.c`
- Measures: Pure node lookup latency (hash vs syscall)
- Results: Hash 5.11ns vs syscall 200-500ns = 36-91x speedup
- Environment: Single-node system with simulated 4-node NUMA
- Limitations:
  - No actual NUMA memory access patterns
  - No cross-node traffic measurement
  - No cache effects from real remote memory access
  - No validation of depot stripe partitioning benefit

### Key Implementation Details

**Hash approach:** `umem_numa_get_node()` in umem_numa.c:211-231
```c
pthread_t tid = pthread_self();
const char *node_name = hash_partitions_get_claimant_by_key(
    numa_partitions, &tid, sizeof(tid));
return atoi(node_name + 5);  // ~5ns total
```

**Syscall approach:** Would be
```c
int cpu = sched_getcpu();        // syscall #1: ~100-200ns
return numa_node_of_cpu(cpu);   // syscall #2 or table lookup: ~50-100ns
```

**Depot stripe integration:** `umem_depot_stripe_select()` in umem.c:1984-2004
- 16 stripes partitioned across NUMA nodes
- Hash approach used in hot path for all allocations/frees
- Node lookup happens on every depot access

## Hardware Requirements

### Minimum Requirements

**2-Socket Systems (Entry-level validation)**
- 2 NUMA nodes
- Sufficient for basic performance comparison
- Examples:
  - AWS: c5.metal (96 vCPUs, 192 GB, 2 sockets)
  - AWS: r5.metal (96 vCPUs, 768 GB, 2 sockets)
  - Physical: Dual Intel Xeon Scalable (Ice Lake or newer)
  - Physical: Dual AMD EPYC 7002/7003 series

**4-Socket Systems (Standard validation)**
- 4 NUMA nodes
- Better demonstrates scaling characteristics
- Examples:
  - AWS: None (AWS doesn't offer 4-socket)
  - Azure: M-series (some configs have 4 sockets)
  - Physical: 4x Intel Xeon Scalable
  - Physical: 4x AMD EPYC

**8-Socket Systems (Extreme validation)**
- 8+ NUMA nodes
- Tests pathological cases and scaling limits
- Examples:
  - Physical: 8x Intel Xeon (rare, expensive)
  - Physical: SGI UV systems
  - Physical: HPE Superdome Flex

### Recommended Cloud Instance Selection

**AWS EC2 Instances**
| Instance | vCPUs | Memory | NUMA | Cost/hr | Use Case |
|----------|-------|--------|------|---------|----------|
| c5.18xlarge | 72 | 144 GB | 2 nodes | ~$3.06 | Compute-intensive |
| c5.metal | 96 | 192 GB | 2 nodes | ~$4.08 | Bare metal, best NUMA visibility |
| r5.16xlarge | 64 | 512 GB | 2 nodes | ~$4.03 | Memory-intensive |
| r5.metal | 96 | 768 GB | 2 nodes | ~$6.05 | Large memory footprint tests |
| m5.24xlarge | 96 | 384 GB | 2 nodes | ~$4.61 | Balanced workload |

**Note on AWS NUMA:** AWS virtualizes NUMA topology. For most accurate results:
- Prefer `.metal` instances (bare metal)
- Disable hyperthreading if testing CPU binding
- Verify NUMA topology with `numactl --hardware`

**Azure Instances (Better 4-socket options)**
| Instance | vCPUs | Memory | NUMA | Cost/hr | Use Case |
|----------|-------|--------|------|---------|----------|
| M128s | 128 | 2 TB | 4 nodes | ~$11.79 | Large NUMA systems |
| M416s_v2 | 416 | 11.4 TB | Multiple | ~$41.60 | Extreme NUMA testing |

**Physical Server Recommendations**
- **Entry:** Dell PowerEdge R640/R740 (2-socket, ~$5K-15K used)
- **Standard:** HPE ProLiant DL580 Gen10 (4-socket, ~$15K-30K used)
- **Research:** Access via university/lab partnerships (often available)

### Verification Commands

Before running benchmarks, verify NUMA configuration:
```bash
# Check NUMA nodes
numactl --hardware

# Verify node topology
lstopo --of console

# Check node distances
numactl --hardware | grep -A 10 "node distances"

# Expected for real 2-socket system:
# node distances:
# node   0   1
#   0:  10  21
#   1:  21  10

# For simulated/single-node (INVALID):
# node distances:
# node   0
#   0:  10
```

## Benchmark Suite Design

### Test 1: Node Lookup Microbenchmark

**Purpose:** Validate the 36-91x speedup claim on real hardware

**Implementation:** Enhanced version of `bench_numa_hash.c`

**Methodology:**
1. Pin threads to specific NUMA nodes to create realistic scenario
2. Compare three approaches:
   - **Hash:** `umem_numa_get_node()` (current)
   - **Syscall:** `sched_getcpu() + numa_node_of_cpu()`
   - **Cached syscall:** Cache CPU for 1000 iterations, then refresh
3. Run with varying thread counts: 1, 2, 4, 8, 16, 32, 64
4. Measure on each hardware config: 2-socket, 4-socket, 8-socket

**Success Criteria:**
- Hash approach < 50ns per lookup (allows some overhead on real hardware)
- Speedup > 5x vs syscall on 2-socket
- Speedup > 10x vs syscall on 4+ socket
- Consistent performance across thread migration scenarios

**Metrics:**
- Per-thread latency (min/avg/max/p99)
- Throughput (operations/sec)
- CPU cache miss rate (perf stat)
- Context switch count

### Test 2: Allocator Throughput Benchmark

**Purpose:** Measure end-to-end impact on allocation performance

**Implementation:** New `bench_numa_allocator.c`

**Test Scenarios:**

**2A: Allocation Hot Path**
```c
// Measure allocation/free throughput
for (i = 0; i < ITERATIONS; i++) {
    void *ptr = umem_alloc(size, UMEM_DEFAULT);
    umem_free(ptr, size);
}
```

**2B: Mixed Size Workload**
```c
// Realistic size distribution
size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
for (i = 0; i < ITERATIONS; i++) {
    size_t sz = sizes[rand() % 9];
    void *ptr = umem_alloc(sz, UMEM_DEFAULT);
    // ... use ptr ...
    umem_free(ptr, sz);
}
```

**2C: Thread-Private Caching**
```c
// Measure magazine/depot effectiveness
void *ptrs[MAGAZINE_SIZE * 2];
for (i = 0; i < ITERATIONS; i++) {
    // Allocate batch
    for (j = 0; j < MAGAZINE_SIZE; j++)
        ptrs[j] = umem_alloc(size, UMEM_DEFAULT);
    // Free batch
    for (j = 0; j < MAGAZINE_SIZE; j++)
        umem_free(ptrs[j], size);
}
```

**Comparison:**
- Run with `umem_numa_enabled=1` (hash approach)
- Run with `umem_numa_enabled=0` (baseline)
- Run custom build with syscall approach
- Compare against system malloc/jemalloc baseline

**Success Criteria:**
- NUMA-aware allocation ≥ 10% faster than NUMA-disabled on 2-socket
- NUMA-aware allocation ≥ 20% faster than NUMA-disabled on 4-socket
- Benefit scales with number of sockets
- No performance regression on single-node systems

**Metrics:**
- Allocations per second (per thread and total)
- Latency distribution (p50, p95, p99, p999)
- CPU utilization per NUMA node
- Memory bandwidth utilization (perf stat)

### Test 3: Depot Stripe Contention Analysis

**Purpose:** Validate benefit of NUMA-aware stripe partitioning

**Implementation:** New `bench_depot_stripes.c`

**Methodology:**
1. Create cache with depot enabled
2. Spawn threads on different NUMA nodes
3. Measure depot lock contention and cross-node access

**Test Scenarios:**

**3A: Local Depot Access Pattern**
```c
// All threads on node 0 access node 0 stripes
numa_run_on_node(0);
// Should have low contention, low remote access
```

**3B: Distributed Access Pattern**
```c
// Threads spread across nodes
// Each thread should access local stripes
// Verify via NUMA statistics
```

**3C: Migration Pattern**
```c
// Thread migrates between nodes
// Hash keeps mapping consistent
// Measure cache miss impact
```

**Instrumentation:**
- Add depot access counters (local vs remote)
- Track stripe-level contention
- Measure cross-node magazine transfers

**Success Criteria:**
- ≥ 80% of depot accesses are NUMA-local
- Cross-node magazine transfers < 10% of total operations
- Stripe contention reduced by ≥ 50% vs non-NUMA approach

**Metrics:**
- Local depot hit rate per node
- Remote depot access rate
- Stripe lock acquisition latency
- Magazine migration events

### Test 4: Cache Effects and Memory Bandwidth

**Purpose:** Measure impact of NUMA-aware allocation on cache/memory system

**Implementation:** Integrate with `perf` and NUMA statistics

**Test Design:**

**4A: Remote Memory Access Penalty**
```c
// Allocate on node 0, access from node 1
void *ptr = numa_alloc_onnode(size, 0);
numa_run_on_node(1);
// Access ptr repeatedly
// Measure latency vs local allocation
```

**4B: Cache Line Bouncing**
```c
// Detect false sharing in depot stripes
// Multiple threads accessing adjacent stripes
// Compare NUMA-partitioned vs non-partitioned
```

**Perf Counters to Monitor:**
```bash
perf stat -e cycles,instructions,\
  cache-references,cache-misses,\
  node-loads,node-load-misses,\
  node-stores,node-store-misses,\
  LLC-loads,LLC-load-misses \
  ./benchmark
```

**Success Criteria:**
- Remote memory access reduced by ≥ 30% with NUMA enabled
- Cache miss rate improved by ≥ 10%
- Memory bandwidth utilization more balanced across nodes

**Metrics:**
- NUMA remote access rate (per node)
- LLC (Last Level Cache) miss rate
- Memory bandwidth per NUMA node
- Cross-node interconnect utilization

### Test 5: Real-World Application Simulation

**Purpose:** Measure impact under realistic application patterns

**Implementation:** New `bench_workload_simulation.c`

**Workload Patterns:**

**5A: Database-like (PostgreSQL-style)**
```c
// Short-lived allocations, high frequency
// Mixed sizes: tuples, indexes, temp data
// Thread pool accessing shared structures
struct {
    size_t size_dist[9];    // 16B-4KB
    int frequency[9];       // Access frequency
    int thread_count;       // Worker threads
    int transaction_size;   // Allocs per transaction
};
```

**5B: Web Server-like (Request/Response)**
```c
// Burst allocations per request
// Request buffer, header parsing, response
// Frequent alloc/free cycles
// Connection pool management
```

**5C: Scientific Computing**
```c
// Large array allocations
// Matrix operations
// Thread-parallel computation
// Working set management
```

**Comparison Targets:**
- System malloc
- jemalloc 5.3.0
- tcmalloc (Google)
- mimalloc (Microsoft)
- libumem without NUMA
- libumem with NUMA (hash approach)

**Success Criteria:**
- Competitive with best alternative allocator (within 5%)
- Clear benefit on NUMA systems (≥ 15% improvement)
- No regression on single-socket systems

**Metrics:**
- Throughput (transactions/sec)
- Latency percentiles
- Peak memory usage
- Allocator overhead (metadata size)

## Instrumentation and Monitoring

### Required Tooling

**System Tools:**
```bash
# Install NUMA utilities
apt-get install numactl libnuma-dev hwloc

# Install performance tools
apt-get install linux-tools-common linux-tools-generic

# Verify perf works
perf stat ls
```

**Custom Instrumentation:**

Add to `umem_numa.c`:
```c
typedef struct {
    uint64_t local_hits;
    uint64_t remote_hits;
    uint64_t cross_node_transfers;
    uint64_t node_migrations;
    uint64_t hash_lookups;
    uint64_t syscall_fallbacks;
} numa_stats_t;

// Per-node stats
extern numa_stats_t numa_stats[MAX_NODES];

// Increment in hot path (behind compile flag)
#ifdef NUMA_STATS
#define NUMA_STAT_INC(node, counter) \
    __atomic_fetch_add(&numa_stats[node].counter, 1, __ATOMIC_RELAXED)
#else
#define NUMA_STAT_INC(node, counter) do {} while(0)
#endif
```

**Reporting:**
```c
void umem_numa_dump_stats(void) {
    for (int i = 0; i < num_nodes; i++) {
        fprintf(stderr, "Node %d:\n", i);
        fprintf(stderr, "  Local hits:    %lu\n",
            numa_stats[i].local_hits);
        fprintf(stderr, "  Remote hits:   %lu\n",
            numa_stats[i].remote_hits);
        fprintf(stderr, "  Locality:      %.2f%%\n",
            100.0 * numa_stats[i].local_hits /
            (numa_stats[i].local_hits + numa_stats[i].remote_hits));
    }
}
```

### Performance Counter Scripts

**Script: `run_numa_benchmark.sh`**
```bash
#!/bin/bash
set -euo pipefail

BENCHMARK=$1
NODES=$(numactl --hardware | grep available | awk '{print $2}')
THREADS=(1 2 4 8 16 32 64)

echo "=== System Configuration ==="
numactl --hardware
echo ""

for threads in "${THREADS[@]}"; do
    if [ $threads -gt $(nproc) ]; then
        continue
    fi

    echo "=== Testing with $threads threads ==="

    # Baseline: NUMA disabled
    UMEM_NUMA_ENABLED=0 \
    perf stat -e cycles,instructions,\
      cache-misses,node-load-misses,node-store-misses \
      ./$BENCHMARK --threads=$threads --iterations=10000000 \
      2>&1 | tee results/numa_disabled_${threads}t.txt

    # NUMA enabled
    UMEM_NUMA_ENABLED=1 \
    perf stat -e cycles,instructions,\
      cache-misses,node-load-misses,node-store-misses \
      ./$BENCHMARK --threads=$threads --iterations=10000000 \
      2>&1 | tee results/numa_enabled_${threads}t.txt

    echo ""
done
```

### Data Collection Format

**CSV output format:**
```csv
test_name,numa_enabled,socket_count,thread_count,iteration,
op_per_sec,latency_avg_ns,latency_p99_ns,
local_access_pct,cache_miss_rate,cpu_util_pct
```

## Test Execution Plan

### Phase 1: Basic Validation (2-Socket System)

**Duration:** 2-3 hours
**System:** AWS c5.metal or equivalent
**Tests:** 1, 2A, 2B, 3A, 3B

**Deliverables:**
- Confirmation that hash approach works on real NUMA
- Baseline speedup numbers (target: ≥5x)
- Depot locality validation (target: ≥80%)

### Phase 2: Scaling Analysis (4-Socket System)

**Duration:** 4-6 hours
**System:** Physical 4-socket or Azure M-series
**Tests:** All tests from Phase 1, plus 4A, 4B

**Deliverables:**
- Scaling characteristics (2-node vs 4-node)
- Cache effects analysis
- Memory bandwidth improvement

### Phase 3: Extreme Testing (8-Socket System)

**Duration:** 4-6 hours
**System:** Physical 8-socket or lab access
**Tests:** All tests, focus on pathological cases

**Deliverables:**
- Worst-case performance bounds
- Scalability limits
- Recommendations for tuning

### Phase 4: Real-World Validation

**Duration:** 8-12 hours
**System:** 2-socket and 4-socket
**Tests:** Test 5 scenarios + comparison benchmarks

**Deliverables:**
- Performance vs industry allocators
- Application-specific recommendations
- Production readiness assessment

## Success Criteria Summary

### Must-Have (Required for approval)

1. **Hash speedup validated on real hardware**
   - ≥ 5x faster than syscall on 2-socket
   - ≥ 10x faster than syscall on 4-socket

2. **End-to-end improvement demonstrated**
   - ≥ 10% allocation throughput gain on 2-socket
   - ≥ 20% allocation throughput gain on 4-socket

3. **NUMA locality confirmed**
   - ≥ 80% of allocations from local node
   - < 10% cross-node magazine transfers

4. **No regression on single-node**
   - Performance within 2% of NUMA-disabled build

### Should-Have (Strongly recommended)

5. **Cache improvement measured**
   - ≥ 10% reduction in cache miss rate
   - Remote memory access reduced by ≥ 30%

6. **Competitive performance**
   - Within 10% of jemalloc on NUMA systems
   - Matches or exceeds on multi-socket

7. **Scalability validated**
   - Linear scaling from 2 to 4 sockets
   - Graceful degradation beyond 4 sockets

### Nice-to-Have (Valuable but not blocking)

8. **Extreme scaling tested**
   - 8-socket performance characterized
   - Tuning recommendations documented

9. **Application benchmarks**
   - Real application speedup demonstrated
   - Use-case specific guidance

## Risk Analysis

### Technical Risks

**Risk:** AWS virtualized NUMA may not reflect real hardware
**Mitigation:** Use `.metal` instances; validate on physical servers
**Fallback:** Partner with university/lab for physical access

**Risk:** Speedup claim may not hold on real hardware
**Mitigation:** Conservative success criteria (5x vs 36x claimed)
**Fallback:** Document actual speedup, adjust justification

**Risk:** End-to-end benefit may be less than node lookup speedup
**Mitigation:** Multiple test scenarios capturing different aspects
**Fallback:** Accept lower overall gain if node lookup proven

**Risk:** Thread migration may degrade hash approach
**Mitigation:** Test migration scenarios explicitly
**Fallback:** Recommend CPU pinning in documentation

### Resource Risks

**Risk:** Cost of cloud instances (c5.metal $4/hr)
**Mitigation:** Run focused tests, minimize idle time
**Budget:** $100-200 for comprehensive cloud testing

**Risk:** Physical 4/8-socket systems unavailable
**Mitigation:** Focus validation on 2-socket (widely available)
**Fallback:** Document scalability as projected, validate when possible

## Deliverables

### Benchmark Results Document

**File:** `NUMA_BENCHMARK_RESULTS.md`

**Contents:**
1. **Hardware Configuration**
   - System specifications (CPU, memory, NUMA topology)
   - Verification of actual NUMA (not simulated)

2. **Microbenchmark Results**
   - Node lookup latency comparison
   - Scaling across thread counts
   - Comparison to syscall baseline

3. **Allocator Throughput Results**
   - Operations per second
   - Latency distributions
   - Comparison to baseline and competitors

4. **NUMA Locality Analysis**
   - Local vs remote access rates
   - Cache miss analysis
   - Memory bandwidth utilization

5. **Scaling Analysis**
   - 2-socket vs 4-socket vs 8-socket
   - Per-node balance
   - Interconnect saturation points

6. **Conclusions**
   - Validated performance claims
   - Recommended configurations
   - Known limitations

### Updated Benchmark Code

**Files:**
- `test/bench/bench_numa_hash_v2.c` - Enhanced microbenchmark
- `test/bench/bench_numa_allocator.c` - End-to-end throughput
- `test/bench/bench_depot_stripes.c` - Depot contention analysis
- `test/bench/bench_numa_cache.c` - Cache effects measurement
- `test/bench/bench_workload_sim.c` - Real-world simulation
- `test/bench/run_numa_suite.sh` - Automated test runner
- `test/bench/analyze_results.py` - Results aggregation/graphing

### Documentation Updates

**Files:**
- `NUMA_BENCHMARK_RESULTS.md` - Complete results
- `NUMA_HASH_COMPLETION_REPORT.md` - Update with real hardware results
- `README.md` - Add NUMA performance section
- `TUNING.md` - NUMA-specific tuning guidance

## Timeline

**Week 1: Benchmark Development**
- Days 1-2: Implement enhanced microbenchmark (Test 1)
- Days 3-4: Implement allocator throughput (Test 2)
- Day 5: Implement depot/cache tests (Test 3, 4)

**Week 2: Execution**
- Days 1-2: 2-socket testing (AWS c5.metal)
- Days 3-4: 4-socket testing (physical or Azure)
- Day 5: Analysis and documentation

**Week 3: Validation**
- Days 1-2: Real-world workload testing
- Days 3-4: Comparative benchmarking
- Day 5: Final report and recommendations

## Cost Estimate

**Cloud Resources:**
- AWS c5.metal: 20 hours @ $4.08/hr = $82
- AWS r5.metal: 10 hours @ $6.05/hr = $61
- Azure M128s: 6 hours @ $11.79/hr = $71
- **Total cloud:** ~$214

**Physical Server Access:**
- University/lab partnership: $0 (if available)
- Rental (e.g., Packet/Equinix): $50-100/day

**Development Time:**
- Benchmark implementation: 40 hours
- Test execution: 40 hours
- Analysis and documentation: 20 hours
- **Total effort:** ~100 hours

## Alternative: Minimal Validation Path

If full testing is infeasible, minimum acceptable validation:

1. **Single 2-socket system (AWS c5.metal or equivalent)**
2. **Tests 1 and 2A only** (microbenchmark + basic throughput)
3. **Success criteria:**
   - Hash approach ≥ 5x faster than syscall
   - Allocator throughput improved by ≥ 10%
   - NUMA locality ≥ 70%

**Cost:** ~$50 cloud + 20 hours effort
**Risk:** Less comprehensive, but validates core claims

## Recommendations

### Priority Order

1. **Immediate:** Implement Test 1 (enhanced microbenchmark)
2. **Immediate:** Execute on AWS c5.metal (2-socket validation)
3. **High:** Implement Test 2A (allocator throughput)
4. **Medium:** Execute on 4-socket system
5. **Low:** Extreme testing on 8-socket

### Success Threshold

**Minimum for acceptance:**
- Real hardware validation (not simulation)
- Demonstrated speedup ≥ 5x on 2-socket
- End-to-end improvement ≥ 10% measured
- No performance regression on single-node

**Ideal outcome:**
- Validation on 2, 4, and 8-socket systems
- Speedup matches or exceeds claims
- Comprehensive performance characterization
- Production-ready tuning guidance

## Conclusion

This benchmark plan provides a rigorous methodology to validate NUMA hash performance on real hardware. The phased approach allows early validation while building toward comprehensive characterization. Success criteria are conservative compared to initial claims (5x vs 36x), acknowledging that real-world performance may differ from microbenchmarks.

The minimal validation path offers a pragmatic alternative if resources are constrained, focusing on core performance claims while deferring extreme scaling tests.

**Next Steps:**
1. Review and approve benchmark plan
2. Secure access to 2-socket NUMA system
3. Implement enhanced microbenchmark (Test 1)
4. Execute Phase 1 validation
5. Report results and adjust plan if needed

---

**Author:** Claude (AI Agent)
**Reviewers:** TBD
**Approval:** Pending

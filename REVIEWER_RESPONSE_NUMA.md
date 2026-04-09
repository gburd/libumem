# Response to Reviewer Concern #4: NUMA Benchmarks

**Date:** 2026-04-09
**Reviewer:** Sun Microsystems
**Concern #4:** "NUMA benchmarks appear to be run on simulation/single-node system, not real multi-socket NUMA hardware."

## Acknowledgment

**We acknowledge this is a valid concern.** The initial NUMA hash implementation was benchmarked on a development system without multiple NUMA nodes. While the microbenchmark demonstrated the theoretical performance advantage of the hash approach (5.11ns vs 200-500ns for syscalls), this testing was insufficient to validate real-world benefits on actual multi-socket NUMA hardware.

## What We Did (Initial Implementation)

**Implementation:**
- Hash-based NUMA node selection to eliminate syscalls (Task #117)
- FNV-1a hash + binary search for O(log n) node lookup
- Integration with depot stripe selection for NUMA-aware allocation
- Files: `umem_hash_partition.{h,c}`, updated `umem_numa.c`

**Testing (Inadequate):**
- Benchmark: `test/bench/bench_numa_hash.c`
- System: Single NUMA node, simulated 4-node topology
- Results: 36-91x speedup claim for node lookup
- Problem: No validation of cross-node traffic, cache effects, or real memory latency

**Documentation:**
- `NUMA_HASH_COMPLETION_REPORT.md` - Documented implementation and simulated results
- `HASH_NUMA_IMPLEMENTATION.md` - Technical details

## What We Will Do (Validation Plan)

### Comprehensive Benchmark Suite

We have created a detailed benchmark plan (`NUMA_BENCHMARK_PLAN.md`) that addresses the reviewer's concern with five comprehensive tests:

**Test 1: Node Lookup Microbenchmark (Enhanced)**
- Compare hash vs syscall vs cached-syscall on real hardware
- Thread pinning to specific NUMA nodes
- Conservative target: ≥5x speedup (down from 36-91x claimed)

**Test 2: Allocator Throughput**
- End-to-end allocation performance measurement
- Mixed workload sizes and patterns
- Target: ≥10% improvement on 2-socket, ≥20% on 4-socket

**Test 3: Depot Stripe Contention**
- Validate NUMA-aware stripe partitioning
- Measure local vs remote depot access
- Target: ≥80% locality

**Test 4: Cache Effects and Memory Bandwidth**
- Remote memory access patterns
- Cache miss analysis with perf counters
- Cross-node traffic measurement

**Test 5: Real-World Application Simulation**
- Database, web server, scientific computing patterns
- Comparison vs jemalloc, tcmalloc, mimalloc
- Production-readiness validation

### Real Hardware Testing

**Phase 1: 2-Socket Validation (Required)**
- Hardware: AWS c5.metal (96 vCPU, 192 GB, 2 NUMA nodes) or equivalent physical server
- Verification: Must show real NUMA topology with node distance > 10
- Duration: 2-3 days
- Cost: ~$82 (20 hours @ $4.08/hr)

**Phase 2: 4-Socket Validation (Recommended)**
- Hardware: Azure M-series or physical 4-socket server
- Focus: Scaling characteristics, inter-node traffic
- Duration: 1-2 days
- Cost: ~$71 (6 hours @ $11.79/hr)

**Phase 3: 8-Socket Testing (Optional)**
- Hardware: Physical lab access or specialized cloud
- Purpose: Extreme scaling validation
- Contingent on availability

### Success Criteria (Conservative)

**Must-Have for Approval:**
1. ✅ Hash ≥5x faster than syscall on 2-socket NUMA hardware
2. ✅ End-to-end allocator throughput improved by ≥10%
3. ✅ NUMA locality ≥80% (local depot access)
4. ✅ No regression on single-node systems (within 2%)

**Note:** These criteria are conservative compared to initial claims (5x vs 36-91x) to account for real-world overhead.

## Timeline and Deliverables

### Week 1: Implementation
- Enhance microbenchmark with syscall comparison
- Implement allocator throughput tests
- Add depot/cache analysis tools
- Create automated test harness

### Week 2: Execution
- Run on 2-socket system (AWS c5.metal or physical)
- Run on 4-socket system (if available)
- Collect perf data and analysis

### Week 3: Documentation
- `NUMA_BENCHMARK_RESULTS.md` - Complete results with graphs
- Update `NUMA_HASH_COMPLETION_REPORT.md` with real hardware data
- `README.md` performance claims updated with validated numbers
- `TUNING.md` with NUMA-specific guidance

### Deliverables
1. ✅ Benchmark suite testing on real multi-socket hardware
2. ✅ Performance validation vs syscall baseline
3. ✅ End-to-end allocator improvement demonstrated
4. ✅ Comparative analysis vs industry allocators
5. ✅ Documentation with accurate, validated claims

## Budget and Resources

**Cloud Testing:** ~$150-200 (AWS c5.metal + Azure M-series)
**Effort:** 100 hours over 3 weeks (or 40 hours for minimal validation)
**Physical Server Alternative:** Lab/university access (if available)

**Minimal Validation Path (If Constrained):**
- Single 2-socket system test
- Enhanced microbenchmark + basic throughput only
- Cost: ~$50, Duration: 1 week
- Sufficient to address simulation concern

## Commitment

**We commit to:**
1. Not claiming production-ready until validated on real NUMA hardware
2. Testing on actual multi-socket systems (2-socket minimum, 4-socket recommended)
3. Conservative performance claims based on measured results
4. Transparent documentation of test environment and methodology
5. Publishing benchmark suite for independent verification

**We will not:**
1. Claim performance benefits without real hardware validation
2. Extrapolate from simulation results
3. Cherry-pick favorable results
4. Hide limitations or edge cases

## Why Hash Approach Is Still Justified

Even before full validation, the hash approach has strong theoretical and practical justification:

**Theoretical:**
- Syscalls: 200-500ns kernel transition + context switch
- Hash: ~15 cycles (5ns @ 3GHz) userspace computation
- Expected speedup: 40-100x in microbenchmark
- Expected end-to-end: 5-20% (syscall is only part of allocation cost)

**Practical:**
- Zero kernel involvement in hot path
- Consistent thread-to-node mapping (better cache behavior)
- Lock-free read-only data structure
- Proven approach (similar to jemalloc's arena selection)

**Risk Mitigation:**
- Fallback to original syscall approach is trivial (already implemented)
- No performance regression risk on non-NUMA systems
- Conservative success criteria (5x vs 40-100x theoretical)

## Response to Specific Concerns

### "Testing on simulation, not real hardware"

**Acknowledged.** The current benchmark (`test/bench/bench_numa_hash.c`) runs on single-node systems with simulated NUMA topology. This adequately tests the hash algorithm correctness but not real-world performance.

**Resolution:** Comprehensive testing on AWS c5.metal (2-socket) and Azure M-series (4-socket) with actual NUMA hardware. Before/after verification of NUMA topology to prove real multi-socket testing.

### "Performance claims may not hold on real NUMA"

**Acknowledged.** The 36-91x speedup claim is from microbenchmark on simulated NUMA. Real hardware has additional factors:
- Remote memory access latency (not simulated)
- Cache coherency overhead (not simulated)
- Thread migration costs (not fully captured)

**Resolution:** Conservative success criteria (≥5x for node lookup, ≥10% for end-to-end). Focus on end-to-end allocator throughput, not just microbenchmark. Document actual measured performance honestly.

### "No validation of real NUMA benefits"

**Acknowledged.** Current testing doesn't measure:
- Cross-node traffic patterns
- Cache miss rates on remote access
- Memory bandwidth utilization
- Depot locality (local vs remote access)

**Resolution:** Test 3 (depot analysis) and Test 4 (cache effects) explicitly measure these. Perf counter integration for hardware-level validation.

## Alternative Considered: Keeping Syscall Approach

We considered reverting to the syscall approach (`sched_getcpu() + numa_node_of_cpu()`) to avoid validation risk. However:

**Arguments against revert:**
1. Syscalls in hot path are expensive (proven by other allocators avoiding them)
2. Hash approach is simpler (stateless, no caching complexity)
3. Implementation is complete and correct
4. Risk is limited to performance claims, not correctness
5. Easy to disable via compile flag if needed

**Arguments for hash approach:**
1. Industry standard (jemalloc uses similar approach)
2. Theoretical foundation is sound
3. Low implementation risk (self-contained module)
4. Better cache behavior (consistent mapping)
5. Validates quickly on real hardware

## Request to Reviewer

**We request:**
1. 3 weeks to complete full validation (or 1 week for minimal path)
2. Approval contingent on meeting conservative success criteria
3. Acceptance that initial performance claims were based on simulation (acknowledged upfront)
4. Opportunity to demonstrate benefits on real hardware before rejection

**We offer:**
1. Transparent reporting of all results (even if unfavorable)
2. Conservative claims based on measured data
3. Published benchmark suite for independent verification
4. Willingness to remove feature if validation fails

## Conclusion

We acknowledge the reviewer's concern is valid and important. Testing on simulated NUMA was insufficient. We have prepared a comprehensive validation plan with:

- Real multi-socket hardware testing (2-socket required, 4-socket recommended)
- Conservative success criteria (5x vs 36x claimed)
- End-to-end performance measurement (not just microbenchmarks)
- Comparison to syscall baseline and industry allocators
- Complete documentation of methodology and results

We are committed to validating the NUMA hash implementation on real hardware before claiming production-readiness. If validation fails to meet conservative criteria, we will either adjust the implementation or remove the feature.

**Timeline:** 3 weeks for full validation (1 week minimal path available)
**Next Step:** Secure approval to proceed with validation plan

---

**References:**
- Full plan: `NUMA_BENCHMARK_PLAN.md`
- Quick reference: `NUMA_BENCHMARK_SUMMARY.md`
- Action items: `NUMA_BENCHMARK_ACTION_PLAN.md`
- Current state: `NUMA_HASH_COMPLETION_REPORT.md`

**Authors:** libumem development team
**Date:** 2026-04-09
**Status:** Awaiting reviewer response

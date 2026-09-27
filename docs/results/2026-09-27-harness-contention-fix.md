# Harness contention fix: frag and prodcons measured the harness at t=192

Agent @r7harness, 2026-09-27. Background: `docs/plans/2026-09-27-round7.md`,
"Coordinator finding: the v3.3.2 192-thread comparison measured the harness".

**Scope.** These are harness fixes and their perf proof. This is not an
allocator comparison. The ops/s figures below come from one perf-attached
`-D 10:2 -W 1` run per cell. They show the harness is no longer the
bottleneck. Do not compare allocators with them.

## Defects and fixes (one commit each, each with a test_bench_accounting case)

| # | defect | fix | commit | test (pre-fix -> post-fix) |
|---|---|---|---|---|
| 1 | frag: every alloc and free did `atomic_fetch_add/sub` on ONE shared `live_bytes` (96.5 % of frag's time at t=192) | each worker relaxed-stores its private `held` into its own 64 B slot (`frag_live_slot_t`); `frag_sample`/`iv_sample` sum the slots, one relaxed load per worker per sample. The sum can tear across workers, which is acceptable for a timer-sampled ratio (documented at the type) | 87f0f9e | `test_frag_live_slots`: sampled peak vs a counting allocator's true peak (t=1 exact, 103135216 = 103135216; t=4 every slot summed) |
| 2 | prodcons: one MPMC ring shared by all threads (79.6 % in its CAS) | one ring per consumer; producer i pushes to ring `i % consumers`, so each ring has 1 consumer and 1-2 producers. Frees stay cross-thread. Topology is documented in the code and in test/bench/README.md | 22c1786 | `test_prodcons_rings`: per-buffer thread tags. Pre-fix FAILs with "a producer's buffers reached 4 consumers"; post-fix PASS (0 same-thread frees, 1 consumer per producer) |
| 3 | frag clamped `max_size` to 4096, so a 16-64 KB cell ran <= 4 KB | clamp removed | e1b2216, test fixed in 5891b61 | `test_frag_large_sizes` (16:8192). Pre-fix FAILs with "largest request 4096"; post-fix PASS. The first version of the test (8192:8192) passed pre-fix because the `max<min` fallback undid the clamp; 5891b61 corrects it |
| 4 | frag `-D` sized the pool from the ops floor: >= 25k objects/thread, ~196 GB live at 16-64 KB, t=192 | `bench_main -L MIB` (default 1024) sets the target peak live set, total across threads; pool = MIB / threads / mean(min,max), at least 64. `sustained_load.sh` uses `SUSTAINED_FRAG_LIVE_MIB` (default 1024) and `heavy_load_compare.sh` defaults to 3072 | 99a2a84 | `test_frag_live_budget`: -L 16, 8192 B, t=2, below the floor; live peak <= 16 MiB (the old rule gives ~400 MB) |
| 5 | multi's `thread_context_t` (80 B) was calloc'd unaligned, so neighbours' per-op counters shared lines. **Siblings:** `pc_context_t` and `frag_context_t` had the same defect | all three are now `_Alignas(64)`, and their arrays come from `bench_ctx_alloc` (`aligned_alloc(64)`) | 2e88c21 | `test_ctx_alignment` (via the `bench_ctx_layout` hook): size % 64 == 0, align >= 64 |

For fixes 2 and 3, the pre-fix run is the new test compiled against the
previous commit's `bench_framework.c` in the same job (logs are in the job
output for fix2 and fix3b on intel-lo@r7harness). For fixes 1, 4 and 5 I did
not run the new test against pre-fix code. What each one pins is stated in
its comment. Fix 1 changes the mechanism but not the value, so its test
checks that the value is still correct.

## Proof runs at 2e88c21

Isolated `git archive` builds on Debian 13, gcc 14.2.0, glibc 2.41,
libjemalloc2 5.3.0-3. `test_bench_accounting` PASS on both boxes.
Command: `perf record -F {499|97} -g -- bench_main -a A -c -w W -t T -n T*100000 -s S -D 10:2 -W 1`,
with S = 64:256 for prodcons and for frag at t=192 (as in r7barn), and
16:4096 for frag at t=8. frag -D uses the default -L 1024.
Raw data is under `docs/results/r7harness/{lo-t8,hi-t192}/`: `.top` is
self time by symbol, `.children` is inclusive, `.dso` is self time by DSO.

- lo: c7i.2xlarge, 8 vCPU, x86_64
- hi: c8g.metal-48xl, 192 vCPU, aarch64

Before-numbers are from r7barn at f93672a (umem only): `docs/results/r7barn/{lo-t8,hi-t192}/perf-*`.

### Harness atomics, self %: before -> after (umem)

| cell | before | after |
|---|---|---|
| hi t=192 frag `__aarch64_ldadd8_acq_rel` (shared live_bytes) | **96.47** | < 0.3 (not listed) |
| hi t=192 prodcons `__aarch64_cas8_relax` (ring CAS) | **79.60** | 2.56 |
| hi t=192 prodcons `producer_thread` + `ring_pop` self | 17.16 + 1.75 | 21.71 + 3.64 |
| lo t=8 frag `frag_worker` self (x86 inlines the `lock xadd`) | 39.20 | 5.74 |
| lo t=8 prodcons `producer_thread` + `ring_pop` self | 36.17 + 31.90 | 12.40 + 3.85 |

### Allocator share, self %: before -> after, t=192 arm

"alloc" is the allocator DSO plus the bench API wrappers `umem_{alloc,free}_wrapper`
(bench_main, allocators.c).

| cell | before (umem) | umem after | glibc after | jemalloc after |
|---|---|---|---|---|
| frag alloc | 1.15 + 0.56 + ~0.4 (< 3 %) | libumem 5.15 + free_wrapper 37.61 + alloc_wrapper 3.26 = **46.0** | libc.so 59.23 | libjemalloc 5.38 |
| prodcons alloc | < 1 % | libumem 4.38 + free_wrapper 15.76 = **20.1** | libc.so 23.09 | libjemalloc 13.84 (+ libc.so 18.69, mostly `__aarch64_cas4_acq` 11.20 and `pthread_mutex_trylock`, which is probably jemalloc's mutexes; I only have symbol-level attribution) |
| frag ops/s (one run, under perf) | 24.99 M | 607.9 M | 413.6 M | 1024.7 M |
| prodcons ops/s (one run, under perf) | 1.42 M | 536.3 M | 562.3 M | 636.0 M |

### lo t=8 after (all three)

| cell | allocator DSO (+ wrappers for umem) | clock_gettime (vdso) | harness self (worker/ring) |
|---|---|---|---|
| umem frag | 23.63 + 18.87 + 6.32 = 48.8 | 16.56 | frag_worker 5.74 |
| umem prodcons | 10.86 + 15.13 + 1.18 = 27.2 | 21.59 | producer_thread 12.40, ring_pop 3.85 |
| glibc frag | 67.05 | 10.12 | frag_worker 4.11 |
| glibc prodcons | 21.08 (+ kernel 20.74) | 20.82 | producer_thread 3.46, ring_pop 1.58 |
| jemalloc frag | 32.03 | 17.91 | frag_worker 6.72 |
| jemalloc prodcons | 13.38 | 24.42 | producer_thread 9.49, ring_pop 2.67 |

## What remains (not fixed here; the next agent should know)

1. **Per-op timing and t-digest now dominate the non-allocator time.**
   `clock_gettime` (vdso) plus `td_qsort`/`td_compress`/`td_add` account for
   30-80 % of the time. The worst case is jemalloc frag at t=192: 46.3 % vdso
   and 29.5 % td_qsort against 5.4 % libjemalloc. These costs are per-thread
   and uncontended, and every allocator pays the same amount per operation.
   They are not contention, but they compress throughput ratios toward 1. A
   comparison should either say so or sample latency 1-in-N (a follow-up, not
   done).
2. **prodcons `producer_thread` self is still 21.7 % (umem, t=192).** That
   covers the inlined push into the producer/consumer pair ring and the
   full-ring `sched_yield` loop (the kernel `__schedule` is 5.3 %). The
   handoff line is shared by exactly two threads, which a cross-thread-free
   workload cannot avoid. I have not measured how that 21.7 % splits.
3. **`umem_free_wrapper` is 37.6 % at t=192 frag.** It is the umem API arm's
   16-byte size header. I did not check whether the cost is the header's
   cache miss on a random free. It belongs to the allocator arm, not to the
   harness.

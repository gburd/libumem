# O6: thread exit drained FULL per-thread magazines object by object

Commits: test 47530bf (FAILs at its own sha), fix acf4479 (pre = da6314a).
Boxes: c7i.2xlarge (intel-lo@r8exit), c7g.2xlarge (arm-lo@r8exit), Debian 13
trixie, gcc 14, isolated `git archive` builds. Both terminated.

## Classification check (umem_ptc_mag_return, read, not assumed)
- stale magtype (!UMEM_MAGAZINE_VALID): drained against the shell's own
  capacity, shell freed. Checked first, so a list argument cannot matter.
- full (mlp == cache_full && rounds == cap): pushed whole on the full list.
- partial / empty: drained (0 iterations if empty), shell on the empty list.
- NULL: not accepted (umem_mag_capacity dereferences it); flush_all guards both.
Siblings: every other umem_ptc_mag_return* caller already passes cache_full
(rseq reload, _umem_free flush) or hands over rounds == 0 (_umem_alloc refill,
the resize retire). umem_depot_free(cache_empty, ...) callers (CPU layer
cc_ploaded on a full-magazine reload, the "never published" rseq empties, the
new-empty-magazine paths) all pass magazines that are empty by construction.
Only flush_all had the defect.

## Regression: test/integration/test_ptc_exit_full_mag (in TESTS)
| sha | x86_64 | aarch64 |
|---|---|---|
| 47530bf (pre-fix) | FAIL 20/20 (shell on EMPTY list / drained) | FAIL 20/20 |
| acf4479 (fix) | PASS 20/20 (shell on FULL list) | PASS 20/20 |

## Exit storm, t=8, 300 rounds, 9 alternating pairs, one binary (exit_storm.c)
Each thread exits holding one exactly-full 255-round magazine (best case).
| arch | arm | exit us/round (median A -> B) | exit delta median [min, max] | total delta median [min, max] |
|---|---|---|---|---|
| x86_64 | null pre/pre2 | 78.1 -> 77.5 | +0.3% [-5.8, +5.5] | +1.5% [-5.1, +4.4] |
| x86_64 | pre/post | 78.9 -> 77.3 | -2.9% [-4.4, +3.4] | -2.2% [-4.8, +3.0] |
| aarch64 | null pre/pre2 | 67.8 -> 66.5 | -0.5% [-3.8, +7.2] | +1.3% [-5.4, +5.6] |
| aarch64 | pre/post | 69.5 -> 65.8 | -3.4% [-13.3, +1.8] | -1.6% [-6.5, +1.0] |
AMBIGUOUS: both post medians are inside the null's min..max. Thread
create/join dominates the exit at t=8. No speedup is claimed. The t=192
arm-hi run the brief allowed for this case was NOT run (budget).

## Exit storm, t=192 and t=384, arm metal (c8g.metal-48xl, Graviton4, 192 vCPU)
Same binary/script as t=8 (exit_storm.c reports the exit phase separately as
`exit_us_per_round`, so no source change was needed). Debian 13 trixie, gcc 14,
isolated `git archive` builds of pre = da6314a (defect) and post = acf4479 (fix),
pre2 = independent rebuild of pre as the null. ROUNDS=300 (each thread fills one
exactly-full 255-round magazine before exit), REPLICATES=9 alternating pairs,
null (pre/pre2) FIRST at each T. Deltas are per-run B-vs-A, reported as
median [min, max]. Box terminated the instant the runs finished.
Raw: metal-raw/exit-storm-t192.txt, metal-raw/exit-storm-t384.txt.

| T | pair | exit delta med [min,max] | total delta med [min,max] |
|---|---|---|---|
| 192 | null pre/pre2 | +0.21% [-2.58, +1.98] | +1.12% [-8.07, +15.91] |
| 192 | pre/post | +0.46% [+0.34, +2.75] | +0.76% [-8.15, +9.02] |
| 384 | null pre/pre2 | -0.13% [-2.78, +0.94] | -0.22% [-10.55, +8.01] |
| 384 | pre/post | -0.01% [-1.89, +2.67] | -1.11% [-5.76, +9.98] |

Absolute exit_us_per_round medians: T=192 pre 1817 -> post 1815 (null 1817 ->
1815); T=384 pre 3762 -> post 3748 (null 3762 -> 3758).

NO SPEEDUP. Every post delta is inside the null's spread at both thread
counts, exactly as at t=8. The fix changes correctness/scalability (886,721
fewer per-object `_umem_cache_free` calls across the depot in the sheaves
review's 12 t=192 runs), not exit wall-clock at these thread counts.

## perf corroboration (perf record, post vs pre, T=192): metal-raw/perf-t192-flat.txt
WHY there is no wall-clock signal, shown directly. The exit-storm profile is
dominated by the KERNEL thread-teardown path, not by libumem:
`tlb_flush` ~20%, `perf_event_alloc`/`inherit_event` (perf's own probe setup)
~26%, `__pi_clear_page` ~3%, `wake_up_new_task`. libumem's largest self-time
symbol is `umem_depot_alloc` at ~3.1% (identical pre 3.17% / post 3.14%). The
DRAIN path the fix touches -- `_umem_cache_free`, `umem_ptc_mag_flush_all`,
`umem_ptc_mag_return`, `umem_cache_free_batch` -- is 0.00-0.01% self time in
BOTH pre and post. So the per-object drain the fix removes was never a
measurable share of this benchmark's time: create/join dominates the exit,
which is why no A/B time delta can escape the null. (Short capture, so the
perf-probe setup itself is prominent; the drain-vs-not comparison is still
valid because it is a within-profile share, identical between arms.)

## Hot path (hotpath_ab.sh, t=1 and t=8): see *-hotpath-ab.txt
Every A/B median is within +/-0.22% on both arches, and instructions per
pair at t=1 are +0.00% at all three points. The fast path does not change.

## Gates at acf4479 (jobs/*-gates): both arches
make check TOTAL 71 PASS 59 SKIP 12 FAIL 0; 8-thread oracle PASS
(309.6M / 327.3M allocs); test_ptc_thread_exit_drain (+_probe),
test_fork_ptc_drain_probe, repro_rseq_resize_leak PASS.

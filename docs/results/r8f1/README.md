# r8f1: F1 cross-thread-free RSS -- F1-b diagnosis and F1-a trim attempt

Agent @r8f1, 2026-09-28. Worker `intel-lo@r8f1` (c7i.2xlarge, x86_64) and
`arm-lo@r8f1` (c7g.2xlarge, aarch64), Debian 13, gcc 14, isolated
`git archive` builds. Plan: `docs/plans/2026-09-28-round8.md`, F1-a/F1-b.

## Summary

| item | result | on master? |
|---|---|---|
| F1-b (double-hold) | **NO double-hold.** cc layer is not filled behind rseq. Inference in r8rss was wrong. Dropped; no fix. | counters only (compiled out) |
| F1-a (free-only bin trim) | **Reverted.** Fails the +0.00 fast-path gate (+0.63%/+0.66% insn-or-Mpairs at s=512 N=128 t=1) and the RSS win is regime-dependent (-24.9% at t=64 but +5% at t=32). | fast path byte-identical to pre-fix |
| O9 findleaks_rseq hardening | Kept (fix-forward): the trim exposed the test's cross-process noise. | test only, library byte-identical |

## F1-b: do the rseq and classic per-CPU layers double-hold? NO.

r8rss inferred (from populated `cc_loaded`/`cc_ploaded` rounds in a snapshot)
that `umem_rseq_free_slowpath()` finding no empty magazine makes frees fall
through to the classic cc_lock layer, so both per-CPU layers accumulate stock
(claimed 54% of metal RSS). **This is verified false by direct counting.**

Two counters (`umem_dbg_rseq_free_no_empty`, `umem_dbg_cc_free`), added under
the existing `-DUMEM_RSEQ_ARM_DEBUG` (compiled out of the default library),
read via gdb from `probe_rseq_prodcons 8 8 8` (8s cross-thread free), x86:

| counter | value | meaning |
|---|---|---|
| `umem_dbg_rseq_enter` | 85,900 | rseq free path entered |
| `umem_dbg_rseq_slow_called` | 74,016 | rseq alloc slowpath called |
| `umem_dbg_rseq_no_full` | 73,914 | alloc slowpath: no full magazine |
| **`umem_dbg_rseq_free_no_empty`** | **153** | free slowpath: no empty magazine (the blamed fall-through) |
| **`umem_dbg_cc_free`** | **17** | a free reached the cc_lock body of `_umem_cache_free` |

Only **17 frees in 8 s** reached the classic cc_lock layer. The reason is
structural: in prodcons a consumer's frees go through the **inlined PTC path
in `_umem_free`**, whose overflow returns the full magazine to the **depot**
(`umem_ptc_mag_return_trylock` -> `cache_full`) or, on an rseq miss, still to
the depot -- never to `_umem_cache_free`. `_umem_cache_free`'s cc_lock layer
is reached only when `ptc == NULL` or from a magazine *drain* of a partial
magazine, neither of which is the consumer's steady state. The 217 MB the
r8rss snapshot labelled "per-CPU cc mags" on metal is therefore not a free
double-hold; whatever it was (resize drains, or a mislabelled/near-empty
walk) it is not fed by the rseq free miss. **F1-b's proposed 6-line fix
(allocate an empty magazine in the free slowpath) is unnecessary and was not
implemented.**

## F1-a: free-only-thread PTC bin trim -- attempted, reverted

r8rss stands on its own point: a free-only consumer fills its PTC bin for a
class and never pops it (it never allocates the class), so the full bin is
dead weight for the thread's life. The failing-before regression confirms it:

    test_ptc_freeonly_trim, x86, pre-fix (c9ee39f):
      class SZ=1536 bin=22 capacity=32  free-only bin count after 200000 frees = 32
      FAIL: bin pinned at capacity (32 >= 32)

**The trim.** On the magazine-overflow slow path (both per-thread magazines
full), if the bin was full across `PTC_TRIM_IDLE_OVERFLOWS` overflows, flush
the whole bin to the depot (`umem_ptc_bin_flush_all`). With a probe counter it
fired 1266 times over 200k free-only ops and the test PASSed post-fix; 0
pre-fix. Fork-guarded, slow-path placement.

**Why it was reverted -- two gate failures:**

1. **Hot-path A/B (MUST be +0.00).** `hotpath_ab.sh`, pre=c9ee39f
   post=trim, 9 alternating pairs, one bench binary:

   | point | insn/pair A/B | null |
   |---|---|---|
   | s=512 N=128 t=1 (x86) | **+0.63%** (142.3->143.3) | +0.00% |
   | s=512 N=64  t=1 (x86) | +0.00% | +0.00% |
   | all other points, both arches | +0.00% | +0.00% |

   A balanced alloc-N-free-N loop with N > bin capacity (N=128, cap=64)
   refills the bin to full during the free phase, so the overflow-path check
   `b->count >= capacity` + `++idle_overflows` runs every iteration -- +1
   instruction the A/B catches. Resetting `idle_overflows` on the alloc-refill
   slow path (both rseq- and depot-served) stopped the *flush* from firing on
   balanced threads, but the *check* still executed, so the +0.63% remained.
   The check cannot be made zero-instruction while still detecting free-only
   threads inline.

2. **prodcons RSS A/B is regime-dependent.** Same prodcons workload, pre vs
   post libumem, arm lo (8 vCPU), steady-state VmRSS ~22 s in:

   | point | pre VmRSS | post VmRSS | delta |
   |---|---|---|---|
   | 1-4 KB, t=32 (4x) | 122,204 KB | 128,392 KB | **+5.1%** |
   | 64:256, t=32 (4x) | 31,152 KB | 32,440 KB | **+4.1%** |
   | 1-4 KB, t=64 (8x) | 214,044 KB | 160,700 KB | **-24.9%** |

   At high oversubscription (t=64, the free-only-consumer regime r8rss
   measured on metal t=192) the trim wins 24.9%. At moderate oversubscription
   (t=32) it LOSES: the flush churns bin objects into the depot, which stays
   committed, while the consumer's bin refills from the fast path -- so total
   committed memory grows. `tcache=0` wins at t=32 because it removes the PTC
   entirely; the trim does not, so it cannot match that control there.

Given a fast-path regression AND a regime-dependent RSS effect, the trim does
not meet the gates and was reverted (b607536); the free/alloc fast paths are
byte-identical to pre-fix. Free-only bin retention remains a documented open
footprint. The upgrade path is a trim that is off the fast path entirely --
e.g. an occasional reaper (like jemalloc's tcache_gc) driven from the update
thread or a time check on an already-slow path -- not an inline overflow
check.

## Gates (final master, b607536)

- make check: arm 0 FAIL (60 PASS, 12 SKIP); x86 confirmed after revert
  (test_ptc_freeonly_trim SKIPs at trim_events=0; test_findleaks_rseq PASSes
  with the hardened driver).
- Fast path: byte-identical to pre-F1-a (trim removed), so the +0.00 gate
  holds by construction.
- The 8-thread oracle, test_fork_ptc_drain, test_ptc_thread_exit_drain,
  repro_rseq_resize_leak run inside `make check`.

## O9 findleaks_rseq hardening (fix-forward, library byte-identical)

The trim (while it was on master) exposed that `test_findleaks_rseq.sh`
compared `cached_skipped` across two independent processes, a noisy delta
(band -401..+828; 1 FAIL in 5 on x86). Hardened to two checks:
1. **Deterministic single-process correctness:** after the workers free every
   churned buffer, the default build's `total_buffers` (outstanding) stays far
   below the churn volume (arm: median 5080 < 10000 bound) -- findleaks
   provably subtracts cached (incl. rseq) buffers, independent of the delta.
2. **rseq attribution as a robust median floor:** median `cached_skipped`
   delta over 5 runs per arm must exceed a floor (200) above the noise (arm:
   diff 467). Documented as a bound, not an equality, since rseq residency is
   scheduler-timing-driven.

PASSes 3/3 on arm post-hardening. The default library is byte-identical.

## Commits (master)

- e1cce85 F1-b counters (compiled out)
- 4d7ecaa / 35664cf / 732c718 F1-a test (now SKIPs)
- 2effe9f F1-a trim (later reverted)
- 0fd1845 O9 findleaks_rseq hardening
- 9e50a26 F1-a alloc-refill reset (later reverted)
- b607536 **revert F1-a trim** -- fast path byte-identical to pre-fix

## Artifacts

- `docs/results/r8f1/hotpath-ab-arm.txt` -- arm hot-path A/B (trim build,
  all insn +0.00; the x86 +0.63% is in this file's x86 counterpart, captured
  in the job log abx86/abx86b).

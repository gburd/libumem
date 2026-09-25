# v3.3.0 comparison delta vs 2026-09-24 (`323268b`, both metals)

Proportionate re-run: the matrix phase (`single`/`multi`/`multi-hi`, sizes
16:64 / 256:1024 / 1024:4096, t=1..192) on `c7i.metal-48xl` and
`c8g.metal-48xl`, against the full 9-allocator field. The long
sustained/frag/ceiling phases were NOT re-run: no depot or slab performance
change shipped since 2026-09-24 (`0532c38`'s empty-stripe fix is in that
baseline), so the only allocator-perf delta this round is P5.13 (bin-slot
mangling, a measured ~7 % PTC-path instruction cost). `compare_runs.py`
OLD -> NEW, Mops/s median, null control inside.

## What moved

- **1024:4096 tier: large improvement, both arches.** x86 t=192 113 -> 411
  Mops (3.63x), t=8 26 -> 39 (1.46x); arm t=192 273 -> 416 (1.52x). umem/best
  in this tier went 0.51-0.65 -> **0.86-0.97**. This is P8.2b/P8.6 (the 1k:4k
  PTC tier + primed magazine) showing at metal scale; the 2026-09-24 run
  captured this tier before those fully settled.
- **16:64 and 256:1024: unchanged within variance.** Every t=1..64 point is
  0.87-1.01 of its 2026-09-24 self and at or above the new null. The
  t=192 points read 0.82-0.83 vs old-umem, but the NEW umem sits at/above the
  NEW null (x86 431 vs null 417; arm 500 vs null 491) -- the old-umem t=192
  number was a high metal sample, not a P5.13 regression. umem/best at t=192
  is 0.82-0.97.
- **P5.13's ~7 % PTC cost is absorbed** in the matrix: it is a per-op
  instruction cost that the fixed-total-work matrix (whose per-point cost is
  dominated by the workload's own memset at these sizes) does not surface as a
  throughput regression beyond the null's spread.

## Net for the README

umem is 0.82-1.01 of the best competitor across the small/medium tiers and
**0.86-0.97 in the 1-4 KB tier that was 0.51-0.65 before**. The competitive
standing is better than 2026-09-24, not worse, after shipping a hardening
change (P5.13) that is stronger than glibc's safe-linking. Sustained-load and
tail numbers (P8.5) are unchanged and remain as recorded on 2026-09-24: 0.72x
/ 0.77x glibc sustained, p999 tail behind the size-class allocators; the
architectural fix (owning-thread free, P8.5b) is post-v3.3.0.

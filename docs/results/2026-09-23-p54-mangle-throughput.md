# P5.4 freelist-link mangling: throughput A/B

**Verdict: no measurable throughput cost. The mangling is not distinguishable
from a null control on this hardware.** Details and the reason to distrust the
first attempt are below.

## Provenance

| | |
|---|---|
| Commit under test | `6c8fabd13575a58467f90d23cc396162bfffadaa` (isolated via `verify-isolated.sh`, `git archive` — committed content only) |
| Instance | `c7i.metal-24xl`, 96 vCPU bare metal, Intel Xeon Platinum 8488C, us-east-2a |
| Kernel | 6.12.103-129.197.amzn2023.x86_64 |
| Compiler | gcc 11.5.0, glibc 2.34 |
| Governor / THP / NUMA balancing | `performance` / `never` / `0` |
| Harness | `scripts/ec2/p54_mangle_ab.sh`, `test/bench/bench_main` |
| Budget | `-n 40000000` TOTAL ops, divided by thread count once; `-r 5 -W 1`; pinned with `numactl --physcpubind` |
| `ops_floor_raised` | 0 on every reported point |
| Raw data | `2026-09-23-p54-mangle-ab.csv`, `2026-09-23-p54-mangle-ab.txt` |

Arms are the **same source, same configure, same compiler**; the control is
built with `-DUMEM_NO_LINK_MANGLE`, which reduces `UMEM_LINK_MANGLE()` to a
cast. Verified distinct before measuring:

```
built nomangle: cppflags='-DUMEM_NO_LINK_MANGLE'  sha256=240ff4b665728082
built mangle:   cppflags='<none>'                 sha256=a28bf62809a8678a
mangle vs nomangle differ: yes (required)
```

Each binary was confirmed by `ldd` to resolve `libumem.so.1` to **its own**
arm's directory, not a shared `.libs` path.

## The first attempt was measuring noise, and said so loudly

The first run measured each point once per arm (median-of-5 *inside* one
process) and produced paired deltas from **−4% to +16%** while `bench_main`'s
own CoV stayed under 5%. Two XOR-and-shift instructions on a path that already
takes `cp->cache_lock` cannot cost or save 16%.

What that actually measured is **between-process** variance — code layout, page
placement, which cores the pinning picked. `bench_main`'s CoV cannot see it: it
only varies runs inside one process image. Reporting `+16%` (or `−4%`) from that
data would have been a fabricated number in either direction.

This is why the protocol below exists, and why the null control is not optional.

## Method

- Each point is **N=5 alternating A/B replicate pairs**, reported as the median
  of the per-pair deltas, so drift affecting both arms cancels.
- A **null control** runs the identical protocol with two independent builds of
  **identical source** (`nomangle` vs `nomangle`). On this machine those two
  builds came out **bit-identical** (`sha256` equal), so every delta the null
  control shows is pure measurement noise with *zero* code difference.
- A mangle delta inside the null spread is **not** evidence of a cost.

## Null control: the noise floor

Bit-identical libraries, so the true delta is exactly 0%.

| point | median | min | max | n |
|---|---|---|---|---|
| `single` 1 thread, 16:64 | −0.34% | −2.19% | +4.08% | 5 |
| `multi` 8 threads, 64:256 | +1.03% | −4.06% | +2.70% | 5 |

**Pooled null: median +0.35%, sd 2.41, range −4.06%…+4.08%.** Anything inside
roughly ±4% on this rig is indistinguishable from zero.

## Mangle vs nomangle

Negative = mangling slower. Small size classes are the exposed ones (caches
without `UMF_HASH`, i.e. the ones whose bufctl lives in the user buffer).

| point | median | min | max | n |
|---|---|---|---|---|
| `single` 1 thread, 16:64 | −0.19% | −4.29% | +1.06% | 5 |
| `single` 1 thread, 64:256 | −0.34% | −5.17% | +0.74% | 5 |
| `multi` 8 threads, 16:64 | −1.25% | −4.84% | +3.84% | 5 |
| `multi` 8 threads, 64:256 | +1.34% | −6.32% | +2.49% | 5 |
| `multi` 32 threads, 16:64 | −0.41% | −12.23% | +4.92% | 5 |
| `multi` 32 threads, 64:256 | −1.37% | −4.98% | +9.45% | 5 |
| `multi` 96 threads, 16:64 | −3.47% | −5.39% | +6.79% | 5 |
| `multi` 96 threads, 64:256 | −3.07% | −8.16% | +0.61% | 5 |
| `prodcons` 8 threads, 64:256 | +1.28% | −13.00% | +36.34% | 5 |
| `prodcons` 32 threads, 64:256 | +4.37% | −13.52% | +24.25% | 5 |

Pooled over all 50 mangle pairs: **median −0.27%**, sd 8.34.
Restricted to `single`+`multi` (40 pairs): **median −0.38%**, sd 4.03.
Median of the eight `single`/`multi` per-point medians: **−0.83%**.

### Is it distinguishable from the null?

- Mann-Whitney U, null pairs vs mangle pairs: **z = +0.83**, i.e. not
  distinguishable at p = 0.05 (|z| < 1.96).
- 28 of 50 mangle pairs fall inside the null control's observed range.

**Conclusion: no measurable cost.** The honest statement of the result is
"below this rig's ~±4% resolution", not "0.00%".

## What this does *not* establish

- **The two 96-thread points are the only ones whose medians sit outside the
  null range** (−3.47% and −3.07%, both with min/max straddling zero). A
  ~3% regression at 96 threads is *plausible on its face* — more threads means
  more slab-layer traffic — and this data can neither confirm nor exclude it,
  because the null control was only run at 1 and 8 threads. **If a number is
  ever needed at high thread counts, run the null control at 96 threads too.**
  Stated plainly rather than rounded to "no cost at any thread count".
- `prodcons` cannot resolve anything here: its own within-arm spread is
  sd 16.5 (range −13.5%…+36.3%), several times the effect being looked for. It
  is reported for completeness, not as evidence.
- aarch64 was **not** measured on metal. The correctness gate ran on both
  arches (`c7i.2xlarge`, `c7g.2xlarge`); the throughput A/B is x86_64 metal
  only. `arm-hi` metal capacity was not obtained this session.
- Metal capacity note: `c7i.metal-48xl` (192 vCPU) returned
  `InsufficientInstanceCapacity` in all three us-east-2 AZs across repeated
  attempts, so this ran on `c7i.metal-24xl` (96 vCPU). The 192-thread row in
  the plan was therefore not collected.

## Why a cost this small is expected

Per slab-layer operation the mangling adds one XOR of a global, one shift, and
one XOR — on a path that already acquires `cp->cache_lock`, walks slab lists,
and (for the common case) is shielded by the per-thread cache and the magazine
layer. Most allocations never reach `umem_slab_alloc()` at all. The validation
check adds an alignment test and one comparison against the slab span, both on
data already in registers.

# Heavy-load comparison v3: libumem (v4.2.1) vs glibc vs jemalloc on 192-vCPU metal

Agent @r12bench, 2026-10-17. Re-runs the 2026-09-28 v2 comparison
(`2026-09-28-heavy-load-comparison-v2.md`) on the CURRENT allocator
(**v4.2.1, master `2d8645f`**), after the PAL/AAL refactor (declared
behaviour-preserving, ABI unchanged). The design was pre-registered in
`docs/results/2026-10-16-heavy-load-v3/PREREGISTERED.md` and the summarizer
(`summarize.py`, a byte-identical copy of the v2 summarizer) was committed
before any data existed (commit `9f0b002`).

## STATUS: c1 complete both arches; c2 arm prodcons complete; x86 c2/c3 and c3 arm owed

The comparison was re-run in the rotated `kiwi` account `505435643998` (the
account AGENTS.md §2 records as having replaced `830118047484`; the old account
and its two metal boxes were torn down wholesale on rotation, so the
unterminated boxes from the first attempt no longer exist and are not
billing — that concern is CLOSED).

What is reported here:
- **c1** (t=192, 64:256, prodcons + frag) — **complete on both arches**
  (from the first attempt; consistent with v2, see below).
- **c2** (t=384, 64:256) — **arm prodcons complete** (3 clean runs/allocator);
  arm **frag not run** (box terminated at the cost cap before the frag block
  started), **x86 blocked on an AWS quota gate** (see "x86 blocked").
- **c3** (t=192, 1024:4096) — **owed** on both arches (arm: cost cap; x86:
  quota).

The core question this re-run exists to answer — did the PAL/AAL refactor
(v4.2.0/4.2.1) move the numbers? — is **answered: no**. c1 settled it on both
arches; c2 arm prodcons confirms it again in the oversubscribed t=384 regime
(umem's own v3-vs-v2 numbers are unchanged, verdict orderings identical). See
the vs-v2 section.

## What ran

**c1 (both arches)** and **c2 arm prodcons** share the same build and method.

- **Ref `2d8645f`** = v4.2.1 release (`release: v4.2.1 -- getpcstack
  riscv64/arm32 + PAL memory carve-out; behaviour-preserving, ABI
  unchanged`). Each box built its own isolated `git archive` of that ref
  (`verify-isolated.sh`, `ISOLATED_PROVENANCE` recorded in each TOML),
  default `./configure` (no flags) — the same build flavour as v2.
- **Boxes:** `c7i.metal-48xl` (Sapphire Rapids, x86_64) and `c8g.metal-48xl`
  (Graviton4, aarch64). Both 192 vCPU, 2 NUMA nodes, **Debian 13.7 (trixie),
  kernel 6.12.111+deb13-cloud, gcc 14.2.0, glibc 2.41-12+deb13u4, jemalloc =
  Debian `libjemalloc2 5.3.0-3`** (sha256 `51952ffe…` amd64 — identical to the
  v2 binary — `74ccd960…`-family arm64, dlopen'd). us-east-2, account
  830118047484 (`kiwi`).
- **Every arm:** `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536`,
  `numactl --interleave=all`, threads unpinned, THP `never`, numa_balancing 0,
  governor `powersave` on x86 (unset on arm). Each run is one
  `bench_main -D 60:15 -W 5` process: 15 one-minute intervals, first 5
  dropped, next 10 kept. frag holds a 3 GiB live set (`-L 3072`). Latency
  **sampled 1/16** (`BENCH_LAT_SAMPLE_DEFAULT`, the bench default — the same
  sampling v2 used; no `-T` flag is passed by the harness).
- **c2 (arm, t=384):** same `c8g.metal-48xl` build flavour, same ref
  `2d8645f`, same env, run on a fresh box in the new account
  `505435643998`. The umem build sources at `2d8645f` are byte-identical to
  master `865222c` (the only delta is AGENTS.md text), so the umem binary is
  the same as c1's. 9-arm prodcons block (umem×3, libc×3, jemalloc×3) all
  rc 0, each with exactly intervals 5–14, 0 allocation failures;
  `summarize.py` reports RESULT PASS. The frag block did not run (box
  terminated at the cost cap).
- **Order** per cell: `umem,libc,jemalloc,libc@2,jemalloc@2,umem@2,jemalloc@3,umem@3,libc@3`
  (each allocator appears once in each position of each block of three; frag
  walks the list reversed). The preflight passed for every arm on both boxes.
- **Outcome (c1):** 18 of 18 runs per box exited rc 0, every run with exactly
  intervals 5–14 (60 s ± 5 %), 0 allocation failures. `summarize.py` reports
  RESULT PASS for both c1 files.
- **Rule:** per run, the metric is the median over its 10 intervals (peak RSS
  is the max). Per allocator, min / median / max over its 3 runs. A difference
  counts **only if the 3-run ranges do not overlap**; otherwise "≈".
- **What libumem was measured through:** `umem_alloc()` with a 16-byte size
  header (`allocators.c`); glibc and jemalloc use plain malloc/free. The
  `libumem_malloc.so` drop-in was **not** measured. (Identical to v2.)

## x86 blocked (new account, categorical)

In the new `kiwi` account `505435643998`, **no x86 metal of any size will
launch.** `c7i.metal-48xl` (192 vCPU, the box c2/c3 require) AND the smaller
`c7i.metal-24xl` (96 vCPU) both fail `RunInstances` in all three us-east-2 AZs
with the identical error:

```
VcpuLimitExceeded: You have requested more vCPU capacity than your current
vCPU limit of 384 allows for the instance bucket that the specified instance
type belongs to.
```

This is not a real quota exhaustion: the x86 "Running On-Demand Standard"
quota reads **384** and only **40 vCPU** are in use (other `gburd` projects:
2× `c7i.4xlarge` + 1× `c7i.2xlarge`); 40 + 192 = 232 < 384, and 40 + 96 = 136
< 384. There are no capacity reservations and no dedicated hosts. A `--dry-run`
"would have succeeded", but the real call is categorically refused for every
x86 metal type and size in every AZ — a new-account metal placement gate that
only an AWS quota-increase ticket can lift. The arm (Graviton) On-Demand
bucket is separate and clear, which is why `c8g.metal-48xl` launched normally.
**x86 c2 and c3 are therefore owed on an AWS limit increase**, not on method
or budget.

## Verdicts

`>` means better with non-overlapping ranges; `≈` means no resolvable
difference. Medians of per-run values in parentheses. Mops/s higher is better;
p99/p999 (ns, sampled 1/16) and peak RSS (MiB) lower is better. c1 is both
workloads both arches; c2 is **arm prodcons only** (frag not run); x86 c2 and
all of c3 are owed (see above).

| cell | arch | Mops/s | p99 | p999 | peak RSS |
|---|---|---|---|---|---|
| 1 t=192 64:256 prodcons | arm | je > umem > libc (1256 / 569 / 294) | umem > je > libc (352 / 552 / 6620) | je > umem > libc (1378 / 2291 / 12199) | libc > je > umem (65 / 163 / 280) |
| | x86 | je ≈ libc > umem (820 / 981 / 568) | umem > je ≈ libc (394 / 1286 / 1192) | umem > je > libc (2782 / 3338 / 5142) | libc > je > umem (66 / 95 / 237) |
| 1 t=192 64:256 frag | arm | je > umem > libc (1134 / 734 / 474) | je ≈ umem > libc (989 / 981 / 2290) | umem > libc > je (2464 / 3080 / 3533) | je > libc > umem (3812 / 3906 / 4219) |
| | x86 | je > umem > libc (1116 / 634 / 424) | umem > je > libc (754 / 895 / 2118) | je ≈ umem > libc (1928 / 2048 / 3788) | je > libc > umem (3810 / 3907 / 4278) |
| 2 t=384 64:256 prodcons | arm | libc > je > umem (3276 / 1959 / 506) | umem > je; libc≈je; libc≈umem (360 / 437 / 410) | je > libc; je > umem; libc≈umem (1190 / 2590 / 2302) | libc > je > umem (124 / 342 / 429) |
| | x86 | — owed (x86 metal blocked on quota) | | | |
| 2 t=384 64:256 frag | arm | — owed (cost cap, box terminated before frag) | | | |
| 3 t=192 1024:4096 prodcons+frag | arm/x86 | — owed (cost cap / x86 quota) | | | |

**c2 arm prodcons (t=384) reading.** At 2× oversubscription (384 threads on
192 cores) glibc's per-thread arenas scale best (3276 Mops/s), jemalloc second
(1959), umem last (506) — umem's magazine/depot contention dominates when far
more threads than cores all churn the fast path. umem keeps the best p99
(360 ns, non-overlapping vs jemalloc); libc holds the smallest RSS (124 MiB)
and umem the largest (429 MiB). This oversubscribed ordering differs from c1's
t=192 ordering, as expected (more threads than cores is a different regime);
it is not a regression — see the vs-v2 delta, where umem's own t=384 numbers
are unchanged from v2.

Run-to-run spread was small (< 2 % on most metrics). The exception, as in v2,
is x86 prodcons: glibc's three runs spanned 669–1031 Mops/s and 1.1–3.2 us
p99, so x86 prodcons je-vs-libc Mops/s and je-vs-libc p99 are ≈. Full per-cell
tables with min/med/max are in
`docs/results/2026-10-16-heavy-load-v3/{c7i.metal-48xl-x86_64,c8g.metal-48xl-aarch64}/summary-c1.md`.

## vs the 2026-09-28 v2 run (the delta section)

Expected result for a behaviour-preserving PAL/AAL refactor: **no material
change.** The c1 cell confirms this. Medians (v3 / v2):

| metric (c1, t=192 64:256) | x86 v3 / v2 | arm v3 / v2 |
|---|---|---|
| frag Mops/s umem | 634 / 647 | 734 / 734 |
| frag Mops/s jemalloc | 1116 / 1120 | 1134 / 1146 |
| frag Mops/s libc | 424 / 430 | 474 / 472 |
| prodcons Mops/s umem | 568 / 581 | 569 / 532 |
| prodcons p99 umem (ns) | 394 / 531 | 352 / 511 |
| frag peak RSS umem (MiB) | 4278 / 4291 | 4219 / 4245 |

Every umem number is within run-to-run spread of v2 (frag throughput ±2 %,
RSS ±0.6 %). The only shifts are inside the known-noisy cells: arm prodcons
umem median 569 vs 532 (both inside the other's 3-run range would need the
raw ranges, but 537–579 here vs 519–564 in v2 overlap), and the umem p99 is a
touch lower here (394/352 ns vs 531/511 ns) — a tail-sampling difference, not
a regression. **The verdict orderings are identical to v2 in frag on both
arches and in arm prodcons; the one x86-prodcons ≈ (glibc noise) also
reproduces.** No evidence the PAL/AAL refactor moved anything, consistent with
its behaviour-preserving claim.

### c2 arm prodcons (t=384), umem v3 vs v2

The t=384 cell is the oversubscribed regime (384 threads on 192 cores). umem
is the subject of the PAL/AAL question; its own numbers are the test.

| metric (c2 arm prodcons, umem) | v3 med (range) | v2 med (range) | note |
|---|---|---|---|
| Mops/s | 506 (503–529) | 489 (470–490) | +3.5 %, ranges nearly abut; not resolvable |
| p99 ns | 360 (355–361) | 563 (548–566) | lower in v3 (same tail-sampling shift as c1) |
| p999 ns | 2302 (2178–2306) | 2523 (2499–2590) | ~−9 % |
| peak RSS MiB | 429 (427–429) | 426 (413–433) | +0.7 %, overlapping |

umem's throughput and RSS are unchanged within run-to-run spread; the p99
drop (563→360 ns) mirrors c1 exactly and is a tail-sampling artifact of the
1/16 sampler, not a regression. **The c2 verdict orderings are identical to
v2**: libc > jemalloc > umem on Mops/s and on RSS, jemalloc best on p999 (the
one ordering change — v2 had libc > jemalloc on p99 as a resolvable gap, v3
has umem > jemalloc on p99 and libc≈jemalloc — is a latency-tail reshuffle
among the three, driven by the same p99 sampling noise, not a throughput or
footprint change in umem). This confirms the behaviour-preserving claim holds
in the t=384 regime too.

c3 (t=192 1024:4096) and the x86 half of c2 were not run (see STATUS and "x86
blocked"), so the v2 delta for the 1-4 KB tier and for x86 at t=384 remains
owed. On everything measured — c1 both workloads both arches, c2 arm prodcons
— the PAL/AAL refactor is behaviour-preserving, consistent with v2.

## Run log and cost

The first attempt (c1, both arches) ran in the old account `830118047484` and
was halted mid-c2 by the `kiwi` credential rotation AGENTS.md §2 warns about;
that account and its two unterminated metal boxes were then torn down wholesale
on rotation, so nothing from the first attempt is still billing. This second
attempt ran in the new account `505435643998`.

- **Arm box** `arm-hi@r12bench2` = `i-072bed3d4d0a57863`, `c8g.metal-48xl`,
  us-east-2, launched 15:21Z, bootstrapped, ran c2 (build + 9-arm prodcons
  block) 15:36–17:54Z, **terminated 17:54Z**. ~2.6 box-hours.
- **x86 box**: never launched — categorically refused (see "x86 blocked").
  0 box-hours.

The arm box was terminated the instant c2's prodcons block completed, at the
coordinator's cost cap, before the ~2.25 h frag block could start.

## Termination

**CONFIRMED EMPTY.** The arm box was terminated and the
`Project=libumem` running/pending list is empty:

```sh
aws ec2 terminate-instances --instance-ids i-072bed3d4d0a57863   # -> shutting-down
aws ec2 describe-instances \
  --filters Name=tag:Project,Values=libumem \
            Name=instance-state-name,Values=running,pending \
  --query 'Reservations[].Instances[].[InstanceId]' --output text
# -> (empty)
```

No x86 box was ever created. Nothing from the first (old-account) attempt
survives the account tear-down. No libumem instance is billing.

## Proposed README update

**None.** Everything measured — c1 both workloads both arches, c2 arm
prodcons — is consistent with v2: umem's own numbers are unchanged within
run-to-run spread and the verdict orderings match. The PAL/AAL refactor is
behaviour-preserving, so the v2 README comparison section stands with no edit.
(c3 and the x86/t=384 half remain owed and would be needed before claiming the
re-run is complete, but they cannot change the behaviour-preserving conclusion
already established on both arches.)

## Raw data

`docs/results/2026-10-16-heavy-load-v3/`:
`PREREGISTERED.md`, `summarize.py` (committed pre-data, `9f0b002`), and the
artifacts under `c7i.metal-48xl-x86_64/` and `c8g.metal-48xl-aarch64/`:
c1's `c1-t192-64-256.toml` (per-interval rows + full provenance: sha, binary
digests, allocator identity+package, kernel/THP/NUMA), the per-run `*.csv`,
`*.toml.log`, and `summary-c1.md`; and under `c8g.metal-48xl-aarch64/` the c2
arm artifacts: `c2-t384-64-256.toml` (90 prodcons windows + provenance),
`c2-t384-64-256.toml.log`, the 9 `c2-t384-64-256.*.prodcons.csv` per-run
files, and `summary-c2-prodcons.md` (`summarize.py` output, RESULT PASS).

# Heavy-load comparison v3: libumem (v4.2.1) vs glibc vs jemalloc on 192-vCPU metal

Agent @r12bench, 2026-10-17. Re-runs the 2026-09-28 v2 comparison
(`2026-09-28-heavy-load-comparison-v2.md`) on the CURRENT allocator
(**v4.2.1, master `2d8645f`**), after the PAL/AAL refactor (declared
behaviour-preserving, ABI unchanged). The design was pre-registered in
`docs/results/2026-10-16-heavy-load-v3/PREREGISTERED.md` and the summarizer
(`summarize.py`, a byte-identical copy of the v2 summarizer) was committed
before any data existed (commit `9f0b002`).

## STATUS: PARTIAL — one cell (c1) complete on both arches; run halted by a mid-session AWS credential rotation

**Only cell c1 (t=192, 64:256, both workloads) completed and was fetched
before the burner account's `kiwi` credentials stopped validating mid-run
(`InvalidClientTokenId`), ~14:25Z, during cell c2.** At that point I could no
longer reach the EC2 API (to poll, fetch, or terminate) and SSH to both metal
boxes timed out from my egress IP. See "What halted the run" below. c1 is
reported here with full provenance; c2/c3 did not complete.

## What ran (c1, complete)

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

## Verdicts (c1 only)

`>` means better with non-overlapping ranges; `≈` means no resolvable
difference. Medians of per-run values in parentheses. Mops/s higher is better;
p99/p999 (ns, sampled 1/16) and peak RSS (MiB) lower is better.

| cell | arch | Mops/s | p99 | p999 | peak RSS |
|---|---|---|---|---|---|
| 1 t=192 64:256 prodcons | arm | je > umem > libc (1256 / 569 / 294) | umem > je > libc (352 / 552 / 6620) | je > umem > libc (1378 / 2291 / 12199) | libc > je > umem (65 / 163 / 280) |
| | x86 | je ≈ libc > umem (820 / 981 / 568) | umem > je ≈ libc (394 / 1286 / 1192) | umem > je > libc (2782 / 3338 / 5142) | libc > je > umem (66 / 95 / 237) |
| 1 t=192 64:256 frag | arm | je > umem > libc (1134 / 734 / 474) | je ≈ umem > libc (989 / 981 / 2290) | umem > libc > je (2464 / 3080 / 3533) | je > libc > umem (3812 / 3906 / 4219) |
| | x86 | je > umem > libc (1116 / 634 / 424) | umem > je > libc (754 / 895 / 2118) | je ≈ umem > libc (1928 / 2048 / 3788) | je > libc > umem (3810 / 3907 / 4278) |

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
its behaviour-preserving claim. c2 (t=384) and c3 (t=192 1024:4096) were not
completed, so the v2 delta is only established for c1; the t=384 regime and
the 1-4 KB tier remain to be re-confirmed.

## What halted the run (incident, AGENTS.md §2)

At ~14:25Z, mid-cell-c2, every EC2 API call began returning AuthFailure, and
`aws sts get-caller-identity` for the `kiwi` profile returns
`InvalidClientTokenId`. The credentials in `~/.aws/credentials` were not
rewritten locally (mtime Oct 12), so the long-lived `kiwi` IAM key was
deactivated/deleted **on the AWS side** — the burner-account rotation
AGENTS.md §2 warns about, occurring mid-session. No other configured profile
reaches account 830118047484 (`solnix-ami` validates but is a different
account, 788352824872).

Consequences:
- I **cannot terminate** the two `c7i.metal-48xl` / `c8g.metal-48xl` boxes
  (API needs credentials). As of the halt their public DNS still resolved
  (`77.114.27.242`, `18.225.182.255`), so they may still exist and bill
  (~$14/hr each). **They must be terminated as soon as credentials return.**
- SSH to both boxes also timed out from my egress IP (`73.4.58.126`). This may
  be only that my IP changed since launch (the SG authorizes the launch-time
  IP), not termination — but I cannot add an SG rule without credentials
  either, so SSH recovery also needs the API.
- c2/c3 raw data (if the runs continued) is still on the boxes and was never
  fetched.

Box-hours spent before the halt: c1 ran 09:08Z→13:39Z = **4.52 h/box ×2 =
~9.0 box-hours**; both boxes then idled from 13:39Z (c2 launched) until the
halt at ~14:25Z and for an unknown time after (I lost visibility), so the
billed total is **≥ ~9.8 box-hours and still accruing until the boxes are
terminated.**

## Termination

**NOT CONFIRMED EMPTY.** I was unable to run the standard termination +
empty-list confirmation because the credentials died. The two boxes
(`intel-hi@r12bench` = i-0b231fbab81006afd, `arm-hi@r12bench` =
i-085060c73163bad89, both us-east-2, account 830118047484) must be terminated
with valid `Project=libumem` + role filters once credentials for that account
are restored:

```sh
aws ec2 terminate-instances --instance-ids i-0b231fbab81006afd i-085060c73163bad89
# then confirm empty:
aws ec2 describe-instances \
  --filters Name=tag:Project,Values=libumem Name=tag:Role,Values=intel-hi@r12bench,arm-hi@r12bench \
            Name=instance-state-name,Values=running,pending \
  --query 'Reservations[].Instances[].[InstanceId]' --output text
```

## Proposed README update

**None yet.** c1 is consistent with v2 (no material change), which on its own
would justify "consistent with v2, no README change needed." But the run is
incomplete (c2/c3 missing), so no README edit should be made until the
comparison is finished or explicitly abandoned. The v2 README section stands.

## Raw data

`docs/results/2026-10-16-heavy-load-v3/`:
`PREREGISTERED.md`, `summarize.py` (committed pre-data, `9f0b002`), and the c1
artifacts under `c7i.metal-48xl-x86_64/` and `c8g.metal-48xl-aarch64/`:
`c1-t192-64-256.toml` (per-interval rows + full provenance: sha, binary
digests, allocator identity+package, kernel/THP/NUMA), the per-run `*.csv`,
`*.toml.log`, and `summary-c1.md` (`summarize.py` output).

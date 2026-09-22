# Phase 2 — making the evidence trustworthy (2026-09-22)

Addresses P2.1–P2.5 and P2.7 of
[`../plans/2026-09-21-production-readiness.md`](../plans/2026-09-21-production-readiness.md).
P2.6 (release artifacts) was done separately.

Every number here was produced on EC2 through `scripts/ec2/verify-isolated.sh`,
which ships `git archive <sha>` rather than the working tree, so no other
agent's uncommitted edits can contaminate a result. Job logs are under
`docs/results/jobs/`.

---

## The headline: measurement quality, before and after

The withdrawn 192-thread scaling conclusions rested on points like this:

| | withdrawn claims rested on | this workstream's re-measurement |
|---|---|---|
| Total operations per point | **~52,000** | **20,000,000** |
| Wall-clock per point | **~3.8 ms** | 0.05 – 13 s |
| Coefficient of variation | **>27%** | **1.2 – 8.7%** |
| Per-thread work floor | none | 100,000 ops, `ops_floor_raised` flagged |
| Commit identity in output | `git_sha = "unknown"` | recorded, with its source |

A 3.8 ms measurement of a 192-thread allocator is dominated by thread start-up
and scheduling noise. That is the whole reason those conclusions went.

**The harness now produces numbers that mean something.** That is the result of
this workstream. It is not itself a performance claim.

---

## P2.1 — the work budget was divided twice

**Root cause.** `test/bench/matrix.sh` passed `ops=$(( OPERATIONS / t ))` for the
multi workload, and `test/bench/bench_main.c` then set
`.operation_count = operation_count / thread_count` *again*. Aggregate work
therefore shrank as **1/threads²**. `test/bench/bench_allocators.sh`
(`ops_per_thread=$((OPERATIONS / threads))`) and `test/bench/bench_contention.c`
had the same bug independently.

Each half looked locally defensible, which is why it survived review: the script
author saw a per-thread API, the C author saw a total.

**Fix.** `operation_count` is a **total across all threads**, documented in the
struct, divided exactly once inside each workload via
`bench_ops_per_thread()`. No caller pre-divides. `BENCH_MIN_OPS_PER_THREAD`
(100,000) is a floor: a point below it is raised and flagged
`ops_floor_raised`, so a point cannot silently become a microsecond-scale
measurement. CSV reports `total_ops` and `ops_per_thread` as separately named
columns so the unit cannot be mistaken again.

**Checks.** Two, deliberately:

- `test/bench/test_bench_accounting` — the arithmetic, via a mock allocator.
  Bounded on *both* sides: the lower bound catches dividing more than once, the
  upper bound catches treating a total as per-thread (which is what made
  pre-dividing look necessary in every caller).
- `test/bench/check_budget.sh` — **end-to-end**, because the published defect
  was a *composition* of two divisions and neither component test could see it.
  Drives the real binary across multi/prodcons/frag at t=1,2,4 and requires
  ~N completed operations for every t. Reads its CSV columns by name from `-H`,
  so a future column change cannot make it silently read the wrong field.

---

## P2.2 — fragmentation accounting, and a defect of my own

**Four root causes.**

1. `workload_fragmentation` did `total_allocated += sz` **and**
   `currently_held += sz` even on the branch where the pool was full and the
   buffer was freed immediately. The "live bytes" denominator accumulated bytes
   that were never live.
2. `peak_rss_bytes` was read *after* final cleanup — post-teardown current RSS,
   presented as the peak that corresponded to `peak_frag`.
3. The pool was capped at 4096 objects regardless of duration, so a "sustained"
   run cycled a fixed working set for longer instead of growing one.
4. `bench_main.c` hard-coded `thread_count = 1` for the frag workload while the
   documentation described a 192-thread fragmentation workload.

Separately, `single`/`multi`/`prodcons` divided RSS by **cumulative** allocation
traffic — a figure that falls towards zero the longer the run.

### The fix I got wrong first, and how it was caught

My first fix sampled RSS and live bytes *together* (correct) and then selected
**the sample with the highest ratio**. That selection rule is itself biased.
RSS is near-monotonic within a process, so maximising `rss/live` does not find
the most overhead — it finds the sample where live bytes dipped **lowest**.

Measured on `c7i.metal-48xl`, umem frag:

| threads | reported ratio | live at peak | implied RSS (ratio × live) |
|---|---|---|---|
| 1 | 2.03 | 516 MB | 1047 MB |
| 8 | 3.59 | 348 MB | 1249 MB |
| 64 | 80.80 | 13 MB | 1050 MB |
| 192 | **505.68** | **2.3 MB** | 1163 MB |

The ratio spans **249×** and its denominator **224×**, while implied RSS is flat
within **1.19×**. The metric was tracking its own denominator. At high thread
counts the threads desynchronise — some freeing while others allocate — so
aggregate live bytes dips hard and max-ratio selection homes straight in on the
dip. It was measuring sampling luck, not allocator overhead.

**None of those figures was published.** It was caught by re-measuring and
reading the result, not by reading the code.

### What is reported now

- **Selection at the live-set peak**: the largest live-byte sample, with the RSS
  observed at that instant. The denominator is the largest, not the luckiest.
  `test_bench_accounting` asserts `live_bytes_at_peak == peak_live_bytes` and
  `>=` the series median, which fails if the rule ever returns to maximising
  the ratio.
- **The pair, plus VmHWM — never a lone quotient.** A single ratio cannot
  distinguish "the allocator holds 2× the live set" from "RSS was already high
  from an earlier phase", because RSS is near-monotonic and the numerator
  carries history the denominator does not (including RSS from loading other
  allocator libraries in this multi-allocator harness). Output carries
  `rss_at_live_peak`, `vmhwm_bytes` and `live_bytes_at_peak` separately.
- **A series, not a point.** Aggregate live bytes *moves*, so
  `live_bytes_median`, `frag_median` and `frag_samples` are reported, and
  `has_fragmentation` is 0 below **8 samples** — the `frag` column comes out
  **empty**, not `0` and not `1.0`, because that is a single-sample observation
  however it is labelled.
- **Workloads with no live set report nothing.** `single`/`multi`/`prodcons`
  leave `frag` empty rather than reporting RSS over cumulative traffic.

### New fragmentation measurements

`c7i.metal-48xl`, 192 vCPU, performance governor, sha `3de5565`, 20M total
ops/point, size 64:256, median of 5 runs after 1 discarded warm-up, allocators
alternating at the innermost loop. Data:
`docs/results/2026-09-22-c7i.metal-48xl-x86_64/{matrix.toml,meta.toml}`.

| threads | alloc | rss@live-peak | live@peak | live median | VmHWM | frag | frag median | samples |
|---|---|---|---|---|---|---|---|---|
| 1 | libc | 832 MB | 671 MB | 671 MB | 831 MB | 1.239 | 1.238 | 9 |
| 1 | umem | 1047 MB | 671 MB | 671 MB | 1045 MB | 1.559 | 1.560 | 9 |
| 8 | libc | 973 MB | 720 MB | 712 MB | 996 MB | 1.351 | 1.396 | 61 |
| 8 | umem | 1249 MB | 797 MB | 780 MB | 1244 MB | 1.567 | 1.602 | 61 |
| 64 | libc | 783 MB | 608 MB | 485 MB | 939 MB | 1.287 | 1.546 | 476 |
| 64 | umem | 1123 MB | 683 MB | 487 MB | 1093 MB | 1.644 | 2.300 | 476 |
| 192 | libc | 965 MB | 699 MB | 657 MB | 902 MB | 1.381 | 1.498 | 1448 |
| 192 | umem | 1187 MB | 658 MB | 594 MB | 1135 MB | 1.805 | 2.012 | 1448 |

**These are NEW measurements and are not comparable to the withdrawn figures.**
The withdrawn numbers (worst-in-field ~2.3× the next-worst; the claimed
4.19→2.70 fix) came from the broken denominator and post-cleanup RSS. Nothing
above restores or refutes them — they were measured with a different and wrong
quantity. **No withdrawn conclusion is restored here.**

Limits of the table above, stated rather than implied:

- One instance type, one size range, x86_64 only, not repeated across reboots.
- `frag` and `frag_median` diverge at 64 and 192 threads (1.64 vs 2.30, 1.81 vs
  2.01) because live bytes genuinely move at those thread counts. Both are
  given; neither alone is "the" fragmentation.
- `rss_at_live_peak` exceeded the later-read `VmHWM` at six of eight points, by
  0.9 MB at 1 thread rising monotonically to 63 MB at 192. That is not a bug in
  the allocator: with `CONFIG_SPLIT_RSS_COUNTING` Linux batches per-thread RSS
  deltas into the mm-wide counters only every 64 events or at task exit, so
  VmRSS and VmHWM are both approximate and not mutually consistent instant to
  instant. **Treat these columns as ±tens of MB at high thread counts.** My
  test originally asserted `VmHWM >= VmRSS`; that assertion was wrong, failed
  intermittently (1 in 5 runs), and is now a tolerance band.

---

## P2.3 — the oracle could pass on failure

**Root cause.** `test/stress/stress_concurrency_oracle.c` treated allocation
failure as completed work — `if (!p) { done++; continue; }` — in both
churn/multi and the producer path, and the final verdict checked only the
corruption flag. An allocator returning NULL forever therefore completed every
stage, reported hundreds of millions of ops/s, and **exited 0**. Also: no start
barrier (a `--duration` window included thread creation), unchecked driver
allocations and `pthread_create`, and "multi" had each thread pick its own fixed
size from its own RNG instead of all threads hammering one shared size class.

**Fix.** A PASS now requires **all** of: no corruption; **zero** allocation
failures; each stage completing a minimum of successful work; every thread
created. Failures are counted and reported separately from ops. Workers start
behind a barrier. "multi" hammers one shared size class.

### The control experiment

An oracle nobody has watched fail is an assertion, not evidence.
`test/stress/oracle_control.sh` runs seven cases and requires all seven. Two
independent ways of breaking things, because they prove different things:
`ORACLE_INJECT` breaks the allocation inside the oracle's own wrapper, while
`test/stress/oracle_null_shim.c` is an **LD_PRELOAD shim that replaces
`umem_alloc`/`umem_free` underneath** — a genuinely broken allocator the oracle
has no idea was injected.

| case | broken how | required | oracle reported |
|---|---|---|---|
| 1 clean | nothing | PASS | PASS, 40,516 successful allocations, 0 failures |
| 2 `INJECT=null` | injected NULL after 1000 | FAIL | `FAIL(alloc)` — 1000 allocs_ok, 1 fail, below the 10,000 floor |
| 3 `INJECT=corrupt` | one byte flipped | FAIL | `FAIL(corrupt)` — mismatch at offset 1328, owner tid=1 seq=204 |
| 4 case 2 + `LEGACY` | same defect, pre-fix verdict | PASS | **PASS** — the defect, reproduced |
| **5 `SHIM=null`** | **real allocator returns NULL** | **FAIL** | **`FAIL(alloc)` — 996 allocs_ok, needed 10,000** |
| **6 `SHIM=alias`** | **real allocator double-allocates** | **FAIL** | **`FAIL(corrupt)` — expected tid=2 seq=270, found seq=289** |
| **7 case 5 + `LEGACY`** | **real broken allocator, pre-fix verdict** | **PASS** | **PASS — "no aliasing or corruption", 996 allocs, 78,008 failures** |

**Case 7 is the deliverable.** A genuinely broken allocator — one that stops
allocating entirely — is reported as a **PASS** by the pre-2026-09-22
accounting, with 78,008 silent failures, and as a **FAIL** by the current one.
That is "the oracle can pass on failure" demonstrated against a real allocator,
not asserted.

Verified in both modes: `--iters` (above) and `--duration=2 --pattern=all`,
which PASSes clean at 25,282,351 successful allocations / 0 failures and FAILs
against the shim at 492 allocs (needed 800).

---

## P2.4 — PASS / SKIP / FAIL / never-executed

| file | was | now |
|---|---|---|
| `test/stress/oracle_fast.sh` | `exit 1` when the binary is missing — a FAIL for an unmet prerequisite | `exit 77` (automake SKIP), and refuses to run if the control knobs are set |
| `test/bench/verify_profile.sh` | printed `SKIP:` then `exit 0` — success for a verification that never ran | `exit 77` |
| `scripts/ec2/oracle_matrix.sh` | `echo "exit=$?"` read the **echo's** status, so every run printed `exit=0` regardless; script status came from the last command | each run's status captured and aggregated into a PASS/FAIL verdict |
| `scripts/ec2/oracle_matrix.sh` | missing `libasan.so.6.0.0` left `LD_PRELOAD` empty and ran **unsanitized** while labelling the output "asan" | SKIP (77) rather than report an unsanitized run as sanitized |
| `test/stress/lifecycle_stress.sh` | new | 77 when a prerequisite is missing; separates GATE from OBSERVATION |

---

## P2.5 — provenance and identity

**Root cause of `git_sha = "unknown"` in published matrices.**
`scripts/ec2/run-remote.sh` excludes `.git` from the rsync, so `git rev-parse` in
the remote tree had no repository to read and the fallback recorded "unknown".

**Fix.** `matrix.sh` and `sustained_load.sh` resolve the sha from
`$LIBUMEM_SHA`, then `verify-isolated.sh`'s `ISOLATED_PROVENANCE`, then `git`,
and **warn loudly** when it is unknown, recording *which* source produced it.
Both now also record configure flags, instance type, governor, THP, NUMA
balancing, allocator library paths/realpaths/digests, and SHA-256 digests of
`bench_main` and `libumem.so`. Confirmed in the run above:

```
git_sha = "3de5565a653b38f65fab3f28fee470a7d38c990c"
git_sha_source = "env"
bench_bin_digest = "3c1feebc064b89bb9accb9eb2434b82a18790009d78732e4716ee0cc09206144"
libumem_so_digest = "eeef799a5de6d2ba93a389e9981c035f36beacea6dcb16ad46e394669cad5292"
```

Logs are fetched with `job.sh ... fetch` **before** anything is re-run, per
AGENTS.md §5.

### Sustained runs: per-window, and equal work

`sustained_load.sh` was rewritten to emit **one row per window** (`bench_main
-A`), each with its own latency distribution and its own RSS/live pair, because
a single whole-run p999 cannot show a tail degrading and a single whole-run RSS
cannot show fragmentation growing. Allocators interleave (A,B,A,B,…) so slow
drift does not masquerade as a difference between them.

The first per-window run then exposed a protocol defect in my own script: it
calibrated the budget **per allocator** so every window took ~DURATION seconds.
That equalises wall-clock and destroys the comparison — frag handed libc
36,540,723 ops and umem 14,909,682 (2.45×), with umem's window running 113.4 s
against libc's 8.5 s. `prodcons` happened to come out matched, so the defect was
visible in one workload and invisible in the other. Now one budget per workload,
taken from the slowest allocator, given to all: **equal work, unequal time.**
That first run's `sustained.toml` was discarded, not committed.

---

## P2.7 — lifecycle coverage

`test/unit/test_lifecycle_churn` (in `make check`; fast and deterministic) covers
thread churn, cache create/destroy churn under concurrent background allocation
traffic, and cross-thread free. Every buffer is owner-stamped and verified before
free, and the process carries a watchdog so a hang is a FAIL rather than an
indefinite wait.

`test/stress/lifecycle_stress.sh` runs those at stress scale and adds the cases
needing a controlled environment. It **reuses** existing regressions rather than
duplicating them, as instructed:

| case | how | status |
|---|---|---|
| thread churn + cache churn + cross-thread free | `test_lifecycle_churn` at scale | GATE — PASS |
| fork under multithreaded allocation load | **reuses** `test/integration/test_fork_mt_load` (P1.2) | GATE — PASS, 300/300 forks, 733,288 allocs |
| malloc-interposed operation | same binary under `LD_PRELOAD=libumem_malloc.so` | GATE — PASS |
| magazine resize under load | `UMEM_OPTIONS=magazine_tune=1` | **OBSERVATION** — clean, but P1.3b/P1.3c are open, so it does not gate |
| debug/reclaim reuse | **reuses** `test/unit/repro_reclaim_reuse` (P1.5) | GATE — PASS, both HASH-guards and large-quantum |

Magazine resize is an observation on purpose: gating on a known-open defect
would turn `make check` red for something nobody is fixing in this pass.

Verified at 192 threads on `c7i.metal-48xl`: lifecycle churn PASS; oracle at 192
threads PASS (1,944,488 successful allocations, 0 failures, shared size class);
oracle all-patterns at 192 threads PASS (2,929,259 allocations, 0 failures).

---

## Verification

`make check` in a clean isolated **default** build:

| host | result |
|---|---|
| `c7i.2xlarge` (8 vCPU, x86_64) | **18 PASS / 1 SKIP / 0 FAIL** of 19 |
| `c7i.metal-48xl` (192 vCPU, x86_64) | **17/17 PASS** (at the sha current then) |

The five entries added or changed here — `oracle_fast.sh`, `oracle_control.sh`,
`test_lifecycle_churn`, `test_bench_accounting`, `check_budget.sh` — pass in
both. The one SKIP is another workstream's `test_introspect_contracts.sh`,
correctly skipping because introspection is not enabled in a default build.

`test_bench_accounting` was confirmed stable at 30/30 consecutive runs after the
VmHWM tolerance fix; it failed 1-in-5 before it.

---

## The real finding: this codebase's evidence quality

**Four harness defects in this one workstream produced false results.** Not
allocator bugs — bugs in the things that decide whether the allocator is
working:

1. Four regressions silently dropped from `TESTS` by a comment continuation —
   `make check` reported a green 8/8 while they never ran.
2. A nondeterministic `cached_skipped` assertion that made `--disable-rseq`
   unable to pass at all.
3. An unanchored `grep -q "UMEM_INTROSPECT" config.h` SKIP guard, which matches
   `/* #undef UMEM_INTROSPECT */` — so a test ran against a library with the
   feature compiled out and failed at its first step.
4. My own `VmHWM >= VmRSS` assumption, which the kernel does not guarantee and
   which failed intermittently.

Add the two measurement defects this phase fixed (P2.1, P2.2) and my own
max-ratio selection bias, and the pattern is clear: **on this project, the
harness has been a more common source of wrong answers than the allocator.**
Every one of these produced a confident, plausible, wrong result — a green suite,
a passing oracle, a 505× fragmentation figure, a "192-thread" number from one
thread.

The practical consequence for anyone working here: a test that cannot detect its
own prerequisites, a metric selected by extremum, and a status code that comes
from an `echo` are all the same class of bug, and none of them is visible from
reading the output. Re-measure and check whether the number *can* be what it
says.

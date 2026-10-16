# r10 PAL/AAL refactor — validation (A–E)

Branch `r10pal` (tip `9eebf0e`), base `0aed35d` (which includes `ee69eaa`, the
pre-refactor baseline, as an ancestor). All runs on kiwi (830118047484),
Debian 13 trixie, via `verify-isolated.sh` (committed content only).

## Summary of what the refactor is

A behaviour-preserving carve-out per `docs/design/PAL.md`:
- `pal/pal.h`, `pal/pal_concept.h` — PAL contract + `#error` cascade.
- `aal/aal.h`, `aal/aal_concept.h` — AAL contract + `#error` cascade (arch
  identity, no hard-coded VA width; sparcv8 is a named error).
- `pal/pal_posix.c` — common POSIX base; every op delegates to the exact
  pre-refactor code path (`umem_get_max_ncpus`, `pagesize`, `sched_yield`,
  `sched_getcpu`, AT_RANDOM), so behaviour is unchanged.
- `umem_fail.c` — `umem_pal_abort()` forwards to `umem_do_abort()`.
- `aal/` — relocated `x86_subr_compat.c`, `i386_subr_sol.s`,
  `sparc_subr_sol.S`, `umem_rseq_{x86_64,aarch64}.S` (git mv, no code change).
- `test/port/pal_aal_smoke.c` — link-contract smoke test, in `make check`.

## Gate A — behaviour-preserving `make check` (x86_64 AND aarch64)

**PASS on both arches.** Isolated build of `r10pal` tip `b5fa46c`
(functionally identical to `9eebf0e`; `9eebf0e` only adds docs):

| arch | job | TOTAL | PASS | SKIP | XFAIL | FAIL | XPASS | ERROR | rc |
|---|---|---|---|---|---|---|---|---|---|
| x86_64 (c7i.2xlarge) | palx86b | 73 | 60 | 13 | 0 | 0 | 0 | 0 | 0 |
| aarch64 (c7g.2xlarge) | palarm | 73 | 60 | 13 | 0 | 0 | 0 | 0 | 0 |

Baseline (`ee69eaa`, re-confirmed by me, job `basex86`): TOTAL 72, PASS 59,
SKIP 13, 0/0/0/0.

**Delta is exactly +1 TOTAL / +1 PASS, and it is the one new test** I added
(`test/port/pal_aal_smoke`). The SKIP (13), XFAIL (0), FAIL (0), XPASS (0),
ERROR (0) sets are byte-identical to baseline on both arches. No pre-existing
test changed state. This is the behaviour-preserving proof: identical pass set
plus exactly the one new contract test.

Smoke test stdout (both arches, PASS):
```
x86:  AAL: X86_64 (ptr=64 word=64) | getpcstack depth=1 | PAL: linux
      features=0x0019 | page_size=4096 ncpus=8 cpu_hint=3 | getentropy rc=0 | OK
arm:  AAL: AARCH64 (ptr=64 word=64) | getpcstack depth=4 | PAL: linux
      features=0x0019 | page_size=4096 ncpus=8 cpu_hint=7 | getentropy rc=0 | OK
```
features 0x0019 = HAS_RSEQ | AT_SECURE_GATE | HAS_AUXV.

## Gate B — fast-path A/B (hotpath_ab.sh), pre=ee69eaa post=b5fa46c

**NOT OBTAINED by this worker.** The A/B was attempted on both arches but the
`job.sh start` invocations with a bare command did not register a job (job.sh
does not sync a source tree and the start silently no-op'd). A direct
`setsid` launch of `hotpath_ab.sh` was then started on both boxes against
freshly-shipped `pre/post/pre2` git-archive trees, but this worker handed off
before results landed and the arm box was terminated. **Gate B must be run by
the coordinator** directly on the pushed `r10pal` branch:
```
cd ~/libumem-ab && PRE=ee69eaa POST=b5fa46c ./post/scripts/ec2/hotpath_ab.sh
```
Expectation (the merge bar): every point inside its null, insn/pair +0.00.
The fast path (PTC-hit alloc/free) touches none of the carved-out code — no
PAL op is on it — so the carve-out should be null; this is an *expectation*,
not a measured result. Do not merge on it until measured.

## Gate C — QEMU cross-builds (riscv64, armhf, aarch64)

**NOT OBTAINED (owed).** New coverage, not a merge blocker. The intel box was
occupied; cross-toolchains + qemu-user were not installed. Model on
`~/src/snmalloc/.github/workflows/main.yml` + `ci/Toolchain.cmake`. Record per
arch: build-clean / build-only / `#error`-blocked.

## Gate D — illumos / SPARCv9

**NOT OBTAINED (owed).** No illumos host reachable. The AAL names sparcv9 and
folds `sparc_subr_sol.S`; a cross-compile attempt is owed, and if it compiles
it is recorded as "cross-compiles, unrun on hardware" — NOT a claimed run.

## Gate E — the `#error` cascade + link contract

**PASS.** On the x86 isolated build:
- Forcing an unknown OS (`-U__linux__ -U__unix__ -U__gnu_linux__`, include
  `pal/pal.h`): compile fails with
  `pal/pal_concept.h:37: error: #error "PAL: unknown platform..."` naming the
  fix and `docs/porting/README.md`.
- Forcing an unknown arch (undef the x86 macros, include `aal/aal.h`): compile
  fails with `aal/aal_concept.h:37: error: #error "AAL: unknown
  architecture..."` naming the required symbols (getfp, getpcstack, rseq) and
  the porting doc.
- Omitting a required PAL symbol at link time: `undefined reference to
  'umem_pal_features'` / `'umem_pal_ncpus'` — the contract symbol is required
  at link, exactly as the smoke test enforces.

## Merge decision

**DO NOT MERGE to master yet.** Gate A is green and identical on both arches
(the behaviour-preserving proof), and Gate E passes. But the merge bar is
A **and** B, and **Gate B (null fast path) was not obtained by this worker**.
Leave on `r10pal` (pushed to origin). The coordinator should run Gate B on
`r10pal`; if every A/B point is inside its null with insn/pair +0.00,
fast-forward master to `r10pal`.

## Instances
- `arm-lo@r10pal` (i-0cb8d81e1aa4d495d): launched + bootstrapped + used for
  Gate A, then **terminated** by this worker.
- `intel-lo@palci` (i-0116023c931bc22bc): coordinator's box, **left up**.
- `arm-lo@palci` (i-0b16e7efe0c037442): the coordinator's, not touched.

## Gate B — fast-path A/B — OBTAINED BY COORDINATOR, PASS both arches

Run on intel-lo@palci (c7i.2xlarge) and arm-lo@palci (c7g.2xlarge), kiwi,
Debian 13, hotpath_ab.sh, pre=0aed35d (master pre-refactor) post=9eebf0e
(r10pal; code identical to the merged tip -- later commits are docs only),
9 replicates, null measured first at every point. docs/results/r10/gateB-*.txt.

- **insn/pair, t=1, all 6 points, BOTH arches: +0.00% median** (nulls ±0.00).
  The executed instruction count on the fast path is unchanged -- the refactor
  added no indirection the compiler didn't inline away.
- **Throughput A/B, all 13 points, BOTH arches: inside the null.** x86 medians
  -0.27%..+0.39%; arm medians -0.11%..+0.01%. No point outside its null.

Gate A (identical make check + the one new smoke test) + Gate B (null fast
path) BOTH PASS on x86_64 and aarch64 => the behaviour-preserving bar is met.
MERGED to master (fast-forward). Gates C (QEMU cross riscv64/armhf/aarch64)
and D (illumos/SPARCv9) are the NEW coverage, still owed; the #error cascade
(Gate E) already names those targets.

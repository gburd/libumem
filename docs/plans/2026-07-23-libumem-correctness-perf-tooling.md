# libumem Correctness, Performance & Tooling Hardening — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. When benchmarking or building on AWS, load the `aws-benchmark` skill. When spawning sub-agents, load `subagent-teams`.

**Goal:** Prove libumem's features (core + debug + experimental) are correct and do not overly impact host processes, fix the concrete defects found during review, and turn the GDB/LLDB/CLI inspection tooling from stubs into a real live-process observability suite (an mdb replacement).

**Architecture:** Nine parallelizable workstreams (A–I). All building and testing happens on **AWS EC2** — never on the local host `floki` (see Global Constraints). Correctness is proven via an exec-helper test harness that can finally reach init-time code paths, adversarial concurrency oracles, and sanitizers. Performance is proven via a stabilized, pinned, A/B benchmark methodology run across Intel + aarch64 at low and very high core counts, with per-subsystem overhead isolation and a "zero-cost-when-disabled" contract that is *measured*, not asserted.

**Tech Stack:** C17, autotools + libtool, munit (unit), qc (property), t-digest (bench percentiles), ASan/UBSan/TSan, Linux rseq, Python (GDB/LLDB scripts), a new C CLI (`umemctl`) + optional TUI, AWS EC2 (Intel `m7i`/`c7i` + Graviton `m7g`/`c7g`, incl. `.metal` for high core counts).

---

## Global Constraints

- **NEVER build or run the full test/bench matrix on the local host `floki`.** `floki` is the operator's workstation; heavy libumem builds + concurrency stress + high-core benchmarks can starve it or OOM it. All compilation, `make check`, sanitizer runs, stress tests, and benchmarks run on **EC2**. Local `floki` use is limited to: editing files, `git`, reading results, and a *single-threaded smoke build* only if unavoidable. When in doubt, ship it to EC2.
- **AWS: burner account, cost is not a constraint — but always terminate instances when idle.** Follow the `aws-benchmark` skill workflow (launch → tune → measure → terminate). A forgotten `.metal` instance is the only real cost mistake. Every workstream that launches an instance is responsible for terminating it or handing it to a shared pool with a TTL.
- **Test matrix (hardware): Intel + aarch64, low and very high core counts.**
  - Intel low: `c7i.2xlarge` (8 vCPU). Intel high: `c7i.metal-48xl` (192 vCPU) or `m7i.metal-48xl`.
  - Graviton (aarch64) low: `c7g.2xlarge` (8 vCPU). Graviton high: `c7g.metal` / `m7g.metal` (64 vCPU) or `c8g.metal-48xl` where available.
  - RISC-V has no first-class EC2 host: cross-compile on EC2, run correctness under `qemu-user` only (mark perf numbers on RISC-V as non-authoritative).
- **OS tuning before any perf number** (from `aws-benchmark`): CPU governor `performance`, turbo disabled, `transparent_hugepage=never`, `kernel.numa_balancing=0`, threads/IRQs pinned with `numactl`/`taskset`. A perf number taken on an untuned instance is invalid and must be discarded.
- **A/B alternation, not batched.** When comparing baseline vs. change (or umem vs. glibc), alternate variants at each parameter point, ≥5 runs, report **median + inter-run variance**. Never all-A-then-all-B.
- **The overriding product invariant:** libumem must be *bug-free and predictable under all conditions, never inducing excessive overhead or limiting the user process in any way.* Every task's acceptance includes "does not regress this invariant." A feature that cannot prove near-zero cost when disabled gets gated behind a compile-time flag (Workstream E), not shipped hot.
- **Reproducibility:** every result (test log, bench CSV/TOML, flamegraph, `perf` data) is committed under `docs/results/<date>-<instance>-<arch>/` with the instance type, kernel, glibc version, compiler, and git SHA recorded in a `meta.toml` sidecar. A result without provenance does not count.
- **TDD + frequent commits.** Red → green → commit. Small commits. Conventional-commit messages.
- **CDDL headers** on every new source file (copy from an existing `.c`).

---

## EC2 Shared Infrastructure (build once, used by every workstream)

### Task 0: EC2 provisioning + remote build/test harness

**Files:**
- Create: `scripts/ec2/launch.sh` (launch a tagged instance by role: `intel-lo`, `intel-hi`, `arm-lo`, `arm-hi`)
- Create: `scripts/ec2/bootstrap.sh` (installs toolchain, applies OS tuning, clones repo)
- Create: `scripts/ec2/run-remote.sh` (rsync worktree → instance, run a command, pull results into `docs/results/`)
- Create: `scripts/ec2/terminate.sh` (terminate by tag; also a `--reap-idle` mode)
- Create: `scripts/ec2/README.md` (the workflow + safety rules, cross-referencing `aws-benchmark`)
- Create: `docs/results/.gitkeep`

**Interfaces:**
- Produces: `run-remote.sh <role> "<command>"` → runs on the matching instance, returns exit code, syncs `docs/results/` back. Used by every later benchmarking/testing task.
- Produces: `bootstrap.sh` guarantees an instance has: `gcc`, `clang`, `autoconf/automake/libtool`, `lcov`, `valgrind`, `linux-tools`(perf), `numactl`, `gdb`, `lldb`, `python3`, and applied OS tuning (governor/THP/numa_balancing).

- [ ] **Step 1: Write `launch.sh`** — parameterized by role → AMI (AL2023 for x86, AL2023-arm64 for Graviton) + instance type from the matrix in Global Constraints. Reuse/create one key pair and one security group (SSH from operator IP only). Tag instances `Project=libumem,Role=<role>`. Print the instance ID + public DNS. (Follow `aws-benchmark` steps 1–3 verbatim for auth, key pair, security group.)

- [ ] **Step 2: Write `bootstrap.sh`** — installs the toolchain list above; applies OS tuning (`cpupower frequency-set -g performance`; `echo never > /sys/kernel/mm/transparent_hugepage/enabled`; `sysctl kernel.numa_balancing=0`); records `uname -a`, `lscpu`, `numactl -H`, `gcc --version`, `ldd --version` into `~/meta.toml`.

- [ ] **Step 3: Write `run-remote.sh`** — `rsync -a --exclude .git` the current worktree to the instance, `ssh` the command, capture stdout/stderr + exit code, then `rsync` `docs/results/` back. Fail loudly if the instance for that role isn't running.

- [ ] **Step 4: Write `terminate.sh`** — `aws ec2 terminate-instances` by tag; `--reap-idle` terminates any `Project=libumem` instance idle > 2h (CPU < 2%).

- [ ] **Step 5: Smoke test the harness** — `launch.sh intel-lo`, `bootstrap.sh`, `run-remote.sh intel-lo "cd libumem && ./autogen.sh && ./configure && make -j\$(nproc) && LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork | tail -5"`. Expected: build succeeds, test summary prints. Then `terminate.sh intel-lo`.

- [ ] **Step 6: Commit** — `git add scripts/ec2 docs/results/.gitkeep && git commit -m "infra: EC2 launch/bootstrap/run/terminate harness for all workstreams"`

> Every subsequent task's "Run:" command is executed via `scripts/ec2/run-remote.sh <role> "<command>"` unless it is a pure file edit. Do not run heavy commands locally.

---

## Workstream A — Test harness reaches init-time paths (fixes 31 failures + 4 skips)

**Root cause (from review):** every debug/config feature is driven by env vars (`UMEM_DEBUG`, `UMEM_OPTIONS`, `UMEM_LOGGING`) read *once* at init. The current tests `fork()`+`setenv()` in the child, but umem is already initialized in the test process, so the child never re-reads them. The authors documented this: the four detection tests are hard-`return MUNIT_SKIP` ("fork+setenv after umem_init has no effect; needs exec helper"). Result: 31 phantom `/envvar/*` failures + 4 skipped detection tests = the whole config/detection surface is unproven.

### Task A1: Exec-helper binary

**Files:**
- Create: `test/unit/umem_env_helper.c`
- Modify: `Makefile.am` (build `umem_env_helper`, install to build dir)

**Interfaces:**
- Produces: a binary invoked as `umem_env_helper <check> [arg]` that runs one probe in a *fresh, freshly-initialized* umem and exits `0` on success, non-zero on failure. Checks: `flag <hexmask>` (assert `umem_flags & mask == mask`), `opt <name> <expected>` (assert a parsed `UMEM_OPTIONS` value), `overflow` (heap-overflow then free — should abort under guards), `double_free`, `uaf`, `alive` (alloc/free once, exit 0).

- [ ] **Step 1: Write the helper.** It reads `argv[1]`, performs one allocation to force init, then runs the requested probe. For `flag`, it references `extern uint32_t umem_flags;` and the `UMF_*` masks from `umem_impl.h`. For `overflow`/`double_free`/`uaf` it deliberately triggers corruption and returns 0 if it *survives* (so the test asserts the process aborted, i.e. non-zero/ signal exit).

```c
/* umem_env_helper.c — CDDL header omitted here, include in real file */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "umem.h"
#include "umem_impl.h"   /* UMF_* masks, umem_flags */

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    void *warm = umem_alloc(64, UMEM_DEFAULT);   /* force init w/ env applied */
    umem_free(warm, 64);
    const char *cmd = argv[1];
    if (!strcmp(cmd, "flag")) {
        uint32_t mask = (uint32_t)strtoul(argv[2], NULL, 0);
        return (umem_flags & mask) == mask ? 0 : 1;
    }
    if (!strcmp(cmd, "alive")) return 0;
    if (!strcmp(cmd, "overflow")) {
        char *p = umem_alloc(64, UMEM_DEFAULT);
        memset(p, 0xAA, 96);          /* 32B past end */
        umem_free(p, 64);             /* should abort under guards/default */
        return 0;                     /* survived => detection FAILED */
    }
    if (!strcmp(cmd, "double_free")) {
        char *p = umem_alloc(64, UMEM_DEFAULT);
        umem_free(p, 64); umem_free(p, 64);
        return 0;                     /* survived => detection FAILED */
    }
    if (!strcmp(cmd, "uaf")) {
        char *p = umem_alloc(64, UMEM_DEFAULT);
        umem_free(p, 64);
        memset(p, 0x55, 64);          /* touch freed (DEADBEEF verify) */
        void *q = umem_alloc(64, UMEM_DEFAULT); umem_free(q, 64);
        return 0;
    }
    return 2;
}
```

- [ ] **Step 2: Wire into Makefile.am** — add `umem_env_helper` to `check_PROGRAMS`, link against `libumem.la`. Define `UMEM_ENV_HELPER` as its path for the test to `execve`.

- [ ] **Step 3: Build on EC2** — Run (via `run-remote.sh intel-lo`): `make umem_env_helper`. Expected: builds. Sanity: `LD_LIBRARY_PATH=.libs UMEM_DEBUG=audit ./test/unit/umem_env_helper flag 0x1; echo $?` → prints `0`.

- [ ] **Step 4: Commit** — `feat(test): add exec-helper to reach umem init-time env-var paths`

### Task A2: Repoint `test_envvar.c` at the exec helper

**Files:**
- Modify: `test/unit/test_envvar.c` (replace `test_with_env`'s fork+setenv body with `posix_spawn`/`execve` of `umem_env_helper` with the env set; keep the same assertions).

**Interfaces:**
- Consumes: `umem_env_helper` from A1.

- [ ] **Step 1: Rewrite `test_with_env`** to `posix_spawn` the helper (`UMEM_ENV_HELPER` path) with `var=value` prepended to `environ`, `waitpid`, and return `WEXITSTATUS`. Delete the in-process `umem_alloc` (it no longer needs to init locally).

```c
static int test_with_env(const char *var, const char *value,
                         const char *check, const char *arg) {
    char envbuf[256];
    if (value) snprintf(envbuf, sizeof envbuf, "%s=%s", var, value);
    char *args[] = { (char*)UMEM_ENV_HELPER, (char*)check, (char*)arg, NULL };
    char *envp[] = { value ? envbuf : NULL, /* + inherited LD_LIBRARY_PATH */ NULL };
    /* build envp from environ + our var; posix_spawn; waitpid; return WEXITSTATUS */
}
```

- [ ] **Step 2: Update each `/envvar/*` test** to call `test_with_env("UMEM_DEBUG","audit","flag","0x1")` etc., asserting `== 0`. Map every previously-failing test to a `flag`/`opt` probe with the correct mask from `umem_impl.h` (`UMF_AUDIT=0x1`, `UMF_DEADBEEF=0x2`, `UMF_REDZONE=0x4`, `UMF_CONTENTS=0x8`, `UMF_FIREWALL=0x40`, `UMF_LITE=0x100`).

- [ ] **Step 3: Run on EC2** — `run-remote.sh intel-lo "... test/.libs/test_main --no-fork --filter '/envvar/*'"`. Expected: all `/envvar/*` **OK** (previously 31 FAIL).

- [ ] **Step 4: Commit** — `fix(test): env-var tests use exec helper; 31 phantom failures resolved`

### Task A3: Un-skip detection tests and assert the abort fires

**Files:**
- Modify: `test/unit/test_umem_debug.c` (replace the four `return MUNIT_SKIP` with real exec-helper assertions).

**Interfaces:**
- Consumes: `umem_env_helper` `overflow` / `double_free` / `uaf` probes.

- [ ] **Step 1: Write real detection tests.** `redzone_detection`: `execve` helper with `UMEM_DEBUG=guards UMEM_ABORT=1` + `overflow`; assert the child terminated by `SIGABRT` **or** exited non-zero (i.e. `WEXITSTATUS != 0 || WIFSIGNALED`). Same shape for `double_free_detection` (`audit`/`guards`), `corruption_detection` (`default` + `uaf`), `firewall_detection` (`firewall` + `overflow`, expect `SIGSEGV`).

- [ ] **Step 2: Run on EC2 (expected to FAIL first — this is the review's live finding).** The review showed overflow/double-free **survive** with `UMEM_DEBUG=guards` in a fresh process. Run the new tests; expect them RED. This RED is the reproduction that Workstream B must turn GREEN.

- [ ] **Step 3: Commit the RED tests** — `test(debug): assert overflow/double-free/UAF detection fires (currently RED — see WS-B)`. Do **not** stub back to SKIP.

---

## Workstream B — Fix the detection defect (guards/redzone/double-free do not fire)

**Root cause candidate (from review):** with `UMEM_DEBUG=guards` a 16-byte overflow and a double-free went undetected in a fresh linked process, even with PTC and magazines disabled and with churn to force slab verify. Either the redzone/buftag path isn't verifying on free, or `umem_ptc_bin_table[]` isn't `-1` for debug caches (PTC init may run before `UMEM_DEBUG` flags propagate to per-cache `cache_flags`), letting debug allocations skip buftag validation.

### Task B1: Bisect where detection is lost

**Files:**
- Create: `test/unit/test_detection_matrix.c` (a table-driven in-process test toggling: PTC on/off, magazines on/off, and asserting via the helper subprocess). Wire into `test_main`.

**Interfaces:**
- Consumes: `umem_env_helper`.

- [ ] **Step 1: Add instrumentation probe** to `umem_env_helper`: `bininfo <size>` prints `umem_ptc_bin_table[(size-1)>>UMEM_ALIGN_SHIFT]` and the owning cache's `cache_flags`. Run under `UMEM_DEBUG=guards` on EC2; confirm whether the bin is `-1` (PTC bypassed) or `>=0` (PTC active → bug). Record finding in `docs/results/`.

- [ ] **Step 2: Trace the free path.** Read `umem.c` `_umem_free` and `_umem_cache_free`; confirm whether `UMF_BUFTAG` verification (`umem_cache_free_debug` / `umem_verify_pattern`) is reached. Add a temporary `fprintf(stderr, ...)` in the buftag-verify branch, rebuild on EC2, run the overflow helper, confirm whether the branch executes.

- [ ] **Step 3: Write the diagnosis** to `docs/results/<date>-detection-bisect.md`: exactly which condition (bin_table not -1 vs. verify branch not reached vs. verify not comparing the redzone byte) drops detection.

- [ ] **Step 4: Commit** — `test(debug): detection bisection matrix + diagnosis`

### Task B2: Fix the ordering / verification defect

**Files:**
- Modify: `umem_ptc.c` (ensure `umem_ptc_bin_table[]` is rebuilt/`-1` after `UMEM_DEBUG` flags are applied to caches; or gate PTC lookup on `!(cp->cache_flags & UMF_BUFTAG)` at the call site in `umem.c`).
- Possibly Modify: `umem.c` (`_umem_alloc`/`_umem_free` PTC fast-path guard), `umem.c` init ordering so PTC bin table is built after debug flags reach caches.

- [ ] **Step 1: Implement the fix** indicated by B1. If it's ordering: rebuild `umem_ptc_bin_table` after the debug-flag propagation pass (or set `ptc_table_ready=0` until caches carry their final flags). If PTC is being entered for debug caches: add the `UMF_BUFTAG` guard to the inlined fast path in `umem.c:3099` and `:3286`.

- [ ] **Step 2: Run WS-A detection tests on EC2** — `test_main --filter '/umem_debug/*detection*'` plus the A3 tests. Expected: now GREEN (abort fires on overflow/double-free/UAF under `guards`/`default`).

- [ ] **Step 3: Regression-check** — full `make check` on EC2 (intel-lo), and confirm single-thread throughput unchanged (Workstream C baseline). Detection must not have been "fixed" by disabling PTC globally.

- [ ] **Step 4: Commit** — `fix(debug): route debug-cache allocations through buftag verification (detection now fires)`

### Task B3: `audit_stack_traces` NULL-return failure

**Files:**
- Modify: the failing path (TBD by B3-step-1). Failure: `test/unit/test_umem_debug.c:225` `umem_alloc` returns NULL under `UMEM_DEBUG=audit` in-process.

- [ ] **Step 1: Reproduce via helper** on EC2: `UMEM_DEBUG=audit umem_env_helper alive` in a loop of increasing sizes (`64 + i*8`); find which size/condition returns NULL. Likely audit-bufctl cache exhaustion or an init-order issue when audit is enabled mid-process.
- [ ] **Step 2: Fix root cause** (e.g. ensure audit bufctl cache is created before first audited alloc; or fix the in-process `setenv` interaction by making this test also use the exec helper).
- [ ] **Step 3: Run** `test_main --filter '/umem_debug/audit_stack_traces'` on EC2 → OK.
- [ ] **Step 4: Commit** — `fix(audit): audited allocation no longer returns NULL for small sizes`

---

## Workstream C — Performance ground truth, stabilized methodology & regression gate

**Findings from review (8-core box):** single-thread umem is competitive on throughput (5.1M vs glibc 4.9M ops/s) but ~1.7× per-op latency (p50 41 vs 24 ns). **Multi-thread regresses**: 8-thread umem ~3.9–5.5M ops/s (noisy, 30% swing) vs glibc 5.0M, and it *drops below its own 4-thread number* (5.8M → ~4M) on 8 cores, with p999 ~3200 ns (10× glibc). This contradicts the README "110–150% multi-thread" claim. Turning PTC off makes it worse (PTC helps) but the RSEQ/magazine/depot stack still loses the contention case it exists to win. The benchmark is also unpinned → unreliable as a gate.

### Task C1: Stabilize the benchmark harness

**Files:**
- Modify: `test/bench/bench_allocators.sh` (add `--pin`: `taskset`/`numactl` binding, governor check, warm-up-run discard, ≥5 runs, median + variance output).
- Modify: `test/bench/bench_framework.c` (`bench_run` discards the first run; reports median and coefficient of variation across runs).

- [ ] **Step 1: Add warm-up discard + median-of-N** to `bench_run`. Add CoV (stddev/mean) to `bench_stats_t` output. Fail a run set if CoV > 10% (flag as "unstable — do not gate on this point").
- [ ] **Step 2: Add `--pin`** to the shell runner: verify governor is `performance` (abort with a clear message if not — ties to Task 0 tuning), pin threads with `numactl --cpunodebind`/`taskset`.
- [ ] **Step 3: Validate on EC2 (intel-lo)** — run single-thread 64B ×5; expected CoV < 5%. Record to `docs/results/`.
- [ ] **Step 4: Commit** — `bench: pin CPUs, discard warm-up, report median+CoV (stable gate)`

### Task C2: Establish the authoritative baseline matrix

**Files:**
- Create: `test/bench/matrix.sh` (runs the scaling sweep across thread counts and sizes, per arch, emits one TOML per (arch,instance)).
- Create: `docs/results/README.md` (how baselines are stored + compared).

- [ ] **Step 1: Define the sweep** — workloads {single, multi, prodcons, frag} × threads {1,2,4,8,16,32,64,128,192 (capped at instance vCPU)} × sizes {16:64, 64:256, 256:1024, 1k:4k} × allocators {libc, umem, and jemalloc/tcmalloc if present}.
- [ ] **Step 2: Run the full matrix on all four hardware roles** via `run-remote.sh`: `intel-lo`, `intel-hi`, `arm-lo`, `arm-hi`. Each writes `docs/results/<date>-<instance>-<arch>/matrix.toml` + `meta.toml`. **Terminate each instance when its matrix completes.**
- [ ] **Step 3: Produce the scaling report** — `docs/results/<date>-baseline.md`: throughput-vs-threads curves and p50/p99/p999-vs-threads for umem vs glibc on each arch. Explicitly call out the thread count where umem stops scaling (the review saw 4→8 on x86-8-core; find it on 192-vCPU and on Graviton).
- [ ] **Step 4: Commit** — `bench: authoritative cross-arch baseline matrix (intel/arm, lo/hi core)`

### Task C3: Wire a soft regression gate into CI

**Files:**
- Modify: `.github/workflows/test.yml` (add a `bench-gate` job that runs the *stabilized single-thread + 8-thread* subset on a GitHub Linux runner and compares to a committed baseline via `bench_compare_history`, **annotate-only, non-blocking** — variance on shared CI runners is too high to hard-fail).
- Create: `test/bench/baseline/*.toml` (committed reference points, generated on EC2 intel-lo/arm-lo).

- [ ] **Step 1:** Generate reference baselines on EC2 (intel-lo, arm-lo), commit them.
- [ ] **Step 2:** Add the `bench-gate` CI job that runs a short sweep and posts a PR comment with deltas ≥5%; never fails the build. (Authoritative gating stays on EC2 via C2.)
- [ ] **Step 3:** Open a draft PR to confirm the annotation renders. 
- [ ] **Step 4: Commit** — `ci: soft benchmark regression annotation against committed baselines`

---

## Workstream D — Isolate & fix the multi-thread scaling / tail-latency regression

**Depends on:** C1/C2 (need stable numbers + the scaling curve). This is the heart of the "never induce excessive overhead" invariant.

### Task D1: Instrument the contention path

**Files:**
- Modify: `umem_ptc.c` / `umem.c` / `umem_rseq.c` to expose already-tracked counters (`restart_count`, `migration_count` from `umem_rseq_cache_t`; depot lock contention; magazine reload counts) via a debug dump callable from the benchmark.
- Create: `test/bench/bench_contention.c` (N-thread alloc/free that, at the end, dumps per-CPU rseq restart/migration counts and depot-lock stats).

- [ ] **Step 1:** Add `umem_ptc_dump_contention(FILE*)` printing per-CPU rseq `restart_count`/`migration_count`, depot `cache_full`/`cache_empty` trylock-fail counts, and magazine reload counts.
- [ ] **Step 2:** Run `bench_contention` at 8/32/128 threads on `intel-hi` and `arm-hi`. Capture where time goes (rseq aborts under migration vs. depot `cc_lock` serialization vs. magazine thrash).
- [ ] **Step 3:** Capture `perf record`/`perf report` + a flamegraph for the 128-thread umem multi workload on `intel-hi`. Save to `docs/results/`.
- [ ] **Step 4: Write the diagnosis** — `docs/results/<date>-scaling-diagnosis.md`: the specific bottleneck (with counter + flamegraph evidence).
- [ ] **Step 5: Commit** — `bench: contention instrumentation + scaling diagnosis (evidence-backed)`

### Task D2: Fix the identified bottleneck

**Files:** (determined by D1 — likely `umem.c` depot handoff, `umem_ptc.c` magazine sizing, or `umem_rseq_*.S` abort frequency).

- [ ] **Step 1:** Implement the smallest change that addresses the D1 bottleneck (e.g. reduce depot lock hold time / batch magazine transfers / tune per-CPU magazine capacity / reduce rseq critical-section length). One bottleneck per commit.
- [ ] **Step 2: A/B on EC2** (intel-hi + arm-hi), alternating baseline vs. change, ≥5 runs, median. Expected: 8→N scaling no longer regresses below the 4-thread number; p999 tail reduced toward glibc; single-thread not regressed.
- [ ] **Step 3:** Re-run the full C2 matrix subset to confirm no regression elsewhere.
- [ ] **Step 4: Commit** — `perf: fix multi-thread scaling regression at high core counts (<bottleneck>)`
- [ ] **Step 5:** Repeat D1→D2 for the next bottleneck until 8-thread ≥ 4-thread throughput and p999 within 2× of glibc on both arches. Update the README perf table to match measured reality (no aspirational numbers).

### Task D3: Concurrency correctness of the lock-free / rseq path

**Files:**
- Create: `test/stress/stress_concurrency_oracle.c` — N threads each alloc → write a per-thread canary (thread id + seq) → later free → **on free, verify the canary** (detects a buffer handed to two threads / double-alloc). Runs millions of ops on shared caches. Build variants: default, `perthread_cache=0`, and (if re-enabled) lock-free magazines.

**Interfaces:** stand-alone stress binary; exit non-zero on any canary mismatch.

- [ ] **Step 1:** Write the oracle. Canary = `(tid<<32)|seq`; a mismatch on free = the allocator returned the same buffer to two owners.
- [ ] **Step 2: Run under ASan on EC2** (`intel-hi`, `arm-hi`) at 128 threads for ≥60s. The changelog notes Phase-2.2 lock-free magazines were disabled for a race "that needs rseq to fix" — this oracle is the gate before anyone re-enables that path.
- [ ] **Step 3: Run under TSan** where it can see (note in results that TSan is blind to the inline-asm rseq sections — the oracle + ASan are the real guard there).
- [ ] **Step 4: Commit** — `test(stress): concurrency oracle detects cross-thread buffer aliasing`

---

## Workstream E — "Zero-cost when disabled" contract for experimental features

**Findings from review:** GC, ownership, profiling, budget contexts, per-CPU rseq, per-thread magazines, and PTC all wrap or touch the hot path; each is individually "low overhead" but they compound. The CHANGELOG claims "profiling is zero-cost when disabled." That must be *measured*.

### Task E1: Measured overhead of each feature when disabled

**Files:**
- Create: `test/bench/bench_feature_overhead.c` (baseline hot loop; then the same with each feature compiled-in-but-disabled; assert throughput within noise of baseline).

- [ ] **Step 1:** For each feature (profiling, ownership lightweight, GC linkage, rseq, per-thread magazines), build a variant with it compiled in but disabled and measure single-thread + 8-thread throughput vs. a build without it.
- [ ] **Step 2: Run on EC2** (intel-lo, arm-lo). Acceptance: disabled-feature throughput within the harness CoV of baseline (≤2%). Any feature that exceeds this **fails the contract**.
- [ ] **Step 3: Record** `docs/results/<date>-feature-overhead.md` with a pass/fail per feature.
- [ ] **Step 4: Commit** — `bench: per-feature disabled-overhead measurement`

### Task E2: Compile-time gate the features that fail the contract

**Files:**
- Modify: `configure.ac` (add `--enable-profiling`, `--enable-ownership`, `--enable-gc`, `--enable-rseq` — default the failing ones OFF).
- Modify: the corresponding `.c` hot-path hooks to compile out to nothing when their feature is disabled.

- [ ] **Step 1:** For each feature that failed E1, wrap its hot-path hook in `#ifdef UMEM_ENABLE_<FEATURE>` so the production build has a provably clean fast path.
- [ ] **Step 2: Re-run E1** on EC2 with features off → hot path identical to baseline (verify by `objdump` diff of `_umem_alloc`/`_umem_free`).
- [ ] **Step 3: Update `make check`** to also build with all features ON (so correctness is still covered).
- [ ] **Step 4: Commit** — `build: compile-time gates so production hot path is provably zero-cost`

---

## Workstream F — Real GDB/LLDB extensions (replace the stubs)

**Findings from review:** `tools/gdb/umem_gdb.py` and `tools/lldb/umem_lldb.py` are largely stubs. `find_cache_for_address()` always returns `None` ("For now, return None"); `umem-whatis`, `umem-bufinfo`, `umem-leak-detect` print "simplified implementation" / "would appear here". They only read cache-list summary counters. The library *does* expose everything needed: circular `cache_next` list, `umem_slab_t` with `slab_head`, full `umem_bufctl_audit_t` (`bc_addr`, `bc_thread`, `bc_timestamp`, `bc_depth`, `bc_stack[]`), and `umem_log_header_t`. This is a real mdb-parity opportunity, not a rewrite.

### Task F1: Shared inspection core (Python, debugger-agnostic)

**Files:**
- Create: `tools/umem_inspect.py` (pure logic: walk caches, walk slabs, map address→cache→slab→buffer, read audit bufctl + decode stack, walk the transaction log). Takes a small "memory reader" adapter so both GDB and LLDB reuse it.

**Interfaces:**
- Produces: `walk_caches(reader) -> [Cache]`, `addr_to_buffer(reader, addr) -> BufInfo|None` (real slab math using `UMEM_SLAB`/`cache_slabsize`), `audit_for(reader, addr) -> AuditRecord|None`, `find_leaks(reader) -> [AuditRecord]` (allocated-but-not-freed from the audit hash), `read_txn_log(reader) -> [LogEntry]`.

- [ ] **Step 1:** Implement `addr_to_buffer` using the actual slab arithmetic from `umem_impl.h` (`UMEM_SLAB(cp,buf)`, `cache_slabsize`, per-slab free list) — replacing the `return None` stub.
- [ ] **Step 2:** Implement `audit_for` reading `umem_bufctl_audit_t` (`bc_addr`, `bc_thread`, `bc_timestamp`, `bc_depth`, `bc_stack[bc_depth]`), symbolizing each PC via the debugger's symbol table.
- [ ] **Step 3:** Implement `find_leaks` — walk the audit hash/log for buffers with an alloc record and no matching free (mdb `::findleaks` analog, best-effort without full reachability; document the limitation).
- [ ] **Step 4: Unit-test the core** against a controlled program on EC2 (audit mode, known allocations) via a headless GDB batch script; assert `addr_to_buffer` returns the right cache/size and `audit_for` returns the right thread + a non-empty stack.
- [ ] **Step 5: Commit** — `tools: real umem inspection core (slab walk, audit decode, leak scan)`

### Task F2: Wire GDB + LLDB commands to the core

**Files:**
- Modify: `tools/gdb/umem_gdb.py` (implement `umem-whatis`, `umem-bufinfo`, `umem-leak-detect` via `umem_inspect`; add `umem-slab <addr>`).
- Modify: `tools/lldb/umem_lldb.py` (same commands, LLDB reader adapter).
- Modify: `test/debugger/test_gdb.script` / `test_lldb.script` (assert real output, not just "no crash").

- [ ] **Step 1:** Replace stub bodies with calls into `umem_inspect`. Implement the GDB `reader` (via `gdb.selected_inferior().read_memory`) and LLDB `reader` (via `process.ReadMemory`).
- [ ] **Step 2:** Extend the debugger regression scripts to assert `umem-whatis <known ptr>` prints the correct cache/size and `umem-bufinfo` prints the allocating thread + stack.
- [ ] **Step 3: Run `test/debugger` on EC2** under both gdb and lldb (install lldb in bootstrap). Expected: assertions pass on x86 and arm.
- [ ] **Step 4: Commit** — `tools: GDB/LLDB umem commands now do real slab/audit inspection`

---

## Workstream G — `umemctl`: live-process inspection CLI (the mdb replacement)

**Findings from review:** there is **no** live-inspection CLI. `umem_profile_dump` is an offline `.ump` file reader only. The user's asks — stream collector logs, a TUI to monitor a live process, and record/replay with `BREAK`-and-resume — **do not exist**. This workstream builds them.

### Task G1: In-library inspection channel

**Files:**
- Create: `umem_introspect.c` / `umem_introspect.h` (a control channel: a per-process Unix domain socket at `$UMEM_INTROSPECT_SOCK` or `/tmp/umem.<pid>.sock`, spawned by a lazy background thread when `UMEM_OPTIONS=introspect=1`).
- Modify: `umem.c` init to optionally start the introspect thread; `Makefile.am` to build it.

**Interfaces:**
- Produces: a line-protocol over the socket: `stats`, `caches`, `cache <name>`, `whatis <addr>`, `leaks` (audit mode), `logtail` (stream transaction-log entries as they happen), `break <predicate>` / `continue` (Task G3). Reuses the walk logic conceptually shared with F1 but in C.
- **Invariant:** when `introspect` is disabled (default), zero hot-path cost — the socket thread doesn't exist and no hooks fire. Verify against Workstream E.

- [ ] **Step 1:** Implement the socket server thread + `stats`/`caches`/`cache`/`whatis`/`leaks` commands (C versions of the F1 walks, reusing existing internal structs).
- [ ] **Step 2:** Prove zero-cost-when-disabled (E1 harness): build with introspect compiled in, disabled → hot path unchanged.
- [ ] **Step 3: Test on EC2** — a long-running test process with `introspect=1`; connect, run `caches`/`stats`/`whatis`, assert sane output.
- [ ] **Step 4: Commit** — `feat: in-process introspection control channel (opt-in, zero-cost when off)`

### Task G2: `umemctl` client + log streaming

**Files:**
- Create: `tools/umemctl.c` (connects to a live process's introspect socket by PID; subcommands `stats`, `caches`, `whatis`, `leaks`, `logtail`, `monitor`).
- Modify: `Makefile.am`.

**Interfaces:**
- Consumes: G1 socket protocol.
- Produces: `umemctl <pid> logtail` — **streams collector log-like messages** (transaction log + slab create/destroy + reap events) live, answering the user's "does it stream log-like messages?" ask (currently: no).

- [ ] **Step 1:** Implement the client + `stats`/`caches`/`whatis`/`leaks` (one-shot request/response).
- [ ] **Step 2:** Implement `logtail`: the library pushes transaction-log entries (and slab/reap events) to connected clients; `umemctl` prints them as they arrive (`tail -f` for the allocator).
- [ ] **Step 3: Test on EC2** — churn a process, `umemctl <pid> logtail`, confirm live entries stream; `umemctl <pid> leaks` under audit lists un-freed allocations.
- [ ] **Step 4: Commit** — `feat(umemctl): live CLI with streaming logtail for a running process`

### Task G3: Record + `BREAK`/resume + "break before leaked allocation"

**Files:**
- Modify: `umem_introspect.c` (add a breakpoint predicate engine + a spin-on-condvar stop, and a recording mode).
- Modify: `tools/umemctl.c` (`record <out.log>`, `break <predicate>`, `continue`).

**Interfaces:**
- Produces:
  - **Recording:** `umemctl <pid> record out.log` captures the collector log stream to a file (the "recording of a process collection logs" the user asked for).
  - **BREAK/resume:** when a recording (or the live stream) contains the token `BREAK` (or a client sends `break <predicate>`), the *allocating thread* stops — spin-loops on a condvar — until `umemctl <pid> continue` broadcasts it. This is the "insert BREAK → stop → restart (unblock)" flow, especially useful under GDB.
  - **Predicate: "break just before the allocation of memory that is eventually leaked."** Two-phase: (1) run once with audit + recording to learn which allocation *call sites / sizes / sequence numbers* are never freed (leak set, from `find_leaks`); (2) re-run with `break leaked` — on the Nth allocation matching a known-leaked signature, stop (condvar spin) before returning the buffer, so a debugger attached to the stopped thread sees the exact allocating stack.

- [ ] **Step 1:** Implement the condvar stop primitive in the alloc path, guarded so it only ever arms when `introspect` + a break predicate are active (zero cost otherwise — verify via E1).
- [ ] **Step 2:** Implement predicates: `break size=<n>`, `break cache=<name>`, `break seq=<n>`, `break token=BREAK` (stop when a `BREAK` marker is read from an input recording), and `break leaked` (uses a leak set file produced by a prior `record`+`leaks` run).
- [ ] **Step 3:** Implement the leak-set workflow: `umemctl <pid> record --learn-leaks leaks.set` (phase 1), then `umemctl <pid> break leaked --set leaks.set` (phase 2).
- [ ] **Step 4: Test on EC2** — a program with a deliberate leak; phase-1 learn; phase-2 attach GDB, `umemctl break leaked`, confirm the process stops in the allocator with the leaking call stack visible; `umemctl continue` resumes.
- [ ] **Step 5: Prove no overhead when unused** (E1): break engine disarmed → hot path unchanged.
- [ ] **Step 6: Commit** — `feat(umemctl): record, BREAK/resume, and break-before-leaked-allocation`

### Task G4: Optional TUI monitor

**Files:**
- Modify: `tools/umemctl.c` (add `monitor` using terminal escape codes — **no ncurses dependency**; a full-screen refresh loop over the introspect socket).

**Interfaces:**
- Produces: `umemctl <pid> monitor` — the live TUI the user asked about (currently: none). Panels: per-cache in-use/alloc-rate/free-rate, depot contention, rseq restarts/migrations, RSS, top caches by growth; refresh ~2 Hz.

- [ ] **Step 1:** Implement a plain-ANSI full-screen refresh (clear + home + redraw) reading `stats`/`caches`/contention counters each tick. Keep it dependency-free (YAGNI on ncurses; add only if a reviewer shows it's needed for input handling).
- [ ] **Step 2: Test on EC2** against a running stress process; eyeball + a scripted non-interactive `--once` mode asserted in a test.
- [ ] **Step 3: Commit** — `feat(umemctl): dependency-free live TUI monitor (monitor / --once)`

---

## Workstream H — Feature-level correctness proofs (experimental APIs)

Property/invariant tests for the experimental features, so "works correctly" is proven, not asserted. All run on EC2 under ASan.

### Task H1: Ownership tracking invariants
**Files:** `test/property/prop_ownership.c` (new), wired into build.
- [ ] Every UAF / double-free / borrow-conflict / cross-thread violation the API is documented to catch is caught (positive tests); no false positives on valid ownership transfers (negative tests). Run lightweight + full-debug modes. Commit.

### Task H2: GC invariants
**Files:** `test/property/prop_gc.c` (new).
- [ ] No live (reachable) object is ever swept; all unreachable objects are eventually collected; finalizers run exactly once; concurrent-mark + stop-the-world under multithread stress on `intel-hi`/`arm-hi` (the STW signal path had recent fixes per git log — stress it). Commit.

### Task H3: Profiling round-trip
**Files:** `test/property/prop_profile.c` (new).
- [ ] Record → dump (`umem_profile_dump`) → replay is correctness-neutral (replay never changes program-visible allocation behavior, only pre-warms). Verify `.ump` parse matches recorded stats. Commit.

### Task H4: Budget contexts
**Files:** extend `examples/`/test coverage.
- [ ] Budgets are enforced (over-budget alloc backpressures/fails per policy); parent/child hierarchy frees correctly; shared-memory contexts survive fork. Commit.

---

## Workstream I — Docs & CI truth-up

### Task I1: README/CHANGELOG match measured reality
**Files:** `README.md`, `CHANGELOG.md`.
- [ ] Replace the aspirational perf table with the C2 measured matrix (per arch, per core count). State the honest multi-thread story post-D2. Remove any claim not backed by a committed result. Commit.

### Task I2: Expand CI matrix
**Files:** `.github/workflows/test.yml`.
- [ ] Add `arch: [amd64, arm64]` (GitHub arm runners) for the correctness matrix; keep the heavy high-core perf/stress on EC2 (documented in `scripts/ec2/README.md`, not CI). Add the WS-A/B detection tests and WS-D3 oracle (short run) to `make check`. Commit.

### Task I3: Tooling docs
**Files:** `tools/DEBUGGER_QUICKREF.md`, new `docs/UMEMCTL.md`.
- [ ] Document the now-real GDB/LLDB commands and the full `umemctl` workflow (logtail, monitor, record, BREAK/resume, break-before-leaked). Commit.

---

## Dependency graph & parallelization (for the agent team)

```
Task 0 (EC2 infra) ─────────────────────────────► prerequisite for ALL
                     │
   ┌─────────────────┼───────────────────────────────────────┐
   ▼                 ▼                     ▼                   ▼
 WS-A (harness)   WS-C (bench stabilize)  WS-F (gdb/lldb)   WS-H (feature proofs)
   │                 │                     │                   
   ▼                 ▼                     │                   
 WS-B (detection    WS-D (scaling fix,     │
 fix; needs A3      needs C1/C2)           │
 RED tests)         │                      │
   │                 ▼                     ▼
   │              WS-E (zero-cost)      WS-G (umemctl; F1 core informs it)
   └──────────────────┴──────────────────┴───► WS-I (docs/CI truth-up, LAST)
```

- **Immediately parallel after Task 0:** WS-A, WS-C, WS-F, WS-H (independent).
- **WS-B** starts once **A3** lands the RED detection tests.
- **WS-D** starts once **C1+C2** give stable numbers + the scaling curve.
- **WS-E** consumes C1's harness; **WS-G** benefits from F1's walk logic.
- **WS-I** runs last (it truths-up docs against everyone's committed results).

**Sub-agent dispatch:** one agent (or small worker→reviewer team per the `subagent-teams` skill) per workstream. Each agent owns its EC2 instance role for its runs and **must terminate it** (or return it to the pool) when idle. Reviewers gate each task at its independently-testable deliverable.

---

## Self-review (spec coverage)

- (a) prove correctness → WS-A (reach init paths), WS-B (detection actually fires), WS-D3 (concurrency oracle), WS-H (feature invariants). ✅
- (b) prove no undue perf impact → WS-C (stable ground truth + gate), WS-D (fix the real regression), WS-E (measured zero-cost-when-disabled). ✅
- keep essence, address perf → WS-D2 (smallest fix per bottleneck), WS-E2 (gate compounding features off the hot path), WS-I1 (honest docs). ✅
- EC2 build/test, spare `floki`, no OOM → Global Constraints + Task 0 (all heavy work remote); burner account, terminate-when-idle. ✅
- Intel + aarch64, low + very-high core → matrix in Global Constraints; WS-C2/D exercise all four roles. ✅
- empirically isolate perf/overhead → WS-D1 (counters + perf/flamegraph), WS-E1 (per-feature overhead). ✅
- review + improve GDB/LLDB → WS-F (stubs → real slab/audit inspection). ✅
- CLI runtime inspection (mdb replacement) → WS-G. ✅
  - stream log-like messages → G2 `logtail`. ✅
  - TUI for live process → G4 `monitor`. ✅
  - record + BREAK-stop(condvar)/resume → G3. ✅
  - "stop just before allocation of eventually-leaked memory" → G3 `break leaked`. ✅
- collector bug-free & predictable, never over-limits user process → global invariant enforced per task; opt-in + zero-cost-when-off for all inspection/experimental machinery. ✅

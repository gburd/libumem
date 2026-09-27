# @r7verify: `make check` on origin/master with aa36fdd (getpcstack fix)

- **Ref:** `origin/master` = `fbedab9b1380d1d65f211dba5c3db16c94b9562a`. It contains
  `aa36fdd`, and `aa36fdd~1..fbedab9` changes no `.c`/`.h` file except `getpcstack.c`.
- **Isolation:** `scripts/ec2/verify-isolated.sh` (`git archive`, committed content
  only). Each config was copied from that pristine tree into its own directory and
  built there. `ISOLATED_PROVENANCE` sits next to every config's logs.
- **Boxes:** `intel-lo@r7verify` c7i.2xlarge (8 vCPU), `arm-lo@r7verify` c7g.2xlarge
  (8 vCPU). Debian 13, kernel 6.12.107+deb13-cloud, gcc 14.2.0-19, `AWS_PROFILE=hotdog`
  (account 373102893032). Both instances are terminated.
- **Command:** one job per box, `bash ~/r7v_run.sh r7v <configs>` (`r7v_run.sh`
  is in this directory). For each config in turn it runs
  `clean-regen.sh <flags>`, then `make -j8`, then `make check -j8`, all in the same job.

## Totals (first run, 2026-09-27)

| config | box | TOTAL | PASS | SKIP | XFAIL | FAIL | XPASS | ERROR | make check rc |
|---|---|---|---|---|---|---|---|---|---|
| x86_64 default | intel-lo | 67 | 52 | 12 | 2 | **1** | 0 | 0 | 2 |
| x86_64 `--enable-introspect` | intel-lo | 67 | 61 | 3 | 3 | 0 | 0 | 0 | 0 |
| x86_64 `--enable-introspect --enable-frame-pointers` | intel-lo | 67 | 62 | 2 | 3 | 0 | 0 | 0 | 0 |
| aarch64 default | arm-lo | 67 | 53 | 12 | 2 | 0 | 0 | 0 | 0 |
| aarch64 `--enable-introspect` | arm-lo | 67 | 62 | 2 | 3 | 0 | 0 | 0 | 0 |

## The two target tests (test-driver `exit status`)

| config | test_inspect_e2e.sh | test_tool_T9_leak_stack.sh |
|---|---|---|
| x86_64 default | PASS (0) | SKIP (77) |
| x86_64 introspect | PASS (0) | SKIP (77) *(x86_64, no frame pointers)* |
| x86_64 introspect + frame-pointers | PASS (0) | PASS (0) |
| aarch64 default | PASS (0) | SKIP (77) |
| aarch64 introspect | PASS (0) | PASS (0) |

test_inspect_e2e passed in all five configs, so there is no failing assertion to report.
T9 on x86_64 introspect without frame pointers SKIPs as db4ac62 intends. In both default
configs T9 SKIPs through `skip_unless_introspect`. The logs are
`*/<config>/test/debugger/*.log`.

## The one FAIL: `test/stress/interpose_regress.sh`, x86_64 default

The failing check is the P8.3 per-call-cost gate (`repro_interpose_free_ratio`,
bar 0.48). The other four interpose_regress cases passed.

    t=8 9 pairs: null API/API median 1.025 min 0.628; preload/API median 0.476 min 0.375; bar 0.48
    RESULT: FAIL (preload/API 0.476 < 0.48 at t=8; the interposer's per-call cost is back)

The original log was fetched before any re-run:
`intel-lo-r7verify-r7v/x86-default/test/stress/interpose_regress.log` and `test-suite.log`.

Re-run afterwards (`intel-lo-r7verify-r7v-diag/`). Same box, same x86-default build
directory, same binaries:

| run | preload/API median | null API/API min | result |
|---|---|---|---|
| first run (full `make check -j8`) | **0.476** | 0.628 | FAIL |
| isolated `TESTS=interpose_regress.sh` #1 | 0.576 | 0.994 | PASS |
| isolated #2 | 0.554 | 0.953 | PASS |
| isolated #3 | 0.567 | 0.982 | PASS |
| full `make check -j8` #1 (67 tests: 0 FAIL) | 0.563 | 0.627 | PASS |
| full `make check -j8` #2 (67 tests: 0 FAIL) | 0.629 | 0.735 | PASS |

The same gate in the other first-run configs gave x86 introspect 0.576, x86
introspect+fp 0.540, arm default 0.540, arm introspect 0.547. All passed.

**What is shown:**
- The failure did not reproduce in five re-runs on the same binaries.
- The null control's minimum was 0.63 to 0.74 in every full `-j8` run (fail or pass) and
  0.95 to 0.99 in isolated runs. The measurement is noisier while the suite runs alongside it.
- aa36fdd touches only `getpcstack.c`, and getpcstack is not on the preload free path.

**What is not shown:** the cause of the 0.476. The null-control spread points to
co-scheduled `make check -j8` load, but nothing here proves it. The first run's
median sits just under the bar. The pre-fix values were 0.409 / 0.395, and the
post-fix median documented in the test header is 0.61. This gate is therefore
thin at `-j8` on an 8-vCPU box. It should be treated as an open flake, and this
note should not be read as proof that aa36fdd is clean on x86 default.

## Files

- `r7v_run.sh` is the per-config harness that ran on the boxes.
- `<role>-r7v/out.log` is the job transcript. `<config>/` holds `check.log`,
  `totals.txt`, `test-suite.log`, the two target tests' `.log`/`.trs`, any
  non-passing test's `.log`/`.trs`, and gzipped `build.log` and `config.log`.
- `intel-lo-r7verify-r7v-diag/` holds the re-run transcript and both full-suite `check*.log` files.

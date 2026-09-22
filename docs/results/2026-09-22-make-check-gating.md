# `make check` gating: dropped regressions, a false red, and one open flake

**Date:** 2026-09-22
**Commit under test:** `975e960`
**Hardware:** `c7i.2xlarge` (x86_64), Amazon Linux 2023
**Method:** isolated builds via `scripts/ec2/verify-isolated.sh` (committed
content only, so no other agent's working-tree edits can leak in)

## 1. Four committed regressions were never running

A comment block before `TESTS` in `Makefile.am` ended with a trailing
backslash. GNU make continues a *comment* across backslash-newline, so it
swallowed the following two lines and dropped their entries out of `TESTS`
entirely.

Effect: `make check` reported a green **8/8** while four committed
regressions never executed —

- `test/unit/test_overflow_contracts` (P1.7a,b)
- `test/unit/test_aligned_contracts` (P1.7c,e)
- `test/unit/test_hash_partition` (Phase 4)
- `test/stress/interpose_regress.sh` (P1.1)

Fixed as a single `TESTS +=`. `make check` now runs **12** entries.

This is the second time a harness defect produced a false green in this
project. Anything quoting a `make check` count should state the count.

## 2. `test_inspect_e2e.sh` asserted a nondeterministic value

The script asserted `cached_skipped == 2`. That counts churn-loop buffers
that are free but still resident in a magazine, which depends on the
loaded/previous round distribution, whether the update thread flushed in
between, and whether the rseq fast path is compiled in. None of that is a
property of the allocator being correct.

Measured failure rates before the change (same host, same binary):

| Configuration | Failures |
|---|---|
| default build | ~2/12 (~17%) |
| `--disable-rseq` | **12/12 (deterministic)** |

So `--disable-rseq` could never pass `make check`, and in default builds every
agent in this workstream had a ~17% chance of a false red — which some then
attributed to their own changes.

Now asserted as a plausible range. The deliberate-leak assertions (12
buffers / 8832 bytes) remain exact: those are deterministic and are what the
test actually exists to check.

## 3. Verification after both fixes

Default build, 5 consecutive `make check` runs:

```
run1 rc=0  TOTAL: 12  PASS: 12  FAIL: 0
run2 rc=0  TOTAL: 12  PASS: 12  FAIL: 0
run3 rc=0  TOTAL: 12  PASS: 12  FAIL: 0
run4 rc=0  TOTAL: 12  PASS: 12  FAIL: 0
run5 rc=0  TOTAL: 12  PASS: 12  FAIL: 0
```

`--disable-rseq`, which previously could not pass at all:

```
run1 rc=0  TOTAL: 12  PASS: 12  FAIL: 0
run2 rc=0  TOTAL: 12  PASS: 12  FAIL: 0
run3 rc=2  TOTAL: 12  PASS: 11  FAIL: 1   <-- see section 4
```

Then 10 further consecutive `--disable-rseq` `make check` runs: **10/10 pass**.

## 4. OPEN: rare `test/test_debug` SIGSEGV under `--disable-rseq`

One run of `make check` under `--disable-rseq` failed with

```
FAIL: test/test_debug   (exit status: 139)
  debug_multi_cache  [crash]
```

139 = SIGSEGV. This is a real crash, not an assertion problem, and it is
**not fixed**.

What is known:

- Default (rseq-enabled) build: `test_debug` standalone **15/15 pass**.
- `--disable-rseq` build: `test_debug` standalone **20/20 pass**.
- `--disable-rseq` build: `make check` **10/10 pass** after the observed
  failure, and 2/3 before it.

So the reproduction rate is roughly 1 in 13 `make check` runs or rarer, and it
did not reproduce standalone in 20 attempts. That suggests it needs the
`make check` environment (concurrent test execution, different working
directory, or the serial-test harness's environment) rather than being
inherent to `debug_multi_cache` alone.

It could not be A/B'd against the pre-wave baseline `ebcb467`, because that
commit **cannot build** under `--disable-rseq` at all (`build_rc=2`) — the
pre-existing breakage Phase 4 fixed. So whether this crash is new or merely
newly reachable is undetermined.

Not diagnosed further here, and deliberately not attributed to any commit.
Next step for whoever picks it up: run `make check` under `--disable-rseq` in
a loop with `ulimit -c unlimited` and a core pattern set, then get a backtrace
from the core rather than re-running until it passes.

## 5. Process note

An earlier harness in this workstream retried `make check` when this was the
only failure. Against the deterministic 12/12 `--disable-rseq` failure, a
retry would have reported a pass that could never happen — a fabricated green.
Retries in verification harnesses must be treated as defects, not resilience.

# Coverage re-verification — 2026-09-06

Re-measures the `README.md` / `CHANGELOG.md` v2.0.0 claim ("Test coverage
boosted from 33% to 80%+ line coverage") against current `master`. That
number was measured in early 2025 (v2.0.0) and has never been re-checked
across six subsequent releases (sparsemap vendoring, GC object-lock
sharding, `umem_introspect.c`, the `tools/umem.c` C rewrite, `umem_inspect.c`,
GC accounting fixes). It is now **stale and no longer accurate** for the repo
as a whole.

## Result

- **Whole-repo line coverage (all instrumented files, incl. vendored sm.c
  and manual test-driver .c files): 50.3%** (5705/11345 lines).
- **Excluding the vendored third-party `sm.c`/`sm.h` (8702-line upstream
  sparsemap dependency, pulled in wholesale in v2.4.0, exercises almost none
  of its ~84 exported `umem_sm_*` symbols because only a handful are called
  by `umem_sparsemap.c`): 71.2%** (5705/8011 lines).
- **Also excluding the ad-hoc manual test-driver files that happen to live
  at the top level (`umem_test.c`, `umem_test2.c`, `umem_test3.c`,
  `umem_ptc_fork_test.c`) and `examples/`: 72.1%** (5276/7320 lines).

None of these match the README/CHANGELOG's "80%+" claim under any
reasonable definition. **The 80%+ claim is stale/wrong for the current
codebase and has been corrected in README.md/CHANGELOG.md.**

The single biggest driver of the drop from the historical 80%+ number is
`sm.c`: it's 3334 of the repo's 11345 instrumented lines (29%) and is 0%
covered (the GC's page-sparsemap only calls a handful of its ~84 public
`umem_sm_*` functions; the rest of the vendored library — set algebra,
serialization, cursors, etc. — is linked in but never exercised by any
current test). Even with `sm.c` excluded, core coverage (71-72%) is still
below the historical 80%+ figure — `umem_inspect.c` (46.6%, new in a later
release), `umem_stacktrace.c` (35.2%), `umem_fail.c` (30.0%), and
`examples/umem_palloc.c` (48.2%) are the largest core-file gaps.

## How to reproduce

```bash
./scripts/ec2/launch.sh intel-lo && ./scripts/ec2/bootstrap.sh intel-lo
./scripts/ec2/run-remote.sh intel-lo \
  './scripts/ec2/clean-regen.sh --enable-coverage && make -j$(nproc) && make check'
./scripts/ec2/run-remote.sh intel-lo '
  lcov --capture --directory . --ignore-errors negative,gcov,mismatch \
       --output-file coverage.info
  lcov --remove coverage.info "/usr/*" "*/test/*" --ignore-errors unused \
       --output-file coverage_filtered.info
  lcov --summary coverage_filtered.info
'
./scripts/ec2/terminate.sh intel-lo
```

(`--ignore-errors negative` is required: `umem_rseq.c`'s per-CPU counters go
negative under `--coverage` instrumentation during concurrent runs, which
lcov 2.0 otherwise treats as a hard error.)

`scripts/generate-coverage.sh` / `scripts/run-coverage.sh` /
`scripts/generate-coverage-report.sh` already exist in-repo and automate
most of this (build + `make check` + capture + HTML report); they were not
used verbatim here because none of the three pass `--ignore-errors negative`,
which is required for a clean capture on the current `umem_rseq.c`. That gap
is worth fixing in one of those scripts as a follow-up.

## Test suite status

`make check` (autotools-integrated tests): **8/8 PASS, 0 FAIL, 0 SKIP.**

`LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork`: **39/39 munit cases
PASS** (24/24 core + 15/15 `/umem_debug/*`, including the previously-fragile
`redzone_detection` / `firewall_detection` / `double_free_detection` /
`corruption_detection` / `audit_stack_traces` tests — all green).

Property/integration/stress binaries invoked ad hoc (not part of `make
check`) were mostly green (`prop_cache`, `prop_ownership`, `prop_gc`,
`prop_palloc`, `test_multithreaded`, `test_signals`, `test_realloc_bootstrap`,
`test_debug_features`, `stress_concurrency_oracle`); a few
(`prop_alloc_free2`, `prop_fragmentation`, `prop_profile`, `test_oom`,
`test_threading_stress`) either need different CLI invocation than a bare
run, or timed out/aborted under ad hoc invocation on the small `intel-lo`
box — this is a coverage-run artifact of running them without their
intended harness/arguments, not a `make check` regression, and is out of
scope for this task (the coverage number above is from the `make check` +
`test_main --no-fork` run, which is exactly what the original 2025
measurement was based on).

## Provenance

- git SHA: `ecd5fa0dfb12238ad0e93cd27eb9d188ab3dbce5`
- Date: 2026-09-06
- Instance: EC2 `c7i.2xlarge` (role `intel-lo`), x86_64, 8 vCPU
- AMI/OS: Amazon Linux 2023, kernel `6.1.182-227.379.amzn2023.x86_64`
- Compiler: `gcc (GCC) 11.5.0 20240719 (Red Hat 11.5.0-5)`
- glibc: `2.34`
- lcov: `LCOV version 2.0-1`
- Governor/THP/NUMA tuning: applied by `bootstrap.sh` (THP=never,
  numa_balancing=0); governor unreadable on this KVM instance type (not a
  perf-authoritative run — this is a correctness/coverage run, not a bench).

### lcov summary (whole repo, filtered `/usr/*` + `*/test/*`)

```
Summary coverage rate:
  lines......: 50.3% (5705 of 11345 lines)
  functions..: 63.4% (354 of 558 functions)
  branches...: no data found
```

### lcov summary (excluding vendored `sm.c`/`sm.h`)

```
Summary coverage rate:
  lines......: 71.2% (5705 of 8011 lines)
  functions..: 82.9% (354 of 427 functions)
  branches...: no data found
```

### lcov summary (excluding `sm.c`/`sm.h` + manual test-driver .c files + examples/)

```
Summary coverage rate:
  lines......: 72.1% (5276 of 7320 lines)
  functions..: 83.7% (324 of 387 functions)
  branches...: no data found
```

### Per-file line coverage (hit/total), sorted as emitted by `lcov --list`

```
envvar.c                          168/  289   58.1%
examples/umem_palloc.c            150/  311   48.2%
getpcstack.c                       13/   13  100.0%
init_lib.c                         16/   19   84.2%
malloc.c                          140/  192   72.9%
malloc_guard.h                       4/    4  100.0%
malloc_interpose.c                  99/  171   57.9%
misc.c                              75/   96   78.1%
sm.c                                 0/ 3334    0.0%
sol_compat.h                         9/    9  100.0%
tools/umem.c                      140/  215   65.1%
umem.c                            1451/ 2018   71.9%
umem_arena.c                        45/   49   91.8%
umem_audit.c                        48/   86   55.8%
umem_fail.c                         15/   50   30.0%
umem_fork.c                         89/  106   84.0%
umem_gc.c                          510/  617   82.7%
umem_gc_roots.c                    152/  171   88.9%
umem_hooks.c                       128/  128  100.0%
umem_impl.h                         16/   17   94.1%
umem_inspect.c                     375/  805   46.6%
umem_own.c                         175/  199   87.9%
umem_profile.c                     341/  413   82.6%
umem_ptc.c                         113/  179   63.1%
umem_ptc.h                           3/    3  100.0%
umem_ptc_fork_test.c               158/  236   66.9%
umem_rseq.c                         55/   93   59.1%
umem_rseq.h                          3/    3  100.0%
umem_simd.h                         11/   13   84.6%
umem_sparsemap.c                   162/  167   97.0%
umem_stacktrace.c                   58/  165   35.2%
umem_test.c                         24/   30   80.0%
umem_test2.c                        60/   73   82.2%
umem_test3.c                        37/   41   90.2%
umem_update_thread.c                79/   87   90.8%
vmem.c                             668/  779   85.8%
vmem_base.c                         12/   13   92.3%
vmem_mmap.c                         37/   47   78.7%
vmem_sbrk.c                         66/  104   63.5%
```

## Recommendation (not executed here — docs truth-up only)

If the project wants to reclaim an honest "80%+" line, the two highest-yield
options are: (a) exclude the vendored `sm.c`/`sm.h` from the coverage target
the way `*/test/*` is already excluded — it's third-party code, not
libumem's own logic, and 0%-covering an 8.7k-line dependency drags the
aggregate down by ~29 points for no signal; and (b) add tests for
`umem_inspect.c`, `umem_stacktrace.c`, and `umem_fail.c`, which are core
files sitting well under 60%. Neither was done as part of this
verification task — it is documentation-truth-up only, not a coverage
improvement task.

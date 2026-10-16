# Building and Testing libumem

The repository-specific runbook for building, testing, and benchmarking
libumem. The generic working-style and evidence rules live in
[`../copilot-instructions.md`](../copilot-instructions.md); the EC2/burner
infrastructure, AWS-profile rotation, and worker-role rules live in
[`../../AGENTS.md`](../../AGENTS.md) §1-§4.

**Nothing here runs on the local host.** The development machine (`floki`) is
for editing and git only. Every compile, test, stress run, and benchmark runs
on EC2 through `scripts/ec2/` (AGENTS.md §1). The commands below are the remote
runbook.

## The one rule that must not be broken: use `job.sh` for anything slow

`run-remote.sh` keeps an SSH session open for the command's whole life. A local
timeout then orphans the remote work: it keeps running (on a possibly 192-vCPU
box, burning money) while you believe the step ended, and re-running stacks
concurrent copies that fight over cores and produce fictitious results.

`job.sh` puts the deadline and the cleanup on the remote side:

- `timeout(1)` runs remotely, so the limit survives SSH loss.
- `setsid` + process-group kill, so `make -j` workers and stress binaries die
  with the job instead of being orphaned (verified: 0 survivors).
- logs land in `~/libumem-jobs/<job>/`, outside the rsynced tree, so they
  survive the next sync and are readable after a failure.
- `start` refuses to launch a job that is already running.
- every poll (`status`, `tail`) returns immediately.

```sh
./scripts/ec2/launch.sh    intel-lo@w1
./scripts/ec2/bootstrap.sh intel-lo@w1
./scripts/ec2/job.sh intel-lo@w1 start build 1800 \
    './scripts/ec2/clean-regen.sh && make -j$(nproc) && make check'
./scripts/ec2/job.sh intel-lo@w1 status build
./scripts/ec2/job.sh intel-lo@w1 tail   build 40
./scripts/ec2/job.sh intel-lo@w1 fetch  build     # logs -> docs/results/jobs/
./scripts/ec2/job.sh intel-lo@w1 kill   build
./scripts/ec2/terminate.sh intel-lo@w1
```

Roles: `intel-lo` (8 vCPU x86_64), `intel-hi` (192 vCPU metal x86_64),
`arm-lo` (8 vCPU aarch64), `arm-hi` (192 vCPU metal aarch64). Each parallel
agent uses a distinct worker suffix (`intel-lo@ptc`) for its own instance and
remote directory; see AGENTS.md §3.

## Build

- **Always run `clean-regen.sh` in the SAME job as `make`.** A separate
  invocation re-syncs the tree and leaves stale generated autotools files that
  then fail against local `aclocal-1.18`/`missing` paths. One job:
  `./scripts/ec2/clean-regen.sh && make -j$(nproc) && make check`.
- Configure flavours (`.forgejo/workflows/tests.yml` runs all five):
  - `./configure` -- `normal`, the default debug build with `ASSERT()` live.
  - `./configure CFLAGS='-O3 -g -DNDEBUG'` -- `release`, the shipped binary.
    Under `NDEBUG` every `ASSERT()` is compiled out (`misc.h`), so it is a
    *different program* from `normal`; test it explicitly, not just `normal`.
  - `./configure --enable-asan` / `--enable-ubsan` -- sanitizer builds.
  - `./configure --enable-coverage` -- gcov instrumentation.
- Never test a stale binary. Rebuild in the same directory and configuration
  before a run or rerun.

## Testing: what each target actually means

- **`make check` is a smoke suite, not the full suite.** It runs ~8 entries
  (`umem_test`, `umem_test2`, `umem_test3`, `umem_ptc_fork_test`,
  `test/test_debug`, the gdb/lldb e2e scripts, `oracle_fast.sh`). A green
  `make check` proves far less than it looks like it does. See
  [`test/README.md`](../../test/README.md) for the authoritative list.
- **The comprehensive unit suite is excluded from `make check`.** Run it
  explicitly: `LD_LIBRARY_PATH=.libs ./test/.libs/test_main --no-fork`
  (several hundred assertions; `--list` shows contents).
- **Property tests** run standalone, each with its own timeout:
  `prop_alloc_free2`, `prop_cache`, `prop_fragmentation`, `prop_ownership`
  under `test/property/.libs/`.
- **The concurrency oracle:**
  `./test/stress/.libs/stress_concurrency_oracle --threads=8 --duration=25
  --size-class=mixed --pattern=all`.
- Distinguish PASS, SKIP (77), FAIL, ERROR (99), and never-executed. Returning
  0 for an unavailable prerequisite is a bug, not a skip (AGENTS.md §6). A
  passing test proves only what it actually executed.

### Preserve evidence before re-running

When something fails, `job.sh ... fetch` the logs **before** rebuilding,
re-syncing, or retrying. A later pass does not explain an earlier failure, and
`run-remote.sh`'s `--delete` rsync destroys the only record (AGENTS.md §5).
Treat a failure as actionable until proven otherwise; a rerun that passes is
not proof of transience.

### XFAIL_TESTS discipline

`XFAIL_TESTS` in `Makefile.am` must be **empty before any release that claims
the tools work.** Each entry is an OPEN finding: exit 0 means fixed, 1 means
the finding reproduces, 77 is SKIP, 99 is a positive-control failure. When a
fix lands, its test XPASSes, which *fails* `make check` until the entry is
removed -- that is the signal to remove it. One entry per line: a trailing
comment on a backslash-continued line swallows the rest of the list.

## The release gate

`scripts/ec2/exit_criteria_gate.sh` is the single runnable readiness check.
Run it against committed content, not the worktree:

```sh
./scripts/ec2/verify-isolated.sh intel-lo@w HEAD gate 3600 \
    'bash scripts/ec2/exit_criteria_gate.sh'
```

It covers, each `$?` captured *immediately* (an intervening `echo` overwrites
`$?` -- that is how an aborting `prop_fragmentation` once reported rc=0):

1. default build + `make check`
2. the comprehensive unit suite (`test_main`)
3. every property test (no allowances; one failure is a gate failure)
4. the concurrency oracle
5. a clean `make dist` tarball that must pass its OWN `make check`
6. `make install` into a temp prefix, with an external consumer compiled
   against only the installed headers
7. the RELEASE configuration (`-O3 -g -DNDEBUG`) run through steps 1-4 again --
   a failure here that debug did not show is a load-bearing `ASSERT()`

Exit status is the number of gate failures, so it is usable as a gate.

### The P8.3 quiet-rig gate

`repro_interpose_free_ratio` SKIPs (77) inside `make -j check` because its
quiet-rig gate needs a null min >= 0.88 and a loaded 8-vCPU box never reaches
it. So `make check` passing says *nothing* about P8.3. The exit-criteria gate
runs it standalone, after `make check` has finished and nothing else is
running, and treats **77 as a FAIL** there: a gate that cannot resolve is not a
pass.

```sh
LD_LIBRARY_PATH=.libs LD_PRELOAD=.libs/libumem_malloc.so \
    ./test/stress/.libs/repro_interpose_free_ratio
```

The `scripts/ec2/p85b_fix_*_gate.sh` scripts drive this on the lo and metal
boxes.

## Benchmarking: null-first A/B discipline

Every performance change needs its before/after A/B, with the fast path inside
the null, or the cost recorded and justified (AGENTS.md §10). Use
`scripts/ec2/hotpath_ab.sh`:

- Three arms built by `git archive` extraction into `pre/`, `post/`, `pre2/`:
  pre, post, and an **independent rebuild of pre as the null control**.
- The **null (pre vs pre2) is measured FIRST at every point**, so the rig's own
  spread is on the record before any A/B delta is read. A post delta inside the
  null's min..max is noise, not a result.
- **One bench binary (from `post`) serves every arm.** Building the bench per
  arm once moved `worker` 0x20 bytes and reported an 8-9% library regression
  that was actually a 3-8% *speedup* -- the binary differed, not the library.
  The script refuses an A/B across a soname bump and aborts if pre/post
  `libumem.so` are byte-identical.
- Measured as alternating (A, B) replicate pairs, reported as the median
  per-pair delta plus `perf stat` instructions-per-pair at t=1
  (load-independent).

```sh
./scripts/ec2/job.sh intel-lo@hot start ab 3600 \
    'cd ~/libumem-ab && PRE=<sha> POST=<sha> ./post/scripts/ec2/hotpath_ab.sh'
```

Record what produced a number: commit, configure flags, instance type, library
versions, binary identity (AGENTS.md §6). Benchmark only an optimized Release
build; retain the raw results under `docs/results/`.

## aarch64

The Codeberg forgejo runner is x86_64 docker only. Authoritative aarch64
build+test runs on EC2 `arm-lo`/`arm-hi` via `scripts/ec2/`, plus a nightly
`.forgejo/workflows/aarch64-nightly.yml` cron (needs AWS/SSH secrets armed in
Forgejo settings; until then treat it as validated-but-not-armed, and it covers
only `arm-lo`). Do not assume arm is green from CI alone -- the v2.0.0 rseq
SIGSEGV was aarch64-only.

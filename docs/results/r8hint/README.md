# r8hint: O1 (test_cpu_hint_spread measured the scheduler) and O10 (norseq 54/13 vs 57/11)

Debian 13 (trixie), gcc 14, c7i.2xlarge (x86_64) and c7g.2xlarge (aarch64),
8 vCPU each, `verify-isolated.sh` (committed content only). Agent @r8hint.
Raw logs are in `jobs/`, and the runner is `cpuhint_runs.sh` (20 runs idle, then 20 with
64 busy loops started first).

## O1: chosen fix is option (a), pin each worker

Test commit a0584d3 (test only). Worker i pins itself to `allowed[i % n]` from
`sched_getaffinity()` and only then makes its first allocation. The test then
requires the per-CPU cache of every pinned worker's CPU
(`cpu & cache_cpu_mask`) to have `cc_alloc != 0`. The bar is exact: all 8, not
half. The test returns SKIP 77 if pinning is refused, if fewer than 2 CPUs are
allowed, or if the CPUs map to fewer than 2 caches.

Why (a) and not (b): pinning removes the scheduler from the measurement, so
the bar can be exact. (b) would keep a ratio against whatever CPUs the load
allowed. The evidence below also shows that the old half bar was weaker than
its header claimed ("pre-fix: 1 or 2 caches"). Unpinned, pre-fix used 1 cache in
40/40 runs, but pinned it used up to 5, and 4 of 80 pinned pre-fix runs used 4 or
5, which clears a >= 4 bar. The
exact bar catches both. (c), a quiet-rig gate, would give no verdict inside
`make check`.

| build | test | x86_64 idle | x86_64 +64 busy | aarch64 idle | aarch64 +64 busy |
|---|---|---|---|---|---|
| a7bcdc4 (P8.2 pre-fix, parent of 20999ee) | **new** | FAIL 20/20 | FAIL 20/20 | FAIL 20/20 | FAIL 20/20 |
| a7bcdc4 | old (a7bcdc4's own) | FAIL 20/20 | FAIL 20/20 | FAIL 20/20 | FAIL 20/20 |
| a0584d3 (master + new test) | **new** | PASS 20/20 | PASS 20/20 | PASS 20/20 | PASS 20/20 |
| a0584d3 | old (a0584d3^'s) | PASS 20/20 | **FAIL 10/20** | PASS 20/20 | PASS 20/20 |
| a0584d3 `make check -j8` | | 70: 58 PASS / 12 SKIP / 0 FAIL | | 70: 58 PASS / 12 SKIP / 0 FAIL | |

- Pre-fix with the new test (both arches together, 80 runs): 1 cache used 33x, 2 used 11x,
  3 used 32x, 4 used 2x, 5 used 2x. In every run 3-7 of the 8 pinned workers'
  caches were empty. Every FAIL is the test's own `RESULT: FAIL` with rc 1.
- The old test on current code at +64 busy loops, x86: distinct caches 2-7,
  10/20 below 4. This reproduces the original O1 failure on the same box that
  gave the new test 20/20.
- After the fix, pinning held in every run: `ran_on == cpu` for all 8 workers.
  With rseq on, a worker's cache can show `cc_alloc` as low as 2, because the
  rseq slot serves the loop and `cc_alloc` counts only refills. It is never 0,
  because the slot starts empty and its first refill goes through the hint.
  The header now says this.
- The pre-fix build needs `CFLAGS=-Wno-error=implicit-function-declaration`.
  gcc 14 rejects a7bcdc4's undeclared `numa_*` calls, and that tree predates the
  fix for them. **The first pre-fix attempt (`jobs/*-r8hint-r8hint`, "PRE"
  lines) is void:** the build failed, every run exited 127, and the first
  version of `cpuhint_runs.sh` counted 127 as FAIL. Commit 60b0f32 fixed the
  runner: only `RESULT: FAIL` counts as FAIL, and a crash or 127 counts as
  ERROR. The table's pre-fix rows come from the re-run (`jobs/*-r8hint-r8hpre`, 0 ERROR).

## O10: norseq 54/13 vs 57/11 is vm.max_map_count, not flakiness

Both runs had 68 tests. The only SKIP difference is `test_heap_ceiling` and
`test_heap_ceiling_512`. Both correctly return 77 when `vm.max_map_count` >
200000, because a raised limit cannot tell the fix from a permissive kernel.
Debian 13 ships 1048576 (`/usr/lib/sysctl.d/50-default.conf`).
`r7rel_run.sh` runs its norseq step before step 5, and step 5
(`p85b_fix_lo_gate.sh`) is the first thing that sets 65530. So the first run
saw 1048576 and skipped both tests: 13 SKIP. The 32e1bca re-run came after step 5 had set 65530, so both tests PASSed: 11 SKIP. Its log does
not record the limit; the reproduction below shows the mechanism. The interpose_regress FAIL
became a PASS in the re-run, which accounts for the third PASS
(54 + 2 + 1 = 57). Reproduced at 32e1bca, x86 `--disable-rseq`
(`jobs/intel-lo-r8hint-o10`): 55 PASS / 13 SKIP at 1048576 and 57 PASS / 11 SKIP at 65530,
same build, back to back. No FAIL in either, so interpose_regress PASSed both
times here.
**This is not a test bug.** The prerequisite, a default map limit, really was
absent, and the SKIP message says so. **It is a harness gap:**
`exit_criteria_gate.sh` never sets the limit, so on Debian the release gate's
`make check` has never run the ~5 GB ceiling regression. `r7rel-w-gate`,
`-introspect` and `-introspect-fp` all show it SKIPped on both arches. Only
the P8.5b step exercises it. That fix belongs in `scripts/ec2/`, which is
outside this task's scope, so it is left for the coordinator.

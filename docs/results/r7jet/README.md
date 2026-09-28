# r7jet: bench_main -a exit codes, and "jemalloc stopped loading"

Box: one c7i.2xlarge (x86_64), Debian 13, gcc 14.2.0, glibc 2.41-12+deb13u4,
libjemalloc2 5.3.0-3. Every build is an isolated `git archive` (verify-isolated.sh)
with clean-regen + make in the same job. Logs: `intel-lo-r7jet-*/out.log`.

## Part A: test_bench_missing_alloc.sh
- a536bd2 (test only, `pa-test`): `unavailable umem-preload rc=0; unknown allocator rc=0`, **TEST_EXIT=1**.
- b5213ae (fix, `pa-fix`): `rc=2; rc=2`, RESULT: PASS, **TEST_EXIT=0**.
- `make check -j8` at b5213ae, x86 default configure (`pa-fix-check/`):
  TOTAL 68, PASS 53, SKIP 12, XFAIL 2, FAIL 1, XPASS 0, ERROR 0. The one FAIL is
  `test/stress/interpose_regress`, the free() per-call cost timing check
  (preload/API median 0.419 < bar 0.48 at t=8). It is the same check that failed
  once with 0.476 in r7verify/round7. It was not re-run here, and b5213ae touches
  only test/bench.

## Part B: this is not a regression. It is a misreading.
The r7samp README said jemalloc loaded at 2e88c21 and stopped loading since. That
is false. At 2e88c21 with GLIBC_TUNABLES unset (`pb-2e88c21`; `pb-ctl` shows the
env and dotfiles are clean):
- `bench_main -a jemalloc -w single -n 1000 -s 16:16 -c` prints **no row**. It
  exits 0, because 2e88c21 predates b5213ae. With the tunable it prints a row.
- The r7harness-style `perf record ... -a jemalloc -w frag -t 8 ... -D 10:2 -W 1`
  also prints no row.
- A 5-line program that is **not linked to libumem** fails too:
  `dlopen("libjemalloc.so.2")` gives `cannot allocate memory in static TLS block`.
  With the tunable it gives OK. So this glibc cannot dlopen jemalloc at all without
  the tunable, with or without libumem.

The TLS sizes at good and bad are the same (`readelf -lW`, PT_TLS MemSiz):

| object | 2e88c21 | b5213ae |
|---|---|---|
| .libs/libumem.so.1 | 0x10b4 (STATIC_TLS) | 0x10b4 (STATIC_TLS) |
| test/bench/.libs/bench_main | 0x4 | 0x4 |
| libjemalloc.so.2 | 0xa48 | 0xa48 |

So no bisect was needed. r7harness got its jemalloc rows because the tunable was
set in that run. Its proof script was never committed, so how it was set is not
on record. The binary hashes in r7harness/lo-t8/provenance.txt do not match this
rebuild, so byte-for-byte identity was not checked.

## The real defect: callers that request jemalloc without the tunable
I grepped scripts/ec2/*.sh, test/bench/*.sh and scripts/*.sh for bench_main callers.
These export the tunable: matrix.sh, allocator_comparison.sh, sustained_load.sh,
heavy_load_compare.sh, r6arm_stack_probe{,2}.sh, r6arm_fix_experiment.sh.
These do not export it but never request jemalloc (umem/libc/umem-preload only):
aarch64_ab.sh, d2_ab.sh, hotpath_ab.sh, interpose_ab.sh, p54_mangle_ab.sh,
feature_overhead.sh, p85b_fix_t_ab.sh, bench_gate.sh, check_budget.sh,
verify-integration.sh, and test_bench_missing_alloc.sh (umem only, by design).
**The only caller that requested jemalloc (named, or via the default `all`)
without the tunable was test/bench/bench_allocators.sh.** It is fixed in 721fcf6,
which does `export GLIBC_TUNABLES=${GLIBC_TUNABLES:-...=65536}` as matrix.sh does.
The r7samp proof script is not in the tree.

Check (`ba-*`), `env -u GLIBC_TUNABLES bench_allocators.sh -q ... jemalloc`:
b5213ae gives 0 jemalloc rows and "allocator 'jemalloc' is not available".
721fcf6 gives 2 jemalloc rows.

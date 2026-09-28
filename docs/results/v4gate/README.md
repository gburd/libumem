# v4.0.0 gate (sparsemap removed, libumem soname .so.1 -> .so.2)

Commit b8e0e02 (branch v4-nosm: 913fae7 + the gate script), Debian 13
(trixie), gcc 14.2.0, c7i.2xlarge (x86_64) and c7g.2xlarge (aarch64), built
with `scripts/ec2/verify-isolated.sh` (committed content only). Script:
`scripts/ec2/v4_gate.sh`. Baseline for the export diff: `git archive v3.3.3`,
built on the same box.

| check | x86_64 | aarch64 |
|---|---|---|
| sonames | `libumem.so.2` (2.0.0), `libumem_malloc.so.1` (1.0.0) | same |
| libumem_malloc NEEDED | `libumem.so.2 libc.so.6` | same |
| `umem_sm_*` exports / any `sm_` symbol | 0 / 0 | 0 / 0 |
| libumem exports v3.3.3 -> v4 | 371 -> 285: 86 removed (85 `umem_sm_*` + the `umem_sparsemap` object), 0 added | same |
| libumem_malloc exports | 12, identical to v3.3.3 | same |
| `LD_PRELOAD=.libs/libumem_malloc.so.1 /bin/ls` | rc 0, libumem mapped | rc 0, libumem mapped |
| `exit_criteria_gate.sh` | GATE: PASS | GATE: PASS |
| tarball | `umem-4.0.0.tar.gz`, its own make check 68 / 0 FAIL | same |
| P8.3 standalone | 0.563 (null min 0.957) PASS | 0.508 (null min 0.979) PASS |
| RELEASE -O3 -DNDEBUG | make check 0 FAIL, test_main, oracle PASS | same |
| `--enable-introspect` make check | 68: 65 PASS / 3 SKIP / 0 XFAIL / 0 FAIL | 68: 66 PASS / 2 SKIP / 0 XFAIL / 0 FAIL |
| P8.5b (`p85b_fix_lo_gate.sh 30`) | all 12 PASS, commit_abort 0/30 | 11/12 PASS, commit_abort 0/30; its `make check` step 1 FAIL (below) |

## The one FAIL: aarch64 `test_cpu_hint_spread` inside the P8.5b step's make check

`arm-p85b-fail/test_cpu_hint_spread.log`: 8 threads used 3 per-CPU caches, bar
>= 4 (`cc_alloc: 0 0 0 62468 0 0 199999 510834`). The same test PASSed in the
same job's three other make check runs on that box (default, tarball,
RELEASE, introspect), and on x86 everywhere.

Attribution probe, same box, v4 and v3.3.3 binaries, 20 runs each
(`arm-p85b-fail/cpuhint-*.log`):

| load | v4 distinct caches | v4 fails | v3.3.3 distinct caches | v3.3.3 fails |
|---|---|---|---|---|
| idle | all 8 | 0/20 | all 8 | 0/20 |
| +8 busy loops | 6-8 | 0/20 | 7-8 | 0/20 |
| +24 busy loops | 4-7 | 0/20 | 4-7 | 0/20 |
| +64 busy loops | 5-8 | 0/20 | 3-7 | **3/20** |

v4 does not change the CPU hint or the per-CPU layer (the diff is sparsemap,
build flags and scripts), and v3.3.3 fails this test the same way under
oversubscription. The test's own header says the bar is half because the rseq
cpu_id follows where the scheduler actually packs threads. Under `make -j8`
next to other CPU-bound tests, an 8-vCPU box can pack 8 threads onto 3 CPUs.
Not a v4 regression, and not a return of P8.2 (pre-fix was 1-2 caches at ANY
load). Recorded as a load-sensitive test (same class as the P8.3 ratio,
which now has a quiet-rig gate); not fixed in this release.

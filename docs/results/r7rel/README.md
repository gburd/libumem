# v3.3.3 release gate (r7rel)

Debian 13 (trixie), gcc 14, c7i.2xlarge (x86_64) and c7g.2xlarge (aarch64),
built with `scripts/ec2/verify-isolated.sh` (committed content only). Run by
agent @r7rel (6661386c, died on a connection error after its jobs finished);
the norseq re-run and this write-up are by the coordinator.

| step | sha | x86_64 | aarch64 |
|---|---|---|---|
| `exit_criteria_gate.sh` (make check, test_main, property, oracle, dist self-check, install + consumer, RELEASE -O3 -DNDEBUG) | 96b0d3c | GATE: PASS, gate_rc=0 | GATE: PASS, gate_rc=0 |
| tarball | 96b0d3c | `umem-3.3.3.tar.gz`, own make check 0 FAIL | same |
| P8.3 interpose ratio, standalone (77 = FAIL) | 96b0d3c | 0.555 (null min 0.897) PASS | 0.510 (null min 0.978) PASS |
| `--enable-introspect` make check | 96b0d3c | 68: 65 PASS / 3 SKIP / 0 XFAIL / 0 FAIL | 68: 66 PASS / 2 SKIP / 0 XFAIL / 0 FAIL |
| T1_event_cb, T1_gdb_break, T8_break_ptc | 96b0d3c | PASS | PASS |
| T9_leak_stack | 96b0d3c | SKIP 77 (no frame pointers, by design) | PASS |
| `--enable-introspect --enable-frame-pointers` | 96b0d3c | 68: 66 PASS / 2 SKIP / 0 FAIL; T9 PASS | -- |
| `--disable-rseq` make check | 96b0d3c | **1 FAIL** (below) | 68: 55 PASS / 13 SKIP / 0 FAIL |
| `--disable-rseq` make check, re-run | **32e1bca** | 68: 57 PASS / 11 SKIP / 0 FAIL | -- |
| ABI vs v3.3.2 | 96b0d3c | soname `libumem.so.1` / `libumem_malloc.so.1` unchanged; libumem 368 -> 371 exports (+`umem_oversize_arena`, `umem_pcstack_internal`, `umem_ptc_bin_publish`), 0 removed, abidiff 0 changed; libumem_malloc identical | same |
| P8.5b (`p85b_fix_lo_gate.sh 30`) | 96b0d3c | all PASS, commit_abort 0/30, oracle x4 | same |

## The one FAIL, and 32e1bca

x86 `--disable-rseq` `make check -j8` at 96b0d3c: `interpose_regress.sh`'s
per-call ratio read preload/API 0.440 < 0.48, with null API/API **min 1.008,
median 1.486**. Identical code measured 49 % apart, skewed UP, and the
quiet-rig gate added in 3816199 tested only the minimum, so it judged
(`x86/r7rel-w-norseq/test/stress/interpose_regress.log`). The same sha on the
same box, in the gate's standalone step, read 0.525 with null min 0.938.

32e1bca (test-only) makes the gate two-sided: null min >= 0.88 AND null median
<= 1/0.88. Re-run at 32e1bca, x86 `--disable-rseq`
(`x86-r7rel2-32e1bca/out.log`):
- inside `make check -j8`: 4 attempts, all noisy (null min 0.42-0.63), SKIP
  (no verdict, as designed; make check does not gate P8.3, the release gate
  does);
- standalone on the idle box, 3 runs: 0.572 / 0.569 / 0.558, null min
  0.906 / 0.950 / 0.959, all PASS.

The 96b0d3c -> 32e1bca diff is `test/stress/repro_interpose_free_ratio.c`
only; every other row stands for 32e1bca.

Unexplained, minor: x86 norseq PASS/SKIP moved 54/13 -> 57/11 between the two
runs (68 total both times); not investigated.

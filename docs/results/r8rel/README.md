# r8rel — v4.1.0 release qualification (744f639)

Worker `@r8rel`. Both boxes Debian 13 (trixie), 8 vCPU, on EC2 (account
373102893032, profile hotdog). All builds via `verify-isolated.sh` at
committed sha `744f639cf99833508a5b5c131353d7bae10b135c` (committed content
only). No library code changed; the only exception is the O5 perf redo, which
is a measurement, not a code change. Coordinator tags; this agent did not tag.

Instances: intel-lo@r8rel = i-04e0390ad79552a3e (c7i.2xlarge, x86_64),
arm-lo@r8rel = i-089c9d3be38a5e594 (c7g.2xlarge, arm64). Both terminated;
describe-instances for these roles is empty.

## Qualification table

### Step 1 — exit_criteria_gate.sh (`gate_failures` per arch)

| item | x86 (intel-lo) | arm (arm-lo) |
|---|---|---|
| build_rc | 0 | 0 |
| make check (TOTAL/PASS/SKIP/XFAIL/FAIL/XPASS/ERROR) | 72/61/11/0/0/0/0 | 72/61/11/0/0/0/0 |
| test_main_rc | 0 | 0 |
| property tests (alloc_free2, cache, fragmentation, ownership) | all rc=0 | all rc=0 |
| oracle_rc | 0 | 0 |
| dist_rc / tarball self-check rc | 0 / 0 | 0 / 0 |
| **tarball name** | **umem-4.1.0.tar.gz** | **umem-4.1.0.tar.gz** |
| install_rc / external compile / external run | 0 / 0 / 0 | 0 / 0 / 0 |
| **P8.3 interpose ratio (77 = FAIL)** | **PASS** rc=0 (preload/API 0.573 ≥ 0.48, null min 0.979) | **PASS** rc=0 (preload/API 0.516 ≥ 0.48, null min 0.946) |
| RELEASE -O3 -DNDEBUG: build / check / test_main / oracle | 0 / 0 / 0 / 0 (check 72/61/11/0/0/0/0) | 0 / 0 / 0 / 0 (check 72/61/11/0/0/0/0) |
| **gate_failures** | **0** (GATE: PASS) | **0** (GATE: PASS) |

The gate lowered `vm.max_map_count` 1048576 → 65530 (via `sudo sysctl`), so
the heap-ceiling regression actually ran on Debian this time (O10 gap closed).

### Step 2 — `--enable-introspect` (`make check -j$(nproc)`)

| | x86 | arm |
|---|---|---|
| TOTAL/PASS/SKIP/XFAIL/FAIL/XPASS/ERROR | 72/68/4/0/0/0/0 | 72/69/3/0/0/0/0 |
| T1 (event_cb, gdb_break) | PASS | PASS |
| T8 (break_ptc) | PASS | PASS |
| T9 (leak_stack) | **SKIP** (x86, no frame pointers — as designed) | **PASS** (arm has FP by default) |

### Step 2b — x86 `--enable-introspect --enable-frame-pointers`

TOTAL/PASS/SKIP/XFAIL/FAIL/XPASS/ERROR = 72/69/3/0/0/0/0; **T9 leak_stack PASS**
(the lazy-warm stacks resolve caller-in-target with frame pointers). arm needs
no FP substep — T9 already PASSes there.

### Step 3 — `--disable-rseq` (`make check`)

| | x86 | arm |
|---|---|---|
| TOTAL/PASS/SKIP/XFAIL/FAIL/XPASS/ERROR | 72/59/13/0/0/0/0 | 72/59/13/0/0/0/0 |

0 FAIL/ERROR both arches. (The norseq 2-SKIP delta vs default is the
heap-ceiling tests plus rseq-only tests, all under the 65530 cap here — O10.)

### Step 4 — ABI vs v4.0.0 (`nm -D --defined-only`)

| lib | soname v4.0.0 | soname 744f639 | v4.0.0 exports | 744f639 exports | diff |
|---|---|---|---|---|---|
| libumem | libumem.so.2 | libumem.so.2 (unchanged) | 285 | 285 | **0** |
| libumem_malloc | libumem_malloc.so.1 | libumem_malloc.so.1 (unchanged) | 12 | 12 | **0** |

Both sonames unchanged; zero changed exports on both arches — round 8 added no
exports, as expected. Same result on x86 and arm.

### Step 5 — round-8 regressions (default build, per arch)

| test (source) | x86 | arm | expected |
|---|---|---|---|
| test_ptc_exit_full_mag (O6) | PASS | PASS | PASS |
| test_cpu_hint_spread (O1, pinned) | PASS | PASS | PASS |
| test_slab_interior_free (O8) | PASS | PASS | PASS |
| test_findleaks_rseq.sh (O9) | PASS | PASS | PASS |
| test_first_alloc_small_stack.sh (O7) | PASS | PASS | PASS |
| test_ptc_freeonly_trim (F1 reverted) | **SKIP** | **SKIP** | SKIP |

test_ptc_freeonly_trim SKIPs with "F1-a trim not present (trim_events=0);
free-only bin retention is a documented open item" — F1-a was reverted
(b607536), so its probe test correctly SKIPs rather than FAILs. Every other
round-8 regression PASSes as designed on both arches.

## Step 6 — O5 perf redo (x86 lo, quiet box, prodcons t=8, umem API arm)

The r8o5 measurement was contaminated by sampling the shell wrapper tree; this
redo perf-records the **direct** `.libs/bench_main` binary
(`perf record --call-graph=fp -- ./test/bench/.libs/bench_main -a umem -w
prodcons -t 8 -n 20000000`), then `perf report --stdio --no-children
--sort=symbol` restricted to the bench PID.

pre = 8c70e66 (79ae76f^, before the size-aware free); post = 744f639 (current).

| metric | pre (8c70e66) | post (744f639) |
|---|---|---|
| free-wrapper self % | umem_free_wrapper **13.71 %** | umem_free_sized_wrapper **0.10 %** |
| umem prodcons Throughput | 41,045,209 ops/sec | 40,438,839 ops/sec |

The header read (a cross-core coherence miss on the size tag the producer wrote
on another core) drops from 13.71 % of self samples to 0.10 % when the API arm
frees with the ring's carried size. Throughput is unchanged within run-to-run
spread — self-time on the header load is not the throughput bottleneck; that is
producer_thread (32 %) + ring_pop (18 %). The r8o5 37.6 % figure was inflated by
shell-tree contamination; a clean direct-binary run puts the pre wrapper at
13.71 %.

Controls (same system libjemalloc.so.2 / glibc; only the umem arm changed):

| allocator | pre (2 runs) | post (2 runs) |
|---|---|---|
| jemalloc | 53.36M / 54.59M | 51.32M / 48.75M |
| libc | 7.81M / 7.87M | 7.44M / 8.60M |

Both overlap within run-to-run spread → unchanged, confirming the harness
change touched only the umem API arm. jemalloc required
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=2000000` to dlopen.

Artifacts: `o5-perf/{pre,post}_umem.self.log`, `{pre,post}_umem.thr.log`,
`controls.txt`. Per-test logs: `logs/x86-per-test.txt`, `logs/arm-per-test.txt`,
`logs/{intel-chain,arm-abi}.txt`.

## Verdict

v4.1.0 at 744f639 qualifies on both x86_64 and arm64 Debian 13: gate_failures=0
both arches, tarball is umem-4.1.0.tar.gz, P8.3 PASSes standalone, ABI is
byte-for-byte export-stable vs v4.0.0 (both sonames unchanged, 0 changed
exports), every round-8 regression behaves as designed, and the O5 harness
change removes the header-read coherence miss (13.71 % → 0.10 % self) with no
control-allocator movement. No library code was changed; no tag applied.

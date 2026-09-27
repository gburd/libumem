# Heavy-load comparison: libumem vs glibc vs jemalloc, 192-core Debian 13 metal (2026-09-27)

Worker `@r6cmp`. The user's question, verbatim: *"how does libumem compare to
glibc and jemalloc under heavy load when running on a metal 192 core instance
with NUMA and hundreds of active threads? Run this test in EC2 on an instance
running the latest Debian version of Linux. Be sure to run it for 15 minutes
each, ignore the first 5 minutes."*

Status: **Phase A (harness validation) done. Phase B (metal) is running** at
`e7b7ea7`. The Phase A tables below come from lo boxes and say so. None of
them is a result of the comparison.

## Phase A: what was wrong with the harness, and what changed

Phase A ran the shipped `heavy_load_compare.sh` end to end on Debian 13 lo
boxes (intel-lo `c7i.2xlarge`, arm-lo `c7g.2xlarge`, isolated `ec1c98d`). Three
harness defects came out of it. Each one would have made the metal numbers
wrong, and each is fixed in its own commit.

### A1. `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=8388608` starved every thread's stack (`3780624`)

`sustained_load.sh`, `test/bench/matrix.sh` and `allocator_comparison.sh`
exported an 8 MiB optional static-TLS surplus. glibc carves that surplus out
of **every thread's stack**. The usable stack below `sp` on a fresh thread,
measured with `pthread_getattr_np` (Debian 13, glibc 2.41-12+deb13u4, default
`ulimit -s` 8192), `phaseA/phaseA-*.log`:

| optional_static_tls | x86_64 | arm64 |
|---|---:|---:|
| unset | 8,384,127 B | 8,382,623 B |
| 65536 | 8,319,103 B | 8,317,599 B |
| 8388608 | **8,319 B** | **2,719 B** |

`bench_main -w prodcons -t 16 -n 2000000 -s 64:256` on arm-lo
(`phaseA/tls-arm-lo.log`):

| environment | umem | jemalloc | glibc |
|---|---|---|---|
| tls=8388608, ulimit -s 8192 | rc=139 | rc=139 | rc=0 |
| tls=8388608, ulimit -s 16384 | rc=0 | rc=0 | rc=0 |
| tls=65536, ulimit -s 8192 | rc=0 | rc=0 | rc=0 |

So the arm "`umem_init -> dlopen` stack overflow" recorded in
`2026-09-26-p85b-aliasing-mechanism-v2.md` comes from the harness's
environment. It is not a umem defect: ld.so's 0x440-byte frame does not fit in
2.7 KB. jemalloc crashes the same way, and glibc survives only because its
call path is shallow. The shipped tiny run on arm-lo recorded **every umem and
jemalloc window as `CRASH rc=139`**, and only libc produced rows
(`phaseA/phaseA-arm-lo.log`).

**Why the tunable exists.** `b6d955c` (2026-09-08) added it so `allocators.c`
can `dlopen()` jemalloc. Debian 13's `libjemalloc2` 5.3.0 is `DF_STATIC_TLS`
with a 2.6-2.7 KB `PT_TLS` (`readelf`, `phaseA-*.log`). With the tunable unset,
its dlopen fails with "cannot allocate memory in static TLS block", and the
jemalloc arm silently disappears (the probe printed exactly that). umem and
glibc need no surplus. The 8 MiB value was never sized against the stack cost.

**Decision (approved by the coordinator).** Every script now uses 65536 (about
24x jemalloc's need) for **all three allocators**, the same environment for
each. `sustained_load.sh` compiles the stack probe at startup and exits with a
FATAL if a new thread has less than 1 MiB usable under whatever
`GLIBC_TUNABLES` is exported. Checked: 8388608 gives FATAL rc=1 (8,319 B
intel-lo, 2,719 B arm-lo), and the new default runs. The value in effect and
the measured usable stack are written into every TOML (`glibc_tunables`,
`thread_stack_usable_bytes`).

### A2. Calibration timed 2 M ops but bench_main ran 19.2 M: 192-thread windows were ~10x shorter than labelled (`38ec141`)

`calibrate()` timed `bench_main -n 2000000` and scaled the window budget from
that. But bench_main raises any per-thread share below
`BENCH_MIN_OPS_PER_THREAD` (100,000) to the floor. At t=192 the timed run
really did 19.2 M ops (prodcons 9.6 M over its 96 producers), so the budget
came out about 10x (frag) or 5x (prodcons) too small. The fix starts the
calibration run at max(floor, 2 M) and scales from bench_main's own
`elapsed_sec`.

Pre/post, same box (intel-lo), same command (`sustained_load.sh umem 60 192`,
windows mode, 1 window, frag 64:256), isolated refs
(`phaseA/calpre-intel-lo.log`, `phaseA/calpost-intel-lo.log`):

| ref | calibrate timed | prodcons window (target 60 s) | frag window (target 60 s) |
|---|---|---:|---:|
| `3780624` (pre) | "2,000,000" ops (really 19.2 M) | **15.0 s** | **6.6 s** |
| `38ec141` (post) | 19,200,000 ops | 50.3 s | OOM-killed (rc=137) |

With frag at 64:256, the post-fix budget of 407 M ops made the frag pool hold
more than the 15 GB lo box has: `dmesg` shows the global OOM killer took
`bench_main` at 15.8 GB anon RSS. That is a lo-box memory limit, not a
calibration fault. The same post-fix binary at frag 16:32 gave 67.7 s
(`phaseA/calpost2-intel-lo.log`, a re-run in the same isolated tree). The remaining
±20 % is how well a short calibration predicts a long window. Continuous mode
(A3) does not calibrate at all.

#### Correction note: earlier "sustained" windows were far shorter than labelled

Every high-thread-count result from `sustained_load.sh` before `38ec141` ran
windows much shorter than its `duration_target_sec_per_window` says. The
evidence is in the recorded rows (`ops / ops_per_sec`) or in `elapsed_sec`:

| result | label | actual per-window duration |
|---|---|---|
| `docs/results/r5fix/mdef-intel-hi.log` (P8.5b-fix sustained, intel-hi, the "29.7 vs 41.7 us" p999 table) | 180 s | prodcons 33-43 s, frag 18-19 s |
| `docs/results/r5fix/msus-arm-hi.log` (P8.5b-fix sustained, arm-hi, "5.0 vs 16.9 us") | 180 s | prodcons 32-41 s, frag 17-24 s |
| `2026-09-24-allocator-comparison.md` section 4, `2026-09-24-c7i.metal-48xl-x86_64/sustained.toml` | 20 s | umem frag median 7.7 s, libc frag 2.5 s |
| same, `2026-09-24-c8g.metal-48xl-aarch64/sustained.toml` | 20 s | umem frag median 3.4 s, libc frag 1.7 s |

prodcons came out closer to its label than frag, because its floor applies
over t/2 producers and the calibration was only half as far off. These numbers
are real measurements of shorter windows than stated. The p999 values in those
tables describe windows of 2-43 s, not 20 or 180 s. Those runs also used the
8 MiB tunable (A1), and the arm-hi r5fix sustained run used
`GLIBC_TUNABLES=glibc.malloc.check=0` to get around it. The coordinator will
carry this note to those documents.

### A3. Warm-up windows warmed the OS, not the allocator: one continuous process per run (`153a0c8`, `99acb32`, `ac71949`, `bd12c8d`)

In windows mode, every window, including each "discarded warm-up window", is a
separate `bench_main` process. So the 5 discarded windows warmed the page
cache, THP and CPU frequency, but never the allocator instance that a measured
window then used. The user's "run it for 15 minutes each, ignore the first 5
minutes" means one process per allocator, with its own first 5 minutes
dropped.

- `bench_main -D SEC:COUNT` (`153a0c8`): prodcons and frag run COUNT
  consecutive SEC-second intervals in **one process**, end by time, and print
  one CSV row per interval with that interval's own ops, latency digest, RSS
  and (for frag) (RSS, live) series. `-W K` drops the first K rows, and those
  intervals still run in the same process. Workers poll the interval clock
  between operations, never inside a timed one. The ownership and ordering
  rules are in `iv_clock_t`'s comment. `test_bench_accounting` gained
  `test_interval_mode`.
- `99acb32`: rows stream out as each interval completes, so a crash in minute
  14 keeps minutes 6-13. The `vmhwm_bytes` column in `-D` mode is the
  interval's own maximum sampled VmRSS (100 ms tick), not the process VmHWM.
  **Every column of a kept row comes from that interval alone.** A lifetime
  VmHWM would carry the dropped warm-up.
- `bd12c8d`: the driver used to wait for every worker's slot before timing
  the next interval. At 384 threads on 8 vCPUs, one umem frag operation took
  12.7 s, which stretched an interval to 11.7 s of 10 and left it with no RSS
  sample. The driver now keeps the clock and collects without blocking.
  Regression `test_interval_straggler` (`5e4e07b`, committed alone): **FAIL 3/3
  at `5e4e07b`** (interval 1 0.400 s of 0.3, rss 0), **PASS 3/3 at `bd12c8d`**
  on intel-lo and arm-lo (`phaseA/strag{pre,post}-*.log`).
- `sustained_load.sh SUSTAINED_MODE=continuous` (`ac71949`): for each
  workload, and each allocator in the order given, one `bench_main -D
  60:15 -W 5` run. Each kept interval becomes a `[[window]]` carrying its source
  `interval`. Each run gets a `[[run]]` record with start/end/rc, and its raw
  CSV is kept. The order is whole 15-minute runs, workload-major (prodcons:
  umem, libc, jemalloc, umem@null; then frag in the same order). umem@null goes
  **last** so that it brackets the other two in time.
- `heavy_load_compare.sh` (`ac71949`) drives continuous mode with those four
  arms, frag 64:256, a frag live pool of 100,000 objects per thread (~3 GB live
  at t=192), and a preflight that exits FATAL if any arm cannot load and run
  both workloads at the requested thread count.
- More provenance goes into every TOML: `glibc_tunables`,
  `thread_stack_usable_bytes`, `UMEM_OPTIONS`, the `config.h` rseq define,
  os-release, kernel, THP, numa_balancing, max_map_count, `ulimit -s`, the NUMA
  node count and `numactl -H`, and for jemalloc its resolved path, realpath,
  sha256 and Debian package version.

### How "the first 5 minutes are discarded from every reported statistic" is checked

1. **In the process.** bench_main `-D 60:15 -W 5` runs intervals 0-14 and
   `print_interval()` returns without printing for `k < 5`. Rows 0-4 never reach
   stdout (bench_main.c, `iv_skip`).
2. **Per column.** Each interval's digest, op/failure/byte counts, CPU delta,
   RSS samples and (frag) (RSS, live) series are opened fresh at that
   interval's start and closed at its end (`iv_poll` hands off and re-inits
   the per-worker digest; `iv_drive` resets `rss_max` and the series at every
   boundary). The one lifetime quantity bench_main has, VmHWM, is not used in
   `-D` rows (99acb32).
3. **In the output.** `summarize.py` fails the file unless each (allocator,
   workload) has exactly intervals 5..14, all within 5 % of 60 s, rc 0, zero
   allocation failures, one thread count and a nonzero RSS sample. Every table
   is computed from those rows only.

### Phase A validation runs (lo boxes: harness checks, not results)

| check | where | result |
|---|---|---|
| shipped `heavy_load_compare.sh` tiny run at `ec1c98d` | intel-lo | all 4 arms produced rows; TOML parses |
| same | arm-lo | **umem, jemalloc, umem@null: every window CRASH rc=139** (A1); libc only |
| jemalloc resolution | both | `libjemalloc2 5.3.0-3` via `ldconfig -p` (631596d bootstrap works) |
| stack guard | both | 8388608 gives FATAL; default runs |
| interval mode, 3 x 2 s, t=16, all arms | both | every interval 2.000 s; op-budget mode unchanged; bad-arg rejection |
| continuous `heavy_load_compare.sh`, 60 s x (1+1), **t=192**, all 4 arms, both workloads, `ac71949` | arm-lo | summarize PASS, 8 runs, 0 failures |
| same, 10 s x (1+1), t=384 | arm-lo | frag intervals 11.7 / 13.0 s and rss 0: the straggler defect, fixed in `bd12c8d` |
| `test_bench_accounting` incl. straggler | both | PASS 3/3 at `bd12c8d` |
| continuous end-to-end at `bd12c8d`, t=192 and t=384 | both | see "E2E" below |

### A4. Two more defects found by the replicate design, before metal (`fde96c1`, `e7b7ea7`)

- **The lo "null gap" came from oversubscription, not from the allocators.**
  At `bd12c8d`, t=192 on 8 vCPUs, the single umem@null differed from umem by
  30.6 % in prodcons Mops/s on intel-lo, while each run's own 10 intervals
  moved less than 0.6 %. With 24 threads per vCPU, whichever threads the
  scheduler happens to favour for a whole run set its throughput. The six-arm
  lo run at `e7b7ea7`'s harness (`phaseA/v3-*.log`, t=384 on 8 vCPUs) shows
  the same thing for every allocator: prodcons run-to-run gap umem 36.5 %,
  libc 7.1 % (p99 gap 113 %), jemalloc 0.4 % (p999 gap 21 %) on intel-lo;
  frag gap jemalloc 18.1 % / 23.3 %. That is why each allocator now gets two
  runs. Metal at t=192 (one thread per vCPU) shows whether the gap survives
  without oversubscription (see Phase B).
- **Harness frees on the libc arm's own arenas (`fde96c1`).** At `bd12c8d`,
  t=384, a 10 s libc frag interval took 66.9 s (intel-lo) / 39.8 s (arm-lo).
  gdb on the live process, sampled 8 times over 90 s
  (`phaseA/stall-gdb-intel-lo.log`), found the driver in `iv_drive -> td_free
  -> _int_free_chunk -> __lll_lock_wait_private` every time: it was freeing
  each worker's digest into the glibc arena that worker was hammering. That is
  also a cross-arm confound at any thread count, because only the libc arm's
  arenas saw harness frees. Now every digest is allocated before the start
  barrier, reset instead of freed, and freed after the join. **No harness
  malloc or free happens while intervals run**, for any arm. The t=384 run at
  the fixed tree passes on both lo boxes.
- **Design (`e7b7ea7`).** Six arms `umem, libc, jemalloc, jemalloc@2, libc@2,
  umem@2`: each allocator twice, independent processes, palindromic order so
  each pair straddles the middle. frag walks the reversed order, so no arm is
  always first or last on the box. NUMA: `numactl --interleave=all` for every
  run of every arm, threads unpinned for all. Neither bench_main nor libumem
  sets CPU affinity or a memory policy (libumem reads the CPU-to-node map only
  for depot steal order; `umem_numa_bind_thread` and `_migrate_memory` have no
  callers), so the numactl prefix is the only placement policy in effect.
  `summarize.py` counts a difference between A and B on a metric only if
  `|A - B| / min(A, B)` exceeds the larger of A's and B's own run-to-run gaps
  on that metric. Otherwise it reports "inside noise".
- Tip check at `e7b7ea7`, isolated, intel-lo: accounting PASS, six-arm
  heavy_load_compare t=384 5 s x (1+2) PASS (`phaseA/tip-e7b7ea7-intel-lo.log`).

## Phase B: metal

**Ref measured: `e7b7ea7b49cdd0e03d62b817268dec5714c75787`** (origin/master
HEAD at launch, 2026-09-27 06:55 UTC). It contains @r6arm's `59ab75e` (first
allocation needs ~5 KB of stack) and every harness commit above. Between
`59ab75e` and `e7b7ea7` the only C changes are `test/bench/bench_framework.c`
and `test/bench/test_bench_accounting.c` (`git diff --stat -- '*.c' '*.h'`);
no library source changed. @r6sm held sparsemap at 5.5.0 (`1e004ee`), so no
vendored change is pending either.

| | intel-hi | arm-hi |
|---|---|---|
| instance | `c7i.metal-48xl`, Intel Xeon Platinum 8488C, 2 sockets, SMT 2 | `c8g.metal-48xl`, Neoverse-V2 (Graviton4), 2 sockets, SMT 1 |
| vCPU / NUMA | 192, 2 nodes: 0-47,96-143 / 48-95,144-191, distance 10/21 | 192, 2 nodes: 0-95 / 96-191, distance 10/100 |
| memory | 193,254 + 193,466 MB | 193,509 + 192,652 MB |
| runs | t=192 (12 x 15 min), then t=384 if t=192's gaps allow | t=192 (12 x 15 min) |

(Full `numactl -H` and the rest of the provenance are in each TOML header.)

# Heavy-load comparison: libumem vs glibc vs jemalloc, 192-core Debian 13 metal (2026-09-27)

Worker `@r6cmp`. The user's question, verbatim: *"how does libumem compare to
glibc and jemalloc under heavy load when running on a metal 192 core instance
with NUMA and hundreds of active threads? Run this test in EC2 on an instance
running the latest Debian version of Linux. Be sure to run it for 15 minutes
each, ignore the first 5 minutes."*

Status: **done.** Phase A (harness validation, lo boxes) found and fixed six
harness defects. Phase B measured `e7b7ea7` on Debian 13 metal, both arches,
at t=192. t=384 was not run: it failed its pre-registered go/no-go (see
below). **The results are in "Phase B results"; the text for the README is in
"Proposed README text".** Phase A tables come from lo boxes and say so. None
of them is a result of the comparison.

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
| continuous end-to-end at `bd12c8d`, t=192 / t=384 | both | t=192 PASS; t=384 FAIL: libc frag interval 67 s / 40 s, fixed in `fde96c1` (A4); `phaseA/e2e-*.log` |
| six-arm design, t=384 and t=192, fixed tree | both | PASS (`phaseA/v3-*.log`) |

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

**t=384 go/no-go, fixed before any t=192 result was seen (07:05 UTC).**
intel-hi runs t=384 after its t=192 only if, at t=192, **every allocator's
own run-to-run gap in Mops/s is at most 10 % on both workloads**. With two
runs per arm, a pair that already disagrees by more than 10 % at one thread
per vCPU leaves no room to read a 2x-oversubscribed run. On lo, that
oversubscription produced 18-37 % gaps (A4). If the criterion fails,
intel-hi is terminated after t=192, and the reason is recorded here.

**t=384 decision: NO-GO (08:30 UTC, after intel-hi t=192 prodcons, before any
frag result).** On intel-hi at t=192, libc's two prodcons runs differ by
**16.8 %** in Mops/s (1.067 vs 0.919 median), which fails the criterion above
(every allocator's gap at most 10 %). On arm-hi, umem's and libc's prodcons
gaps are 12.6 % and 13.1 %. If two identical 15-minute runs at one thread per
vCPU disagree that much, a 2x-oversubscribed run could not separate the
allocators, and on lo, oversubscription made the gaps larger (18-37 %, A4).
intel-hi's job was launched with t=384 enabled, so it is killed as soon as its
t=192 summary is written, and the box is terminated. The brief's minimum is
therefore what gets delivered: t=192 on both arches, no t=384.

## Phase B results: t=192, both arches, ref `e7b7ea7`

**What ran.** For each box: 12 processes, 6 arms x 2 workloads. Each process
ran 15 consecutive 60 s intervals; the first 5 were dropped inside the process
and the next 10 kept. 12/12 runs exited rc=0 on both boxes. 120 kept
intervals per box, each 60.00 s (+-0.01), 0 allocation failures, every
interval with its own RSS samples (`summarize.py` RESULT PASS on both). Wall
time: 06:57-09:57 UTC arm-hi, 06:58-09:58 intel-hi.

**Provenance (both boxes, from the TOML headers).** `git_sha
e7b7ea7b49cdd0e03d62b817268dec5714c75787` (isolated `git archive`). Default
`./configure` (no flags; `config.h` has `HAVE_LINUX_RSEQ_H 1`;
`UMEM_OPTIONS` empty, so rseq is ON). Debian GNU/Linux 13.7 (trixie), kernel
6.12.107+deb13-cloud, GCC 14.2.0-19, glibc 2.41-12+deb13u4, jemalloc =
Debian `libjemalloc2 5.3.0-3` (sha256 `51952ffe…` amd64, `74ccd960…` arm64,
dlopen'd through the ld.so cache). `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536`
for every arm (usable thread stack 8,319,103 / 8,317,599 B). NUMA policy
`numactl --interleave=all` for every arm, threads not pinned.
numa_balancing 0, THP never, `vm.max_map_count` 1,966,080, `ulimit -s` 8192,
governor `powersave` (intel; the metal default, not changed by bootstrap) /
`unknown` (arm). bench_main sha256 `860885d5…` (x86) / `fdfef201…` (arm);
libumem.so `7414ff01…` / `6ca11c70…`. Each TOML records the per-arm identity.

**Workloads.** Both use sizes 64-256 B, 192 threads, and timed alloc/free calls.
The umem arm is called through the umem API with the bench's 16-byte size
header (`allocators.c`); glibc and jemalloc are plain malloc/free.
- `prodcons`: 96 producer threads allocate, 96 consumer threads free, all
  through one shared 8,192-slot lock-free ring. Every buffer is freed on a
  different thread than the one that allocated it, the all-to-all
  cross-thread and cross-node pattern. Throughput here is **bounded by the
  shared ring**, not by the allocator: allocator calls take 0.1-1.3 % of
  thread time on every arm (mean latency x ops / thread-time). The Mops/s
  column therefore mostly measures how the ring behaves under each
  allocator's timing, and the latency columns are the allocator signal.
- `frag`: each thread churns a private pool of up to 100,000 live objects
  (frees ~50 % at random, refills). Live set at peak about 2.2-2.3 GB, 3.6-4.1 GB
  RSS. Same-thread alloc/free. (64:256 rather than the historical 16:4096,
  which at 192 threads holds ~10 GB and measured umem's heap ceiling rather
  than fragmentation; see `sustained_load.sh`.)

### The noise, measured: each allocator's own run-to-run gap

Gap = |run1 - run2| / mean of the two 15-minute runs. Within one run, the 10
intervals are very tight (Mops/s min-max typically under 1 %). **Between
two identical runs the difference is much larger**, and that between-run gap
is the noise every comparison below is held to:

| box | workload | umem | glibc | jemalloc |
|---|---|---|---|---|
| intel-hi | prodcons Mops/s / p99 / p999 | 1.6 / 27.7 / 25.9 % | **16.8** / 1.3 / 9.2 % | 5.8 / 17.8 / 13.4 % |
| intel-hi | frag Mops/s / p99 / p999 | 4.3 / 6.3 / 4.6 % | 9.6 / 7.8 / 5.7 % | 8.3 / 7.2 / 9.7 % |
| arm-hi | prodcons Mops/s / p99 / p999 | 12.6 / 42.4 / **108.8** % | 13.1 / **126.8** / **102.6** % | 0.3 / 48.0 / 8.1 % |
| arm-hi | frag Mops/s / p99 / p999 | 5.1 / 2.0 / 8.1 % | 6.6 / 1.6 / 13.1 % | 0.2 / 2.4 / 1.6 % |

The lo boxes showed large gaps (A4). This confirms they are **not only
oversubscription**: at one thread per vCPU on metal, two identical runs of
glibc prodcons still differ by 17 % in throughput (intel), and umem's and
glibc's prodcons p999 on arm differ by 2-3x between runs (umem 1.6 vs 5.4 us,
glibc 6.9 vs 21.5 us). Peak RSS is the steadiest metric (gaps 0-2.4 %).
Two runs per arm is a minimum, not a comfortable margin. A difference that
clears the bar by only a little is weak evidence, and the tables say so.

### Per allocator (mean of 2 runs; the per-run and per-interval tables are in `*/summary-t192.md`)

Latency is per call (alloc or free), ns, the median over the 10 kept intervals
of each interval's percentile. Peak RSS is the largest VmRSS sampled
(100 ms) in any kept interval.

**intel-hi, c7i.metal-48xl (Sapphire Rapids, 2 nodes)**

| workload | allocator | Mops/s | p50 | p99 | p999 | peak RSS MB |
|---|---|---:|---:|---:|---:|---:|
| prodcons | umem | 2.03 | 162 | 3,686 | 16,932 | 166 |
| prodcons | glibc | 1.01 | 885 | 18,930 | 44,412 | 49 |
| prodcons | jemalloc | 1.94 | 271 | 14,751 | 28,988 | 64 |
| frag | umem | 16.24 | 209 | 12,195 | 25,052 | 4,127 |
| frag | glibc | 15.68 | 456 | 16,859 | 30,117 | 3,717 |
| frag | jemalloc | 14.56 | 27 | 13,257 | 26,395 | 3,612 |

**arm-hi, c8g.metal-48xl (Graviton4, 2 nodes)**

| workload | allocator | Mops/s | p50 | p99 | p999 | peak RSS MB |
|---|---|---:|---:|---:|---:|---:|
| prodcons | umem | 1.52 | 72 | 367 | 3,468 | 169 |
| prodcons | glibc | 1.54 | 284 | 4,289 | 14,240 | 46 |
| prodcons | jemalloc | 1.98 | 141 | 639 | 1,333 | 96 |
| frag | umem | 23.70 | 121 | 692 | 4,495 | 3,985 |
| frag | glibc | 23.68 | 288 | 1,788 | 17,748 | 3,715 |
| frag | jemalloc | 24.51 | 33 | 390 | 870 | 3,622 |

### Verdicts: each difference against the larger of the two allocators' own gaps

"Clears" means the difference between the means is larger than the noise.
**"Separated"** means, stricter, that both of one allocator's runs beat both of
the other's. With two runs each, that is the strongest statement the data
supports.

**umem vs glibc**

| box / workload | Mops/s | p50 | p99 | p999 | peak RSS |
|---|---|---|---|---|---|
| intel prodcons | **umem 2.0x**, separated | **umem 5.5x lower**, separated | **umem 5.1x lower**, separated | **umem 2.6x lower**, separated | **glibc 3.4x lower** (49 vs 166 MB), separated |
| intel frag | inside noise (1.04x, noise 9.6 %) | **umem 2.2x lower**, separated | **umem 1.4x lower**, separated | umem 1.2x lower, separated (17 % vs noise 5.7 %) | glibc 11 % lower, separated |
| arm prodcons | inside noise (0.98x, noise 13 %) | umem 3.9x lower, separated (noise 51 %) | umem 11.7x lower, separated (noise 127 %) | umem 4.1x lower, separated (noise 109 %) | **glibc 3.7x lower** (46 vs 169 MB), separated |
| arm frag | inside noise (1.00x) | **umem 2.4x lower**, separated | **umem 2.6x lower**, separated | **umem 3.9x lower**, separated | glibc 7 % lower, separated |

**umem vs jemalloc**

| box / workload | Mops/s | p50 | p99 | p999 | peak RSS |
|---|---|---|---|---|---|
| intel prodcons | inside noise (1.05x, noise 5.8 %) | umem 1.7x lower, separated | **umem 4.0x lower**, separated | umem 1.7x lower, separated | **jemalloc 2.6x lower** (64 vs 166 MB), separated |
| intel frag | umem 12 % higher, separated (noise 8.3 %) | **jemalloc 7.7x lower** (27 vs 209), separated | umem 8 % lower, separated (noise 7.2 %) | inside noise (0.95x) | jemalloc 14 % lower, separated |
| arm prodcons | **jemalloc 1.30x higher**, separated | umem 1.9x lower, separated | umem 1.7x lower, separated (noise 48 %) | **jemalloc 2.6x lower**, separated (jemalloc runs 1.28 / 1.39 us, umem 1.58 / 5.36 us; 160 % vs noise 109 %) | **jemalloc 1.8x lower** (96 vs 169 MB), separated |
| arm frag | inside noise (0.97x, noise 5.1 %) | **jemalloc 3.7x lower**, separated | **jemalloc 1.8x lower**, separated | **jemalloc 5.2x lower** (0.87 vs 4.5 us), separated | jemalloc 10 % lower, separated |

### What the numbers say

- **Cross-thread producer/consumer, intel.** umem has the lowest latency at
  every percentile and ties jemalloc on throughput (both about 2x glibc):
  p99 3.7 us vs jemalloc 14.8 and glibc 18.9; p999 16.9 vs 29.0 and 44.4 us.
  Every one of these is separated across runs.
- **Cross-thread producer/consumer, arm.** umem has the lowest p50 and p99 (72
  and 367 ns, vs jemalloc 141 / 639 and glibc 284 / 4,289). **jemalloc has
  30 % more throughput** than umem and glibc, which tie, **and the lowest
  p999**: 1.28 / 1.39 us in its two runs vs umem 1.58 / 5.36 us and glibc
  6.9 / 21.5 us. umem's p999 is 4x below glibc's but 2.6x above jemalloc's,
  and it was the least repeatable number in the study (its two runs differ
  3.4x).
- **Fragmentation churn.** Throughput is a three-way tie on arm and a tie
  with glibc on intel (umem +12 % over jemalloc there, only just past
  jemalloc's 8 % noise). **jemalloc has by far the lowest median** (27-33 ns
  vs umem 121-209 and glibc 288-456). On intel, umem's and jemalloc's p999
  are inside noise of each other (25.1 vs 26.4 us), and both are below
  glibc's 30.1 us (umem by 20 % vs 6 % noise). On arm, **jemalloc's tail is 5x lower than umem's** (p999 0.87 vs
  4.5 us), and umem's is 4x lower than glibc's (17.7 us).
- **Memory: umem uses the most, on every box and workload, and the result
  is separated in every case.** In prodcons (a ring-bounded live set) that
  is 166-169 MB vs glibc 46-49 and jemalloc 64-96, i.e. 1.8-3.7x. In frag
  (~2.25 GB live) it is 7-14 % more than the others: 4.0-4.1 GB vs 3.6-3.7
  (RSS / live at the live-set peak: umem 1.72-1.80, glibc 1.64-1.67,
  jemalloc 1.59-1.62).
- **NUMA and "hundreds of threads".** Everything here is 192 threads on 192
  vCPUs over 2 nodes with interleaved memory. Oversubscription (t=384) was
  not measured on metal; see the go/no-go. Two lo runs at 24-48 threads per
  vCPU (Phase A) only show that run-to-run noise gets worse with
  oversubscription. They are not results.

### Limits of this result

- Two runs per arm. Where a verdict says "separated", both runs of one
  allocator beat both runs of the other. That is not a confidence
  interval, and a third run could move a borderline case (intel frag
  Mops/s umem vs jemalloc +12 % against 8 % noise; intel frag p999 umem vs
  glibc).
- One size range (64-256 B), two synthetic workloads, one NUMA policy
  (interleave). First-touch placement, pinned threads and larger objects
  were not measured. The 2026-09-24 comparison found umem weakest at
  1-4 KB objects at 128+ threads (P8.2b).
- prodcons throughput is ring-bound (allocator time 0.1-1.3 % of thread
  time), so the prodcons Mops/s column is not a measure of allocator speed.
  Its latency columns are.
- umem is measured through its API with a 16-byte header, not as the
  `libumem_malloc.so` drop-in. The drop-in was not measured here.
- intel-hi's governor was `powersave` (the metal default; bootstrap's
  `performance` write did not take). It was the same for all arms. Clocks
  were not pinned.

## Proposed README text

> ### How it compares under heavy load (192 cores, 2 NUMA nodes)
>
> On Debian 13 metal (`c7i.metal-48xl` Sapphire Rapids and `c8g.metal-48xl`
> Graviton4, 192 vCPU, 2 NUMA nodes), with 192 threads, sizes 64-256 B, and
> memory interleaved over both nodes, each allocator ran twice for 15
> minutes per workload. The first 5 minutes of each run were discarded.
> A difference is only reported if it is larger than the gap between that
> allocator's own two runs (full method, raw data and every verdict:
> `docs/results/2026-09-27-heavy-load-comparison.md`, v3.3.2 at `e7b7ea7`).
>
> | 192 threads | libumem | glibc 2.41 | jemalloc 5.3 |
> |---|---|---|---|
> | cross-thread alloc/free, p99 latency (x86 / arm) | **3.7 / 0.37 us** | 18.9 / 4.3 us | 14.8 / 0.64 us |
> | cross-thread alloc/free, peak RSS | 166-169 MB | **46-49 MB** | 64-96 MB |
> | fragmentation churn (2.2 GB live), p999 latency (x86 / arm) | 25\* / 4.5 us | 30 / 17.7 us | 26\* / **0.87 us** |
> | fragmentation churn, peak RSS | 4.0-4.1 GB | 3.7 GB | **3.6 GB** |
>
> \* inside run-to-run noise of each other.
>
> - **Cross-thread allocate/free** (the all-to-all NUMA pattern): libumem has
>   the lowest p50 and p99 of the three on both architectures (x86 p99 4-5x
>   lower than glibc and jemalloc). On x86 it also has the lowest p999, and
>   its throughput matches jemalloc at about 2x glibc. On arm, jemalloc has
>   30 % more throughput and a 2.6x lower p999; libumem's p999 is still 4x
>   below glibc's.
> - **Fragmentation churn**: throughput is tied with glibc on both arches.
>   libumem's tail is 2.6-3.9x lower than glibc's on arm and 17-28 % lower on
>   x86. jemalloc is faster at the median everywhere and has a 5x lower tail
>   than libumem on arm.
> - **Memory**: libumem uses the most. It takes 1.8-3.7x the RSS of glibc and
>   jemalloc when the live set is small, and 7-14 % more with a 2 GB live set.
> - Not measured: more threads than cores, objects above 256 B, and the
>   `libumem_malloc.so` drop-in.

### Verification of the verdict tables

Every "clears / inside noise / separated" cell above was recomputed from the
raw TOML with an independent script after the doc was drafted (the same
per-run medians `summarize.py` uses; clears = |dA-B|/min > max(gap_A,
gap_B); separated = both runs of one arm beyond both of the other). The
check caught two errors in the first draft, both corrected in the same
commit: arm prodcons p999 umem vs jemalloc had been called "not separated"
(it is: jemalloc 1.28/1.39 us vs umem 1.58/5.36 us), and the prodcons RSS
range had been written 2-3.7x (it is 1.8-3.7x; umem vs jemalloc on arm is
1.76x). Also removed: "umem lowest" for intel frag p999 (inside noise of
jemalloc).

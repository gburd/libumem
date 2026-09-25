# Changelog

All notable changes to libumem are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Security -- fixed

- **Exact heap-ownership check on `free()`** (P7.4, `b978a0f`, `f253f6d`,
  `a0fd6ba`). `umem_may_own()` used a `[lo,hi)` convex hull, so a forged object
  header placed in the address gap between two heap spans passed the check and
  the pointer was taken onto a per-thread bin and later returned by `malloc()`
  (attacker position D). It now confirms exact containment against a sorted
  span table published lock-free by `vmem_span_create` (jemalloc's approach, no
  secret; span removal handled). A normal heap free short-circuits on the hull
  and pays nothing new (measured within the null, both arches); only a foreign
  pointer pays a bounded binary search. On saturation of the fixed 16 Ki-entry
  table the check falls back to the hull -- a conservative false-yes, never
  accept-all. `test/security/test_forged_span_gap`: forgery accepted before,
  refused after. Stronger than glibc (no range check), matches jemalloc's
  rtree; the unmangled magazine-round pointers remain (P5.13b, below).

- **Per-thread magazine round pointers XOR-encoded** (P5.13b, `49f4da6`,
  `e4e3d45`). P5.13 encoded the per-thread bin slots; this extends the same
  transform (`ptr ^ cookie ^ (&slot >> 12)`) to the `mag_round[]` entries in
  the magazine layer behind them, across 26 sites in `umem.c` and 6 in
  `umem_inspect.c` (no out-of-process reader exists to teach). Empty slots are
  count-driven (`umem_mag_init_fast` keeps a raw zero-fill; live rounds are
  exactly `[0,rounds)` and never NULL-tested), so there is no mangled-NULL
  trap. Amortized ~0 % on the fast path (the P5.13 bins absorb steady-state
  traffic; the magazine transform only fires on a batch that spills past a
  bin), +2.9 % / +3.9 % insn (x86 / arm64) at the one spill-heavy point,
  recorded and shipped per the hardening rule.
  `test/security/test_mag_round_mangle`: overwritten round not returned;
  FAILs under `-DUMEM_NO_LINK_MANGLE`.

- **Per-thread magazine round pointers are now pointer-mangled** (P5.13b,
  `e4e3d45`, test `49f4da6`). P5.13 mangled the per-thread cache bin slots but
  left the magazine layer behind them raw: `umem_magazine_t.mag_round[]` held
  plain object addresses, so a write that reached a cached round steered the
  next `malloc`/`umem_alloc` of that size class to a chosen address -- the
  same double-allocation primitive P5.13 closed for the bins, one layer down.
  Every round is now stored `ptr ^ umem_link_cookie ^ (&round >> 12)` (the
  same `UMEM_SLOT_MANGLE` and per-process `AT_RANDOM` cookie as the bins, a
  secret glibc's safe-linking does not have). Rounds are count-driven, so
  fresh magazines still zero-initialise at no cost and only in-use rounds are
  encoded. `test/security/test_mag_round_mangle` returns the overwritten live
  buffer after 128 pops under `-DUMEM_NO_LINK_MANGLE` and refuses the chosen
  address across 1024 pops by default, on both x86-64 and arm64. Attacker
  position D (controls allocation patterns and buffer contents, not the
  environment and not the code). No API/ABI change.

  Cost is amortised to zero on the fast path -- the P5.13 bins absorb
  steady-state traffic and the magazine transform runs only when a batch
  spills past a bin -- so `bench_pairs` instructions/pair moved +0.00% (inside
  the +-0.07% null) at 12 of 13 A/B points; the exception is a bin-spilling
  512 B, N=128 batch, which cost +2.89% (x86-64) / +3.85% (arm64)
  instructions/pair and up to -3.9% throughput at that one point. Shipped per
  the hardening policy (AGENTS.md 7a); the number is on the record.

## [3.3.0] - 2026-09-25

The theme is *finishing the hardening and closing the open limits with
evidence*. v3.2.0's audit left P5.13 (per-thread cache slots unmangled) as the
last item that made libumem's freelist integrity worse than glibc; this
release mangles them, with a per-process cookie glibc does not have, at a
measured cost that is on the record. The Phase 6/8 open items were each
re-measured on metal and closed with a decision rather than a speculative fix:
the 16k-thread drain is a kernel `mmap_lock` property, not an allocator lock
(P6.3b, refuted); the 50k-cache fork and 117 KB/cache footprint are structural
to per-CPU layering and their only "fixes" are a hot-path cost for a
pathological workload or a provably-unsound lock skip (P6.4b, closed); the
2.5-8 KB per-thread retention is ~280 KB, not the feared 5.4 MB (P8.2c,
measured-fine); the sustained-load slab-batch cannot meet its own p999 target
and the real fix is an architectural owning-thread free (P8.5, partial,
remainder deferred as P8.5b). A proportionate metal comparison confirms the
1-4 KB tier is now 0.86-0.97 of the best competitor (was 0.51-0.65) and the
small tiers are unchanged within variance. No API/ABI change:
`sizeof(umem_hook_t)` is still 120.

### Security -- fixed

- **Per-thread cache bin slots are now pointer-mangled** (P5.13, `8c79ac3`,
  `f1b4f12`). Slots in `umem_ptc_t.pool[]` held raw object addresses; a write to
  a cached slot could steer the next `malloc`/`umem_alloc` of that size class to
  a chosen address. Slots are now stored `ptr ^ cookie ^ (&slot >> 12)`,
  glibc's tcache safe-linking plus a per-process `AT_RANDOM` cookie -- stronger
  than glibc, which uses no cookie. This was the last item that left libumem
  worse than glibc on freelist integrity. Costs ~7 % instructions and ~6-8 %
  throughput on the per-thread fast path (measured, both arches); shipped
  because hardening is not traded for single-digit percent. `umem_link_cookie`
  gained hidden visibility, which also cheapened the P5.4 freelist mangling.
  Magazine-round mangling (P5.13b) is deferred: the depot is not in an
  overrun's reach.

- **`free()` could unmap a range named by the caller's own bytes** (P5.10,
  `3767b4c`). The bootstrap-pointer check read the 8 bytes before every freed
  pointer and, on a magic match, `munmap`ed the address and length found there
  -- before anything had established the pointer was libumem's. Position D. The
  first fix (a live-count gate) did not hold: 28 bootstrap mappings survive
  `umem_init()` in every process, so the count never reached zero; the second
  (a registry) reintroduced P8.1's collapse; the third adds a hull in front of
  the registry. `test/security/test_forged_bootstrap`: SIGSEGV before, PASS
  after, interposer ratio unchanged. Found by the P8.3 work and the
  production-readiness review independently.
- **`UMEM_OPTIONS=reap_interval=0` spun a core for the life of the process,
  and was honoured under `AT_SECURE`** (P5.11, `cbb1a2e`). The update thread's
  deadline is `now + interval`; at 0 it is always past and the update pass ran
  back to back. Introduced by v3.2.0 starting the thread at init. A hostile
  environment could burn a core of a setuid target -- glibc ignores all its
  tunables there. 0 is now refused at the parser. `test_reap_interval_zero.sh`.
- **Per-thread cache structs shared slabs with user buffers** (P5.12,
  `444b062`). `umem_ptc_t` -- 36 bins of slot pointers and the pool of
  addresses the next `umem_alloc()` returns -- came from the 24 KiB user size
  class, one object per slab, so every PTC sat exactly one object from a user
  buffer (measured 8 of 8). A one-byte overrun reached the allocator's
  pointers; glibc safe-links its tcache, libumem's slots are raw. Now from
  its own `UMC_INTERNAL` cache, as the magazines always were. Slot mangling
  itself remains open (P5.13). `test_ptc_adjacency`.
- **Every foreign `free()` under `LD_PRELOAD` walked the heap under `vm_lock`**
  (P5.15, `3e5d326`). `umem_may_own()`'s ownership check refreshed its bounds
  on a miss by walking every span under the heap arena's lock -- and a miss is
  the common case for a foreign pointer. A signal handler freeing a foreign
  pointer while the interrupted thread was in that walk deadlocked; a program
  freeing foreign pointers in a loop serialised every thread. The heap arena
  now publishes its bounds when it grows and the check is two lock-free loads.
  The error log's own lock (`umem_error_lock`) is a trylock that drops the
  line under contention (`5286dcb`), and the recoverable path no longer
  symbolises a stack through libdw unless the output will be seen (`a074636`;
  on aarch64 that was ~34 frames and milliseconds per refused free).
  `test_errlog_signal.sh`.
- **`mmap_guard` is now ignored under `AT_SECURE`** (P5.14, `a1912e2`):
  `mmap_guard=0` from the environment would have disabled the `PROT_NONE`
  use-after-free guard on a setuid target.

### Fixed

- **Fork children leaked every non-forking thread's per-thread cache**
  (P1.3d, `1efdaa0`). The PTC had no fork handler: after `fork()` the child
  held a copy of every parent thread's `umem_ptc_t` (up to ~24 KB and ~600
  cached objects each) with no owner and no drain. A registry of live PTCs is
  now walked by the child's atfork handler and each orphan is drained as a
  thread exit would. Because the PTC push paths are lock-free and were left
  unordered -- every ordered form measured a 4-5 % hot-path cost -- the child
  drops the top entry of each non-empty bin and magazine before draining (at
  most 38 objects per PTC, never a stale pointer) and skips a PTC caught
  mid-swap. Hot path: +0.00 % instructions, inside the null control.
  Regression `test_fork_ptc_drain_probe`.
- **`umem_free(NULL, size)` with a non-zero size put NULL on a free list, and
  the next `umem_alloc(size)` on that thread returned it -- NULL with
  `errno == 0`.** `_umem_free()` indexed the size-class table and stored the
  pointer into the per-thread bin (or, with `tcache=0`, the CPU magazine)
  without checking it; the only NULL check was for `size == 0` on the
  oversize branch. `umem_cache_free(cp, NULL)` had the same hole;
  `umem_free_align(NULL, n)` panicked ("bad free") instead. All three are now
  no-ops, as `free(NULL)` is, and the man pages say so
  (`umem_alloc.3`, `umem_cache_create.3`). Found by the 2026-09-24
  production-readiness review (section 2.1). The `LD_PRELOAD` interposer's
  `free()` already returned on NULL and was not affected. Regression
  `test/unit/test_free_null` (in `make check`) fails at the parent commit and
  passes after. `test/unit/test_error_paths.c` had a comment calling this
  undefined behaviour; it now asserts the contract. (P1.8)
- **The per-thread magazine layer behind the PTC bins was never primed.**
  Each thread has two magazines per size class between its bin and the depot,
  meant to make bin overflow lock-free. Nothing ever gave them a magazine:
  they took from the depot by trylock only and never allocated one, and the
  depot's empty list is fed only when a per-CPU magazine drains, which a
  steady alloc/free loop never does. So on a cache one thread cycles, every
  object past the bin paid 1 + min(ncpus, 8) + 1 *failed* trylock/unlock
  pairs and then the per-CPU lock: `perf` 39 % `pthread_mutex_trylock`,
  34 % `pthread_mutex_unlock`. One thread, 512 B, alloc N then free N:
  160 Mpairs/s at N=64, **6.0 at N=128** (c7i.2xlarge; 102 -> 3.9 on
  c7g.2xlarge) -- a 27x cliff at the bin's capacity. Fix: on the first
  free-side miss, allocate one magazine from the magtype cache, as the CPU
  layer does. Post-fix 137 / 97.6 Mpairs/s at N=128; instructions per pair
  past the bin down 90 %; inside the bin and at 16:64 / 16:1024 t=1 and t=8
  within the null control on both arches. Regression
  `test/integration/test_ptc_mag_primed` counts depot trylocks from the PTC
  paths in the probe build: 25,600 per 200 rounds before, 0 after. New
  instrument `test/bench/bench_pairs` (bare loop) and rig
  `scripts/ec2/hotpath_ab.sh`, whose first version measured its own bench
  binary's layout as an 8 % library regression; it now uses one bench
  binary for every arm. (P8.6)

### Performance

- **Per-thread caching now covers 2.5-8 KB objects; the 1k:4k cliff at 128+
  threads is gone.** Sizes above 2048 B bypassed the per-thread cache, so
  every operation took the per-CPU lock and, every 31 operations per CPU, a
  blocking depot trip into a stripe other CPUs had emptied; at 128+ CPUs
  the depot convoyed. `multi` 1024:4096 on `c8g.metal-48xl` (192 vCPU):
  t=64/128/192 **275 / 128 / 179 Mops -> 314 / 352 / 429**, 0.42x -> 1.24x
  glibc at t=128, p999 2.8 us -> 39 ns; on `c7i.2xlarge` t=8 0.69x -> 1.22x
  glibc, p999 921 -> 130 ns. Per-class bare loop at t=8: 2560-8192 B were
  23-66 Mpairs/s against 600 for 1536 B; all are 590-620 now. `PTC_NBINS`
  28 -> 36 (`sizeof (umem_ptc_t)` 22,144 -> 24,000 B), `tcache_max` default
  2048 -> 8192. The 2-8 KB magazines also went from 31/15 rounds to 63,
  measured separately and inside the null at every point; it is kept as the
  right table for a 16-object slab, not as the fix. This works because P8.6
  gave the new bins a working L2 behind them. `c7i.metal-48xl` was not
  re-measured (no capacity in us-east-2 during the run). (P8.2b)

- **Depot steal scans no longer lock stripes that are empty.** Both depot
  pop primitives took a stripe's lock before looking at its list. A reload
  that misses its own stripe scans up to all the others, and on a workload
  whose live set grows faster than its frees return (`frag`) nearly every
  stripe is empty: eight threads each took and released 16 locks per miss to
  read 16 NULLs, and held each other's stripes doing it -- 98.6 % of 17M
  depot pops on `c7i.2xlarge`, 434k blocking-lock contention events per 80k
  successful reloads. An unlocked head check first: contention events
  434k -> 0, `pthread_mutex_trylock` gone from the profile (was 17-22 % of
  cycles), sustained `frag` 16:64 at t=8 0.57x -> 0.72x glibc (x86) and
  0.77x (arm). The p999 tail (19 us vs glibc 2 us) is unchanged and is the
  slab layer's one-object-per-lock, not the depot; the 192-thread metal
  figure is not re-measured. The plan's diagnosis for this point --
  cross-stripe *stealing* -- was wrong at 8 threads (9 of 10 reloads are
  local); the cost was cross-stripe *scanning*. Regression
  `test/integration/test_depot_empty_scan` (probe build): locked-empty pops
  <= 1 %, pre-fix 98.6 %. (P8.5, partial)

- **The `LD_PRELOAD` interposer's per-call overhead is down by a third; the
  gap it hid was 2.5x, not 20 %.** The 2026-09-24 comparison put preload at
  0.74-0.91x of the `umem_alloc` API in `bench_main`; a bare loop with one
  binary and the same `libumem.so` put it at **0.31x** (c7i.2xlarge, t=1,
  16:64) -- 418 vs 147 instructions per pair -- because `bench_main`'s own
  per-op cost (t-digest, two clock reads) is most of what it measures.
  Five candidates, each measured on its own commit with a null control
  (`scripts/ec2/interp_ab.sh`, 9 alternating pairs, t=1 and t=8):
  `is_bootstrap_pointer()` called out of line three times per free, now one
  inline read (-42 insn/pair, +30 %); the hull loaded once per free instead
  of two out-of-line `umem_may_own()` calls (-47, +12..20 %); the thread's
  `errno` address cached instead of a `__errno_location()` PLT call per free
  (-7, +1.5 %, arm); the `libc_ptr_live` gate inline in `free()` and a
  straight-line `umem_malloc_free()` for the two layouts `umem_malloc()`
  produces below `UMEM_MAXBUF` (-51, +24 %). A live-count gate on the
  bootstrap magic read was measured and dropped (+1.6 % instructions, no
  throughput change on arm; the compare is against a line the decode loads
  anyway). Net: **418 -> 277 instructions per pair, 35.6 -> 69.7 Mpairs/s
  (+96 %) at t=1, 167 -> 265 at t=8**; preload/API 0.31 -> 0.59 (x86),
  0.40 -> 0.53 (arm) in the bare loop. The P5.8 validation order is
  unchanged in every path: header inside the hull before it is read, size
  consistent with the layout the magic names and the whole object inside
  the hull before anything is written; `test_forged_free`,
  `test_abort_option.sh` and the new `test/security/test_free_errno` (free
  leaves errno alone, every layout, both entry points, two threads) pass
  on both arches. The remaining 130 instructions are what `malloc`/`free`
  must do that `umem_alloc`/`umem_free` need not -- write, read and
  validate a header, and the `umem_malloc()` wrapper's ready check,
  recursion guard and size arithmetic -- so the plan's 0.95 target is not
  reachable in a bare loop with this header design and the entry records
  that. Regression `test/stress/repro_interpose_free_ratio` (via
  `interpose_regress.sh` in `make check`): preload/API >= 0.48 at t=8
  against an API/API null, pre-fix 0.41 / 0.40, post 0.61 / 0.53. One
  correction on the record: `malloc.c` described `is_bootstrap_pointer()`
  as a range check; it is a magic compare on `buf[-1]`, made before the
  hull test by design (a bootstrap mmap can lie inside the hull's gaps),
  and the comment now says so. (P8.3)

### Removed

- **`cache_mag_reloads`, a counter nothing incremented.** `921b502`
  (2026-04-20) removed its hot-path increments and left every reader:
  `umemctl stats`/`cache` and the introspect socket printed `mag_reloads 0`
  forever, `UMEM_PROFILE`'s `optimal_magazine_size` column was always 15
  (the `total_reloads == 0` branch), the reload-ratio arm of
  `magazine_tune=1` could never fire, and `test_umem_stats/mag_reloads` had
  returned SKIP on it since. Field, `_prev` twin, printers, the profile
  function and the test are gone; the profile file keeps the column at its
  historical constant 15 for format compatibility. `cache_alloc_ops_prev`,
  which only that arm read, goes with it. Comment review #4.

## [3.2.0] - 2026-09-24

The theme of this release is *things that were never running*. Three of its
fixes are for machinery that existed in the source, was documented, and did
nothing in any ordinary process: the maintenance thread (never started), the
per-CPU cache layer (every thread on slot 0), and the documented
`UMEM_OPTIONS=abort` escape hatch (no such option). A fourth, the ~5 GB Linux
heap ceiling, was fixed in v3.1.0's follow-up and then found to be half-fixed
by a systematic hard-limit hunt, whose other findings are also here. Every fix
carries a regression that fails on the parent commit and passes after, on
x86_64 and aarch64; where the first diagnosis was wrong, the entry says so.

### Compatibility

- **No API or ABI change.** Public headers (`umem.h`, `umem_hooks.h`,
  `umem_inspect.h`) are byte-identical to v3.1.0; `sizeof(umem_hook_t)` is 120;
  soname `libumem.so.1`.
- **Behaviour changes a deployment may notice:** every process now has a second
  thread from `umem_init()` onward (see Known); freed heap memory is returned to
  the OS after `reclaim_delay` (30 s) where before it never was; freed spans
  under 16 MiB are `MADV_DONTNEED`'d rather than `PROT_NONE`-remapped, so a
  use-after-free into such a span reads zeros instead of faulting
  (`UMEM_OPTIONS=mmap_guard=0..N` adjusts).
- **New tunables:** `UMEM_OPTIONS=abort`, `UMEM_OPTIONS=mmap_guard=N`.

### Fixed

- **The ~5 GB heap ceiling on Linux is removed -- in two halves, the second of
  which corrected the first.** Root cause was never the mmap backend (three
  attempts there failed and were reverted) but slab density: under Linux's 4 KiB
  heap quantum, libumem created far smaller slabs than under Solaris's 64 KiB
  quantum, and each slab span is its own `mmap(MAP_FIXED)` that the kernel does
  not merge, so the heap exhausted `vm.max_map_count` (65,530) long before
  memory ran out.
  - Half one (`3f2e67c`): the hashed best-fit path gave 1 object per 4 KiB slab
    for 4 KiB chunks (Solaris: 16). `UMEM_MIN_SLAB_OBJECTS` (16, capped at a
    64 KiB slab) restores Solaris density. VMAs at 2 GB of 4 KiB objects:
    16,283 -> 75. A 9 GB regression went from 9 failures / 65,532 VMAs to 0 / 74.
  - Half two (`cf3f762`): that fix was declared complete and was not. The
    Phase 6 hard-limit hunt found 512 B objects still failing at **8.2 GB** with
    63,323 VMAs (glibc: 54). Objects <= 512 B take a different path -- one-page
    slabs served through `umem_va`'s quantum cache, whose slabs were 128 KiB, one
    VMA each. The first floor explicitly excluded that path. `UMEM_MIN_QCACHE_SLAB`
    (4 MiB) floors quantum-cache slabs: VMAs at 2 GB of 512 B objects
    15,702 -> 274, at zero measured small-heap RSS cost (5 MB either way; the
    slabs are `MAP_NORESERVE` and touched only as they fill). The lever was
    chosen from a measured table of six alternatives, not argued.
  - Both floors are arithmetically no-ops at a 64 KiB quantum, and on illumos
    `umem_va` has no quantum caches at all (its requested `qcache_max` of
    8 pages is smaller than one 64 KiB quantum). This is checked, not assumed:
    `test/unit/test_slab_floor` simulates a 64 KiB-quantum arena on Linux and
    asserts every slab size equals unfloored best-fit.
  - `test_heap_ceiling` gained a 512 B arm (`test_heap_ceiling_512.sh`). The 4 KiB
    arm passed at 9 GB throughout the second defect; a test of one path was blind
    to the other.
- **The update thread was never started; freed memory was never returned.**
  The only creator of libumem's maintenance thread was `umem_reap()`, which is
  called by the application or by a *failed* backend allocation. In a process
  that never ran out of memory -- almost every process -- there was no thread,
  so the periodic pass never ran: no hash-rescale requests, no magazine
  resize, no depot working-set reaping, no slab page reclaim. Every feature
  documenting itself as "background" was dead. Found by the Phase 6 limit hunt
  (P6.8: 2 GB freed, 100 % resident at 100 s) and then traced one layer
  deeper than the first diagnosis when the first fix measured as no change
  (`gdb`: one task in the process). Three changes, each shown necessary by
  isolation:
  - `umem_init()` starts the update thread (`9bbe58b`).
  - The periodic pass requests a depot reap when any list is above its working
    set (`147d5ff`); before, only `umem_reap()` did, so freed objects stayed in
    depot magazines and `slab_refcnt` never reached zero.
  - `umem_maglist_mark_excess()` no longer clamps `ml_min` to 8, which had
    capped every reap at 8 magazines per list per pass -- ~8 MB per 10 s
    against a 2 GB surplus. With `umem_reap()` every 1 s: 135 -> 6 MB in 12 s
    (was 135 -> 126).
  - Regression `test_reclaim_returns`: 128 MB freed with **no** `umem_reap()`
    call, RSS 135 -> 8 MB at t = 6 s (was 135 -> 135 over 20 s).
  - The `umem.c` header's "Nuance" section, which told Linux users to call
    `umem_reap()` periodically, is rewritten.
  - Forked children recreate the thread in the child atfork handler
    (`cceae1d`); before, a pre-fork server's every worker was in the dead
    state above for its whole life. Regression `test_fork_child_reclaim`.
- **`UMEM_OPTIONS=abort` now exists.** The interposer clears `umem_abort` so
  foreign pointers under `LD_PRELOAD` are logged rather than fatal, and both
  the source and README told users `UMEM_OPTIONS=abort=1` restores the abort.
  No such option existed -- only `noabort`, in the `UMEM_DEBUG` table -- and
  `abort=1` would have been rejected for carrying a value. `abort` is now an
  `UMEM_OPTIONS` flag, honoured in secure mode (arming can only make the
  process crash, never continue). Regression `test_abort_option.sh`: default
  refuses and completes, `UMEM_OPTIONS=abort` on the same forged free dies with
  SIGABRT. The first attempt put it in the wrong table and the test caught it.
- **Per-thread cache footprint 31.6 -> 22.1 KB; thread exit hands each bin
  back in one batch** (`b8c39e6`, `06559e5`). The slot arrays were padded to
  128 for all 28 bins though 15 of them index 64 or 32; they are now packed at
  their real capacities. `umem_ptc_destroy` freed cached objects one `cc_lock`
  at a time (~600 per thread); a bin is now one `umem_cache_free_batch`.
  A first version also halved the bin capacities and lost 7 % on the `single`
  bench; isolating that found a 28x cliff at the bin boundary that exists in
  every prior release (the per-thread magazine layer behind the bins is never
  primed, recorded as P8.6) and, separately, that the bench's own t-digest
  histogram was the -7 % -- the allocator itself got faster (p50 35 -> 33 ns).
  Capacities are unchanged. Regression `test_ptc_footprint`.
- **Freed spans are `MADV_DONTNEED`'d, not `PROT_NONE`-remapped, below 16 MiB**
  (`ab8a73d`). `vmem_mmap_free` remapped every freed span `PROT_NONE` with
  `MAP_FIXED`, splitting the RW mapping it came from: one permanent kernel VMA
  per freed span. 40,000 half-freed 136 KiB oversize objects cost 40,102 VMAs
  (glibc: 54); 2,000 destroyed caches left 1,216. `MADV_DONTNEED` returns the
  pages identically (measured) without touching the VMA. The fault-on-use-
  after-free property of `PROT_NONE` is kept for spans >= 16 MiB, where VMA
  count cannot matter; tunable `UMEM_OPTIONS=mmap_guard=N` (0 = never guard).
  Regression `test_oversize_vma.sh`: 1,999 -> 0 new VMAs per 1,000 frees, with
  a guard arm proving the `PROT_NONE` path still exists.
- **Per-cache footprint: three page-rounded `mmap`s became one exact block**
  (`e00fdf2`). Each `umem_cache_create()` mapped two depot arrays and the rseq
  array separately, 512 B each in a 4 KiB page on 8 CPUs: 12 KB of an 18.6 KB
  per-cache footprint was page rounding. Now carved from one mapping sized to
  its contents, allocated from `umem_cache_arena` (`e00fdf2`, `ede1849`):
  18,964 -> 11,173 B/cache. The VMAs left behind by `umem_cache_destroy`
  (1,134 per 2,000 destroys) were two further defects the same test exposed:
  freed descriptor pages `PROT_NONE`-remapped by `vmem_mmap_free` (above),
  and the per-cache block's own `munmap` orphaning the heap pages on either
  side of it (`mmap(NULL)` hands out addresses top-down, so it landed
  between them). Now 3 VMAs per 2,000 destroys; 50,000 caches create in
  23.6 s (was 36), 11.0 KB each (was 18.6), and leave 175 VMAs (was 29,159).
  Regression `test_cache_footprint`.
- **Every thread used the same per-CPU cache.** The per-thread CPU hint that
  selects `cache_cpu[]` was `pthread_self()` cast to `int` -- a page-aligned
  address whose low bits are always zero -- so `hint & cache_cpu_mask` was 0
  for every thread, cached forever. Every operation that reached the magazine
  layer (all sizes above `tcache_max`, every PTC miss below it) serialised on
  one `cc_lock` for the whole process: the P8.2 collapse, 0.06x glibc at 64
  threads on metal, 1.4 Mops/s at 8 threads for 2560-byte objects where
  1536-byte ones did 32. On Solaris the hint is `thr_self()`, a small integer;
  the port never had a working one. Fixed at `get_cached_cpu_hint()`: rseq
  `cpu_id` (registering first), else `sched_getcpu()`, cached once. 8 threads
  now use 8 slots (was 1); 2560 B t=8 1.4 -> 16.4 Mops (glibc 22.8); PTC-served
  sizes unchanged; t=1 within noise. Regression `test_cpu_hint_spread`.
- **Interposer `free()` ~500x collapse** (`a74065e`). `interpose_owner_of()` took
  a global lock and scanned 512 slots on every `free()` -- a table that is empty
  after bootstrap -- plus decoded the header twice. An atomic `libc_ptr_live`
  gate skips the scan once READY. Regression `repro_interpose_free_scaling`:
  pre-fix 0.29x of API throughput, post 4.10x; on 192-vCPU metal, t=192
  0.80 -> 313 Mops/s, null-controlled.
- **P5.4 mangling evidence gap closed.** The original regression could not
  distinguish freelist-link mangling from slab containment, because its target
  was outside the victim slab and containment caught it first. A new `inslab`
  case targets the *live neighbour* -- inside the slab, aligned, so only
  mangling stands in the way -- and FAILs with `-DUMEM_NO_LINK_MANGLE` (the
  allocator hands back a still-allocated buffer: a double allocation) while
  PASSing by default. Both controls are now independently demonstrated.

### Changed

- **P1.3c's exact ledger counted stale pops as lost objects.** The oracle
  behind `test_ptc_resize_no_loss_probe` scanned all `cap` slots of a freed
  magazine shell; the alloc paths pop `mag_round[--rounds]` without clearing
  the slot, so a once-full magazine carries a tail of stale pointers, and the
  ledger counted that tail -- 127 or 254 at a time, the size of the real
  defect. Latent until the update thread ran routinely (resizes are what make
  a shell stale), then ~1 in 8 runs on x86_64. It counts only owned slots now;
  discrimination re-verified: with the P1.3c drain disabled it reports exactly
  127 per shell (635, 1016, 635 over three runs), fixed 0/12.
- `test_inspect_e2e` walks with `-n 0` (unlimited). It passed at `-n 200` only
  because the whole heap was 74 entries; the walk lists internal metadata
  caches first, so any real process would have shown 200 bufctls and none of
  the user's buffers. The 4 MiB qcache slab, which pre-creates ~1,100 bufctls
  (~35 KB) at startup, turned the latent assumption red.

### Known

- Every process now has a second thread from `umem_init()` onward. Programs
  that count their own threads, or fork-then-exec paths that assumed a
  single-threaded parent, will see it. It is detached, handles no signals,
  and sleeps between intervals.
- Startup allocates ~1,100 more bufctl records (~35 KB) than before, for the
  `umem_va_4096` and `umem_va_32768` quantum-cache slabs. This is fixed cost,
  not per-object.

## [3.1.0] - 2026-09-23

Security hardening. An adversarial audit of v3.0.0 (2026-09-22) found one
critical and three high-severity issues; all ten findings are fixed, each with a
regression that demonstrates the pre-fix exposure and passes after, on x86_64 and
aarch64.

**Read this if you deploy libumem:** v3.0.0 and earlier should not be used in
setuid/setgid processes, as root, or with an attacker-influenced environment.
This release closes those holes. See "Security status" in README.md.

### Security — fixed

- **Critical: `execlp("addr2line")` ran on the allocator's startup path with no
  privilege gate.** `umem_stacktrace_init()` is called unconditionally from
  `umem_init()`, and `execlp` resolves through `PATH`, so a setuid binary *linked*
  against libumem (`AT_SECURE` blocks `LD_PRELOAD`, not linkage) executed whatever
  `addr2line` the attacker's `PATH` named, as the elevated user, before `main()`.
  Demonstrated with a hostile `addr2line` that ran and left a sentinel.

  **Deleted rather than gated**, because it never worked: it passed
  `-e /proc/self/exe`, which after the exec names *addr2line itself*, not the
  target. Verified empirically — the tier resolved `?? ??:0` where a correct
  invocation resolved `main at demo.c:38`. `dladdr` and `libdw` remain;
  `UMEM_STACKTRACE_ADDR2LINE` is gone.

- **High: no privilege gating on option parsing at all.** `UMEM_OPTIONS`,
  `UMEM_DEBUG` and `UMEM_LOGGING` were honoured unconditionally, so a hostile
  environment could make a privileged process create and truncate files
  (`profile=record:/path`), open a control socket (`introspect=1`), or change
  memory-safety behaviour. Adds `umem_secure_mode()`
  (`issetugid() || getauxval(AT_SECURE)`), consulted in `process_item()` — the
  single point every option flows through — *before* argument parsing. Options
  with file, socket, or exec side effects are ignored in secure mode; pure tuning
  options still work. `UMEM_PROFILE`, which bypasses the option table, is gated at
  its own call site.

- **High: library file writers followed symlinks.** Snapshot and profile writers
  used `open(..., O_CREAT|O_TRUNC)` and `fopen(path, "w")` with no `O_EXCL` and no
  `O_NOFOLLOW`. Pre-fix a symlinked victim file went from 54 to 5472 bytes. Now
  one shared `umem_open_write()`: `O_NOFOLLOW`, `S_ISREG`, `st_nlink == 1`,
  `st_uid == geteuid()`, mode 0600, and `ftruncate` only *after* the checks so a
  hardlinked victim is not destroyed first. Leading-component symlinks remain
  out of scope and are documented.

- **High: slab freelist links sat inside freed user buffers, unmangled.** For
  non-hash caches — the default for small objects — `bc_next` occupies the last 8
  bytes of the user buffer, so a one-buffer overflow into an adjacent freed buffer
  set the link and the allocation after next returned an attacker-chosen address.
  glibc has mangled these since 2.32. Pre-fix the attack returned the target
  address and then aborted walking the corrupted chain.

  Two controls now: links are stored as `ptr ^ cookie ^ (&slot >> 12)` (cookie
  from `AT_RANDOM` via `getauxval` — no syscall, no allocation, since
  `umem_init()` cannot allocate), and `umem_slab_alloc()` validates alignment and
  slab containment before dereferencing, reporting `UMERR_BADADDR` instead.

  **Honest scope:** isolating the two controls shows the *containment check*
  blocks the tested attack on its own, because that test's target lies outside the
  victim slab. Mangling covers an in-slab target, which no test currently
  exercises. So the verified claim is "this attack shape is blocked", not
  "mangling stops it".
  `docs/results/2026-09-23-p54-which-control-blocks.md`

- **Control socket: predictable path and a reclaim TOCTOU.** The path was
  `/tmp/umem.<pid>.sock`, and the stale-path reclaim used `stat` — which *follows
  symlinks* — so a symlink aimed at another process's socket satisfied
  `S_ISSOCK`, the probe `connect` failed, and the target unlinked a directory
  entry it never created. Demonstrated: the victim socket was removed. Now
  `lstat`, a euid-private directory (`$XDG_RUNTIME_DIR` when it passes an
  `O_NOFOLLOW` ownership check, else `/tmp/umem-<euid>` created by atomic
  `mkdir(0700)`), and a bind-then-`rename()` reclaim that removes nothing it did
  not create.

- **Control socket accepted the real uid.** `SO_PEERCRED` checking
  `cred.uid == getuid()` hands an unprivileged invoker control of a setuid target
  — including `break`, which parks allocating threads and is therefore a DoS
  against the host process. Now `geteuid()` or root only.

- **Interposed `free()` mutated state before validating a foreign pointer.**
  `process_free()` decoded `buf[-1]` for any non-bootstrap pointer, and the magic
  is a fixed constant, hence forgeable. Worse, every successful-magic branch wrote
  `malloc_stat = UMEM_FREE_PATTERN_32` *before* any size check, and the
  oversize/memalign branches wrote one tag before validating the other — so a
  forged header left a half-applied free behind. Adds `umem_may_own()` (a
  refreshed convex hull of the heap's spans, CAS-widened so a stale read can only
  false-miss), checked before the header read and again after decode, with all
  mutation moved after acceptance.

  `umem_abort = 0` in interpose mode is retained but the comment claiming it
  "matches glibc" was false — glibc aborts. It is defensible now only because a
  rejected pointer leaves state untouched; `UMEM_OPTIONS=abort=1` restores
  aborting.

- **`getpcstack()`'s frame walk had no stack bounds.** It validated alignment, a
  16 MiB ceiling and monotonic frames, then dereferenced. Under
  `UMEM_DEBUG=audit` a corrupted chain made the allocator read arbitrary
  addresses — SIGSEGV demonstrated pre-fix on both architectures. Now consults
  the real stack bounds (`pthread_getattr_np`/`pthread_attr_getstack`, cached per
  thread); the 16 MiB heuristic remains only as a documented fallback. Read-only:
  independently confirmed the only writes are into the caller's own bounded
  buffer.

- **`errno` erasure: the v3.0.0 fix was ineffective, not merely partial.**
  Corrected claim. `vmem_mmap_alloc()` erased `errno` **twice**, and the second
  one is on the address-space-exhaustion path: it is reached with `ret == NULL`
  whenever `vmem_alloc(src)` fails, which overwrote the `ENOMEM` that v3.0.0
  carefully preserved one frame below in `vmem_mmap_top_alloc()`. So the
  "errno=0 Success" symptom that fix was written for would still have occurred.
  Both now restore `errno` only on success. Siblings in `vmem_sbrk.c:275` and
  `vmem_stand.c` are reported, not fixed (sbrk is non-default and secure-gated;
  `vmem_stand.c` is not built).

### Documentation — corrected

- **`UMEM_DEBUG=audit` captures only ~2 frames in a default build**, and the
  cause is *libumem's own* compilation, not the application's. `AM_CFLAGS` has no
  `-fno-omit-frame-pointer`, so the walk stops at the first allocator frame:
  measured depth 2 through `umem_alloc()` versus 7 from a frame-pointer-having
  caller. `umem_debugging.7` previously blamed "aggressively-stripped" application
  binaries, so its advice did not work. README's "alloc-site tracebacks" is now
  "alloc-site capture" with the limitation stated and the actual remedy given.

### Notes

- No API or ABI break. `libumem.so.1` unchanged.
- Minor version, not patch: the secure-mode gate changes observable behaviour for
  callers that relied on `UMEM_OPTIONS` taking effect in a privileged process.
- Still open, unchanged: the ~5 GB Linux heap ceiling
  (`docs/results/2026-09-22-umem-heap-ceiling-vma.md`), and the P5.4 evidence gap
  above.

## [3.0.0] - 2026-09-22

### Compatibility

- **No API or ABI break.** `umem_hook_t` is caller-allocated, and the
  unregister-drain work below grew it from 120 to 128 bytes on LP64 — which would
  have made an application compiled against the v2.7.0 header allocate a struct
  too small for the library to write into. The new fields were packed into the
  padding that `int hook_active` already occupied, so `sizeof(umem_hook_t)` is
  back to 120 (verified by compiling the v2.7.0 and current headers
  side by side). The soname stays `libumem.so.1`, which also keeps the documented
  illumos `LD_PRELOAD=.../libumem_malloc.so.1` dual-ABI recipes working.
- **The major version bump is for the removed build options below**, not for a
  source or binary incompatibility: code that compiled and linked against v2.7.0
  still does.

### Removed (breaking)

- **`--enable-percpu-caching` is gone.** It did not produce a slower or
  less-tested allocator; it produced a compile error. `umem_percpu.c`
  indexed a nonexistent `umem_cache_t.cache_percpu`, read a nonexistent
  `umem_magazine_t.mag_size`, used an undeclared `umem_max_ncpus`, and
  called five functions that are `static` inside `umem.c`. Verified on EC2
  before removal:
  `docs/results/prefix-evidence/2026-09-21-percpu-build-failure.log`. The
  design was unfinished besides — nothing ever called
  `umem_init_percpu()`, so even a building version would have allocated
  nothing; the magazine reload leaked a reference; the free path
  dereferenced `pc_loaded` without a NULL check. Source quarantined in
  `attic/` (not built, not installed, not distributed), with each defect
  recorded in `attic/README.md`.

- **`--enable-htm` is gone.** `HTM_SOURCES` was unconditionally empty, so
  the flag never added code to the build, while `umem_htm.h` advertised a
  "5-15% improvement" and `Makefile.am` claimed the files "have not been
  written" — they were in the tree. The prototype's lock elision is also
  unsound: `UMEM_HTM_TRY` never adds the fallback lock to the
  transaction's read set, so a thread holding that lock does not abort a
  concurrent transaction and both can believe they own the depot.
  Additionally the depot fast paths are comments,
  `umem_htm_depot_alloc()` always returns `NULL`, `_xtest()` is read as
  proof the fallback lock is free (it only proves the caller is in a
  transaction), and `_xabort()` is called with a runtime value where the
  instruction requires an immediate. Quarantined in `attic/`.

- **`umem_numa.[ch]`: the unimplemented policy layer is gone; the topology
  queries remain.** Removed because each was advertised and did not work:
  `umem_numa_alloc()`/`umem_numa_free()` (alloc fell back to `malloc()`
  while free unconditionally called `numa_free()` — an allocator mismatch
  on a reachable path), `umem_numa_get_node()` (documented as the current
  CPU's node; actually hashed `pthread_self()` into capacity-weighted
  partitions, an unrelated quantity), `umem_numa_depot_alloc()`/`_free()`
  (both opened with a `numa_info = NULL; if (numa_info == NULL) return;`
  placeholder, so the per-node depots were unreachable, and the depot
  struct's padding expression `64 - (16 + 32 + sizeof(pthread_mutex_t))`
  is negative on x86-64 glibc — an invalid array bound),
  `umem_numa_set_policy()`/`_get_policy()` (stored an enum nothing
  dispatched on), and `umem_numa_stats()` (zeroed the caller's struct and
  returned). What is left — node count, CPU→node mapping, distance
  matrix, explicit bind/migrate helpers — is real and is used by tests and
  benchmarks.

  The separate `HAVE_LIBNUMA` topology code in `umem.c` that fills
  `umem_cpu_node[]` for the depot's cross-CPU steal is **untouched and
  live**; it never went through `umem_numa.[ch]`.

- **`UMEM_OPTIONS=numa` is gone.** It was registered behind `#ifdef
  UMEM_NUMA_AVAILABLE` in `envvar.c` — a macro *defined by* the header the
  `#ifdef` guarded, so the include never happened and the tunable was
  never registered on any build. Not repaired but removed:
  `umem_numa_enabled` is now a detection result, not a policy switch, so
  letting the environment write to it could only falsify it.

- **`--enable-avx2` replaces automatic AVX2.** Configure used to turn
  "the compiler accepts `-mavx2`" into a global `-mavx2` for the entire
  library. Compiler acceptance says nothing about the CPU the binary runs
  on, so a nominally generic x86-64 build acquired an AVX2 requirement and
  would `SIGILL` on pre-Haswell hardware at an arbitrary point inside the
  allocator. The default build now targets the x86-64 SSE2 baseline;
  `--enable-avx2` opts in and warns that the result is not generic. There
  is no runtime dispatch — the SIMD helpers are `static inline` in a
  header consumed library-wide, so dispatch means restructuring them, not
  a configure change.

### Fixed

- **Ten reachable correctness and lifetime defects in default code paths**
  (`docs/plans/2026-09-21-production-readiness.md`, Phase 1). Each was fixed at
  the shared function and each carries a regression that fails before the fix
  and passes after, verified on x86_64 and aarch64 in isolated builds:

  - **Interposed `calloc` handed out overlapping live allocations.** The
    recursion guard was a process-global `int` set on *every* ordinary call, so
    one thread's `calloc` made concurrent callers take the static-buffer path,
    and the buffer offset reset while earlier allocations were still live.
    Pre-fix: cross-thread overlap at identical addresses (`owner=1` vs
    `owner=0`) then SIGSEGV; `pthread_create` failing with "cannot allocate
    memory for thread-local data" at two threads. Now per-thread initial-exec
    TLS, with one ownership classifier shared by `free`, `realloc`, and
    `malloc_usable_size`.
  - **`fork()` deadlocked against ordinary allocation (ABBA).** The fork
    handler took depot `ml_lock`s before per-CPU `cc_lock`s; allocation takes
    `cc_lock` first and then blocks on `ml_lock`. Pre-fix: gdb stacks on both
    architectures showing the cycle on the same cache. The handler's comment
    claimed its order matched normal operation; it was the reverse.
  - **The malloc interposer did not participate in `fork()` at all.** Two
    mutexes that ordinary `free()`/`realloc()` take had no handlers, so a child
    could inherit one held by a thread that no longer existed. Pre-fix: the
    child deadlocked on the *first* fork (300 forks, 1 hang).
  - **Thread exit lost half of every cached PTC bin.** `umem_ptc_destroy()`
    called the half-bin flush once per bin and then freed the PTC, so the
    remainder lost its only reference while the slab layer still counted it
    allocated. Its comment said "Flush all bins". Pre-fix, exact oracle: **2304
    objects stranded**; post-fix 0.
  - **A magazine's capacity was not sampled with the magazine.** A resize
    between obtaining a magazine and reading `mt_magsize` let a 127-round
    magazine be indexed as 255, past the end of its own allocation. Pre-fix:
    **3,133,215 capacity desyncs** (`recorded=255 true=127`) and a SIGSEGV. The
    free side had the same bug reversed, putting a half-filled magazine on the
    depot's *full* list. Capacity now derives from the magazine itself.
  - **Populated magazines were discarded on a magtype mismatch.** Both return
    paths freed the shell without draining it, and callers pass *full*
    magazines. Pre-fix: 127 objects lost from one shell, 381 from three —
    exactly one magazine's capacity each.
  - **`umem_cache_destroy()` leaked every retained empty slab.** With
    reclamation on by default, freeing the last object retains the slab; destroy
    only *logged* the nonzero `cache_buftotal`. Trigger: create a
    `UMC_NOMAGAZINE` cache, allocate one object, free it, destroy. Pre-fix:
    4096 bytes leaked per cache, with `vmem_destroy()` independently agreeing.
  - **Reclamation discarded metadata the allocator reads again.**
    `MADV_DONTNEED` zeroed buftags and free patterns that are written only at
    slab creation and never rebuilt, so a later *valid* allocation failed
    libumem's own corruption check. Pre-fix: `boundary tag corrupted` +
    SIGABRT, and an `ASSERT(sp->slab_refcnt == sp->slab_chunks)` for embedded
    freelist links on larger-quantum arenas.
  - **`slab_state` was published unlocked** while readers hold `cache_lock`.
    TSAN named it exactly: write at `umem_slab_reclaim` vs read at
    `umem_slab_alloc`. 1 report -> 0.
  - **The maintenance thread's startup handshake could lose its wakeup.** The
    worker signalled under a different mutex than the waiter used, so
    `umem_reap()` could hang forever; `pthread_cond_wait()` was also inside an
    `ASSERT`, compiled out entirely under `NDEBUG`.
  - **Unchecked size arithmetic and non-conforming aligned allocation.** Arena
    offsets could wrap (a `SIZE_MAX-15` request moved the bump pointer
    *backward* and aliased a live allocation), bootstrap header addition could
    wrap and return an undersized mapping, bootstrap `realloc` released its
    ownership record before knowing it could succeed, and `posix_memalign()`
    accepted alignments POSIX requires to be `EINVAL`. `aligned_alloc()` is now
    interposed.

- **A pre-existing `umem_reap()` self-deadlock**, found while testing the
  reclaim race and reachable from the real update thread: an update pass holding
  `umem_cache_lock` -> slab destroy -> vmem seg refill -> `vmem_reap` ->
  `umem_reap` -> `umem_updateall` -> the same non-recursive lock. Latent only
  because the reap rate limiter usually returns first. Control: guard removed ->
  hangs; guard present -> completes in 5 s.

### Found, not fixed — a hard ~5 GB heap ceiling on Linux

- **libumem cannot exceed roughly 5 GB of heap on Linux with default kernel
  settings.** `vmem_mmap.c` uses a page-sized quantum where Solaris used 64 KiB
  (there is no `MAP_ALIGN` on Linux), so the heap burns one VMA per ~76 KiB and
  exhausts `vm.max_map_count` (default 65530). Measured at failure: **65,532
  VMAs, 100 % of the limit**; 65530 x 76 KiB is ~4.7 GB, matching the observed
  ceiling. At 192 threads this showed up as **~39 % of allocations returning
  NULL** while glibc on the same box with the same budget reached 96 GB and
  never failed — with libumem using *less* memory (~5 GB vs ~9.4 GB) when it
  began failing, which is what ruled out ordinary exhaustion. Reap-and-retry
  barely moves it (91.7 % -> 89.9 %), because address space rather than a free
  list is exhausted.

  Compounding it: `vmem_mmap_top_alloc()` **restored `errno` over its failure
  paths**, erasing the real `ENOMEM`, which is why this presented for years as
  "libumem is slower on this workload".

  **Correction (2026-09-22, P5.5). The fix announced above did not work.** These
  release notes said the `errno` erasure was fixed and that "failure paths now
  leave `errno` alone". Both halves of that are wrong, and the second is worse
  than a missed sibling:

  1. The fix landed in `vmem_mmap_top_alloc()` only. Its sibling
     `vmem_mmap_alloc()`, in the same file, kept the identical
     `errno = old_errno` on its `MAP_FIXED` failure branch.
  2. `vmem_mmap_alloc()`'s *final* `errno = old_errno` is also reached with
     `ret == NULL`, whenever `vmem_alloc(src, ...)` fails — which is the
     address-space-exhaustion path. The chain is structural, not incidental:
     `vmem_mmap_arena()` builds `mmap_top` from `vmem_mmap_top_alloc` and
     `mmap_heap` from `vmem_mmap_alloc` with `mmap_top` as its source, so on
     exhaustion `vmem_mmap_alloc` → `vmem_alloc(mmap_top)` →
     `vmem_mmap_top_alloc` → `mmap()` sets `ENOMEM`, `top_alloc` preserves it,
     **and then `vmem_mmap_alloc` wipes it one frame up.**

  So this was not a partial fix. **It was ineffective for the exact measured
  case it was written for:** the `FIRST FAILURE at 8269MB (errno=0 Success)`
  that motivated it still showed a stale `errno` at the `umem_alloc()` caller,
  because the frame above the patched function undid the patch. Verified against
  the tags themselves rather than reasoned about: isolated builds of `d22bf03`
  and `553d42e`, both confirmed by grep to contain the `top_alloc` fix, with a
  caller that sets `errno = EDOM` and allocates under a 256 MB `RLIMIT_AS` cap,
  report `errno=33` — the sentinel, restored — at the first failure, on x86_64
  and aarch64 and at chunk sizes 4096 / 65536 / 131072. Post-fix the same probe
  reports `errno=12 ENOMEM`.

  Related correction in `docs/results/2026-09-22-umem-heap-ceiling-vma.md`: the
  `errno=0 → errno=12` measurement that document cites as proof this defect was
  closed does not reproduce from an ordinary caller on any surviving tree, so it
  is recorded there as unattributed rather than confirmed.

  Fixed in v3.0.1 by restoring `errno` only when `ret != NULL`. This is
  recorded here rather than quietly patched because the false claim is itself
  the finding: it is the symptom-fixed-at-one-call-site failure AGENTS.md §7
  forbids, committed by the author of that rule, and the claim in these notes
  is what stopped anyone looking further. A sweep for the same pattern across
  `vmem_*.c` is in the v3.0.1 notes.

  Workaround today: raise `vm.max_map_count`. The fix changes address-space
  layout and is deliberately unassigned until it has its own regression driving
  the heap past 5 GB plus before/after RSS. Evidence:
  `docs/results/2026-09-22-umem-heap-ceiling-vma.md`.

  **This also invalidates previously published libumem fragmentation and
  sustained-fragmentation *throughput* numbers**, independently of the
  fragmentation-ratio defect: those runs were failing roughly two in five
  allocations with nothing in the output disclosing it.

### Fixed — test and release machinery that was reporting false results

These were found by an exit-criteria gate (`scripts/ec2/exit_criteria_gate.sh`)
that deliberately runs what `make check` does not. Each one had been producing a
wrong answer, in some cases for the entire life of the test:

- **The property tests had never tested anything.** `QCC_getValue()` yields the
  generated value's *address* and must be dereferenced — its own doc comment says
  `int a = *QCC_getValue(vals, 0, int*)` — but six call sites cast it to `long`.
  So the value under test was a pointer address, every range check rejected it,
  and the driver reported "Gave up after 0 tests!" with a nonzero exit. Flagged
  in the 2026-09-21 review and never fixed, invisible because these binaries are
  built but were not in `TESTS`.
- **`prop_fragmentation` aborted**, and fixing the extraction above is what
  exposed it. Three separate defects: `vmem_populate()` *asserted* on an
  unsupported `VM_SLEEP` (aborting the process with no hint the caller's flags
  were at fault, and vanishing entirely under `NDEBUG` to continue into a path
  the code says is not allowed); the tests themselves passed `VM_SLEEP` at 11
  sites, against a flag `vmem.c`'s own header documents as unsupported; and it
  freed every allocation with `umem_free(ptr, 0)` under a comment claiming the
  size was tracked internally. It is not. All property tests now pass on both
  architectures.
- **A clean source tarball could not pass its own `make check`.**
  `oracle_control.sh` compiles `test/stress/oracle_null_shim.c` at runtime — the
  deliberately-broken allocator that proves the concurrency oracle discriminates
  — but that source was in no `_SOURCES` and no `EXTRA_DIST`, so it was absent
  from the distribution and the test failed there while passing in-tree.
- **Four regressions were silently dropped from `TESTS`** by a comment line
  ending in a backslash: GNU make continues a *comment* across
  backslash-newline, so it swallowed the next two lines. `make check` reported a
  green 8/8 while four committed regressions never ran.
- **A nondeterministic assertion made `--disable-rseq` unable to pass**
  `make check` at all (12/12 failures), and produced ~17% false reds in default
  builds, which agents then attributed to their own changes.
- **`INCONCLUSIVE` was reported as exit 3**, which automake reads as FAIL, so two
  PTC regressions correctly refusing to claim a pass on an unopened race window
  reddened the whole suite. Now 77 (SKIP).
- **`test_hook_contracts` failed its own vacuity guard** — correctly. It counted
  `umem_hook_track_alloc()` *attempts* rather than calls that entered the
  callback, and its publish/unpublish window was one `sched_yield()`, so on
  x86_64 the tracker threads lost the race every round.

### Fixed — supporting components

- **Weighted hash partitioning assigned every weight to the wrong
  claimant.** `hash_partitions_create_with_weights()` compacted accepted
  entries in one pass, then read `weights[i]` from the *original*
  uncompacted array in the second — so one rejected entry shifted every
  subsequent weight. `A:0, B:1, C:1` gave B a zero-width interval and C
  the entire hash space instead of half each; in `umem_numa_init()` that
  means node weights land on the wrong nodes. Pre-fix control reproduces
  exactly this (`B` 0/10000 samples, `C` 10000/10000):
  `docs/results/prefix-evidence/2026-09-21-hash-partition-prefix-control.log`.
  Three further defects in the same path: nonfinite weights reached the
  `double`→`uint64_t` conversion (undefined behaviour) and poisoned the
  normalizing sum; `hash_partitions_alloc()` multiplied capacity by three
  element sizes unchecked; and `create_with_sizes()` summed intervals then
  adjusted the last one, so a cumulative overflow or a zero-size entry
  left `lower_bounds[]` non-increasing and silently broke the binary
  search in `hash_partitions_get_claimant()`. Regression:
  `test/unit/test_hash_partition.c`.

- **`umem_simd.h`'s "SSE2 fallback" used an SSE4.1 instruction.**
  `_mm_cmpeq_epi64` is SSE4.1; a build configured for the SSE2 baseline
  therefore emitted an instruction that baseline cannot execute. Replaced
  with `_mm_cmpeq_epi32`, which is equivalent for a
  pointer-is-zero test.

- **Installed headers required uninstalled ones.** `--enable-rseq` and
  `--enable-numa` installed `umem_rseq.h` and `umem_numa.h`, both of which
  open with `#include "config.h"` — a build artifact that is never
  installed. An external program including them failed to compile with a
  missing-header error that looked like the user's mistake. Both are now
  private and not installed. New `make install-check` target installs into
  a throwaway `DESTDIR` and compiles `test/install/external_consumer.c`
  against nothing but that prefix, so this cannot regress silently.

- **Malformed `extern "C"` guards** in `umem_rseq.h`, `umem_numa.h`, and
  `umem_htm.h`: all three opened `extern "C"` *outside* the
  feature-availability guard and closed it *inside*, so a C++ translation
  unit on a host without the feature saw an unbalanced brace.

- **A clean `make dist` tarball could not build.**
  `test/property/prop_palloc.c` `#include`s `examples/umem_palloc.c`,
  which was in no source list and no `EXTRA_DIST`. Verified fixed by
  building `prop_palloc` from an extracted tarball on EC2
  (`docs/results/2026-09-21-release-artifact-verification.log`).

- **`SH_LOG_COMPILER = $(SHELL)`** forced the `#!/usr/bin/env bash` test
  scripts through `/bin/sh` (dash on Debian/Ubuntu), where bash-only
  constructs fail for reasons unrelated to the allocator. Now empty, so
  the scripts run under their own shebang.

- **`umemctl` is installed.** README and `docs/UMEMCTL.md` document it as
  a command users run; it was `noinst`, so it only ever existed in a build
  tree.

### Changed — withdrawn and corrected claims

This release withdraws published conclusions that the evidence does not
support. Details in README.md; the two harness defects are P2.1/P2.2 in
`docs/plans/2026-09-21-production-readiness.md`.

- **The v2.7.0 fragmentation conclusion is withdrawn, in both
  directions.** Neither the original "worst-in-field, ~2.3× the next-worst
  allocator" finding nor the "fixed: 4.19→2.70 / 4.12→2.63, landing in
  the field's competitive 2.2-2.5 range" claim is supported, because the
  measurement itself is invalid three ways: the live-bytes denominator
  accumulated bytes that had already been freed, `peak_rss_bytes` was
  sampled after cleanup (so it is not a peak), and the workload labelled
  "192-thread fragmentation" runs on exactly one thread
  (`bench_main.c` sets `thread_count = 1` for it). Separately, 2.63-2.70
  is not inside 2.2-2.5. The `umem_max_ncpus` doubling bug that v2.7.0
  fixed **was real and the fix stands** — it rests on the code, not on
  this benchmark. What the fix does to fragmentation is currently
  unmeasured.

- **All 192-thread scaling conclusions are withdrawn.** The operation
  budget was divided by thread count twice (`matrix.sh` and
  `bench_main.c` each did it), so those points measured ~52k total
  operations in ~3.8 ms with >27% coefficient of variation.

- **The v2.7.0 sustained-p999 improvement is retained but narrowed.** The
  mechanism was identified in the code (a blocking mutex in the depot's
  cross-CPU steal scan, held while the caller held its own per-CPU lock)
  and the fix is real. The measurement — 156.7-163.2us → 83.8-92.6us,
  re-verified at 86,974ns — was taken on x86_64 `c7i.metal-48xl` only,
  with the budget defect present in the harness, and was not re-measured
  on aarch64. Reported with that provenance; the exact percentage is
  provisional. The attribution of the residual gap to the inert rseq
  reload path is labelled a **hypothesis**, not a finding: nothing has
  isolated it.

- **The rseq fast path serves zero allocations.** README said rseq's
  "benefit is limited to fastpath hits". There are no hits. The assembly
  is registered and runs on every qualifying alloc/free, but
  `cache_rseq[cpu].rounds` is permanently 0 because the only functions
  that would populate a per-CPU magazine are the reload paths, and nothing
  calls them. Entering the code is not the code doing its job.

- **`umem --core` does not work and is documented as such.** It is
  accepted, exits 0, and prints nothing — indistinguishable from "no
  leaks found". Verified against a control: the same `findleaks` command
  on a live process reported 200 outstanding buffers; on that process's
  own core it produced zero bytes
  (`docs/results/2026-09-21-core-mode-produces-no-report.log`,
  reproduce with `scripts/ec2/core_mode_probe.sh`). The cause is
  structural: `tools/gdb/umem_gdb.py` runs every command by calling
  `umem_inspect(3)` entry points *inside the target*, and a core has no
  process to call into. A passive core reader is not implemented. The
  snapshot (`--dump`) workflow is the supported post-mortem path.

- **`findleaks` reports outstanding allocations, not leaks.**
  `umem(1)`, `umem_inspect(3)`, and `umem_debugging(7)` all claimed the
  count "reflects actual leaks". The cached-set subtraction covers the
  central depot, per-CPU depot arrays, and per-CPU loaded/previous
  magazines; it does not cover PTC- or rseq-held buffers, and oversize
  allocations are unaccounted. Now documented as an upper bound.

- **Debug-mode overhead figures are now measured.** README and
  `umem_debug(3)` published different unsourced numbers (~10/30/50% vs
  10-20/30-50/50-70%). Measured on `c7i.2xlarge` with
  `test/bench/bench_debug_overhead`: lite 28%, guards 32%, audit 58%,
  default 60%. `contents` and `firewall` are marked **not measured**,
  because that benchmark reports ~0% for both — its 64-byte allocations
  are below the firewall threshold and do not engage `contents` alone,
  which is a fact about the benchmark, not the modes.
  (`docs/results/2026-09-21-debug-mode-overhead-x86_64.log`)

- **Platform "Production" labels removed.** README listed six platforms as
  Production with no recorded evidence for most. Replaced with an evidence
  column per platform. CI is Linux x86_64 only; the aarch64 nightly job is
  validated but **not armed** (repo secrets never added); riscv64 is
  cross-build-only; illumos/SPARC has no recorded run on hardware. No
  platform is production-ready at this commit — the 2026-09-21 review
  found reachable defects in default paths.

- **`make check` scope stated accurately.** It is 8 entries and excludes
  `test/test_main`, every property test, the concurrency oracle, and the
  stress suites. README, `test/README.md`, `Makefile.am`, and the flake's
  `test` app (which printed "All main tests passed (4/4)") all implied
  otherwise.

- **`test/README.md`** claimed ">90% code coverage" and a ">90% Overall /
  On track" target table. Neither was measured, and both contradicted the
  measured 71.2% / 50.3% in README. It also pointed at
  `../.github/workflows/test.yml`, which has never existed in this
  repository, and at five other nonexistent documents.

- **`test/bench/README.md`** directed users at `bench_allocators.sh` and a
  `bench_allocators` target the autotools build does not produce; the
  maintained driver is `matrix.sh` + `bench_main`, which is what every
  committed result came from and the only one that records provenance. Its
  "Adding Allocators" instructions described a compile-time
  pkg-config/`-DHAVE_*` scheme that this harness replaced with runtime
  `dlopen`.

- **`flake.nix`** advertised version `1.0.2` (`configure.ac` says 2.7.0)
  in the derivation and in every generated `.pc` file, copied a
  `docs/html` that has not existed since Doxygen output moved to
  `doxygen-out/` (so the doc output was always empty), and patched
  shebangs on a nonexistent `umem_test4`. `doCheck = false` is now
  labelled: `nix build` is a compile result, not a correctness result.

- **`umem_cache_create(3)`'s "Depot Striping"** described 16 fixed stripes
  selected by hashing thread IDs, a compile-time `UMEM_DEPOT_STRIPES = 16`,
  and per-thread-count speedup percentages. The depot is per-CPU arrays
  sized to `umem_max_ncpus`, indexed by a cached CPU hint, scanned with a
  non-blocking trylock; `UMEM_DEPOT_STRIPES` does not exist.

- **`umem_hooks(3)`** said the hook structure "can be freed after this
  call returns". `umem_hook_unregister()` unlinks under the list lock and
  returns without waiting for in-flight callbacks, so a concurrent
  callback can still be inside the structure.

- **`umem_alloc(3)`** (and a `Makefile.am` comment) cited a deleted
  `PTHREAD_LIMITATION.md` for the claim that `LD_PRELOAD` always uses the
  bootstrap allocator and gets none of umem's optimizations. Stale:
  `malloc_interpose.c` hands off to `umem_malloc()` once umem is ready.

- **Experimental headers now state what they are not.** `umem_own.h` is
  not a memory-safety mechanism (the tracking itself can corrupt memory,
  so enabling it is not strictly safer than not); `umem_profile.h` is not
  a monitoring or accounting guarantee (sampled and lossy);
  `examples/umem_palloc.h` budgets are accounting, **not** enforcement —
  `UMEM_BUDGET_NOWAIT`/`NOFAIL` express intent that is not reliably
  honoured.

- **Restored documentation deleted by `ebcb467`.** That "cleanup" commit
  removed the whole `docs/` results tree, including every document README
  links to. The nine still referenced are restored from git history.

## [2.7.0] - 2026-09-10

> **Retrospective correction (2026-09-21).** Two of this release's headline
> claims do not survive review of the harness that produced them. The
> fragmentation conclusion (`4.19->2.70` / `4.12->2.63`, "lands in the
> field's competitive range") is **withdrawn**: that metric is invalid three
> ways, and 2.63-2.70 is not inside the 2.2-2.5 range it was said to reach.
> The underlying `umem_max_ncpus` doubling bug was real and its fix stands.
> The sustained-p999 improvement is **retained but narrowed** to what was
> measured (x86_64 metal, budget defect present, no aarch64 re-measurement),
> and its attribution of the residual gap to the inert rseq reload is a
> hypothesis, not a finding. See the Unreleased section above. The text
> below is left as written, for provenance.

### Added

- **aarch64-nightly CI job hardened and given an activation runbook.**
  Re-verified the full launch -> bootstrap -> clean-regen -> build ->
  `make check` -> `test_main --no-fork` -> terminate sequence end-to-end
  on fresh `arm-lo` hardware against current master (confirms the
  sparsemap update, allocator-shootout harness changes, and illumos/musl
  fixes since the job was last checked didn't break this path); results
  match the documented baseline exactly (`make check` 5/5 on the CI
  subset, `test_main --no-fork` 417/0/10). Added job- and step-level
  `timeout-minutes` (none existed before, so a hung AWS/SSH call could
  have run indefinitely and left an instance up); confirmed the script's
  own SIGTERM trap still terminates the instance under a simulated
  failure. Wrote `docs/AARCH64_NIGHTLY_ACTIVATION.md`: a numbered,
  copy-pasteable runbook for the one remaining manual step -- a human
  with Codeberg web access adding the `AWS_ACCESS_KEY_ID`/
  `AWS_SECRET_ACCESS_KEY`/`EC2_SSH_PRIVATE_KEY` repo secrets (including
  the exact least-privilege IAM policy JSON to attach) -- since that
  action structurally requires web/API access no agent in this project
  has had. The job remains validated and hardened but explicitly **not
  armed** until that one step happens. (`.forgejo/workflows/
  aarch64-nightly.yml`, `docs/AARCH64_NIGHTLY_ACTIVATION.md`)

### Fixed

- **rseq fast path (already live in the hot path) had six independent,
  pre-existing bugs**, found while evaluating whether to arm the still-inert
  rseq per-CPU reload slowpath (see "Investigated" below). Invisible until
  now because with the reload slowpath inert, `cache_rseq[cpu].rounds` is
  permanently 0 and the fast path's pop/push arithmetic never actually runs
  in production:
  1. x86_64 + aarch64 alloc fast path indexed the magazine with the
     pre-decrement round count instead of post-decrement -- an immediate
     double-allocation the instant `rounds > 0`.
  2. aarch64 fast path hardcoded access to umem's own private, unregistered
     TLS rseq area instead of honoring the runtime glibc-rseq-offset
     detection x86_64 already used -- silently never engaged the fast path
     on any glibc >= 2.35 aarch64 target.
  3. aarch64 free fast path hardcoded the magazine-full bound as 63 instead
     of the cache's actual `magsize` -- heap buffer overflow for most
     magtypes (1, 3, 7, 15, 31).
  4. aarch64 free fast path's "magazine full" case fell through into the
     success epilogue, clobbering the `-1` failure return with `0` --
     silently reported a dropped free as successful.
  5. aarch64 used x86_64's `RSEQ_SIG` (`0x53053053`) instead of the
     architecturally-correct aarch64 value (`0xd428bc00`) -- the kernel
     force-killed the thread with SIGSEGV ("possible attack attempt") on
     any real migration-triggered abort once bug 2 was fixed and the
     critical section actually started executing for real; reproduced live
     on Graviton (c8g.metal) hardware.
  6. Both fast paths placed a trailing stats-counter store *after* the true
     commit store but still inside the kernel-checked critical-section
     window, violating rseq's "single unconditional last store" rule -- a
     plain preemption (no migration needed) landing between the two stores
     produced a real, reproducible leak (alloc) or double-presence/
     double-free (free); confirmed via a SIGALRM-storm repro
     (`test/stress/repro_rseq_trailing_store.c`).
  All fixed and verified on real intel-hi (c7i.metal-48xl) and arm-hi
  (c8g.metal-48xl) hardware with new regression tests
  (`test/unit/test_rseq_fastpath.c`, `test/stress/repro_rseq_trailing_store.c`)
  plus the full concurrency oracle (192 threads/60s, default +
  `--enable-asan`, both arches, 0 failures). (`umem_rseq_x86_64.S`,
  `umem_rseq_aarch64.S`, `umem_rseq.c`)

### Investigated

- **rseq lock-free per-CPU reload slowpath: re-evaluated more rigorously,
  decision confirmed (still inert).** Follow-up to the 2026-08-06 shelving
  decision, asked to either find a genuinely safe C-only/lock-based design
  or confirm the assembly-only conclusion with sharper rigor. Result:
  confirmed, with hardware evidence rather than just race-sequence
  reasoning. A plain-C "recheck cpu_id before the write" design (TOCTOU
  mitigation) was directly reproduced racing the real fast path on shared
  hardware state and measured at a ~42-47% double-issue rate under
  contention on both intel-hi and arm-hi
  (`test/stress/repro_naive_reload_race.c`) -- not a rare corner case, the
  dominant outcome under realistic contention. A per-CPU lock array
  (avoiding the 64-byte-struct constraint) does not provide mutual
  exclusion against the fast path (which never takes any lock, by design);
  adding `sched_setaffinity` pinning closes only the reload's own migration
  path, not a concurrent thread's ordinary scheduling onto the same CPU,
  and costs 12-27x a `mutex_lock`/`unlock` round trip even in its best case
  (`test/bench/bench_affinity_vs_mutex.c`). No design closes the gap other
  than giving the reload its own rseq critical section. Produced a precise,
  mechanical assembly implementation spec
  (`docs/results/2026-09-09-rseq-reload-asm-design.md`) for the next
  attempt -- exact register mapping, instruction ordering (including a
  subtle ordering requirement symmetric to Fixed-bug-6 above), ABI, and
  validation checklist -- so implementing it is translation, not research.
  See `docs/results/2026-09-09-rseq-reload-analysis-v2.md`.
  or tied-worst-in-field p999 tail latency** (2026-09-08 allocator
  shootout section 7: 157us vs. jemalloc's 24.6us/mimalloc's 26.0us/
  rpmalloc's 22.2us on x86_64). Root cause: the magazine layer's depot
  refill (`umem_depot_alloc()`, called from `_umem_cache_alloc`/
  `_umem_cache_free`/the `_batch` variants while holding the caller's
  own per-CPU `cc_lock`) scanned other CPUs' depot stripes with
  `umem_depot_pop()` -- trylock, then a **blocking** `mutex_lock()` on
  failure. Under sustained 192-way `prodcons` pressure (producers and
  consumers on disjoint CPU sets, so a producer's own stripe is nearly
  always empty and it must steal from wherever a consumer's frees
  landed), `cache_depot_contention` (the blocking-fallback counter) ran
  as high as 60-100% of successful-steal count per cache -- hundreds of
  thousands of "thread blocks on a futex while holding its own cc_lock"
  events per run, forming a lock convoy that only compounds over
  sustained duration (not visible in a short burst, and not visible in
  a perf cycles profile either, since a blocked thread is off-CPU --
  the umem_dump_contention() counters were the load-bearing evidence,
  not the profile). A first fix attempt (bounding scan breadth to the
  same 8-stripe limit the PTC-refill trylock path already uses) was
  measured and **reverted**: it broke the cross-thread search itself
  (producers stopped finding consumer-filled magazines within 8 hops),
  regressing p999 to 348-424us and ballooning RSS to 3-5GB. The correct
  fix keeps the full scan breadth and switches each stripe visit from
  the blocking `umem_depot_pop()` to the already-existing non-blocking
  `umem_depot_pop_trylock()` (same primitive the PTC path uses).
  Verified on a dedicated c7i.metal-48xl instance: sustained `prodcons`
  p999 156.7-163.2us -> 83.8-92.6us (41-49% reduction across two
  alternating A/B runs), short-burst p999 253.8us -> 81.0us, throughput/
  RSS/single-thread-throughput flat (no regression), oracle clean
  (default + `--enable-asan`, 192 threads/60s, both PASS), `test_main
  --no-fork` 417/0/10 unchanged. Does not reach the purpose-built
  allocators' tens-of-microseconds tier -- attributed to the still-
  inert rseq lock-free reload path (owned by a separate workstream,
  out of scope here). Independently re-verified from scratch (fresh
  instance, fresh build, no shared state): sustained p999 86,974ns,
  squarely inside the claimed 83.8-92.6us range; the short-burst p999
  number did not reproduce as precisely (295-350us vs. the claimed
  ~81us across 4 direct re-runs, though p99/throughput matched
  closely) -- attributed to p999 being a high-variance statistic at
  ~20-40s/~20M-op sample sizes, not a regression, since the
  load-bearing sustained-methodology number reproduced exactly. See
  `docs/results/2026-09-09-sustained-depot-contention-diagnosis.md`
  section 5a.
  (`umem.c`, `test/bench/bench_contention.c`)

- **`umem_get_max_ncpus()`'s Linux fast path was silently dead on every
  build, doubling `umem_max_ncpus` (and every per-CPU array umem sizes
  off it) on all Linux builds** — root cause of the worst-in-field
  fragmentation/memory-overhead ratio flagged in the 2026-09-08 allocator
  shootout (§7/§8: umem 4.19/4.12 vs. the field's 2.2-2.4 under sustained
  192-thread load). `init_lib.c` gated its `/proc/stat`-based fast CPU
  count on `#ifdef linux`, but GCC/Clang undefine the bare `linux` macro
  in strict ISO conformance modes (`-std=c17`/`-std=c11`), which
  `configure.ac` always passes — so every build silently fell through to
  the generic POSIX fallback, `2 * sysconf(_SC_NPROCESSORS_ONLN)`.
  Invisible at 8 vCPU (16 rounds to the same power-of-two bucket as 8 in
  practice) and a real 2x on 192-vCPU metal (`umem_max_ncpus` 512 instead
  of 256), which sizes `cache_cpu[]`/`cache_depot_full[]`/
  `cache_depot_empty[]`/`cache_rseq[]` on every one of ~58 internal +
  size-class caches. Fixed by testing `__linux__` as well (`#if
  defined(linux) || defined(__linux__)`); also widened the `/proc/stat`
  read buffer 8KB -> 64KB (a 192-vCPU box's `/proc/stat` is ~11.5KB, so
  the old buffer had almost no headroom once this branch became live
  again). Verified on tuned EC2 (intel-hi/arm-hi 192 vCPU,
  intel-lo/arm-lo 8 vCPU): `frag-sustained` ratio 4.19->2.70 (intel-hi),
  4.12->2.63 (arm-hi) — lands in the field's competitive range; short
  `frag` sweep ratio roughly halved on all four roles; no throughput
  regression (several `multi`/`prodcons` throughput points improved);
  `test_main --no-fork` 417/0/10 and `stress_concurrency_oracle` PASS
  unchanged on all four roles. See
  `docs/results/2026-09-09-fragmentation-diagnosis.md` for full
  before/after data. (`init_lib.c`)

- **musl: rseq(2) was registered with the wrong abort signature, causing
  intermittent SIGSEGV under real CPU migration.** `umem_rseq.c`'s manual
  (non-glibc) rseq registration path called `sys_rseq()` with `sig=0` at
  all four call sites, but `umem_rseq_x86_64.S`/`umem_rseq_aarch64.S`
  embed abort-signature `0x53053053` before every rseq critical-section
  abort label, per the rseq(2) ABI. Any CPU migration landing inside a
  critical section made the kernel's signature check fail
  (`Possible attack attempt. Unexpected rseq signature 0x53053053,
  expecting 0x0` in dmesg) and SIGSEGV the thread. This is the root cause
  of the 14 non-reproducible `CRASH: umem ... rc=139` points from the
  2026-09-08 allocator-shootout musl/Alpine run (see that report's §9 and
  `docs/results/2026-09-09-musl-sigsegv-investigation.md`). Invisible on
  every glibc environment because glibc >= 2.35 pre-registers rseq
  itself, so umem's own `sys_rseq()` calls are never reached there —
  confirmed the fix is a no-op on glibc (`test_main --no-fork` unchanged
  at 417 OK / 0 FAIL / 10 SKIP) and reproduced-then-fixed on musl
  (0/50+ repeat-iteration failures post-fix, was crashing on the first
  attempt pre-fix). (`8ee87cd`)
- **musl: `test/unit/umem_env_helper.c`'s warm-up crashed the whole
  `test_main` run under `UMEM_OPTIONS=backend=sbrk`**, unrelated to the
  rseq bug above: `sbrk(2)` itself fails (`ENOMEM`) under this platform's
  PIE/ASLR memory layout in the Alpine AMI used to investigate the rseq
  bug, so `umem_alloc()` legitimately returns `NULL` and the helper's
  unconditional `umem_free(warm, 64)` called `umem_free(NULL, ...)` —
  which, unlike libc's `free()`, `umem_free()` does not tolerate by
  design. Found and fixed (NULL-guarded in the test helper only) while
  verifying the rseq fix didn't regress `test_main` parity; restored
  musl's `test_main --no-fork` to 417 OK / 0 FAIL / 10 SKIP, matching
  the glibc baseline exactly. (`8ee87cd`)

## [2.6.0] - 2026-09-08

### Fixed

- **illumos: `umem_init()` unconditionally panicked** via a dead 48-bit-VA
  tagged-pointer check (`umem_tagged_ptr_check()`) left over from a
  lock-free-depot design removed months ago. illumos places thread stacks
  in the high canonical half of the address space, which the dead code
  wrongly still treated as a violation, so **every umem process aborted at
  startup on illumos** until this was found (while getting the allocator
  shootout below running) and removed. (`a777151`)
- **musl: `backtrace(3)` was assumed present on all Linux, but it is a
  glibc extension musl does not implement**, and `ucontext.h` was never
  probed either — both silently broke stack-trace capture (`getpcstack.c`,
  `umem_stacktrace.c`) on musl/Alpine. Now detected via `AC_CHECK_FUNCS`/
  `AC_CHECK_HEADERS` instead of assumed. (`eb3e15e`)
- **Vendored sparsemap updated v5.4.0 → v5.5.0** (`sm.c`/`sm.h`): upstream
  fixed a big-endian chunk-descriptor corruption, a silent data-loss bug in
  `sm_difference` when both operand chunks were RLE-encoded, and
  structurally invalid maps from `sm_offset`/`sm_select`/`sm_split` on
  negative shifts / word boundaries / undersized destinations. None of
  these were reachable through libumem's own code (the vendored copy is
  namespaced `-DSPARSEMAP_PREFIX=umem_` and shipped for embedders, not
  called by libumem itself), but shipping known-broken third-party code
  under a libumem badge is not acceptable regardless. Re-verified zero
  symbol collision (85 `umem_sm_*` symbols) and full test-suite parity on
  both x86_64 and aarch64. (`0d7b5c4`)

### Benchmarked: an 8-allocator, cross-arch, cross-OS shootout

**[`docs/results/2026-09-08-allocator-shootout.md`](docs/results/2026-09-08-allocator-shootout.md)**
— libc, umem, jemalloc, tcmalloc, mimalloc, snmalloc, scudo, and rpmalloc
(6 on musl/Alpine, 2 on illumos — no third-party allocator ships a package
there), on x86_64 and aarch64, at 8 and 192 vCPU, ~9,600 individual
benchmark runs, plus 3-minute sustained-load runs at full 192-thread
saturation. **This report is now the authoritative performance reference;
the README's Performance section was rewritten around it.** Before any of
this could be trusted, a real pre-existing bug was found and fixed:
`test/bench/allocators.c` **statically linked** competing allocators into
the same binary as the "libc" baseline, so whichever allocator's `malloc`
symbol won the link silently overrode `libc`'s — every prior "libc"
benchmark number captured with a competing allocator also linked in was
measuring the wrong allocator. Fixed via runtime `dlopen`/`LD_PRELOAD`
loading instead of static linking. (`b6d955c`, `2150d81`, `d866a3e`,
`8fba066`)

**Headline findings (full detail in the report):**
- **illumos (the lineage comparison, the most meaningful pairing in the
  whole exercise): umem beats illumos's own libc malloc by up to 4x under
  concurrency** (16.4M vs 4.1M ops/s at 4 threads) and has dramatically
  tighter tail latency — the clearest, most unambiguous win in the report.
- **192-thread `multi` workload: every allocator falls off 70–90% from
  peak to full saturation** — a hardware/workload property, not
  umem-specific. umem's own falloff (83–84%) is mid-to-bad, not best;
  mimalloc and snmalloc are the standouts here.
- **Under 3 minutes of sustained 192-thread cross-thread load, umem has
  the worst or tied-worst p999 tail latency in the field** on both
  architectures — this contradicts a more flattering short-burst
  `prodcons` result and is the more trustworthy number for any real
  workload running longer than a few seconds.
- **umem is worst-in-field on fragmentation on every glibc environment**,
  ~2.3x the next-worst allocator on the two metal boxes — umem's clearest,
  most reproducible weakness in this benchmark.
- One non-reproducible musl SIGSEGV was investigated (two clean full
  re-runs, gdb-attached monitoring) but not pinned down — reported as
  open, not swept under the rug.

**Honest one-line verdict (from the report):** umem is a real, working,
generally competitive allocator that clearly outperforms the traditional
coarse-locked malloc it descends from under concurrency, and holds its own
against modern allocators on 8-vCPU boxes — but at 192-vCPU sustained load
it has the worst tail latency in the field, and its memory overhead under
fragmentation-heavy workloads is roughly double every competitor tested, on
both x86_64 and aarch64. These two findings are open work, not settled.
(`0a4ae09`, `d337147`, `42844b2`, `8e58cdd`, `75f0b14`, `1133870`,
`a8c9013`, `f910d14`, `0d2bcf5`, `bcb9ab9`)

### Documented

- **aarch64 post-fix scaling baseline (closes the Task I1 aarch64 perf-table
  gap).** The x86_64 allocator-scaling story (`2026-07-23-baseline.md`,
  `2026-07-23-d2-fix-validation.md`) never had an aarch64 counterpart after
  the 2.1.0 rseq/PTC fixes landed — the README's aarch64 row just said "not
  yet published". Ran the full stabilized matrix (Task C1/C2 harness) on
  `arm-lo` (c7g.2xlarge, 8 vCPU) and `arm-hi` (c8g.metal-48xl Graviton4, 192
  vCPU): `docs/results/2026-09-08-aarch64-baseline.md`, with raw data under
  `docs/results/2026-09-08-{c7g.2xlarge,c8g.metal-48xl}-aarch64/`.
  **Finding: aarch64 matches (and on the exact same-size-class contention
  case the PTC fix targeted, slightly exceeds) x86_64's post-fix scaling
  story — `multi 160:160` scales near-linearly to 192 threads with a flat
  ~40 ns p999 tail, beating x86_64's 320.9 Mops/s / 299 ns p999 at the same
  thread count with 457.5 Mops/s / 43 ns. It does NOT reproduce x86_64's
  decisive `prodcons` win (~245% of glibc there vs. a mixed 49–120% here),
  and above the 2 KB PTC ceiling (`1024:4096` size range) its high-thread-count
  falloff under contention is steeper than x86_64's.** README's Performance
  section now carries a real aarch64 table instead of the placeholder
  sentence. Superseded as the primary performance reference by the
  allocator shootout above, which subsumes and cross-checks this data.

## [2.5.1] - 2026-09-07

Bug-fix release: two real bugs found while auditing the CI/EC2 harness added
alongside v2.4.0-2.5.0 (never live-tested at the time — the agent that
added them explicitly flagged this; see below), plus a stale doc status.

### Fixed

- **`scripts/ec2/launch.sh`: false-positive "ready" on SSH failure.** The
  post-launch SSH wait loop (40 retries × 5s) never checked whether any
  attempt actually succeeded — it always printed `ready: ...` and handed off
  to `bootstrap.sh` after the loop, even if every SSH attempt failed (e.g. a
  key-pair/secret mismatch in the new `aarch64-nightly.yml` scheduled job,
  or a slow-booting instance). Now the loop tracks success explicitly and
  `launch.sh` exits 1 with a clear diagnostic if SSH never came up, instead
  of silently declaring readiness and letting the failure surface later as
  a confusing SSH error in `bootstrap.sh`. Benefits every workstream that
  uses the EC2 harness, not just CI.
- **`.forgejo/workflows/aarch64-nightly.yml`: wrong hardcoded `KEY_FILE`.**
  The job wrote the SSH private key secret to `~/.ssh/libumem-bench.pem`
  (real `$HOME`) but then overrode `KEY_FILE=/root/.ssh/libumem-bench.pem`
  for the launch step — wrong if the forgejo-runner-debian container's user
  isn't root, silently pointing `launch.sh` at a key file that doesn't
  exist. Removed the override; `scripts/ec2/common.sh` already defaults
  `KEY_FILE` to `$HOME/.ssh/${KEY_NAME}.pem`, consistent with every other
  step in the same job.
- **Stale "Status: OPEN" in the GC-STW investigation doc.**
  `docs/results/2026-07-24-gc-stw-fix-and-oversubscription.md` §4.4 still
  said the aarch64 corruption investigation was open after v2.5.0 removed
  the garbage collector entirely — the code it was investigating no longer
  exists. Updated to "RESOLVED BY REMOVAL", pointing at the v2.5.0 entry.

### Verified

EC2 x86_64 (intel-lo): clean build, `make check` 8/8 PASS, `test_main
--no-fork` 417 OK / 0 FAIL / 10 SKIP (unchanged from v2.5.0), the new
`bench_gate.sh` runs and correctly stays non-blocking on a directional
variance, concurrency oracle clean. `launch.sh`'s fixed ready-check verified
against a real launch (SSH succeeds, `ready:` prints only after confirming
it).

## [2.5.0] - 2026-09-07

### Removed: the experimental garbage collector

**`umem_gc.h`/`umem_gc.c`, `umem_gc_roots.h`/`umem_gc_roots.c`, `gc.h`
(the Boehm-GC-compatible API), and the GC-only `umem_sparsemap.h`/
`umem_sparsemap.c` page map are removed from the library, tests, and docs.**

Rationale: the collector's stop-the-world root-scan handshake had a
long-running soundness investigation (see
`docs/results/2026-07-23-gc-stw-soundness-finding.md` through
`2026-07-24-gc-stw-fix-and-oversubscription.md` §4.1–4.4). Each fix closed
a real, specific bug (a missing park barrier, a resize-under-lock
contention tail, sharding the object lock, a missing acquire/release on the
STW park ACK) and was verified against its own repro — but a further
investigation (2026-09) found the aarch64 oversubscription corruption
persisted after the acquire/release fix, and that the DOMINANT failure mode
(7 of 10 failures in a 30-run sample) was a corrupted/cyclic object chain,
not the sweep-of-a-reachable-object pattern every prior fix targeted —
evidence of a deeper, not-yet-root-caused bug in object lifecycle or
conservative-scan correctness under concurrency.

Rather than continue shipping a conservative garbage collector with an
open, intermittent, unresolved memory-corruption bug under concurrent load
(the GC's core use case), it is removed. libumem's core allocator (slab +
magazine + vmem), debug modes, ownership tracking, profiling, and budget
contexts are unaffected — none of them depend on or share code with the GC.

The historical CHANGELOG entries below that describe GC features, fixes,
and benchmarks are kept as an accurate record of work done at the time;
they no longer describe present-day libumem. `umem_own.h` (ownership
tracking) is unrelated and unaffected — do not confuse the two experimental
features.

## [2.4.0] - 2026-08-07

sparsemap refresh + GC scalability. Verified building + testing on EC2
(x86_64 + aarch64); full unit suite 459 OK / 0 FAIL, now deterministic
(previously ~10%-flaky GC heap-stat tests fixed).

> **Coverage re-verified 2026-09-06** (commit `ecd5fa0`): the v2.0.0 "33%
> to 80%+" claim below was measured in early 2025 and predates six
> releases of substantial new code (this release's sparsemap vendoring
> included). Current measured line coverage is **71.2%** of core sources
> (excluding the vendored third-party `sm.c`/`sm.h`, which is 0%-covered
> and accounts for 29% of the repo's instrumented lines) or **50.3%**
> across the whole repo including `sm.c`. Neither matches "80%+" — the
> README/CHANGELOG claim was stale and has been corrected. Full breakdown,
> methodology, and per-file numbers: `docs/results/2026-09-06-coverage-verification.md`.

### sparsemap: latest upstream, namespaced (no symbol collision)

- Replaced the stale, uncompiled vendored `sparsemap.{c,h}` with upstream
  **sparsemap v5.4.0** (`sm.c`/`sm.h`), compiled into libumem with
  `-DSPARSEMAP_PREFIX=umem_` so every public symbol is `umem_sm_*`. An
  application that links its own copy of sparsemap can no longer collide
  with libumem's (84 `umem_sm_*` symbols exported; zero bare
  `sm_*`/`sparsemap_*`). The GC's internal page map (`umem_sparsemap.c`) is a
  separate table and was already `umem_`-namespaced.

### GC: object lock + page map sharded (removes alloc-path contention)

- The single global `gc_objects_lock` (guarding the object list *and* the
  page sparsemap, held across an O(n) sparsemap rehash) serialized every
  `GC_MALLOC` under concurrency. It is now **64 shards keyed by page**: each
  shard has its own lock, object list, and `umem_sparsemap`;
  `gc_object_add`/remove contend only the object's shard; stop-the-world
  walks all shards (still a complete, quiescent snapshot); `find_header`
  routes a conservative/interior pointer to one shard by page. Removes the
  mutator-vs-mutator serialization on the alloc/free path.
- Sound: strict-stw 32t/1000r ×30 = 0 corruption; 192t/800r ×20 at real 1:1
  parallelism = 0 corruption / 0 timeout.
- Superseded the earlier 4x-sparsemap-growth partial mitigation.
- **Known residual (STW under CPU oversubscription):** GC is sound on both
  arches at ≤1x thread:core (the normal regime; x86 clean to 192t/1:1, arm
  clean through 8t/8-core). Under CPU *oversubscription* the STW
  suspend-barrier has a pre-existing soundness edge: on x86 it manifests as a
  rare bounded stall/skip; on **aarch64** (weaker memory model) it can rarely
  **sweep a reachable object** (16t/8-core: ~2/10 under ASan). This predates
  this release — pre-sharding v2.3.0 is far worse on arm (~14/15) — and the
  sharding here substantially *reduces* it, but does not eliminate it. Root
  cause is the suspend-ack barrier ordering, fixed by a separate
  barrier/safepoint follow-on. **Do not run the conservative GC oversubscribed
  on aarch64 until that lands.**
  (`docs/results/2026-07-24-gc-stw-fix-and-oversubscription.md` §4.2–4.3)

### GC heap-size accounting fixes (deterministic tests)

- `umem_gc_realloc` in-place shrink now drops the freed bytes from
  `gc_heap_size` (was drifting the counter high).
- `gc_free_object` uses a saturating subtract so the unsigned advisory
  counter (`GC_get_heap_size()`) can never wrap to ~2^64 on a double-counted
  free — the root of the intermittent `/gc/large_heap` failure (pre-existing
  since v2.0, ~10% flake).
- `test_gc` `boehm_full` reads `GC_get_heap_size()` with a live allocation
  held rather than relying on cross-test residual leakage.

### rseq lock-free reload: analyzed, formally shelved (not "in progress")

- Arming the per-CPU rseq reload with the existing plain-C slowpath is
  unfixably racy against the lock-free asm fastpath (migration mid-reload
  tears the per-CPU magazine). A correct arming needs a second rseq critical
  section (hand-written per-CPU-commit asm) on both x86_64 and aarch64. Since
  the rseq path is a pure optimization (PTC serves the steady state soundly),
  it stays inert; the exact constraint + oracle gate for a future attempt are
  documented. (`docs/results/2026-08-06-rseq-reload-analysis.md`)
- **Status: formally shelved, permanent until someone does the asm work.**
  The reload slowpath functions (`umem_rseq_alloc_slowpath` /
  `umem_rseq_free_slowpath` in `umem.c`) are dead code, marked `UNUSED` at
  their definition site, and are not on any roadmap or milestone — there is
  no "next release" expectation for this. It becomes live again only if
  someone writes the migration-safe per-CPU-commit assembly on both
  architectures and clears the concurrency-oracle gate in the analysis doc.
  The lock-free fastpath (hit path) is unaffected and stays active; only
  the reload-on-miss falls back to the locked depot path. `README.md`
  corrected to state this explicitly (it previously implied RSEQ was fully
  lock-free end to end).

## [2.3.0] - 2026-08-06

illumos/x86 build-portability release, from the solnix (Nix distribution of
illumos) packaging effort: every item below was a downstream patch solnix
carried against 2.2.0 and is now fixed upstream, so illumos/x86 (and other
distros) build cleanly with no out-of-tree patches. Verified building +
testing on EC2 (459 OK / 0 FAIL).

### Build / portability fixes (illumos x86_64)

- **x86 `getfp`/`_breakpoint` wired into the SOLARIS build (was UNDEF at
  runtime).** On x86 illumos `libumem.so` linked with an undefined `getfp`
  (used by `getpcstack.c`) / `_breakpoint` (`umem_agent_support.c`), so the
  first stack walk — i.e. the moment an `LD_PRELOAD`'d malloc interposer ran
  — aborted with `symbol getfp: referenced symbol not found`. The bundled
  x86 helper was never added to SOURCES on the SOLARIS x86 branch. Added a
  portable, header-independent C compat unit (`x86_subr_compat.c`,
  `getfp` = `__builtin_frame_address(0)`, `_breakpoint` = `int3`) wired via a
  new `X86_ASM_SOURCES` arm of the SOLARIS/`ARCH_SPARC` conditional — no
  dependency on `<sys/asm_linkage.h>`.
- **`MAP_POPULATE` (Linux-only) guarded** in `examples/umem_palloc.c`
  (`#ifndef MAP_POPULATE #define MAP_POPULATE 0`) so it compiles on illumos.
- **Socket/name-service split**: the introspection control channel (server
  in `umem_introspect.c`, client `tools/umemctl`) now links `-lsocket`
  `-lnsl` on illumos (via a configure `AC_SEARCH_LIBS` → `SOLARIS_SOCKET_LIBS`
  substitution; a no-op on Linux/glibc where `socket()` is in libc).
- **SONAME** now `libumem.so.1` / `libumem_malloc.so.1` (was `.so.0`): set an
  explicit libtool `-version-info 1:0:0`, matching the historic
  illumos/Solaris SONAME so `LD_PRELOAD=…/libumem_malloc.so.1` resolves. This
  is an ABI-visible SONAME change on all platforms (intended; a stable
  documented SONAME helps every consumer).
- **Interposer constructor**: `malloc_interpose.c` uses the plain
  `__attribute__((constructor))` form on Solaris/illumos (whose ld has no
  numbered `.init_array`); the numbered priority was a cross-`.so` no-op
  anyway (load order governs `libumem_malloc.so` vs `libumem.so`).
- **Library-only builds are clean on x86 illumos** after the `getfp` fix
  (the bundled `noinst` test/bench/example programs link).
- **Dual-ABI (32+64) illumos install recipe** documented in the README
  (two-pass `-m32`/`-m64` build into `lib/` + `lib/64/`) so one `LD_PRELOAD`
  path covers both ELF classes; added illumos x86_64 to the platform table.

### Tooling

- **`umem(1)` reimplemented in C** (`tools/umem.c`, was a bash wrapper) —
  identical interface and behavior (`--pid`/`--core`/`--exe`/`--dump`;
  `findleaks`/`log`/`status`/`whatis`/`bufctl`/`snapshot`/`walk` with
  `-f`/`-n`), builds/drives gdb the same way, execs `umem_dump_reader` for
  offline snapshots. Verified against a live process (status table,
  JSON findleaks, snapshot + offline `--dump`). Now a `bin_PROGRAMS` entry;
  `umem_dump_reader` remains a script.

## [2.2.0] - 2026-07-24

Concurrency-hardening release. Adds an adversarial concurrency oracle that
found — and this release fixes — two real high-concurrency corruption bugs in
the core allocator, and completes the GC stop-the-world work so it is sound
even under CPU oversubscription. Validated on EC2 across x86_64 and aarch64 at
up to 192 vCPU; provenance under [`docs/results/`](docs/results/).

### Bug fixes (core allocator, found by the new concurrency oracle)

- **Slab-freelist self-corruption via `MADV_DONTNEED`**: `umem_slab_reclaim`
  advised a byte length that, for single-page slabs (all magazine-type
  caches), spanned the whole page — including the `umem_slab_t` metadata
  embedded at the page end. The kernel zero-filled it on next touch, wiping
  `slab_cache`/`slab_next`/`slab_prev` and corrupting the cache freelist
  (surfacing as the `sp->slab_cache == cp` abort at umem.c:1588 or a SEGV in
  the reap thread). Reclaim now advises only whole pages strictly below the
  metadata page; single-page slabs reclaim nothing rather than corrupt
  themselves. Reproduced at 192 threads on x86_64 + aarch64; fix verified
  clean on both. (`docs/results/2026-07-24-slab-freelist-corruption-fix.md`)
- **Depot-reap self-deadlock** (latent, exposed once the above was fixed):
  `umem_maglist_ws_reap` held `ml_lock` across `umem_magazine_destroy`, which
  frees back through the depot and re-locks the same stripe. Now pops
  candidates under the lock and destroys them unlocked.
- Both fixes are in cold reap/reclaim paths only — the alloc/free fast path
  and its locking are unchanged; 192-thread throughput is unregressed
  (~1290–1490 Mops/s `multi` small).

### GC stop-the-world: sound under oversubscription

The v2.1.0 GC STW fix was sound for the common case but could hang/abort under
~4× CPU oversubscription (signal-based suspend timing). Replaced with
**cooperative safepoints**: mutators park at `umem_gc_alloc` entry before
taking any lock; a lightweight critical-section flag replaces the per-alloc
`pthread_sigmask` storm and defers signal-triggered parking until all locks
are released (no more park-while-holding-a-lock deadlock); the collector's
park barrier is bounded and, on timeout, **skips collection entirely rather
than ever sweeping an incomplete snapshot**; the dead set is snapshotted under
STW and reclaimed after resume (removes the allocate-black residual sweep).
Validated 0 corruption / 0 hang / 0 abort: 50/50 at 32t on 8 cores
(oversubscription), 100/100 at 48t+96t and 30/30 at 192t on 192 cores
(v2.1.0 hung 3/25 there). `prop_gc` now asserts STW soundness by default.
Remaining bounded tail at ≥6× oversubscription (global object-list lock)
is documented; sound and bounded, never unsound.
(`docs/results/2026-07-24-gc-stw-fix-and-oversubscription.md`)

### Testing

- **Adversarial concurrency oracle** (`test/stress/stress_concurrency_oracle.c`):
  stamps every allocation with a unique owner token and verifies it at
  alloc/hold/free across `multi`/`prodcons`/`churn` patterns and size classes,
  deterministically catching cross-thread aliasing or corruption. A fast
  variant runs in `make check`; the heavy 192-thread matrix
  (`scripts/ec2/oracle_matrix.sh`) is the EC2 gate that found the slab bug and
  now passes on both arches.
  (`docs/results/2026-07-24-concurrency-oracle-findings.md`)

## [2.1.0] - 2026-07-24

Additive correctness, performance, and live-tooling layer on top of 2.0.0.
All fixes were reproduced and validated on tuned EC2 hardware across x86_64
and aarch64, low- and high-core (up to 192-vCPU metal); provenance under
[`docs/results/`](docs/results/). The unit suite went from 424 OK / 31 FAIL
to **459 OK / 0 FAIL / 10 SKIP**.

### Headline: 2.1.0 is the first release that runs on aarch64 (Graviton)

2.0.0's rseq fast path SIGSEGV'd on the first restart on aarch64 — the abort
signature was placed *after* the abort label, but the kernel reads
`*(abort_ip - 4)`. Moving `.inst RSEQ_SIG` before each abort label (matching
x86_64) makes umem usable on Graviton. (`docs/results/2026-07-23-aarch64-rseq-crash-repro.md`)

### Bug fixes (core allocator)

- **aarch64 rseq fast-path SIGSEGV** — see headline above.
- **`umem_cpu_node[]` out-of-bounds read** on machines where
  `umem_max_ncpus > 256` (a 192-vCPU box rounds to 512): the depot's
  NUMA-node lookup table was fixed at 256 entries while the depot indexed it
  up to `umem_max_ncpus`. Now sized dynamically; validated clean under ASan
  on a 512-detected metal instance.
  (`docs/results/2026-07-23-cpu_node-oob-finding.md`,
  `docs/results/2026-07-23-oob-fix-validation-metal.md`)
- **Multi-thread scaling regression (PTC bin-table gap)**: requests landing
  between size classes (>128B) mapped to `-1` and skipped the thread cache,
  serializing on `cc_lock`. Mapping the index through the backing cache's
  object size closed the gap: 8-thread `multi` 1.17 → 33.83 Mops/s, p999
  1.83 ms → 299 ns (192 threads). Also bounded the depot trylock steal-scan.
  (`docs/results/2026-07-23-scaling-diagnosis.md`,
  `docs/results/2026-07-23-d2-fix-validation.md`)
- **GC stop-the-world soundness**: a reachable object rooted only on a
  suspended thread's stack could be swept. The collector now spills
  suspended threads' registers, scans each parked thread's full stack after
  a park barrier, and serializes object add/remove against STW (0 canary
  corruption over 90 stress runs incl. 192-vCPU). A residual failure remains
  only under heavy CPU oversubscription (~4× threads:cores); the
  safepoint-based follow-up is designed in
  [`docs/results/2026-07-24-gc-stw-fix-and-oversubscription.md`](docs/results/2026-07-24-gc-stw-fix-and-oversubscription.md).
- **`umem_cache_reclaim_pages`** no longer walks the slab list across a lock
  drop (SEGV under GC stress).
- **`getpcstack` frame-pointer walk** bounded to a plausible stack span on
  x86 and aarch64 (avoids a wild-pointer SEGV at thread teardown); the x86
  path now captures real frames (audit stacks were previously empty on x86).
  Retains a `backtrace(3)` fallback for other platforms.
- **palloc dynamic (non-PREALLOC) budget arenas**: were created with no span
  and no source, so every allocation failed (segfaulted). They now import
  spans on demand from the heap arena; budget enforcement unchanged.
  (`docs/results/2026-07-23-palloc-dynamic-arena-finding.md`)

### New: `umemctl` live-process introspection (complements `umem(1)`)

2.0.0's `umem(1)` drives inspection via gdb/ptrace against a live pid, a
core, or an offline snapshot — point-in-time, non-invasive, ideal for CI and
post-mortem. **`umemctl`** covers the live/interactive gap that a ptrace
snapshot cannot: an opt-in in-process channel (`--enable-introspect` +
`UMEM_OPTIONS=introspect=1`) exposing

- streaming `logtail` (slab/reap/alloc events as they happen),
- a dependency-free TUI `monitor`,
- `record`, and `break` with a **break-before-a-leaked-allocation** workflow
  (learn the leaked-allocation signatures under audit, then stop the
  allocating thread in a fresh run so a debugger sees the exact stack).

Zero hot-path cost when off (verified byte-identical `_umem_alloc`/`_umem_free`
disassembly). See [`docs/UMEMCTL.md`](docs/UMEMCTL.md). Use `umem(1)` for
snapshot/core/CI leak-finding; `umemctl` for live streaming and break-on-leak.

### Testing, benchmarking & infrastructure

- **Exec-helper test harness** (`umem_env_helper`) reaches umem's init-time
  env-var parsing, resolving 31 phantom `/envvar/*` failures and un-skipping
  the debug-detection tests (guards/redzone/deadbeef/audit/firewall/
  double-free/UAF are now proven to fire).
- **Property/invariant tests** for ownership, GC, profiling round-trip, and
  budget contexts (`test/property/prop_*.c`).
- **Stabilized benchmark harness** (pinned, warm-up-discarded, median+CoV)
  and a cross-arch scaling matrix; a contention driver + instrumentation.
- **EC2 build/test/bench harness** (`scripts/ec2/`) — all heavy work runs on
  tuned Intel + Graviton instances (8→192 vCPU).
- **CI**: forgejo `tests.yml` now builds and smoke-checks the exec-helper so
  the env-var/detection tests actually run; aarch64 authoritative testing is
  documented as EC2-only.

## [2.0.0] - 2025-01

### Headline: runtime debugging restored on Linux / FreeBSD / macOS

libumem now ships `umem(1)`, a command-line tool that surfaces the
Solaris `mdb` workflow on non-Solaris platforms.  The same commands
(`findleaks`, `log`, `status`, `whatis`, `bufctl`, `walk`, `snapshot`,
`break`) are also exposed inside `gdb` and `lldb` under the `umem`
prefix.  See `tools/DEBUGGING.md`, `umem(1)`, and `umem_inspect(3)`.

### New: introspection API and tooling

- **`umem_inspect.h`** — in-process C API for findleaks, log dump,
  per-cache status, address resolution, and binary snapshots.
  All commands accept text or JSON output for tooling integration.
- **`umem(1)`** — standalone CLI that drives `gdb` in batch mode
  against a live pid, a core dump, or an offline snapshot file.
- **`tools/gdb/umem_gdb.py`** and **`tools/lldb/umem_lldb.py`** —
  same command set inside the debugger; conditional breakpoints on
  allocation, free, and corruption events.
- **Binary snapshot format** (`.ums`) — capture allocator state in
  production, analyze offline.
- **`tools/umem_dump_reader`** — Python reader for the binary
  snapshot format; no live process required.
- **End-to-end tests** under `test/debugger/` exercise the full
  toolchain via gdb and lldb in `make check`.

### Fixed: pre-existing bugs uncovered by the inspection work

- **`getpcstack()` was a no-op on Linux x86_64.** `EC_UMEM_DUMMY_PCSTACK`
  caused the function to return 0 unconditionally, silently
  rendering `UMEM_DEBUG=audit` useless for its primary purpose
  (associating allocations with allocation sites).  Replaced with
  `backtrace(3)` fallback.
- **`umem_audit.c` exported functions whose output was inaccurate.**
  `umem_find_leaks()` reported `cache_buftotal` (total slots, not
  buffers in use) as the leak count.  `umem_get_audit_info()` was a
  stub returning NULL.  Replaced with thin forwarders to
  `umem_inspect_*` so the documented behaviour now matches reality.
- **`umem_stacktrace_init()` forked `addr2line` by default**, which
  interferes with debugger expression evaluation.  The fork is now
  opt-in via `UMEM_STACKTRACE_ADDR2LINE=1`.

### Rework Phases (0-5)

**Phase 0**: Fixed tcache wiring bug and added depot contention metric.

**Phase 1**: Replaced all `__sync_*` builtins with C11 `<stdatomic.h>`
operations.  Added spin hints (`_mm_pause` / `yield`) for lock loops.

**Phase 2.1**: Simplified depot from lock-free striped arrays to
straightforward mutex-protected lists.  Removed 279 lines of complexity.

**Phase 2.2**: Built lock-free magazine infrastructure using CAS on
`cc_rounds`.  Disabled due to a race condition that requires rseq
per-CPU isolation to fix correctly.

**Phase 3**: Introduced finer size classes (~1.25x geometric spacing)
and increased magazine capacity to 255 slots for better cache utilization.

**Phase 4**: Added slab state tracking (ACTIVE/DIRTY/CLEAN) and
madvise-based page reclamation for idle slabs.

**Phase 5**: Renamed tcache to PTC (Per-Thread Cache) throughout
the codebase.  Files are now `umem_ptc.c` / `umem_ptc.h`.

### New Features

- **Stack-Based Objects (SBO)**: Bump allocator and scoped arena for
  temporary allocations that auto-free on scope exit.
- **Ownership tracking** (`umem_own.h`): Rust-inspired ownership and
  borrowing system with lightweight (~2%) and full debug (~15%) modes.
  Detects use-after-free, double-free, borrow conflicts, thread violations.
- **Garbage collector** (`umem_gc.h`): Conservative mark-sweep GC with
  Boehm-compatible API (`gc.h`).  Concurrent marking, finalizers,
  sparsemap for O(1) pointer lookup.
- **Allocation profiling** (`umem_profile.h`): Record allocation patterns
  to binary profiles, replay to pre-warm caches.  Phase detection and
  predictive pre-allocation.
- **Budget contexts** (`umem_palloc.h`): PostgreSQL-style per-context
  memory management with budgets, backpressure, shared memory, and
  parent/child hierarchy.
- **Stack traces**: GDB-style stack trace formatting for error reporting
  in debug and ownership modes.
- **Transfer batching**: `umem_cache_alloc_batch` / `umem_cache_free_batch`
  for bulk operations on object caches.
- **vmem_xcreate()**: Extended arena creation API for custom vmem arenas.
- **NUMA-aware depot**: NUMA node selection for depot stealing and
  allocation statistics.
- **Per-thread magazines**: Reduce `cc_lock` contention by giving each
  thread its own magazine pair.
- **Experimental guards**: Headers for experimental APIs require
  `#define UMEM_ENABLE_EXPERIMENTAL` before inclusion.

### Performance

- PTC coverage expanded from 448 bytes to 2048 bytes.
- PTC fast path inlined into `umem.c` alloc/free (fewer branches, no
  stats overhead, lazy init).
- Per-CPU depot arrays to eliminate cross-CPU contention.
- RSEQ (restartable sequences) wired into x86_64 allocation hot path.
- Alignment audit: hot fields placed in first cache line with
  `_Static_assert` verification.
- Profiling is zero-cost when disabled (no atomic operations on hot path).

### Platform Fixes

- **FreeBSD**: Fixed W^X crash (removed PROT_EXEC from heap), MAP_ANON
  portability, removed broken `_pthread_mutex_init_calloc_cb` constructor.
- **Illumos/SPARC**: GAS syntax for assembly, alloca/pcstack fixes,
  guard pthread shims, `__EXTENSIONS__` for headers, 48-bit VA check
  skipped on SPARC.
- **RISC-V**: Fixed TLS static block exhaustion in benchmarks, added
  fallback dlopen paths.
- **Windows/MinGW**: Guarded mmap/munmap symbols, gettimeofday compat
  wrapper, general portability fixes.
- **Cross-compilation**: Fixed configure.ac for aarch64/riscv64 cross
  builds via Nix.
- **BSD platforms**: `PLATFORM_FEATURE_FLAGS=""` (no `_POSIX_C_SOURCE`).

### Code Quality

- Removed dead genasm/tmem code (-2,514 lines, 7 stale .md files).
- Removed 48 stale documentation files and tracked artifacts.
- Test coverage boosted from 33% to 80%+ line coverage (32 new tests).
  *(historical, as measured at this release; see the 2026-09-06 note
  at the top of this file — the aggregate has since drifted with new
  code and is no longer 80%+.)*
- Property-based tests for allocation patterns, caches, fragmentation.
- Integration tests for signals, OOM, multithreading, debug features.
- Benchmark suite with comparison to libc/jemalloc, cross-platform
  results tracking (TOML output with OS/arch/compiler metadata).

### Breaking Changes

- `umem_genasm_supported` is kept as an ABI-compatible symbol (value 0)
  but genasm functionality is removed.
- Experimental headers now require `#define UMEM_ENABLE_EXPERIMENTAL`.

## [0.1.0] - Historical

Initial portable fork from Solaris libumem (circa 2008).

- Slab allocator with magazine layer
- Object caching with constructors/destructors
- vmem virtual memory management
- Debug features (guards, auditing, logging)
- Linux and Solaris support

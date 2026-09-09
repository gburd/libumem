# Changelog

All notable changes to libumem are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

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
  out of scope here) and not independently re-measured on aarch64 in
  this pass. See
  `docs/results/2026-09-09-sustained-depot-contention-diagnosis.md`.
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

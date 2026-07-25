# Changelog

All notable changes to libumem are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

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

# libumem production-readiness plan (2026-09-21)

**Status:** active. Supersedes the completed
`2026-07-23-libumem-correctness-perf-tooling.md` workstream.

**Origin:** the 2026-09-21 four-part design review. That review found
reachable correctness and lifetime defects in *default* code paths, not only
in experimental features, plus measurement defects that invalidate several
published performance conclusions.

**Verdict being addressed:** libumem is not ready for general daily
production use at `ebcb467`. This plan defines what has to be true before
that claim can be made.

## Ground rules

1. All building, testing, and benchmarking on EC2. See `AGENTS.md`.
2. Every fix needs a check that **fails before** the fix and **passes
   after**. A fix without a demonstrated pre-fix failure is not accepted.
3. Preserve original failure logs before re-running anything.
4. No readiness or performance claim beyond what the evidence shows.
5. Parallel agents use worker-scoped EC2 roles and disjoint file sets.

## Phase 1 — Reachable correctness and lifetime defects (blocking)

Each item: root cause, fix at the shared function, regression that reproduces
the pre-fix failure, EC2 verification on x86_64 and aarch64.

**Status as of 2026-09-22**

| Item | State | Evidence |
|---|---|---|
| P1.1 interposer `calloc` ownership | FIXED | `docs/results/prefix-evidence/` |
| P1.2 fork lock order | FIXED | gdb stacks both arches |
| P1.2 follow-up: interposer fork hooks | FIXED | `2026-09-22` job logs |
| P1.3a PTC thread-exit drain | FIXED | `2026-09-22-p1.3-ptc-lifetime.md` |
| P1.3b magazine capacity/resize race | FIXED | `2026-09-22-p1.3bc-magazine-resize.md` |
| P1.3c populated magazine discarded | FIXED | `2026-09-22-p1.3bc-magazine-resize.md` |
| P1.4 destroy with retained slabs | FIXED | `2026-09-21` reclaim report |
| P1.5a/b/c reclaim metadata + publication | FIXED | TSAN 1 -> 0, aborts pre-fix |
| P1.6 maintenance-thread startup | FIXED | evidence retracted once, then redone |
| P1.7 overflow + alignment contracts | FIXED | `docs/results/prefix-evidence/` |

Also fixed while here, outside the original list: a pre-existing `umem_reap`
self-deadlock reachable from the real update thread; a hash-partition weight
misassignment; two harness defects that were corrupting every verification in
this workstream (four regressions silently dropped from `TESTS` by a comment
continuation, and a nondeterministic `cached_skipped` assertion that made
`--disable-rseq` unable to pass `make check` at all).

Phase 1 is complete as listed. Two caveats a reader should carry forward:

- **P1.3b's regression is a mechanism demonstration, not a rate measurement.**
  Its window is ~20ns and a cache resizes at most once per process, so chance did
  not open it in 67M samples against a build with the defect present. It has to
  be opened deliberately (a test-only probe that parks threads inside the
  window). The defect and the fix are both real -- pre-fix the same run produces
  3.1M capacity desyncs and a SIGSEGV -- but nothing here establishes that this
  occurs at any particular rate in production.
- **P1.3a's statistical oracle was replaced with an exact one (resolved).** Its
  original check compared outstanding-buffer growth between a PTC-on arm and a
  PTC-off control against a fixed margin. When the P1.3b/c fixes reduced the
  *control* arm's retention (196 -> 66) while the arm under test barely moved
  (151 -> 147), the comparison tightened and the test began failing ~17% of
  runs without the behaviour under test having changed -- the yardstick moved,
  not the code. Replaced by `umem_ptc_probe_exit_stranded`, which counts
  objects still in a PTC bin at the instant the PTC is freed: a surgically
  reverted build reports **2304 stranded and FAILs**, the fix reports **0 and
  PASSes**, on x86_64 and aarch64. The two `_probe` variants are what `make
  check` gates; the statistical variants remain buildable for manual use.
  Lesson recorded: do not gate on a statistic when an exact count is
  obtainable.

### P1.1 Interposer `calloc` storage ownership and concurrency
`malloc_interpose.c:189, 443–479`

`in_calloc` is process-global, so one thread's ordinary `calloc` makes
concurrent callers take the static-buffer path, and the buffer offset resets
while earlier allocations are still live. Overlapping live allocations
result. The static bump pointer is also unsynchronized, and `realloc()` does
not recognize these pointers although `free()` does.

Required: per-thread recursion state; no reuse of storage that may still be
live; consistent recognition across `free`/`realloc`; a concurrent
`calloc`/`realloc`/`free` regression under interposition.

### P1.2 Fork lock order and omitted subsystem locks/state
`umem_fork.c:60–75`; `umem.c:2822–2878, 3057–3149`

Fork preparation takes depot `ml_lock`s before per-CPU `cc_lock`s; normal
allocation takes `cc_lock` first and then enters depot code that blocks on
`ml_lock`. Direct ABBA deadlock. The interposer's `bootstrap_ptr_lock`, used
by ordinary `free`, has no fork handling at all. `in_calloc` and the
introspection break state are also inherited without reset.

Required: one documented lock order that matches normal operation; fork
handlers for every lock ordinary paths take; child-side reset of inherited
state; a multithreaded fork-under-load regression (the current test forks
from a single-threaded parent and cannot catch this).

### P1.3 PTC complete draining and resize-safe magazine ownership
`umem_ptc.c:401–429, 473–503`; `umem.c:2413–2445, 3374–3417, 3571–3604`

**P1.3a (FIXED, 2026-09-22).** Thread exit called the half-bin flush once per
bin and then freed the PTC, stranding the remainder while the slab layer still
counted those objects as allocated. See
`docs/results/2026-09-22-p1.3-ptc-lifetime.md`.

**P1.3b (FIXED, 2026-09-22).** A magazine resize between obtaining a magazine
and reading the cache's current capacity let an old smaller magazine be indexed
with the new larger size. Capacity is now derived from the magazine's own source
magtype cache, so the magazine and its capacity are one decision and the window
does not exist. ASan cannot detect this class of overrun at all (magazines live
inside umem's own mmap-backed slabs, no redzone, no poisoning in this tree), so
detection is via a capacity invariant. See
`2026-09-22-p1.3bc-magazine-resize.md`.

**P1.3c (FIXED, 2026-09-22).** Stale magazine handling freed the magazine shell
without draining a populated magazine, and callers passed full magazines down
that path. Both return functions now take the round count, drain before
discarding, and put a magazine on the depot's full list only when it is exactly
full. Pre-fix: exactly 127 objects destroyed per discarded magazine, counted at
the point of loss. See `2026-09-22-p1.3bc-magazine-resize.md`.

### P1.4 Cache destruction with retained empty slabs
`umem.c:1750–1758, 4624–4703`

With default reclamation, freeing the last object retains the empty slab.
`umem_cache_destroy()` logs a nonempty `cache_buftotal` and frees the cache
descriptor without destroying those slabs. Trigger: create
`UMC_NOMAGAZINE` cache, allocate one object, free it, destroy the cache.

Required: destruction drains retained slabs; regression asserting no retained
backing storage or metadata after destroy.

### P1.5 Reclaim metadata preservation and synchronized publication
`umem.c:3900–3922, 1644–1647, 1785–1787`

HASH debug slabs lose in-buffer buftags and free patterns to `MADV_DONTNEED`
and do not rebuild them, so a later valid allocation fails the allocator's own
corruption check. Multi-page non-HASH slabs from a larger-quantum arena can
lose embedded freelist links. `slab_state` is published unlocked while
allocation reads it under `cache_lock`.

Required: never discard metadata the allocator will read again, or rebuild it
on reactivation; publish state under the lock readers use; regressions for
guards-enabled HASH reuse and larger-quantum arena reuse.

### P1.6 Maintenance-thread startup
`umem_update_thread.c:59–62, 201–210`

The worker sets the predicate and signals without holding the waiter's mutex,
so a signal can be lost and `umem_reap()` can hang. The code also destroys
still-locked mutexes and calls `pthread_cond_wait()` inside `ASSERT`, which
removes the wait under `NDEBUG`.

Required: predicate mutation and signal under the same mutex the waiter uses;
no side effects inside `ASSERT`; no destruction of held mutexes.

### P1.7 Overflow and aligned-allocation contracts
`umem_arena.c:44–51, 103–113`; `malloc.c:87`;
`malloc_interpose.c:533–551, 627–653`

Arena offset addition can wrap and accept an impossible request. Bootstrap
header addition can wrap and return an undersized mapping. Bootstrap libc
`realloc` consumes the ownership record before it can succeed, and copies the
wrong length without `malloc_usable_size`. `posix_memalign()` does not enforce
the pointer-size multiple requirement, and `aligned_alloc()` is not
interposed.

Required: checked arithmetic on every size/offset path; ownership released
only after success; POSIX-conformant alignment validation; boundary
regressions.

## Phase 2 — Trustworthy evidence

**Status as of 2026-09-22.** Report:
`docs/results/2026-09-22-phase2-trustworthy-evidence.md`.

| Item | State | Evidence |
|---|---|---|
| P2.1 double-divided work budget | FIXED | `test_bench_accounting` + `check_budget.sh` (end-to-end) |
| P2.2 fragmentation accounting | FIXED | same; pair + VmHWM + series, sample floor |
| P2.3 oracle progress/failure | FIXED | `oracle_control.sh`: 7 cases incl. a broken-allocator shim |
| P2.4 status honesty | FIXED | 4 scripts corrected to SKIP=77 / real verdicts |
| P2.5 evidence and identity | FIXED | sha/flags/digests recorded; per-window sustained |
| P2.6 release artifacts | FIXED | separate workstream |
| P2.7 lifecycle coverage | FIXED | `test_lifecycle_churn` + `lifecycle_stress.sh` |

Withdrawn conclusions were **not** restored. The 192-thread scaling and
fragmentation claims require re-measurement under the protocol in
`test/bench/README.md`; new measurements taken here are labelled as new and
not comparable to the withdrawn figures.

**Found while fixing the harness, and NOT fixed — needs assignment:**
`umem_alloc` returns NULL for a large fraction of attempts at multi-GB heap
sizes because the default mmap backend exhausts `vm.max_map_count` (peak VMA
65,532 of 65,530) at ~5 GB, where glibc reaches 96 GB on the same box. This is
a sizing limit in `vmem_mmap.c` (`CHUNKSIZE = pagesize` on Linux → one VMA per
~75 KiB of heap), not a correctness defect, and `umem_reap()`+retry does not
relieve it. `vmem_mmap_top_alloc()` also restores `errno` on its failure paths,
so the underlying `mmap` `ENOMEM` is unobservable. See
`docs/results/2026-09-22-umem-heap-ceiling-max-map-count.md`.

### P2.1 Benchmark work sizing
`test/bench/matrix.sh:293–301`; `test/bench/bench_main.c:146–147`

The operation budget is divided by thread count twice, so 192-thread points
measured ~52k total operations in ~3.8 ms with >27% CoV. Fix the double
division, make the unit explicit (total vs per-thread), and require a minimum
per-thread work floor.

### P2.2 Fragmentation live-byte accounting
`test/bench/bench_framework.c:663–739`

`currently_held` accumulates bytes that were immediately freed, so the
denominator is wrong; `peak_rss_bytes` is post-cleanup current RSS; the
"192-thread" fragmentation workload actually runs one thread. Fix the
accounting, capture RSS at the ratio's peak, and label thread counts honestly.

### P2.3 Oracle progress and failure accounting
`test/stress/stress_concurrency_oracle.c:243–276, 425–471, 551–567`

Allocation failures count as completed work and the verdict checks only the
corruption flag, so an allocator returning NULL throughout can PASS. Require
successful work, report failures separately, add a start barrier, and
validate against a deliberately broken control.

### P2.4 Result status honesty
Distinguish PASS, SKIP, FAIL, and never-executed everywhere. Tests that exit
0 because a prerequisite is missing must report SKIP. Debug-detection tests
must not accept any nonzero exit as success.

### P2.5 Evidence preservation and identity
Use `job.sh` (remote deadline, process-group cleanup, persistent logs).
Fetch logs before re-running. Record commit, configure flags, instance type,
allocator versions, and binary digests with every result set.

### P2.6 Release artifact validation
`Makefile.am:292–295, 430–457`; `configure.ac:224–238`

`prop_palloc.c` includes `examples/umem_palloc.c`, which is not in any source
list or `EXTRA_DIST`, so a clean tarball cannot build. Installed `umem` looks
for its GDB helper beside the executable while install puts it under
`share/umem/debugger`. `umemctl` is not installed despite documented use.
`SH_LOG_COMPILER = $(SHELL)` runs Bash scripts under `/bin/sh`. Global
`-mavx2` from a compile-only check can make a "generic" binary illegal on
older CPUs.

Required: clean-tarball build, installed-prefix smoke test, and either
runtime ISA dispatch or an explicit non-generic build declaration.

### P2.7 Lifecycle stress coverage
Concurrent fork under allocation load, thread churn, cache create/destroy
churn, magazine resize under load, debug/reclaim reuse, and malloc-interposed
operation — on both architectures.

## Phase 3 — Diagnostic contracts

**Status: COMPLETE (2026-09-22), with four items explicitly left open.** Report:
agent transcript; verification at `0aa6629` independently re-checked by the
coordinator on x86_64 (default: 18 PASS / 1 SKIP / 0 FAIL; `--enable-introspect`:
19/19) and by the agent on aarch64.

All seven contracts are now defined in the headers/man pages, enforced in code,
and covered by a test with a demonstrated pre-fix failure:

| Item | Contract | Pre-fix failure shown |
|---|---|---|
| 1 cache lifetime | C1: cache-list walks hold `umem_cache_lock` throughout | SIGSEGV in `umem_findleaks` concurrent with `umem_cache_destroy`/`munmap` |
| 2 snapshot consistency | C2-C4: two-phase collect-then-emit, no allocation under a lock, truncation reported | deadlock backtrace: `fputc`->`json_escape`->walker holding `cache_lock` |
| 3 callback reentrancy | L1-L5: refcount + quiesce on unregister | 221 callbacks still running after unregister returned; 1 against a freed hook |
| 4 socket auth/disconnect | A1-A3, SIGPIPE blocked, `SO_PEERCRED`, EOF poll | socket mode 777 under `umask 000`; target DIED on mid-response disconnect |
| 5 stop/resume + fork | B1-B5, `umem_introspect_fork_child()` | fork child HUNG on an inherited armed predicate |
| 6 core vs live | `--core` withdrawn and fails loudly; gdb failures propagate; injection refused | `--core` exited 0 printing nothing; `shell touch` via argument accepted |
| 7 outstanding vs leak | renamed throughout; `UMEM_BUF_CACHED` returned; PTC gap reported | not one freed buffer reported CACHED or FREE |

Coordinator-verified independently: injection refused (no `/tmp/PWNED`),
`auto-load safe-path` scoped to the helper directory rather than `/`, `--core`
exits 2 with an explanation, socket mode 0600 explicit, `SO_PEERCRED` used.

`umem_fork.c` gained one weak declaration and one call in the child branch,
following the existing `umem_interpose_release_child()` pattern.

### Still open after Phase 3

- **rseq cached-set subtraction is compiled but not covered by a failing
  test.** The agent found its own `#ifdef UMEM_RSEQ_AVAILABLE` guard could
  never fire (that header *defines* the macro), so the branch had been compiled
  out entirely; fixed to match `umem.c`'s `__linux__ && HAVE_LINUX_RSEQ_H`.
  But a control build then passed identically, because rseq serves zero rounds
  (`rseq_alloc=0 rseq_free=0`), so no test can currently distinguish the two.
  Testable only once rseq magazines are actually populated. The claim that this
  path is covered was retracted rather than left standing.
- **Oversize allocations are still unaccounted.** `umem.c:3440` routes them
  past the caches into vmem, so cache-only walks omit them. Fixing needs a
  vmem-arena walk in the core allocator.
- **PTC bins are not enumerable**, so their held objects cannot be subtracted.
  Reported honestly as `ptc_unaccounted` instead of being silently miscounted.
  Needs a registry in `umem_ptc.c`.
- **`cached_skipped` remains nondeterministic** (measured: `2 1 2 2 2 2 2 1 1 2
  2 2` default; `1`x12 under `--disable-rseq`), so the e2e assertion is a bound
  (<=8), not an equality.
- **`--core` is withdrawn, not implemented.** A passive core reader is a
  separate project.

### Phase 3 as originally written

Define, document, and test:

- **Cache lifetime during inspection** (`umem_inspect.c:147–156`): walking the
  cache list without `umem_cache_lock` races destruction.
- **Snapshot consistency**: which locks are held, what is atomic, what may be
  torn; no allocation while holding allocator locks
  (`umem_inspect.c:256–274, 518, 699`).
- **Callback reentrancy and unregister semantics** (`umem_hooks.c:203–311`):
  unregister must not return while a hook can still be called.
- **Socket authorization and disconnect** (`umem_introspect.c:638–679`):
  explicit permissions, peer check, `SIGPIPE` suppression, no
  single-client monopoly.
- **Stop/resume and fork recovery** (`umem_introspect.c:387–523`):
  synchronized predicate state, no stranded threads, child-side reset.
- **Passive core analysis vs live inspection** (`tools/gdb/umem_gdb.py:74–99`):
  core files cannot execute inferior calls; either implement a passive reader
  or withdraw the claim.
- **"Outstanding allocation" vs "leak"**: report grouped outstanding
  allocations, and account for PTC/rseq-held and oversize allocations
  (`umem_inspect.c:576–616`).

## Phase 4 — Reduce unsupported surface area

Quarantine or remove, and stop advertising as supported:

- `umem_percpu.[ch]` — references nonexistent fields/statics; cannot build as
  configured. Remove the option or mark unavailable.
- `umem_htm.[ch]` — excluded prototype; no depot implementation. Remove the
  configure flag or mark unavailable.
- `umem_numa.[ch]` — invalid padding expression, disconnected initialization,
  `malloc`/`numa_free` mismatch, thread-hash posing as current-node. Separate
  the working topology hints from the unimplemented policy layer.
- `umem_own.[ch]`, `umem_profile.[ch]`, `examples/umem_palloc.*` — keep
  experimental and explicitly not safety/enforcement mechanisms until their
  contracts hold.
- rseq reload — revisit only after a publication/lifecycle protocol survives
  review. The existing design document is unsafe as written.

Documentation must match code: no "production" labels without recorded
evidence, no claims of features that have no production caller.

## Exit criteria

**Verified 2026-09-22 at `4ba7d00`** by `scripts/ec2/exit_criteria_gate.sh`,
run through `verify-isolated.sh` (committed content only) on `c7i.2xlarge`
x86_64 and `c7g.2xlarge` aarch64. **GATE: PASS on both** (`gate_failures=0`).

| # | Criterion | Result |
|---|---|---|
| 1 | Phase 1 fixed, pre-fix failure demonstrated, both arches | 10/10 |
| 2 | Phase 2 harness corrected; withdrawn conclusions not restored | done |
| 3 | Phase 3 contracts documented and tested, or withdrawn | 7/7 |
| 4 | Phase 4 surface reduced; options work or are gone | done |
| 5 | `make check` scope accurate; suites runnable and run | 21 entries, 20 PASS / 1 SKIP / 0 FAIL |
| 6 | Clean tarball builds; installed prefix works | tarball 20 PASS / 0 FAIL; external consumer compiles and runs |
| 7 | README/CHANGELOG describe what is true | done |

The gate deliberately runs what `make check` does not, and that is how it
earned its keep — it found four defects after all four phases had reported
complete:

- `test_hook_contracts` failing its own vacuity guard on x86_64 (the guard was
  right; the test counted attempts rather than tracked calls).
- **Six property-test call sites that never tested anything.**
  `QCC_getValue()` yields the value's address and must be dereferenced; they
  cast it to `long`, so every range check rejected a pointer and the driver
  reported "Gave up after 0 tests!". Flagged in the original review, never
  fixed, invisible because these binaries are built but not in `TESTS`.
- A clean tarball that could not pass its own `make check`, because
  `oracle_null_shim.c` — compiled at runtime by the test that proves the oracle
  discriminates — was in no `EXTRA_DIST`.
- Two PTC regressions returning 3 for INCONCLUSIVE, which automake reads as
  FAIL; a correct "I could not open the window" reddened the suite.

### Known-open at the gate, and disclosed rather than hidden

- **`prop_fragmentation` aborts** on `ASSERT(vmflag & VM_NOSLEEP)` at
  `vmem.c:603`. Confirmed pre-existing (identical `rc=134` at `ebcb467`); it
  went unnoticed because the test is built but not in `TESTS`, so `make check`
  never ran it on any commit or release. The gate labels it
  KNOWN-PREEXISTING rather than counting it as a pass or a new failure.
  `docs/results/2026-09-22-prop-fragmentation-vmem-abort.md`
- **The ~5 GB Linux heap ceiling** (`vm.max_map_count`), unassigned pending its
  own regression. This is what still blocks calling libumem
  production-ready for general use, and it now leads the README's limitations.
  `docs/results/2026-09-22-umem-heap-ceiling-vma.md`
- The four Phase 3 items listed under "Still open after Phase 3" below.

The original criteria, unchanged:

1. Phase 1 fixed, each with a demonstrated pre-fix failure and post-fix pass,
   on x86_64 and aarch64.
2. Phase 2 harness corrections landed; withdrawn conclusions either
   re-measured properly or removed.
3. Phase 3 contracts documented and tested, or the corresponding claims
   withdrawn.
4. Phase 4 surface reduced; build options either work or are gone.
5. `make check` scope stated accurately; comprehensive suite, property tests,
   oracle, and lifecycle stress runnable and run.
6. Clean tarball builds; installed prefix works.
7. README/CHANGELOG describe what is actually true.

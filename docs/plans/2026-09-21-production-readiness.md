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

Thread exit calls the half-bin flush once per bin, then frees the PTC, losing
the remainder (128 cached objects → 64 lost). Separately, a magazine
resize between acquiring a magazine and reading the cache's current capacity
lets an old smaller magazine be indexed with the new larger size; and stale
magazine handling frees the magazine shell without draining a full
magazine's objects.

Required: drain to empty at thread exit; capacity and magazine obtained as
one consistent decision; never discard a populated magazine; regressions for
exit-with-populated-bins and resize-under-load.

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

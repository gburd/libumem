# P1.2 fork lock order + P1.6 update-thread startup — verification record

**Date:** 2026-09-21
**Worker:** `intel-lo@fork` (c7i.2xlarge, x86_64), `arm-lo@fork` (c7g.2xlarge, aarch64)
**Plan items:** `docs/plans/2026-09-21-production-readiness.md` P1.2, P1.6
**Commits:** `1d81992` (tests), `b4a3212` (P1.2 fix), `6bea206` (P1.6 fix),
`d2dce96` (P1.6 evidence retraction + sound harness)
**Final verification commit:** `d9cf08a` (current master, all Phase 1 agents' work integrated)
**Build:** AL2023, gcc 11.5.0, `./configure` defaults unless noted

Raw logs under `docs/results/jobs/{intel,arm}-lo-fork-*`.

## P1.2 — fork lock order

### Root cause

`umem_lockup_cache()` acquired, per cache: `cache_lock` → depot `ml_lock`s
(global full/empty, then every per-CPU stripe) → per-CPU `cc_lock`s, with a
comment asserting this "must match normal operation".

It was the exact reverse of normal operation. `_umem_cache_alloc()` takes
`ccp->cc_lock` (`umem.c:2822`) and then, still holding it, calls
`umem_depot_alloc()`/`umem_depot_free()`, which **block** on `ml_lock`: the
local-stripe pop and the global fallback both use `umem_depot_pop()` (trylock,
then a blocking `mutex_lock` at `umem.c:1993`), and `umem_depot_push()` is
unconditionally blocking. `_umem_cache_free()` (3057/3102) and both `*_batch()`
variants (2960/2987, 3192/3221) do the same. The trylock-based cross-CPU steal
does not remove this — only the remote-stripe scan is non-blocking.

### Pre-fix failure (evidence)

`gdb -p` at t=40s into a 400-fork run, intel-lo — ABBA on one cache
(`0x7fe115f827c0`), full log in `jobs/intel-lo-fork-prefix-stack/out.log`:

```
Thread 1  umem_lockup_cache (umem_fork.c:72)  <- blocked acquiring cc_lock,
          umem_lockup, __run_prefork_handlers, fork()    while holding ml_locks
Thread 7  umem_depot_pop (umem.c:1993)        <- blocked on ml_lock,
          umem_depot_alloc, _umem_cache_free (umem.c:3102) while holding cc_lock
Threads 3,4,5,6,8,9,10  _umem_cache_alloc (umem.c:2822)  piled up on cc_lock
```

Both arches wedge (`jobs/intel-lo-fork-prefix`, `jobs/arm-lo-fork-prefix`):
`FAIL: deadline 90s exceeded -- fork()/alloc deadlock`.

`umem_ptc_fork_test` passes throughout and cannot catch this: it forks from an
effectively single-threaded parent, so no other thread holds an allocator lock
when the prepare handler runs.

### The lock order established

Stated in full in the header comment of `umem_fork.c`. Per cache:

1. `cache_cpu[*].cc_lock` (ascending CPU index)
2. depot maglists: `cache_full.ml_lock`, `cache_empty.ml_lock`, then
   `cache_depot_full[i]` / `cache_depot_empty[i]` (ascending stripe)
3. `cache_lock` (slab layer)

Why it cannot be otherwise: (1) before (2) is forced by every allocation path
holding `cc_lock` across a blocking depot call; (3) below both because
`umem_depot_alloc()` → `umem_depot_destroy_stale()` → `umem_slab_free()` takes
`cache_lock` while `cc_lock` is held, and nothing takes `cc_lock` or `ml_lock`
while holding `cache_lock` (the documented contract on
`umem_depot_alloc`/`umem_depot_free`). This is also the Solaris/illumos lineage
— the original took CPU locks, then depot, then the slab lock — and it agrees
with the hierarchy under "Lock Ordering" in `umem.c`.

### Also fixed: `sbrk_lock` omitted from fork handling

`vmem_sbrk_lockup()`/`vmem_sbrk_release()` held only `sbrk_faillock`, but
`_sbrk_grow_aligned()` holds `sbrk_lock` across `sbrk(0)`/`brk()`, reachable by
any thread through `vmem_sbrk_alloc()` under `UMEM_OPTIONS=backend=sbrk`. A
child could inherit it held by a thread that no longer exists. Both locks are
now held, `sbrk_lock` first. Verified by running the regression under
`backend=sbrk` (150/150 forks, both arches).

## P1.6 — update-thread startup

### Root cause (four defects, one object)

1. **Lost wakeup.** The worker set `obj->flag` and signalled `obj->cond` under
   `obj->mtx`; the creator tested `obj->flag` and waited on `obj->cond` under
   `obj->cmtx`, a mutex the worker never locked. A signal landing between the
   creator's predicate test and its wait was lost, and the worker signals once,
   so `umem_reap()` waited forever.
2. **`pthread_cond_wait()` inside `ASSERT()`.** `misc.h:117-118` defines
   `ASSERT()` to `(void)0` under `NDEBUG`, so release builds did not wait at all
   and returned while the worker still dereferenced the creator's stack object.
3. Worker locked `obj->mtx` and never unlocked it; the creator then destroyed it.
4. `obj->cmtx` destroyed while still held by the creator.

### Fix

One mutex guards both predicates (`go`, `done`); one condvar carries both
signals; every mutation is under that mutex and followed by a broadcast under
it; both sides wait in a plain `while` loop with no side effects in `ASSERT`;
both release before destroy. `cmtx` is gone — the second mutex was the bug.

### Evidence, and a retracted first attempt

The **first** P1.6 pre-fix reproduction was withdrawn (`d2dce96`). The test
interposes `pthread_cond_wait()` to widen the lost-wakeup window and originally
resolved the real implementation with `dlsym(RTLD_NEXT, "pthread_cond_wait")`.
On glibc that returns the **GLIBC_2.2.5 compat** symbol (pre-2.3.2 condvar
layout) while libumem calls `pthread_cond_wait@GLIBC_2.3.2`; forwarding modern
condvars into the compat implementation hangs every wait in the process, so the
"reproduction" would have failed against a correct allocator too.

Controls that established this (intel-lo):

| experiment | result |
|---|---|
| fixed library, no interposer | `umem_reap` returns |
| identical interposer, textbook handshake, **no libumem**, delay off | HANG |
| same program, interposer renamed (not interposed) | OK |
| `dlvsym(RTLD_NEXT, ..., "GLIBC_2.3.2")` interposer, same handshake | OK |

The test now uses `dlvsym`, and — because this failure mode is invisible, it
looks exactly like the bug being hunted — it runs a textbook handshake *through
its own interposer* with the delay armed before touching libumem, and asserts
the interposer was actually entered (catching a missing `-rdynamic`, which would
make the test vacuously pass). A broken harness now reports itself.

Re-measured with the sound harness, both arches, both build modes:

```
pre-fix  (jobs/{intel,arm}-lo-fork-p16prefix2):
  self-check: interposer sound (delay armed, handshake completed)
  FAIL: umem_reap() did not return -- update-thread startup handshake lost
  its wakeup (P1.6)                      [identical under -DNDEBUG]
post-fix (jobs/{intel,arm}-lo-fork-postfix2, -final):
  self-check: interposer sound (delay armed, handshake completed)
  PASS: update-thread startup handshake completed
                                         [identical under -DNDEBUG]
```

## Verification matrix — commit `d9cf08a`, both arches

`jobs/intel-lo-fork-final`, `jobs/arm-lo-fork-final`:

| check | x86_64 | aarch64 |
|---|---|---|
| P1.6 regression, debug | PASS | PASS |
| P1.6 regression, `-DNDEBUG` | PASS | PASS |
| P1.2 regression, 400 forks / 8 threads | PASS 400/400 (2.19M alloc/free) | PASS 400/400 (3.15M) |
| P1.2 regression, `backend=sbrk`, 150 forks | PASS 150/150 | PASS 150/150 |
| `make check` | 8 PASS / 0 FAIL / 0 SKIP | 8 PASS / 0 FAIL / 0 SKIP |
| `test_main --no-fork` | 422 OK / 0 FAIL / 10 SKIP | 422 OK / 0 FAIL / 10 SKIP |
| concurrency oracle | PASS | PASS |
| `umem_ptc_fork_test` | PASS (18/18) | PASS (18/18) |

`test_main` reports 422 OK rather than the 417 in the task statement; other
Phase 1 agents added tests to the runner between then and this run. 0 FAIL and
10 SKIP match.

### ASan — `jobs/{intel,arm}-lo-fork-asan2`

`--enable-asan`, `ASAN_OPTIONS=detect_leaks=0`. Both regressions pass on both
arches with **0 AddressSanitizer errors** (P1.2 at 120 forks / 6 threads; ASan
is slow).

Harness note: AL2023 ships the `libasan.so.6` SONAME symlink under the gcc
directory but not on the loader path, so ASan binaries need that directory in
`LD_LIBRARY_PATH`. `bootstrap.sh` creates the symlink; it does not put it on the
path.

### TSan — usable for this question, noisy in general

`jobs/{intel,arm}-lo-fork-tsan2`, `tsan3`, `tsanctl`.

AL2023 omits the `libtsan.so.0` SONAME symlink entirely (same packaging gap
`bootstrap.sh` patches for libasan); it has to be created before any
`--enable-tsan` binary can start.

Honest summary: **TSan is too noisy on this codebase to use as a gate, but it
did answer the one question P1.2 is about.**

- **0 `lock-order-inversion` reports** in the fork-under-load run on both
  arches — the specific class of defect P1.2 is.
- Both regressions complete under TSan (`exitcode=0`): P1.2 40/40 forks, P1.6
  PASS. TSan's own nonzero exit reflects the reports, not a hang.
- 14–15 `data race` reports per P1.2 run, **none naming `umem_fork.c`,
  `vmem_sbrk.c`, or `umem_update_thread.c`**. They land in
  `umem_init`/`umem_init_thr` (`umem.c:5015` vs `5045`), `umem_depot_alloc`
  (2282, 2344), `umem_cache_init`, `vmem_populate`, `umem_rseq_init` —
  pre-existing, in other agents' files, and outside this task's scope.
- The 2 `double lock of a mutex` reports on intel P1.6 are **harness artifacts,
  not defects**, established by control rather than assertion: one names my own
  `sc_worker` self-check (`test_update_thread_startup.c:122`), which is textbook
  `while (!pred) pthread_cond_wait(...)` code, and the same
  `umem_create_update_thread` report **disappears entirely** when the test runs
  without the interposer (`jobs/intel-lo-fork-tsanctl`: `double_lock_reports=0`,
  `REAP_RETURNED`). Forwarding through `dlvsym` bypasses TSan's own
  `pthread_cond_wait` interceptor, so TSan loses track of the mutex release
  inside the wait and reports the re-acquisition as a double lock.

### A pre-existing flake I did NOT cause, quantified

`test/debugger/test_inspect_e2e.sh` fails intermittently on aarch64 with
`AssertionError: expected 2 cached, got 1` — the test hardcodes how many freed
buffers happen to be sitting in magazines on a live target.

A/B on arm-lo, 30 standalone runs each, same instance, same build recipe
(`jobs/arm-lo-fork-abtest`):

| tree | failures |
|---|---|
| with my three files fixed | 3/30 |
| with my three files at their pre-fix state (`36bbf84`) | **5/30** |

Pre-existing and not made worse; it is the same flake recorded in `7e146d9`. It
did not appear in any of the 6 `make check` runs on x86_64.

## Cross-file requirements I did NOT implement (other agents' files)

### `malloc_interpose.c` — `bootstrap_ptr_lock` has no fork handling (@interp)

`is_libc_pointer()` (~line 131) and `track_bootstrap_ptr()` (~line 115) take
`bootstrap_ptr_lock`, and `is_libc_pointer()` is called by **ordinary `free()`**.
No fork handler touches it, so a child can inherit it held by a thread that no
longer exists and hang on its first `free()`. This is the same class of defect
as the `sbrk_lock` omission I fixed.

What it needs, concretely:

```c
/* in malloc_interpose.c */
void malloc_interpose_prefork(void)  { pthread_mutex_lock(&bootstrap_ptr_lock); }
void malloc_interpose_postfork(void) { pthread_mutex_unlock(&bootstrap_ptr_lock); }
```

called from libumem's existing handlers. Where in the order: this lock is not
below any allocator lock — `free()` takes it *before* entering the allocator —
so it must be acquired **before** step 1 (`umem_init_lock`) of the order in
`umem_fork.c`, i.e. first in `umem_lockup()` and last in `umem_do_release()`.

Registration point: I did **not** add one, to keep out of `umem.c`. Two options,
@interp's or the umem.c owner's call:

- add the two calls to `umem_lockup()`/`umem_do_release()` in `umem_fork.c`
  behind a weak symbol, so libumem links without the interposer (my preference —
  it keeps the single documented order in one file); or
- have `malloc_interpose.c` register its own `pthread_atfork()`. Note the
  ordering caveat: glibc runs prepare handlers in reverse registration order, so
  this only gives the required "before `umem_init_lock`" if the interposer
  registers *after* `umem_forkhandler_init()`, which is fragile. The weak-symbol
  route is deterministic.

Also still unreset in the child, per the plan text: `in_calloc` (P1.1's
per-thread rework may have changed this — @interp should confirm), PTC state
(`umem_ptc.c`, @ptc), and `umem_introspect_break_armed` and the introspection
stop/resume predicate (`umem_introspect.c`). I did not touch any of these.

### `make check` scope

Both new regressions are built by default but deliberately **not** in `TESTS`.
Adding them changes `make check`'s long-advertised 8-entry scope and its
runtime; that should be one coordinated change once all Phase 1 agents have
landed, not each agent editing `TESTS` concurrently. Run them directly:

```sh
LD_LIBRARY_PATH=.libs ./test/integration/.libs/test_fork_mt_load
LD_LIBRARY_PATH=.libs ./test/integration/.libs/test_update_thread_startup
```

`FORK_MT_THREADS`, `FORK_MT_FORKS`, `FORK_MT_DEADLINE`, `FORK_MT_CHILD_WAIT`
tune the fork test.

## Files changed

- `umem_fork.c` — corrected order, one true order documented, false comment removed
- `vmem_sbrk.c` — `sbrk_lock` held across fork
- `umem_update_thread.c` — single-mutex handshake, no wait inside `ASSERT`, no held-mutex destroy
- `test/integration/test_fork_mt_load.c` — new (P1.2 regression)
- `test/integration/test_update_thread_startup.c` — new (P1.6 regression)
- `Makefile.am`, `.gitignore` — build the new tests; stop ignoring new test *sources*

`umem.c` was **not** modified.

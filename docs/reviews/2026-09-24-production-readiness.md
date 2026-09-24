# Production-readiness review of v3.2.0 (2026-09-24)

Read-only review of tag `v3.2.0` (`3c29e5c`) against the question: is libumem
production-ready as a daily-use allocator on Linux, as `-lumem` and as an
`LD_PRELOAD` drop-in, and if not, what specifically stands in the way.

Inputs taken as given (not re-derived): `docs/plans/2026-09-21-production-readiness.md`
Phases 1-8, `docs/results/2026-09-24-allocator-comparison.md`, README status
banner and "What is still open". Everything below is what those miss or get
wrong, plus a weighing of what they say.

Evidence run on `c7i.2xlarge` (`intel-lo@review1`, terminated) via
`verify-isolated.sh v3.2.0`: default build `make check` 40 entries, 37 PASS /
3 SKIP / 0 FAIL; the same under the release workflow's
`CFLAGS='-O3 -g -DNDEBUG'`: 37/3/0. Job logs fetched to
`docs/results/jobs/intel-lo-review1-{build,ops,ndebug,c2,tm,tm2,def,nul}/`
(gitignored, on the reviewing machine). Each finding says which run or which
trace it rests on. Items I could not run are marked **not verified**.

Severity: BLOCKING / HIGH / MEDIUM / LOW / NOTE.

---

## 1. Lifecycle and concurrency

### 1.1 Lock order: the documented order is honoured -- NOTE (fine)

`umem_fork.c:36-83` states THE ONE TRUE LOCK ORDER. I checked every
`mutex_lock` site in `umem.c` (54 sites) against it:

- `umem_updateall()` (`umem.c:1081-1082`): `umem_cache_lock` then
  `umem_update_lock`. Order 3 -> 4. OK.
- `umem_cache_update()` (`:4640-4742`): entered with `umem_cache_lock`;
  takes `cache_lock` (6c), `cache_full.ml_lock` (6b), then `cache_lock` again
  for `reclaim_pages`. Never holds 6b and 6c together. OK.
- `_umem_cache_alloc/_free` (`:3267`, `:3502`): `cc_lock` then blocking
  `ml_lock` inside `umem_depot_alloc/free`. 6a -> 6b, as documented.
- `umem_depot_destroy_stale()` (`:2598-2622`) calls `umem_slab_free()`
  (takes `cache_lock`) from inside `umem_depot_alloc()`, which is called under
  `cc_lock` but after `ml_lock` is released. 6a -> 6c. OK.
- `umem_hash_rescale()` (`:4285-4324`): `vmem_alloc` first, then `cache_lock`.
  OK.
- `umem_cache_magazine_resize()` (`:4258`): `purge` (cc_locks one at a time,
  ml_locks via ws_reap) then `cache_full.ml_lock` alone. OK.
- `umem_cache_create()` (`:5313`): takes `umem_cache_lock` with no other
  allocator lock held. `umem_cache_destroy()` (`:5342`): same, then
  `umem_remove_updates` takes `umem_update_lock` alone. OK.

`umem.c:373-397` "Lock Ordering" is the same order minus the depot stripe
detail. It omits `umem_nofail_exit_lock`, `umem_error_lock` (misc.c),
`hook_list_lock`, `brk_lock`, and the two interposer mutexes, but none of
those is taken while holding an allocator lock (checked: `umem_hooks.c` never
allocates under `hook_list_lock`; `brk_lock` is a documented leaf; the
interposer locks are taken *before* entering the allocator and the fork
handler takes them first). The two comments agree with the code.

### 1.2 `umem_cache_lock` held across the whole walk, and what that implies -- MEDIUM (known, characterised at P6.4(2); one implication is not recorded)

The C1 rule (`umem_inspect.c:161-168`, `umem.c:4446-4451`) is what keeps a
cache alive for the duration of `func(cp)`; `umem_cache_destroy()` unlinks
under the lock and then destroys the descriptor. Enforced: yes, every walker I
found holds it (`umem.c:892-901`, `:935-980`, `:1081`, `umem_fork.c:207-214`,
`umem_inspect.c` x3, `umem_introspect.c` x6).

What it costs: `umem_cache_create()` (`:5313`) and `umem_cache_destroy()`
(`:5342`) queue behind the update thread's O(caches) walk. P6.4(2) measured
40 ms at 50k caches. I re-measured at a smaller scale: **5,000 caches,
`reap_interval=1`, worst create over 3 s = 4.89 ms** (`def` job). Linear in
cache count, as the plan says. It is a create/destroy tail, not an allocation
stall; the plan is honest about it.

**Not recorded anywhere:** a `cache_reclaim` callback (the user's
`umem_cache_create` reclaim argument) runs from `umem_cache_reap()`
(`:4208-4209`) inside `umem_process_updates()`, which runs from the update
thread *after* `umem_cache_applyall` has released `umem_cache_lock`, so a
callback that calls `umem_cache_destroy()` on a different cache does not
deadlock. But a callback that calls `umem_cache_destroy()` on *its own* cache
hits `umem_remove_updates()` (`:1031-1068`), which `ASSERT`s the caller is not
the update thread and, under `NDEBUG`, `cond_wait`s on `umem_update_cv` for
`UMU_ACTIVE` to clear on a cache the waiting thread itself is processing:
self-deadlock. `umem.c:319-321` says "any such application is tremendously
broken", which is true, but the man page for `umem_cache_create(3)` should
say what a reclaim callback may not do. LOW on its own; listed because the
comment's contempt is not a contract.

### 1.3 `umem_reap()` from inside the update thread is dropped, and the drop is now load-bearing -- NOTE (correct, one caveat)

`umem.c:4863` returns early if `IN_UPDATE()`. The comment traces the real
self-deadlock path. Correct. Caveat: `IN_UPDATE()` is a `thr_self()` compare
against two globals read without a lock (`:699-700`). `umem_update_thr` is
written under `umem_update_lock` (`umem_update_thread.c:207-209`) and zeroed
in the child (`umem_fork.c:230`); the racing read is benign on every platform
this builds for (aligned pointer-sized store), but it is a data race by the
letter of C11. TSan will flag it. Not a defect I would fix.

### 1.4 Slab create/destroy vs reclaim (P1.4/P1.5) -- fine

`umem_cache_reclaim_pages()` (`:4453-4553`): single pass under `cache_lock`,
DIRTY slabs marked `SLAB_RECLAIMING` and left linked, CLEAN slabs unlinked
into a private list, lock dropped once, madvise/destroy, lock retaken,
RECLAIMING -> CLEAN under the lock. `umem_slab_alloc()` (`:1735-1740`) skips
RECLAIMING under the same lock; `umem_slab_free()` can never target a
refcnt-0 slab. `umem_slab_keeps_metadata()` (`:4382-4386`) excludes non-HASH
and BUFTAG slabs from page discard, and the reactivation path (`:1760-1780`)
states the invariant it relies on. `umem_cache_drain_slabs()` (`:4572`) runs
only after the cache is off the list. The comments state invariants and the
code matches them. I found nothing wrong here.

### 1.5 Magazine ownership across resize (P1.3b/c) -- fine

Every PTC magazine hand-off goes through `umem_ptc_mag_return[_trylock]()`
(`:2823-2870`): capacity from `umem_mag_capacity(mp)` (the magazine's own
source cache), drain-before-discard for stale magazines, exact-full-or-empty
classification. `umem_depot_destroy_stale()` (`:2598`) uses the same
source-cache sizing. `_umem_cache_free()` (`:3565-3590`) re-checks
`cc_magsize` after dropping `cc_lock` to allocate a shell. The fix is at the
shared function, and I grepped for siblings: `umem_magazine_destroy()`
(`:2048`) is the only other consumer of `mag_round[]` by count and is called
with the caller's `rounds` (`:4206-4211`, `:5369-5371`). OK.

One thing the P1.3b regression does not establish: that a magazine already
*loaded* in a `cache_cpu[i]` with `cc_magsize == old` when the resize's
`purge` runs is handled. It is: `umem_cache_magazine_purge()` (`:4186-4223`)
takes each `cc_lock`, nulls `cc_loaded/cc_ploaded`, and sets `cc_magsize = 0`
so a concurrent free breaks out to the slab layer. Then `enable` restores the
new size. Fine.

### 1.6 PTC per-thread state at thread exit and fork -- HIGH (fork)

Thread exit: `umem_ptc_cleanup()` (`umem_ptc.c:108`) via `pthread_key_create`
destructor drains every bin through `umem_cache_free_batch()` and both
magazines through `umem_ptc_mag_return()`. `umem_ptc_get()` (`:270-329`)
refuses to hand out a PTC if `pthread_setspecific` fails, so no thread can
have a PTC without a destructor. P1.3a's exact oracle gates it. OK.

**Fork: the PTC has no fork handler at all** (`grep -i fork umem_ptc.c`:
nothing). After `fork()` the child inherits every parent thread's
`umem_ptc_t`, each holding up to (28 bins x 128 slots + 2 magazines x 255
rounds) live objects that the slab layer counts as allocated. Those threads
do not exist in the child, so their PTCs are never drained: **every object
cached in a non-forking thread's PTC at the instant of fork is leaked in the
child**, permanently, along with the ~22 KB `umem_ptc_t` itself. For a
pre-fork server that forks once from a quiescent parent this is a few hundred
KB once. For a process that forks repeatedly from a multithreaded parent
(worker respawn, `posix_spawn`-less `popen`, test runners) it is a leak
proportional to `forks x live_threads x cached_objects`. glibc's `tcache` has
the same property in principle but its per-thread cache is ~600 B, not ~22 KB
+ up to ~600 objects.

This is P1.3a (stranded PTC objects at thread death) again, with fork as the
death. `umem.c:280-330` (the fork-inconsistency list) does not mention it. The
fork handler could walk a registry of live PTCs and hand their contents to the
depot in the child (the P3 "PTC bins are not enumerable" item is the same
missing registry). **Not measured**; mechanism is read from source and is
unambiguous. Severity HIGH for the LD_PRELOAD case because forking
multithreaded programs are common under a drop-in; MEDIUM for `-lumem`.

### 1.7 Update thread start (`9bbe58b`) and child recreation (`cceae1d`) -- fine, with one ordering note

`umem_init()` (`umem.c:6078-6081`) creates the thread after `umem_ptc_init()`
and before `READY`. `umem_create_update_thread()`
(`umem_update_thread.c:167-259`) drops `umem_update_lock` around
`pthread_create` (which allocates -- bootstrap allocator, since `umem_ready !=
READY`), signals `go` and waits `done` under one mutex, never returns while the
worker can touch the stack object, destroys nothing held. P1.6 is genuinely
fixed. `pthread_create` failure returns 0, leaves `umem_update_thr == 0`, and
`umem_reap()` retries (`:4896`). Consistent.

Child: `umem_do_release(1)` (`umem_fork.c:225-346`) zeroes `umem_update_thr`,
requeues ACTIVE caches, releases every lock in reverse order, runs the
interposer and introspection child hooks, then takes `umem_update_lock` alone
and recreates. Correct order. Note: `pthread_create` in the child runs before
any application code, so the child always carries the extra thread even if it
is about to `exec` -- ~50 us and one `clone` per fork. `posix_spawn`/`vfork`
users are unaffected. Fine.

### 1.8 Hook registration/unregistration (L1-L5) -- fine

`umem_hooks.c:59-197`: refcount taken under `hook_list_lock`, `hook_active`
cleared first so `hook_hold` fails, list unlink, wait for refcnt 0. Double
unregister waits too. Refcount is 16-bit and refuses rather than wraps. No
allocation under the lock. The contract comment matches the code.

### 1.9 `umem_cache_create()` partial failure -- fine

`:5241-5245` hash table alloc fails -> `fail_lock` destroys `cache_lock`,
`fail` frees the descriptor. The per-CPU block (`:5306-5340`) is allocated
*after* every other allocation and its failure is tolerated (`cache_depot_ncpus
= 0` -> global depot only). Nothing leaks; `errno = EAGAIN`. Sequence checked
end to end.

### 1.10 `umem_cache_destroy()` -- fine, one leftover claim

`:5333-5400`: unlink under `umem_cache_lock`, `umem_remove_updates`, rseq
per-CPU magazines destroyed with their own round counts, purge, drain slabs,
then descriptor. The "not empty" log at `:5389` can now only fire for caches
with outstanding user buffers, which `drain_slabs` already reported one line
earlier (`:4622`). Two log lines for one condition; cosmetic.

---

## 2. Error paths

### 2.1 `umem_free(NULL, size)` with `size != 0` puts NULL into a free list -- HIGH

`umem.c:3989-4023`: `_umem_free()` indexes `umem_alloc_table` by `size` and,
with PTC on, stores `buf` into `b->slots[b->count++]` **without checking
`buf != NULL`**. The only NULL check is at `:4150` on the oversize branch.

Demonstrated (`nul` job, default build):

```
after umem_free(NULL,64): umem_alloc(64) -> (nil) errno=0
next umem_alloc(64)      -> 0x7f48ec3fff40 errno=0
```

The *next* `umem_alloc(64)` on that thread returns **NULL with `errno == 0`**
-- a spurious allocation failure reporting success -- and the one after that
works. With `tcache=0` the NULL goes into the CPU magazine (`:3512-3524`,
same missing check) and comes back out of `_umem_cache_alloc()` the same way.
With `UMEM_DEBUG=guards` `umem_cache_free_debug()` catches it.

`umem_alloc.3:139` says "`umem_free(NULL, 0)` is allowed" and is silent on
`umem_free(NULL, n)`; `test/unit/test_error_paths.c:293` says it is UB
"because the implementation indexes into umem_alloc_table and dereferences
the cache pointer before checking for NULL buf" -- i.e. someone saw this and
wrote a comment instead of a guard. Solaris libumem behaves the same. But
`free(NULL)` being a no-op is the universal expectation, `umem_free(p, sz)`
is the documented pairing for `umem_alloc(sz)` returning NULL on failure, and
the failure mode (a *later*, unrelated allocation fails with `errno` 0) is
exactly the kind of thing P5.5 was about. One `if (buf == NULL) return;` at
the top of `_umem_free()`; `umem_cache_free()` needs the same (or a
documented refusal). The interposer's `free()` (`malloc_interpose.c:692`)
returns on NULL before reaching this, so `LD_PRELOAD` is not affected.

### 2.2 No recovery after the first backend failure -- MEDIUM (P6.7 says so; re-confirmed)

`ops` job, `RLIMIT_AS=1 GB` and `RLIMIT_DATA=1 GB`: first failure clean
(`errno=12`, 74-76 VMAs), then **`post-failure 64B alloc: NULL (errno=12)`**
in both. glibc allocates 64 B from what it already has. P6.7's "keep an
emergency reserve" fix is not done. A daemon that logs on ENOMEM and continues
has nothing to log with. This is the difference between "returns NULL" and
"degrades gracefully"; the plan rates it MEDIUM and I agree, but it belongs in
README's limitations and is not there.

### 2.3 `RLIMIT_DATA` stale-errno hole -- not reproduced at 1 GB; open at 4 GB per P6.7

`probe_rlimit data 1024`: `errno=12` on both phases and post-failure. P6.7
saw `errno 0` at the 4 GB cap only. I did not run 4 GB (box has 16 GB; the
probe touches pages). The plan's own required fix (3) -- add `RLIMIT_DATA` to
`test_errno_preserved` -- has not been done, so the hole is neither closed
nor gated. **Not verified either way at 4 GB.**

### 2.4 `errno` on the `umem_alloc` slab path -- LOW

`_umem_alloc()` (`:3745-3906`) does not set `errno` itself on the cached-size
path; the value comes from whatever `vmem_mmap_alloc()` left, via
`umem_slab_create()`. On the P6.7 forcing mechanisms that is ENOMEM. But
`umem_slab_alloc()` can return NULL from the corrupted-link path (`:1811-1813`,
`umem_error(UMERR_BADADDR)` then NULL) with `errno` untouched, and
`umem_alloc_retry()` (`:1278`) sets nothing. A caller of `umem_alloc()` -- not
`malloc()`, which `malloc.c:239-243` maps to EAGAIN/ENOMEM -- can therefore
get NULL with a stale `errno` on a corruption-detected allocation. The man
page documents `umem_alloc` failure as "returns NULL"; it does not promise
`errno`. Consistent with the contract, inconsistent with what the
`test_errno_preserved` regression teaches users to expect. Grepped the class:
`umem_alloc_align` (`:3928-3932`) and the oversize branch (`:3944-3948`) have
the same shape; all three rely on the backend.

### 2.5 `vmem_mmap_alloc` / `vmem_mmap_top_alloc` errno -- fine

Both functions (`vmem_mmap.c:100-150`, `:203-279`) restore `errno` only on
success and leave the `mmap()` value on failure. The P5.5 two-restore defect is
gone. `vmem_populate()` (`vmem.c:578-660`) sets ENOMEM on the unsupported
`VM_SLEEP` path. `vmem_xalloc` VM_NOSLEEP failure (`vmem.c:1047-1055`) leaves
`errno` alone, so the backend's value survives. I found no further instance
of the erase-after-fix pattern in `vmem.c`, `vmem_sbrk.c`, `umem.c`.

### 2.6 `pthread_create` failure in `umem_create_update_thread()` -- fine

`umem_update_thread.c:250-258`: restores the signal mask, retakes the lock,
destroys its own mutex/cond, returns 0. No leak; `umem_update_thr` stays 0;
`umem_reap()` calls `umem_st_update()` inline. The `__nthreads()` stub in
`sol_compat.h:231` always returns 2, so the single-threaded fallback in
`umem_reap()` is dead code on Linux; harmless.

### 2.7 Interposer error paths -- fine

`bootstrap_malloc()` (`malloc.c:85-128`) checks header overflow, bounds
recursion at 16 and aborts loudly (correct: nothing sane is left to do),
returns NULL on `mmap` failure with `errno` from mmap. `realloc()`
(`malloc_interpose.c:853-914`) frees the old pointer only after the copy.
`memalign()` (`:919-987`) refuses an untrackable libc pointer rather than
leaking an unclassifiable one. `umem_malloc()` (`malloc.c:239-243`) maps a NULL
from `_umem_alloc` to `EAGAIN` for cached sizes and `ENOMEM` above -- the
`EAGAIN` is Solaris-lineage and non-standard for `malloc` (POSIX names only
ENOMEM); glibc never returns EAGAIN. A program that checks `errno == ENOMEM`
after `malloc` fails will mis-classify libumem's small-object exhaustion. LOW,
but it is a documented-nowhere divergence under `LD_PRELOAD`.

---

## 3. What the tests actually cover

`make check` at v3.2.0: 40 entries, 37 PASS / 3 SKIP / 0 FAIL on x86_64
(`build` job). The 3 SKIPs are the `--enable-introspect` tests in a default
build, correctly reported.

### 3.1 The comprehensive suite fails under the release build configuration -- HIGH

`.forgejo/workflows/release.yml:121` builds the shipped tarball with
`CFLAGS='-O3 -g -DNDEBUG'` and runs **no tests** on it. I ran `make check` and
`test_main --no-fork` on exactly that configuration (`ndebug`, `tm`, `tm2`
jobs):

- `make check`: 37/3/0 -- passes.
- `test_main --no-fork`: **FAIL**, `/umem_fail/assert_failed`
  (`test/unit/test_umem_fail.c:253`, `WIFSIGNALED(status) is not true`).

The test forks a child, runs `ASSERT(0 == 1)`, and expects SIGABRT. Under
`NDEBUG` `misc.h:139` compiles `ASSERT` to `(void)0`, the child `_exit(99)`s,
and the test fails. The test is wrong (it tests the macro, not the library),
but it means **the exit-criteria gate's step 2 has never run against the
release binary**. Nothing else in `test_main` failed under `NDEBUG`, which is
the useful result; the default build passes 100 %.

More important than that test: **the release build has no `ASSERT`s, and
several `ASSERT`s in this tree guard things the comments treat as
invariants** -- `umem_remove_updates` (`:1046-1048`, update-thread self-wait),
`umem_ptc_bin_flush_impl` (`umem_ptc.c:514`, `n == flush_count`),
`umem_cache_magazine_purge` (`:4192`, called off-list or in update). P1.6
already found one `ASSERT` with a side effect that vanished under `NDEBUG`. I
grepped for `ASSERT(` bodies containing `=` outside comparison operators
across `umem.c umem_ptc.c vmem.c malloc.c umem_fork.c umem_update_thread.c
malloc_interpose.c umem_inspect.c umem_introspect.c vmem_mmap.c vmem_sbrk.c`
and found none, so no further side-effecting asserts exist. But the
`-DNDEBUG` binary is a different program from the one every regression was
run against, and the difference is untested.

### 3.2 Public API entry points with no test in `make check`

From `umem.h` (26 declarations). Tests exist only in `test_main` (not in
`TESTS`; run by the gate script) for:

- `umem_nofail_callback` -- `test_umem_alloc.c`, `test_coverage.c` only.
- `umem_alloc_align` / `umem_free_align` -- `test_umem_align.c`,
  `test_cache_consistency.c`, `test_coverage.c`, `test_overflow_contracts`
  (the last IS in `TESTS`). OK.
- `umem_sbo_*` (4 functions) -- `test_sbo.c` only.
- `umem_arena_*` (6 functions) -- `test_arena.c` only.
- `umem_dump_contention` -- `bench_contention.c` (not a test) and a
  comment in `test_inspect_contracts.c`.

Nothing in `make check` calls `umem_nofail_callback`, any `umem_sbo_*`, any
`umem_arena_*`, or `umem_dump_contention`. The gate runs `test_main`, so they
are covered *if the gate runs* -- and §3.1 shows the gate has not run on the
release configuration.

### 3.3 `UMEM_OPTIONS` / `UMEM_DEBUG` never set by any test

From `envvar.c`'s tables, options no test under `test/`, `umem_test*.c`,
`umem_ptc_fork_test.c`, or `scripts/ec2/*.sh` ever sets (grep by name,
excluding comments in `test_envvar.c` which parses them into a helper but
does not exercise the behaviour):

- `UMEM_OPTIONS`: `size_add`, `size_clear`, `size_remove` (parsed in
  `test_envvar.c`, behaviour untested), `sbrk_minalloc`, `sbrk_pagesize`,
  `backend=sbrk` (one reference in a script), `concurrency`,
  `max_contention`, `nomagazines` (one each in `test_envvar.c`).
- `UMEM_DEBUG`: `nosignal`, `checknull`, `random`, `allverbose`, `mtbf`
  (one reference), `contents=N` (one), `firewall=N` (one).
- `UMEM_LOGGING=slab`, `=fail` (one reference each, parse-only).

`test_envvar.c` verifies that the variable is *parsed into the right global*;
it does not verify the feature does anything. `UMEM_DEBUG=firewall` and
`=contents` are the two rows README's debug-mode table marks "not measured";
they are also not tested.

**`perthread_cache` is a no-op** -- MEDIUM. README `:529-530` says "Tune via
`UMEM_OPTIONS=perthread_cache=2m` or disable with `perthread_cache=0`".
`envvar.c:246-248` stores it in `umem_ptc_size` (`umem.c:597`). Nothing reads
`umem_ptc_size` except `test/unit/umem_env_helper.c` (which prints it). PTC
capacity is `ptc_bin_capacity()` from compile-time constants; the enable
switch is `tcache=0`, which README does not mention. `test_envvar.c:237-247`
tests that `perthread_cache=8192` sets `ptc_size == 8192` and passes, which is
exactly the vacuous-test shape the brief asks about: **the assertion cannot
fail for the feature being broken because the feature is the assertion.**
This is the P7.2 `abort=1` story again -- a documented tunable with no
implementation behind it.

### 3.4 Configure options with no CI build

`tests.yml` builds `normal`, `asan`, `ubsan`, `coverage` on x86_64 docker.
Never built in CI: `--enable-introspect` (the gate script covers it by hand),
`--disable-rseq`, `--disable-numa`, `--enable-avx2`, `--enable-tsan`,
`--enable-pgo`, `-DNDEBUG` (release only, no tests -- §3.1). The aarch64
nightly is "validated but not armed". Every non-default configuration depends
on someone running `scripts/ec2/`.

### 3.5 Tests I checked for vacuity

- `test_errno_preserved`: sentinel `EDOM`, SKIPs if nothing fails, asserts
  `!= EDOM && != 0 && == ENOMEM`. Real.
- `test_forged_free`: five arms including a control that ordinary
  malloc/free still works. Real.
- `test_cache_footprint`: two FAIL arms with thresholds derived from
  `umem_max_ncpus`; SKIP only on create failure. Real.
- `test_heap_ceiling`: four SKIP exits, all on environment (no
  `max_map_count`, limit already raised >200k, `MemTotal` < 11 GB, tracking
  array). The third is the common path on any box under 11 GB RAM. **On CI
  docker runners this test will SKIP**, and on the `c7i.2xlarge` gate box
  (16 GB) it runs. Recorded as a bound on where the ceiling fix is gated.
- `test_stack_bounds.sh`: sets `UMEM_DEBUG=audit` itself so it cannot
  vacuously pass. Real.
- `oracle_control.sh`: injects two fault classes and requires exit 1; the
  legacy-verdict arm requires exit 0. Real.
- `interpose_regress.sh`: runs each binary with and without `LD_PRELOAD`;
  `repro_interpose_free_scaling` SKIPs (77) if not interposed. The
  `repro_calloc_interpose_race` binary has **no check that it is actually
  running under the interposer** -- it relies on the shell wrapper. If
  `LD_PRELOAD` silently failed (wrong path, `noexec`), the "interposer" arm
  would pass against glibc. LOW; the wrapper checks the .so exists.
- `test_debug` (`test/test_debug.c`): 11 subtests, `tests_passed ==
  tests_run`. Each subtest FAILs by not incrementing; one that returns early
  without `FAIL()` still counts as run-and-not-passed. Real.
- `umem_test`, `umem_test2`, `umem_test3`: smoke; `umem_test3` returns
  `EXIT_FAILURE` on any NULL. Thin but not vacuous.

### 3.6 What no test exercises

- Fork from a multithreaded parent **with live PTCs**, then account for
  objects in the child (§1.6). `test_fork_mt_load` uses sizes half above
  `ptc_maxsize` and checks only for deadlock; `test_fork_child_reclaim`
  checks RSS falls, not that objects are accounted.
- `umem_free(NULL, n)` (§2.1) -- a comment says it is UB.
- `umem_cache_create()` with a `reclaim` callback that allocates or destroys
  (§1.2).
- Any `UMEM_NOFAIL` path where the callback returns `UMEM_CALLBACK_RETRY`
  under real exhaustion.
- Signal-handler re-entry (§4.6).
- Anything under `-DNDEBUG` (§3.1).

---

## 4. Operational

What a deployer needs and README does not say:

### 4.1 Thread count +1 -- README omits it

`ops` job: `LD_PRELOAD=libumem_malloc.so sleep 3` has **2 tasks**; a `-lumem`
program that calls `umem_alloc` once has **1** for ~0 s then 2 (the thread is
created inside `umem_init()`, `umem.c:6078`). Every process gets a detached
thread with all signals blocked, running `umem_cache_applyall` every
`reap_interval` (10 s default). Tools that count threads, seccomp policies that
forbid `clone`, `RLIMIT_NPROC`-tight containers, and anything that
`pthread_kill`s "all my threads" will see it. Not in README. The plan mentions
"Thread count 1 -> 2" once (P6.8 STATUS).

### 4.2 Memory return timing -- README omits the numbers

`ops` job, 32,768 x 4 KiB filled then freed, **defaults** (`reap_interval=10`,
`reclaim_delay=30`): RSS 168 MB flat at t=15/30/45, **72 MB at t=60, 9 MB at
t=90**. So a freed heap returns in 60-90 s by default, in two steps (depot
reap then slab reclaim, each gated on `reclaim_delay` in `reap_interval`
ticks). glibc's `free()` trims immediately above `M_TRIM_THRESHOLD`. A
container with a memory limit sized to the working set plus a little will
OOM in the 60 s window after a spike. README says "Slab page reclaim via
madvise" and nothing about latency or how to tune it (`reap_interval`,
`reclaim_delay` are in `envvar.c` only).

### 4.3 `mmap_guard` behaviour change -- README omits it

`vmem_mmap.c:153-186`: since `ab8a73d`, freed spans **below 16 MiB are
`MADV_DONTNEED`'d and stay `PROT_READ|PROT_WRITE`**; a use-after-free into
them reads zeros instead of faulting. Before v3.2.0 every freed span was
`PROT_NONE`. This is a deliberate safety-for-VMA-count trade, well argued in
the comment, tunable via `UMEM_OPTIONS=mmap_guard=N`, and **not in the README
or CHANGELOG as a behaviour change a security-conscious deployer would want
to know about**. The default 16 MiB threshold is "a policy choice recorded
here, not a measurement" (the comment's own words).

### 4.4 Fork cost at scale -- documented in the plan, not README

P6.4b: 771 mutexes per cache per `fork()`, 3 s at 50k caches on 192 CPUs,
0.6-0.7 s at 10k. Plus, per §1.7, one `pthread_create` in every child. Plus,
per §1.6, the PTC leak. README's fork story is "fixed" (P1.2). A pre-fork
server with many caches on a wide box is a shape this allocator does not
serve; the plan says so (`P6.4b STATUS (4)`), README does not.

### 4.5 `umem_reap()` now that the thread exists

`umem.c:4834-4903`: still rate-limited to once per `reap_interval`, still
returns without doing anything if a reap is in progress, and now returns
immediately if called from the update thread. It queues `UMU_REAP` on every
cache and wakes the thread; it does **not** run `umem_cache_reclaim_pages`
(that is only in the periodic pass, `:4737-4742`, and `:4213-4219` says why).
So an application `umem_reap()` returns depot magazines to slabs but does not
`madvise` the slabs; RSS falls only when the periodic pass follows. The man
page description ("reclaim all unused memory") is optimistic by one
`reap_interval`.

### 4.6 Signal safety -- MEDIUM (same class as glibc; one thing is worse)

Nothing here is async-signal-safe and nothing claims to be; `malloc` is not
required to be. `ops` job: a `SIGALRM` handler doing `malloc/free` every 100 us
inside a 30M-iteration `malloc/free` loop under `LD_PRELOAD` completed 9,155
handler invocations with no hang or crash. That is luck plus the PTC (the
handler and the main loop hit the same thread's lock-free bins; a real
deadlock needs the interrupt to land inside `cc_lock` or `ml_lock` on the same
size class -- rare on an 8-CPU box, not impossible). glibc has the same
hazard with `arena` locks and also survives most careless programs.

What is worse than glibc: the **error path**. `umem_err_recoverable()` /
`umem_error()` -> `umem_error_enter()` (`misc.c:120`) takes
`umem_error_lock`, and `log_message()` uses `vsnprintf` on a 4 KiB stack
buffer. A signal handler that frees a bad pointer while the main thread is
inside `umem_error_enter` deadlocks on `umem_error_lock`. glibc's
`malloc_printerr` writes and aborts without a lock. LOW-MEDIUM: it requires a
bug in the program *and* a signal at the wrong instant, and the interposer
sets `umem_abort = 0` so the error path is reachable in steady state, which is
what makes it different from glibc.

`umem_create_update_thread()` blocks all signals in the worker; good.

### 4.7 `vm.overcommit_memory=2` -- fine

P6.7 measured it: init OK at 4.4 MB, failure at CommitLimit like glibc. The
`MAP_NORESERVE` reservations are not exempt under mode 2 and libumem does not
over-reserve. I did not re-run it; the mechanism (`vmem_mmap.c:90`,
`FREE_FLAGS` with `MAP_NORESERVE`, grown on demand) has not changed since.
**Not re-verified.**

### 4.8 The 16 MiB `PROT_NONE` threshold -- see §4.3

Also affects the "fault on UAF" claim implicit in README's "Buffer overrun /
UAF detect ✅" row: that row is about `UMEM_DEBUG=guards`, which is a
different mechanism (buftags) and is unaffected. But a deployer who relied on
the pre-3.2.0 unconditional `PROT_NONE` for free UAF faulting on oversize
objects between 128 KiB and 16 MiB has lost it silently.

### 4.9 Platform claims -- README's table is honest; the code has no evidence behind three rows

README `:368-390`: "Status here means what has been demonstrated". The rows
say FreeBSD "no CI, no recent run recorded", illumos "manually validated at
one point in time", macOS "tested, not continuously". I checked what "at one
point in time" means: `freebsd_pthread_hooks.c` last changed 2026-04-10; the
only illumos evidence in `docs/results/` is the 2026-09-08 shootout on
`m4.xlarge` (before Phases 1-8 rewrote the fork handler, the reclaim path,
the update-thread start, the PTC exit path, and the slab floors). **Nothing in
Phases 1-8 has been built, let alone run, on FreeBSD, illumos, or macOS.** The
README's first line still says "on Linux, FreeBSD, and macOS". The table
lower down corrects it; the lede does not. A deployer reads the lede.

The `UMEM_MIN_QCACHE_SLAB` and `UMEM_MIN_SLAB_OBJECTS` comments argue
illumos is a no-op "checked rather than assumed" -- by arithmetic on a 64 KiB
quantum in `test_slab_floor`, not by running on illumos. The argument is
sound. It is still not a run.

### 4.10 `EAGAIN` from `malloc()` -- see §2.7. Not in README.

### 4.11 `umem_dump_contention()` -- LOW

`umem.c:935-980` does `fprintf` under `umem_cache_lock`, which C2
(`umem_inspect.c:174-180`) forbids and `umem_introspect.c:167-174` explains
is safe only because allocation never takes `umem_cache_lock`. I ran it under
`LD_PRELOAD` with an unbuffered stream and a thread churning
`umem_cache_create` (`c2` job): completed, no hang. Consistent with the
introspect comment's reasoning: it stalls cache creation, cannot deadlock.
The `umem.h:86-91` doc says "Safe to call from a benchmark after a run",
which is the right scope. Fine, listed for completeness since it is the one
public entry point that breaks C2.

---

## Verdicts

### `-lumem` (linked, API)

**Usable for a daily-use application that does not fork from multithreaded
state, does not call `umem_free(NULL, n)`, tolerates a 60-90 s return-to-OS
latency, and does not need to allocate after ENOMEM.** Every Phase 1 lifetime
defect I traced is fixed at the shared function with the invariant stated in a
comment the code honours; the lock order is documented once and every site
agrees with it; the update thread exists, starts correctly, and survives fork.
Standing in the way of calling it production-ready without qualification:
(a) `umem_free(NULL, n)` poisons a free list and makes a *later* allocation
fail with `errno == 0` (§2.1) -- a one-line fix with no test; (b) no
post-failure reserve (§2.2), which is the difference between glibc's "log and
carry on" and libumem's "nothing left"; (c) a documented tunable
(`perthread_cache`) that does nothing (§3.3) -- the P7.2 pattern again, and
it means README's PTC paragraph is wrong; (d) the release configuration
(`-DNDEBUG`) has never had `test_main` run against it (§3.1). None of these is
a design problem; all four are the "things that were never running/checked"
theme the README banner already names.

### `LD_PRELOAD` drop-in

**Not yet, for two reasons that are not in README.** First, the PTC has no
fork handler (§1.6): a multithreaded program that forks leaks every object
cached in every non-forking thread's PTC in the child, plus 22 KB per thread,
per fork. Programs that fork from multithreaded state are exactly the ones
you preload into (servers, runtimes, test harnesses). Second, the operational
surface a preload user cannot see or control: an extra thread in every
process (§4.1), ENOMEM-then-nothing (§2.2), `EAGAIN` from `malloc` (§2.7),
60-90 s memory return (§4.2), and the `PROT_NONE`->`MADV_DONTNEED` change
(§4.3). The interposer itself -- ownership classification, bootstrap
allocator, fork participation, error refusal before mutation -- is sound;
P1.1/P1.7/P5.8/P8.1 hold and I found no new defect in `malloc_interpose.c` or
`malloc.c`. Performance at 0.74-0.91x of the API is honestly reported. What
stands in the way is the fork leak and the missing README paragraph, in that
order.

### setuid / root / network-facing

**README says no; that is still the right answer, and the reason has shifted.**
The Phase 5 findings are closed with real regressions, and secure mode gates
every side-effecting option; I found no new file/socket/exec side effect. The
residual reasons are the ones README already lists (`umem_may_own` is a hull,
`umem_abort = 0` under preload, leading-component symlinks) plus two from this
review that a hostile position D (controls allocation patterns) can reach: the
`umem_free(NULL, n)` poisoning is a way to make a victim's *next* allocation
fail with `errno == 0` if any caller can be induced to free a NULL with a
size, and the error-path lock (§4.6) is a deadlock reachable by feeding bad
pointers to `free()` from a signal handler. Neither is a memory-safety break;
both are availability. The larger point is §4.3: v3.2.0 quietly *reduced* the
fault-on-UAF property below 16 MiB for VMA-count reasons. That is a defensible
trade for a general allocator and the wrong default for a hardened one, and it
is not mentioned where a security deployer would look. Keep the "no".

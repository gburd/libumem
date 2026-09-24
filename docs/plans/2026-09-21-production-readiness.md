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
| P1.8 `umem_free(NULL, n)` stored NULL on a free list | FIXED (2026-09-24) | `test/unit/test_free_null` fails at parent, passes after |

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

### P1.8 `umem_free(NULL, size)` put NULL on a free list -- FIXED

`umem.c` `_umem_free` (the PTC bin store, the per-thread magazine store,
and via `_umem_cache_free` the CPU magazine store: no `buf != NULL` check on
any of them; the only check was `buf == NULL && size == 0` on the oversize
branch), `_umem_cache_free` (same), `_umem_free_align` (same shape; there
the outcome was `umem_panic("bad free")` from `vmem_hash_delete`).  Found by
the 2026-09-24 production-readiness review, section 2.1.

**Demonstrated** (`c7i.2xlarge`, default build, review's `nul` job):
`umem_free(NULL, 64)`; the next `umem_alloc(64)` on that thread returns
`NULL` with `errno == 0`; the one after it works.  A spurious allocation
failure that reports success, one call removed from its cause.  With
`tcache=0` the NULL goes into the CPU magazine and comes back out of
`_umem_cache_alloc()` the same way.  `test/unit/test_error_paths.c` had a
comment calling this undefined behaviour "because the implementation indexes
into umem_alloc_table ... before checking for NULL buf" -- a description of
the bug, filed as a contract.  `umem_alloc.3` said `umem_free(NULL, 0)` is
allowed and was silent on `umem_free(NULL, n)`; `umem_cache_create.3` said
the argument must not be NULL.  Solaris libumem behaves the same; `free(NULL)`
being a no-op is what every caller expects, and `umem_free(p, sz)` is the
documented pairing for a `umem_alloc(sz)` that may have returned NULL.

**Fix.** `if (buf == NULL) return;` at the top of `_umem_free`,
`_umem_cache_free` and `_umem_free_align` -- one check at the top of each
entry point, above every store, rather than one per store.  The interposer's
`free()` already returned on NULL before reaching any of these
(`malloc_interpose.c`), so `LD_PRELOAD` users were not exposed.  Man pages
now state the no-op for all three.

**STATUS: FIXED.**  Regression `test/unit/test_free_null` (in `make check`):
free NULL at 8 / 64 / 512 / 2048 / 2560 / 8192 / 16384 / 262144 B, once and
300 times, through `umem_free`, and through `umem_cache_free` on a private
cache and `umem_free_align`; every following allocation must be non-NULL.

| build | `umem_free(NULL, n); umem_alloc(n)` for n in 8..16384 | `umem_free(NULL, 262144)` (oversize) | `umem_cache_free(cp, NULL)` / `umem_free_align(NULL, 64)` | result |
|---|---|---|---|---|
| parent `4e7ad1d` (test only; measured as `b1e5b0d` before another agent amended it -- same tree for the files here), `c7g.2xlarge` | **NULL, errno 0, at every one of the seven sizes** (8, 64, 512, 2048 through the PTC; 2560, 8192, 16384 through the CPU magazine), once and after 300 frees alike | **`umem_panic("vmem_hash_delete(..., 0, 262144): bad free")`, SIGABRT** -- the `buf == NULL && size == 0` check did not cover it | not reached (aborted first); the align path has the same `vmem_xfree` | FAIL, rc 134 |
| fix `df7d1d8` | non-NULL | no-op | no-op / no-op | PASS |

The review reproduced the 64 B case; the regression shows it is every size
class below `UMEM_MAXBUF`, and that above it the outcome was a process abort
rather than a bad pointer.

Both arches, default and `--enable-introspect`, in the gate run recorded
under Phase 8's 2026-09-24 entries.

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

## Phase 5 — Security hardening (blocking for privileged or exposed use)

**Origin:** the 2026-09-22 adversarial security audit of tag `v3.0.0`, performed
after the release. Three reviewers were dispatched under instructions to
distrust the release notes and the coordinator's own claims; two died on content
filters and one was stopped, so every finding below was verified directly
against source at `d22bf03` with file:line. The audit also disproved two of the
coordinator's own prior claims (see P5.5 and the note under P5.1).

**Why this is a separate phase, not a patch:** Phases 1-4 were about
*correctness* — does the allocator do what it says under normal use. This phase
is about *hostility* — what happens when the environment, the filesystem, or
the allocation pattern is chosen by an attacker. libumem ships
`libumem_malloc.so` as an `LD_PRELOAD` drop-in, so it can land in setuid
binaries, root daemons, and network-facing servers. It was never hardened for
any of those.

**Threat positions used throughout.** A finding is only meaningful against a
stated attacker: (A) setuid/setgid target, (B) root daemon, (C) attacker
controls the environment but not the code, (D) attacker controls allocation
patterns and buffer contents but not the environment.

### P5.1 `execlp("addr2line")` — PATH-resolved exec at startup (CRITICAL)
`umem_stacktrace.c:159, :211`; reached from `umem.c:5747` via
`umem_stacktrace_init()`

`execlp` resolves through **`PATH`**, and the fork/exec sits on the
unconditional `umem_init()` path. The only gate is
`UMEM_STACKTRACE_ADDR2LINE` at `umem_stacktrace.c:354`, whose `getenv` has **no
`issetugid()` check**. The library's three existing privilege checks
(`misc.c:116`, `vmem_sbrk.c:314`, `umem.c:5552`) guard output, the sbrk backend,
and `umem_mtbf` — none guards this.

Attacker positions **A, B, C**: a setuid binary *linked* against libumem runs
whatever `addr2line` the attacker's `PATH` names, as the elevated user, before
`main()`. glibc's `AT_SECURE` blocks `LD_PRELOAD`, not linkage.

Compounding: `-e /proc/self/exe` at `:212` names *addr2line itself* after exec,
not the target binary, so the feature cannot work as written — it is attack
surface with no benefit. **Preference: delete the fallback.** If it is kept, it
needs an absolute path and an `issetugid()` gate.

### P5.2 No environment hardening for privileged processes (HIGH)
`envvar.c` (no `issetugid`/`AT_SECURE` anywhere in the file — verified by grep)

`UMEM_OPTIONS`/`UMEM_DEBUG`/`UMEM_LOGGING` are parsed with zero privilege
gating. The single post-parse mitigation is `umem.c:5552` zeroing `umem_mtbf`.
Attacker-supplied options with side effects include `profile=record:/path`
(creates/truncates a file, P5.3), `introspect=1` (opens the control socket,
P5.6/P5.7), `backend=sbrk` (gated, but only at `vmem_sbrk.c:314`, after
parsing), and debug toggles that change memory-safety behaviour.

glibc ignores `MALLOC_*` tunables under `AT_SECURE`; libumem has no equivalent.

Required: one `umem_secure_mode()` helper —
`issetugid() || getauxval(AT_SECURE)` — consulted **before** option parsing,
disabling every file, socket, and exec side effect. No such helper exists today
(verified). This one change also closes P5.3, P5.6, and P5.7.

### P5.3 File writers follow symlinks (HIGH)
`umem_profile.c:437`; `umem_inspect.c:2071, :2172`

`open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644)` and `fopen(path, "wb"/"w")`, with
**no `O_EXCL` and no `O_NOFOLLOW` anywhere in the library's writers** (the only
`O_EXCL` in the tree is `examples/umem_palloc.c:514`). Positions **A, B, C**:
combined with P5.2 this is arbitrary-file truncation as the target's UID; even
without it, a predictable snapshot path in a shared directory is a symlink
target.

### P5.4 Freelist links live inside freed user buffers, unmangled (HIGH)
`umem.c:4829` (`cache_bufctl = chunksize - UMEM_ALIGN`);
`umem_impl.h:303` (`UMEM_BUFCTL`); `umem.c` ~1747 (write), ~1656 (follow)

For non-HASH caches — the default for small objects — `umem_bufctl_t` sits at
the **tail of the user buffer**, and `bc_next` is the freelist link.
`umem_slab_free()` writes `bcp->bc_next = sp->slab_head` into it; the next
`umem_slab_alloc()` follows that pointer and returns it as a fresh allocation.

There is **no pointer mangling**. glibc has had safe-linking since 2.32. A
one-buffer overflow into an adjacent freed buffer's tail therefore yields an
arbitrary-address allocation — position **D**, and **worse than glibc for the
most common heap-bug class**.

`umem_slab_free()` does validate `sp->slab_cache == cp` and
`UMEM_SLAB_MEMBER()`, so a fully bogus pointer is caught; the `bc_next` chain
itself is trusted.

Fix: XOR-mangle `bc_next` with a per-process secret and the storing address, as
glibc does. This touches the allocation hot path, so it needs before/after
throughput on both architectures — which is why it is scheduled separately from
the rest of this phase.

**FIXED** (`5c6c342`, v3.1.0 track). Stored form is
`ptr ^ umem_link_cookie ^ (&slot >> 12)`; cookie from `AT_RANDOM` via
`getauxval` (no syscall, no allocation — `umem_init()` cannot allocate), mixed
with a static's load address and the pid. `umem_slab_alloc()` validates the
demangled link (`UMEM_ALIGN` alignment plus slab containment) and routes
failures through `umem_error(UMERR_BADADDR)` instead of dereferencing.
Regression `test/unit/test_freelist_mangle`; pre-fix it SIGABRTs on the attack
case (the allocator returned the attacker-chosen address, then aborted walking
the corrupted chain) and reports nothing on the detect case.

The UMF_HASH allocated-address chain is deliberately **not** mangled: those
bufctls come from `cache_bufctl_cache`, outside any user buffer, where the
overflow class cannot reach them.

**OPEN follow-up, required before `--enable-introspect` is usable:**
`umem_introspect.c:228` (`is_allocated()`, the free-list fallback) walks
`sp->slab_head` and follows `bcp->bc_next` **without demangling**, so it now
dereferences a mangled value — a wild read. It needs the same
`UMEM_LINK_DEMANGLE(&bcp->bc_next, bcp->bc_next)` that `umem_inspect.c:316`
and `:500` received. Not fixed here only because that file belongs to another
agent's in-flight work; it is not reached by a default build (it is inside
`#ifdef UMEM_INTROSPECT`, and `test_introspect_contracts` is one of the two
known `make check` SKIPs), so the default gate is unaffected. Line `:249` in
the same file is the hash chain and is correctly left plain.

### P5.5 `errno` erasure fix was not merely incomplete — it was ineffective (MEDIUM)
`vmem_mmap.c` — `vmem_mmap_alloc()` erases `errno` on failure **twice**, and
the second erasure sits on the exhaustion path the v3.0.0 fix was written for

The v3.0.0 work fixed this in `vmem_mmap_top_alloc()` only and the release notes
claim "failure paths now leave errno alone." That is **false for the sibling**.
This is the coordinator's own error: a symptom fixed at one call site, which is
precisely what AGENTS.md §7 forbids. Fix both, and correct the CHANGELOG claim.

**Corrected 2026-09-22 after verification: the original entry (one erasure, in
the `MAP_FIXED` failure branch) understated it. There are two, and the one not
originally found is the one that matters.**

```c
static void *
vmem_mmap_alloc(vmem_t *src, size_t size, int vmflags)
{
        int old_errno = errno;
        ret = vmem_alloc(src, size, vmflags);          /* (2) fails here */
        if (ret != NULL && mmap(...) == MAP_FAILED) {
                ...
                errno = old_errno;                     /* (1) reported */
                return (NULL);
        }
        errno = old_errno;                             /* (2) NOT reported */
        return (ret);                                  /*     reached with NULL */
}
```

1. The `MAP_FIXED` failure branch restores `errno` — the erasure originally
   reported.
2. The function's **final** `errno = old_errno` is also reached with
   `ret == NULL`, whenever `vmem_alloc(src, ...)` fails. That is the
   address-space-exhaustion path, and the call chain is structural rather than
   incidental: `vmem_mmap_arena()` builds
   `vmem_init("mmap_top", CHUNKSIZE, vmem_mmap_top_alloc, ...)` with
   `"mmap_heap"` using `vmem_mmap_alloc` and `mmap_top` as its source. So on
   exhaustion the chain is
   `vmem_mmap_alloc` → `vmem_alloc(src=mmap_top)` → `vmem_mmap_top_alloc` →
   `mmap()` fails and sets `ENOMEM`; `top_alloc` preserves it (the v3.0.0 fix);
   and `vmem_mmap_alloc`'s final unconditional restore wipes it one frame up.

**Therefore the v3.0.0 fix was not incomplete across siblings, it was
ineffective for the measured case it was written for.** The
`FIRST FAILURE at 8269MB (errno=0 Success)` in
`docs/results/2026-09-22-umem-heap-ceiling-vma.md` still read a stale `errno` at
the `umem_alloc()` caller after that fix, because the frame above undid it.

**Verified, not argued.** Isolated builds; a caller sets `errno = EDOM` as a
sentinel, then allocates under a 256 MB `RLIMIT_AS` cap so the backend `mmap()`
genuinely fails. Every tree that contains the v3.0.0 `top_alloc` fix (confirmed
by grep before measuring) still hands the caller its own sentinel back:

| tree | chunk | caller-visible `errno` |
|---|---|---|
| `d22bf03` (v3.0.0), `c7i.2xlarge` | 4096 | `33` — sentinel restored |
| `d22bf03` | 65536 | `33` — sentinel restored |
| `d22bf03` | 131072 | `33` — sentinel restored |
| `553d42e` (quantum reverted, errno kept), `c7g.2xlarge` | 4096 | `33` — sentinel restored |

4096 B is the size the ceiling measurement itself used, so this is not an
artifact of hitting a different arena. Post-fix the same probe reports
`errno=12 (ENOMEM)`.

A side finding, recorded in the results doc: the `errno=0 → errno=12`
improvement that the heap-ceiling document cites as proof this defect was closed
**does not reproduce from an ordinary caller on any surviving tree**, and the
commit that produced it (`dd658b1`) also raised the mmap-heap quantum and now
aborts under the probe. That number is unattributed rather than confirmed.

Fix: restore `errno` only when `ret != NULL`. A successful allocation must not
perturb it; a failed one must not erase the reason. `RLIMIT_AS` is the right
forcing mechanism for the regression — the same kernel refusal as the
`vm.max_map_count` ceiling, reachable without an 8 GB heap or a sysctl, so it
runs deterministically anywhere (`test/security/test_errno_preserved.c`).

### P5.6 Introspection socket: predictable path and an unlink TOCTOU (MEDIUM)
`umem_introspect.c:907` (path), `:975–990` (reclaim sequence)

The path is `/tmp/umem.<pid>.sock` — predictable. The A1–A3 hardening is
genuine and was verified (`umask(077)` around `bind` at `:969`, `chmod 0600` at
`:999`, `SO_PEERCRED` at `:939`, conditional unlink rather than unconditional).

But the reclaim sequence is `stat` → probe `connect` → `unlink` → `bind`. An
attacker who creates the path first (the pid is predictable, and the target has
not bound yet) can swap in a symlink between the `stat` and the `unlink`, making
the target unlink an attacker-chosen file. Use `$XDG_RUNTIME_DIR` or a private
directory; never a predictable name in a sticky shared directory.

### P5.7 `SO_PEERCRED` accepts the real uid (MEDIUM)
`umem_introspect.c:941`: `cred.uid == getuid() || == geteuid() || == 0`

For a setuid target, real uid is the unprivileged invoker, so this **grants that
invoker control of the privileged process** — including `whatis`/`bufctl` reads
at chosen addresses and the break/continue primitive, which parks allocating
threads and is therefore a DoS against the host process. Should be `geteuid()`
only.

### P5.8 Interposer `free()` decodes a foreign pointer's header (MEDIUM)
`malloc.c:387` (`process_free`); `umem_impl.h:663` (`UMEM_MALLOC_DECODE`)

For a non-bootstrap pointer, `process_free` reads `buf[-1]` and decodes it. A
foreign pointer means an 8-byte read before an arbitrary address. The magic
check usually rejects it, but the magic is a **fixed constant** and therefore
forgeable, and the interposer sets `umem_abort = 0` ("log and continue"), so a
forged header that passes proceeds to free memory libumem does not own. glibc
would abort; this continues into silent corruption. Position **D**.

### P5.9 `getpcstack` frame walk lacks stack bounds (MEDIUM, debug-only)
`getpcstack.c:83–100`

Validates alignment, a 16 MiB ceiling, and monotonically increasing frames, then
dereferences `fp[0]`/`fp[1]`. Under `UMEM_DEBUG=audit` a corrupted chain makes
the allocator **read** arbitrary addresses. Read-only: no write path was found.
Crash or info-leak, not code execution.

**CORRECTION (2026-09-23), measured by the agent fixing it.** My original text
said "or simply a caller compiled without frame pointers, which is the `-O2`
default" — implying `-O2` widens the exposure. **The opposite is true.** The
library is built `-O2` with no `-fno-omit-frame-pointer` (`CFLAGS = "-g -O2
-std=c17 ..."`; `objdump` shows `_umem_alloc` opening with `push %r15` and no
frame setup), so a walk entered through `umem_alloc` terminates after about two
frames and never reaches a corrupted frame at all. Measured depth: **7** from a
frame-pointer-having caller, **2** through `umem_alloc`.

So the arbitrary-read exposure requires the *library itself* to keep frame
pointers (`--enable-asan` adds `-fno-omit-frame-pointer`) or a caller chain that
does. The regression therefore drives `getpcstack()` directly from such a caller;
a test routed through `umem_alloc` in a default build **cannot fail**, which is
how the first version of that regression was found to be vacuous.

**This has a non-security consequence worth more than the finding itself:**
`UMEM_DEBUG=audit` records are only ~2 frames deep in a default `-O2` build. The
audit feature is substantially less useful than `umem_debugging.7` and the README
imply, and that is a documentation accuracy issue independent of hardening.

### P5.10 Minor / verified-good (INFO)

- `_umem_free(buf, 0)`: `(size-1)>>3` underflows to `SIZE_MAX>>3`, which fails
  the `index <` bound and falls through to the oversize path — safe **by
  accident**. Deserves an explicit check.
- **Verified sound, do not "fix":** the gdb argument whitelist
  (`tools/umem.c:159`) is correctly applied to all five interpolated inputs
  (`:179`, `:487–493`), rejecting control characters and `"\$` and backtick. The
  LLDB helper has no equivalent, but `tools/umem.c` only ever generates gdb
  command files, so LLDB is not reachable through that path.

### Phase 5 status (2026-09-23)

| Item | State | Evidence |
|---|---|---|
| P5.1 `execlp` on the init path | **FIXED** (deleted) | hostile-PATH sentinel ran pre-fix; verified the tier resolved nothing |
| P5.2 no privilege gating | **FIXED** | `umem_secure_mode()` before parsing; gate test |
| P5.3 symlink-following writers | **FIXED** | victim 54 -> 5472 bytes pre-fix |
| P5.4 unmangled freelist links | **FIXED** | abort pre-fix; see the control-isolation note below |
| P5.5 incomplete errno fix | **FIXED** | two erasures, second on the exhaustion path |
| P5.6 socket path + reclaim TOCTOU | **FIXED** | victim socket unlinked pre-fix |
| P5.7 `SO_PEERCRED` real uid | **FIXED** | decision function tested directly |
| P5.8 foreign header decode | **FIXED** | writes preceded validation; now after acceptance |
| P5.9 unbounded frame walk | **FIXED** | SIGSEGV pre-fix both arches |
| P5.10 minor / verified-good | **DONE** | gdb whitelist confirmed sound, left alone |

Qualified at `56dbe8d` on x86_64 and aarch64, isolated builds, both configs:
default **31 total / 27 PASS / 4 SKIP / 0 FAIL**, `--enable-introspect`
**31 / 30 PASS / 1 SKIP / 0 FAIL**, gate failures 0.

**Sharper mechanisms than the audit identified**, found by the agents fixing them:

- **P5.6 was worse than "predictable path".** `stat(2)` *follows symlinks*, so a
  symlink aimed at another process's socket satisfied `S_ISSOCK`, the probe
  `connect` failed, and the target then unlinked a directory entry it never
  created. Fixed with `lstat`, a euid-private directory created by atomic
  `mkdir(0700)`, and a bind-then-`rename()` reclaim that removes nobody else's
  entry.
- **P5.8's answer to "does any write precede validation" is yes.** Every
  successful-magic branch wrote `malloc_stat = UMEM_FREE_PATTERN_32` before any
  size check, and the oversize/memalign branches wrote one tag before validating
  the other. All mutation now happens after acceptance.
- **P5.9's exposure is narrower and in the opposite direction** from the audit
  text. See the correction in the P5.9 entry above.

**The P5.4 evidence gap is closed (2026-09-23, `c8d83bd`).** The original
regression could not distinguish mangling from containment, because its target
was outside the victim slab and containment caught it first. A new `inslab` case
targets the live neighbour -- inside the slab, aligned, so only mangling stands
in the way -- and FAILs with `-DUMEM_NO_LINK_MANGLE` (the allocator returns a
still-live buffer: a double allocation) while PASSing by default. Both controls
are now independently demonstrated.
`docs/results/2026-09-23-p54-which-control-blocks.md`

### Phase 5 exit criteria

1. P5.1, P5.2, P5.3, P5.5, P5.6, P5.7 fixed, each with a regression that
   demonstrates the pre-fix exposure and passes after, on x86_64 and aarch64.
   These are the v3.0.1 set.
2. P5.4 fixed with before/after throughput on both architectures, since it is on
   the hot path. This is v3.1.0, deliberately not bundled with the above.
3. P5.8, P5.9, P5.10 fixed or explicitly documented as accepted risk with the
   reasoning recorded.
4. README states plainly which deployments are supported and which are not.
   "Not for privileged or network-facing use" is the current honest answer and
   must stay until 1 and 2 are done.
5. No security fix lands without its regression. A hardening change that cannot
   be shown to close the hole it claims to close is not a fix.

## Phase 6 — Hard limits

**Origin:** the 2026-09-23 hard-limit survey. The position this phase serves:
a limit like "heap under 5 GB" cannot exist in this library. Two such limits
were already fixed before this survey (the ~5 GB heap ceiling, `3f2e67c`; the
interposer's ~500x `free()` collapse, `a74065e`) and are not re-examined here.
This phase records every *remaining* point where libumem fails, aborts, or
degrades sharply where glibc does not -- and, just as deliberately, every
dimension that was pushed and found fine, so nobody re-runs it.

**Method.** One probe per dimension under `test/limits/` (see its README),
each built twice from one source -- against the libumem API and as a plain
glibc binary -- so the glibc comparison is the same program. Every number is
from an isolated build of a pinned sha via `verify-isolated.sh`; the box and
sha are on each entry. Raw logs: `docs/results/jobs/<role>-limits3-<job>/`.
This phase is measurement and diagnosis only; no allocator code changes here.

**Severity scale.** BLOCKING = fails or aborts where glibc does not, or a
cliff a general-purpose user would hit. HIGH = degrades sharply (>10x) vs
glibc in a realistic regime. MEDIUM = measurable, bounded, worth fixing.
FINE = pushed past the target with no cliff; recorded so it is not re-run.

**Status as of 2026-09-23** (all measured at `a2548b8`; boxes: `c7i.2xlarge`
x86 16 GB, `c7g.2xlarge` arm 16 GB, `c7i.metal-48xl` 192 vCPU 377 GB)

| Item | Dimension | Verdict | The number | glibc |
|---|---|---|---|---|
| P6.1 | object count | **FIXED** (`cf3f762`) | 512 B objects: `umem_alloc` -> NULL at 8.2 GB, 63k VMAs (`vm.max_map_count`); 64 B: 48k VMAs at 100M. `3f2e67c` fixed one of two paths. Lever (c) 4 MiB qcache slab floor: 15,702 -> 274 VMAs, zero small-heap cost | 54 VMAs |
| P6.2 | object size ((1) **FIXED** `ab8a73d`; qcache not added) | HIGH | vmem segment supply FINE to 11 GB; every freed oversize object is a permanent VMA (40k at 136 KiB x 40k); oversize alloc 20-100x slower per call | same VMA count at half-freed, but its dynamic mmap threshold exits the regime |
| P6.3 | thread count | **FIXED** (`b8c39e6`, `06559e5`) | 16,000 threads: no cap; 56 KB/idle thread (`umem_ptc_t` is 31 KB, 20 KB of it padding); exit drain 423 us/thread and main stalls 37 ms | 17.5 KB/thread, 11 us/thread flat, 1 ms |
| P6.4/4b | cache count | footprint+leak **FIXED** (`e00fdf2`, `ede1849`, `ab8a73d`); create tail / fork at 50k open | allocation unaffected at 50k caches; 117 KB/cache, 415 ms create tails, **3.0 s `fork()`** at 50k on 192 CPUs; 51k VMAs + 1.8 GB left after destroying all | n/a |
| P6.5 | fragmentation over time | FINE / MEDIUM | 18 min churn: RSS +11 MB over the last 17 min (plateau); ratio 1.13 -> 1.33 is the live set shrinking under a fixed RSS | 1.018 flat |
| P6.6 | fork with 4 GB heap | FINE | 23 ms vs 21 ms; child COW +0.4 MB; handlers +2 ms constant, not heap-proportional | 21 ms |
| P6.7 | kernel knobs | FINE / MEDIUM (the BLOCKING part was P6.1) | clean NULL + ENOMEM under `RLIMIT_AS`, `RLIMIT_DATA`, `overcommit=2`; but no 64 B allocation possible after the first failure (glibc: yes); one stale-errno path on `RLIMIT_DATA`; `max_map_count=4096` fails 512 B at **500 MB** (P6.1 again) | recovers; 2 GB |
| P6.8 | reclaim under pressure | **FIXED** (`147d5ff`, `9bbe58b`; root: update thread never started) |
| P6.9 | fork child has no update thread | **FIXED** (`cceae1d`) | a freed 2 GB slab heap is 100 % resident at t = 100 s with the update thread running: every freed object sits in a depot magazine, `slab_refcnt` never reaches 0, `umem_cache_reclaim_pages` skips them, and the periodic pass never reaps the depot. `umem_reap()` every 10 s: -5 MB / 100 s | keeps interior pages too, but claims nothing |

Two of the eight are BLOCKING and both are one-mechanism fixes with a
measured lever (P6.1) or a named missing call (P6.8). The rest are
`ncpus`-proportional or per-thread constants that are 3-6x glibc and a
VMA-on-free pattern that P6.1's fix and P6.2 (1) address together.

**What was not tested and why.** `arm-hi` was not launched: the 192-CPU
measurements (P6.3, P6.4b) were taken on `intel-hi` and are
`umem_max_ncpus`-driven, not ISA-driven; the 8-CPU arm results for P6.1,
P6.2, P6.4, P6.7, P6.8 match x86 within noise wherever both were run. The
30-minute frag run came out at 18 minutes because the window count was
sized from a per-window estimate that was 40 % high; the plateau was
established by minute 3 and held to minute 18. `probe_caches` has no glibc
arm (no equivalent API). Raw job logs are in `docs/results/jobs/` on the
measuring machine and are gitignored; every number above is transcribed
into its entry.

### P6.1 The heap ceiling is fixed for one of two paths; small objects still hit it -- BLOCKING
`umem.c:5038` (`UMC_QCACHE` branch: `bestfit = MAX(1 << highbit(3 *
vm_qcache_max), 64)`, deliberately excluded from the `3f2e67c` floor);
`umem.c:5466` (`umem_va_arena`, `qcache_max = 8 * pagesize`); `umem.c:5019`
(`cache_slabsize = vmp->vm_quantum`, the one-page non-hash slab, also outside
the floor); `vmem_mmap.c:108` (`mmap(MAP_FIXED)` per span, unmergeable)

**This is a correction to `3f2e67c`, stated as such.** That fix put an
objects-per-slab floor in `umem_cache_create()`'s best-fit branch and
measured 16,283 -> 75 VMAs at 2 GB of **4 KiB** objects. The measurement was
real, and the ceiling was declared closed on its strength. But 4 KiB objects
are the hashed best-fit class -- one of two paths a span request can take to
the mmap backend, and the floor covers only that one. The other path is
every non-hash small object:

1. A cache under the `UMEM_VOID_FRACTION` cutoff (64 B, 512 B, everything
   through ~512 B) takes the `cache_slabsize = vmp->vm_quantum` branch: a
   **one-page slab**, untouched by the floor.
2. Its 4 KiB span request goes to `umem_default_arena`, which imports from
   `umem_va_arena`, created with `qcache_max = 8 * pagesize = 32 KiB`.
   4 KiB <= 32 KiB, so the request is served **through the va-arena's
   quantum cache**, not by a direct import.
3. That qcache is a `UMC_QCACHE` cache whose slab size is the excluded
   branch: `1 << highbit(3 * 32 KiB)` = **128 KiB**. Every 128 KiB qcache slab
   is one `vmem_mmap_alloc` -> `mmap(MAP_FIXED, RW)` over the PROT_NONE
   reservation, and a replacement mapping does not merge with its neighbours
   (`docs/results/2026-09-22-umem-heap-ceiling-vma.md`, "3."). Read from
   `/proc/pid/maps` of the live probe: 1,470 adjacent `rw-p` VMAs, stride
   132 KiB (128 KiB + one page of slab header and colouring).

So it is **one VMA per 128 KiB of small-object heap**, and at
`vm.max_map_count` = 65,530 that is a hard ceiling of ~8.2 GB, the same
number as before `3f2e67c` -- for a different, and far more common, size
class. One of two paths was fixed and the ceiling was declared closed.

**`test_heap_ceiling` passed at 9 GB and did not catch this because it
allocates 4 KiB objects: exactly the class the fix covered.** Required
regression when the fix lands: a second arm of `test_heap_ceiling` at 64 B or
512 B (the 512 B run below fails at 8.2 GB on a 16 GB box, so the existing
target is enough).

**Measured.** `a2548b8`, isolated builds, `c7i.2xlarge` (x86) and
`c7g.2xlarge` (arm), 16 GB, default `vm.max_map_count` 65,530.
`probe_objcount <n> <size>`.

| workload | umem VMAs | glibc VMAs | result |
|---|---:|---:|---|
| 512 B x 18M, x86 | **63,323 at 16.2M; `umem_alloc` -> NULL, ENOMEM, at 16,765,280 objects = 8.2 GB** | 54 at 15M | **umem FAILS; glibc does not** |
| 64 B x 100M, x86 | 48,218 (74 % of cap; fails at ~136M = 8.4 GB) | 54 | no failure yet; would fail |
| 64 B x 100M, arm | 48,215 | 74 | same |
| 4096 B x 2M (8 GB), x86 | 76 | 54 | the class `3f2e67c` fixed -- FINE |
| `reclaim=0`, 64 B x 30M | 14,512 | -- | reclaim is not a factor |

Per-object overhead is fine and is not the problem: 64 B objects cost 61 B
each (glibc 72), flat from 20M to 100M; 512 B cost 567 B (glibc 528).

**Fix levers, measured, not argued.** Each is a one-line hand patch to a copy
of the isolated `a2548b8` tree on `c7i.2xlarge`, run with `probe_objcount
4000000 512` (2 GB of 512 B), plus `1000 x {64, 512, 4096}` for the
small-heap cost and `20M x 64 B` for the slope. `levers` job log.

| lever | patch | VMAs at 2 GB / 512 B | VMAs at 20M x 64 B | RSS 1000x64 / 1000x512 / 1000x4K | worst alloc |
|---|---|---:|---:|---|---:|
| base | -- | **15,702** | 9,711 | 5.0 / 5.6 / 8.9 MB | 58 ms |
| (b) `qcache_max` 8 -> 16 pages (256 KiB slabs; `VMEM_NQCACHE_MAX` cap reached) | `umem.c:5470` | 7,891 | 5,118 | 5.2 / 5.6 / 9.0 | 8.8 ms |
| (b) `qcache_max` 64 pages + `VMEM_NQCACHE_MAX` 64 (1 MiB slabs) | `umem.c:5470`, `vmem_impl_user.h:101` | 2,034 | 1,339 | **6.0 / 6.4 / 9.7** | 66 ms |
| (c) `UMC_QCACHE` slab floor 1 MiB | `umem.c:5039` `MAX(..., 1 MiB)` | 2,026 | 1,339 | 5.1 / 5.5 / 8.9 | 34 ms |
| **(c) `UMC_QCACHE` slab floor 4 MiB** | `umem.c:5039` `MAX(..., 4 MiB)` | **274** | **127** | **5.1 / 5.5 / 8.9** | 89 ms |
| (d) `mprotect` instead of `mmap(MAP_FIXED)` on commit | `vmem_mmap.c:110` | 15,789 | 9,803 | 5.0 / 5.5 / 8.9 (+~85 baseline VMAs) | 11 ms |
| (d) + reserve 64 MiB per top-level import | `vmem_mmap.c:110`, `:158` | 15,706 | 10,158 | 5.0 / 5.5 / 9.0 | 78 ms |

Reading the table:

- **(c) at 4 MiB is the lever.** 15,702 -> 274 VMAs (57x), 64 B slope
  9,711 -> 127 (76x), and **zero small-heap cost**: 1000 x 64 B is 5.1 MB
  either way, because qcache slab spans are `MAP_NORESERVE` and only touched
  pages count, exactly the argument `3f2e67c` made for the hashed floor.
  Extrapolated, 100M x 64 B would sit at ~630 VMAs, and the 512 B ceiling
  moves from 8.2 GB to ~500 GB. It is also the same *kind* of change as
  `3f2e67c` -- a floor on span size in `umem_cache_create()` -- applied to
  the branch that fix skipped, so it belongs next to it.
- (b) is the wrong knob. It reaches 1 MiB slabs only by raising
  `VMEM_NQCACHE_MAX` to 64, which creates 64 qcaches per arena and costs
  +1 MB RSS on a 5 MB heap (each qcache touches its own slab), and the
  create-time cost was visible (the `b_1M` build's runs took ~3x longer to
  complete). (a), the arena quantum, was not tried: `pagesize =
  heap_arena->vm_quantum` at `umem.c:5752` keys every slab size off it, and
  the earlier attempt already showed that path aborts.
- (d) does nothing on its own, and the reason is instructive:
  `vmem_mmap_top_alloc` reserves exactly `size` per import, so each 128 KiB
  qcache slab gets its own PROT_NONE reservation and there is nothing
  contiguous for `mprotect` to merge into. Adding a 64 MiB reservation did
  not help either -- the reservations are handed out by `vmem_alloc` from a
  free list that is not address-ordered under churn, and `vmem_mmap_free`'s
  `PROT_NONE` remap splits whatever did merge. Making (d) work would mean
  address-ordered hand-out *and* deferring the PROT_NONE on free, which is a
  redesign of the backend for a result (c) gets in one line.
- The worst-single-alloc column is noise across levers (8.8 to 89 ms with no
  ordering that tracks VMAs); see the stall note below.

**Required fix.** Apply the span floor to the `UMC_QCACHE` branch at
`umem.c:5038`: `bestfit = MAX(1 << highbit(3 * vm_qcache_max), UMEM_QCACHE_MIN_SLAB)`
with `UMEM_QCACHE_MIN_SLAB = 4 MiB` (or expressed as a multiple of
`UMEM_MIN_SLAB_CEILING`), documented next to `UMEM_MIN_SLAB_OBJECTS` in
`umem_impl.h` as the second half of the same floor. Regressions:
`test_heap_ceiling` gains a 512 B arm (fails today at 8.2 GB on 16 GB, must
pass and report VMAs < 1,000); `probe_objcount 100000000 64` VMAs < 1,000
(today 48,218). Then re-check every other arena created with a `qcache_max`
(`grep -n qcache_max umem.c vmem*.c`) -- `umem_memalign_arena`,
`umem_oversize_arena`, `vmem_seg_arena` -- for the same shape, since a floor
that lands in `umem_cache_create()` covers them all but the cost should be
stated per arena. Owner: the author of `3f2e67c`, by their request.

**Two secondary observations from the same runs, recorded, not diagnosed:**

- *Allocation stalls.* Worst single `umem_alloc` grows with heap: 9 ms at
  10M, 52 ms at 50M, 95 ms at 90M (x86, 64 B); glibc 0.25 ms flat. Rare (19
  in 100M) and absent while the heap is parked across two update passes (so
  not the update thread). `perf record` over the fill shows no
  `vmem_hash_rescale`/`umem_hash_rescale`/`vmem_populate` above 0.15 %; the
  profile is `pthread_mutex_trylock`/`unlock` and the probe's own clock. Not
  ordered by VMA count across the lever table (8.8 ms at 7,891 VMAs; 89 ms at
  274), so **not the VMA count** either. **Control that does move it:**
  `UMEM_OPTIONS=reap_interval=1000` (the update thread never ticks during
  the run) takes the worst alloc at 30M x 64 B from 35 ms (default, same
  job) / 48 ms (`reclaim=0`) to **3.4 ms**, with 5 stalls > 1 ms instead of
  7-19. So the update thread *is* implicated after all, on the fill, even
  though a parked heap shows nothing -- something `umem_cache_update` does
  while slabs are being created costs the allocating thread tens of ms, and
  the parked probe missed it because no slabs are being created then. The
  candidate is `umem_cache_reclaim_pages()` walking the cache's *entire slab
  list* (`for (sp = nullsp->slab_prev; sp != nullsp; ...)`, `umem.c:4416`)
  under `cache_lock` every 10 s: at 30M x 64 B that is ~490k slabs per pass
  for one cache, and `umem_slab_create` needs the same lock. `perf` shows
  `umem_cache_update` at 0.46 % of samples, which is small in aggregate and
  exactly what a rare 35 ms lock hold looks like. Diagnosis needs the lock
  hold time measured directly; the fix shape is to walk only slabs that can
  change state (a DIRTY/CLEAN list, not the full list) so the walk is
  O(empty slabs), not O(all slabs). HIGH: 95 ms tails at 100M objects, and
  they grow with heap size.
- *RSS grows while freeing.* 7368 -> 9022 MB freeing 100M x 64 B (both
  arches), +494 MB at 30M with `reclaim=0`, +165 MB at 512 B x 15M, +38 MB at
  4 KiB: ~16 B per freed object, scaling with count not bytes. glibc flat.
  Arithmetic points at depot magazines (143-round `umem_magazine_t` = 1,152 B
  for a 64 B cache; 100M objects through the depot = 700k magazines =
  806 MB) retained on the full list until `umem_depot_ws_reap` runs on the
  10 s update tick, which the 25 s free phase mostly outruns. **Unconfirmed at
  the smaps level.** MEDIUM: it is 22 % of the heap transiently, and glibc's
  in-buffer bins have no equivalent cost.

### P6.2 Object size: oversize objects cost one VMA each once freed and reallocated -- HIGH (the vmem layer itself is FINE)
`vmem_mmap.c:158` (`vmem_mmap_free`: `mmap(PROT_NONE, MAP_FIXED)` over the
freed span splits the RW mapping it came from); `vmem.c:1098-1106`
(`vmem_xfree` returns a fully-free imported span to its source, one span per
oversize object); `vmem.c:136` (`VMEM_SEG_INITIAL`), `vmem.c:578`
(`vmem_populate`)

Provenance: `a2548b8`, `c7g.2xlarge`, `probe_objsize <size> <count>
<rounds>`; each round fills every slot, frees every other one, and the next
round refills the holes. `objsize`, `objsize2` job logs.

**The vmem segment supply is FINE.** 8,000 x 1 MiB, 180 x 64 MiB, 11 x 1 GiB
(11-11.25 GB of address space touched at 64 KiB stride), 40,000 x 136 KiB:
no allocation failed, no abort, no `vmem_populate` failure. `VMEM_SEG_INITIAL`
= 100 is only the static bootstrap pool; `vmem_populate` grows
`vmem_seg_arena` from the heap on demand (each populate takes
`VMEM_MINFREE + populators * reserve` segments = one page of `vmem_seg_t`),
and the only ceiling is the heap itself. The P1.5-era change that made
`vmem_populate` return ENOMEM on `VM_SLEEP` is not on this path -- every
libumem-internal caller is `VM_NOSLEEP` -- and was not reached. RSS tracks
live bytes exactly (0.71 GB for 11.25 GB touched at 64 KiB stride, same as
glibc), and after freeing everything RSS returns to 0.00-0.02 GB: the
oversize arena hands spans straight back to the mmap heap and the heap
`PROT_NONE`s them, so **oversize memory is returned to the kernel
immediately**, which is what item 8 asks about for this class.

**What is not fine: every freed oversize object becomes its own VMA, and
they never merge back.**

| size x count | umem VMAs full / after freeing half | glibc VMAs full / half | umem VMAs after freeing ALL |
|---|---:|---:|---:|
| 1 MiB x 8,000 | 103 / **8,102** | 74 / 4,073 | **1,303** |
| 136 KiB x 40,000 (just over `UMEM_MAXBUF`) | 103 / **40,102** | 74 / 20,074 | **6,151** |
| 64 MiB x 180 | 103 / 282 | 74 / 163 | 103 |
| 1 GiB x 11 | 103 / 113 | 74 / 78 | 105 |

Mechanism, from the numbers: while full, umem is at 103 VMAs -- the 1 MiB
objects came from a handful of large `MAP_FIXED` RW commits (via
`vmem_mmap_alloc` over a reservation that `_vmem_extend_alloc` had grown), so
the kernel sees one RW VMA per import. Freeing every other object then
`vmem_xfree`s each 1 MiB span back to `mmap_heap`, and `vmem_mmap_free` remaps
each one `PROT_NONE` with `MAP_FIXED`. **A PROT_NONE hole punched into an RW
VMA splits it into three**, and the heap-ceiling doc measured exactly this
("`mprotect(PROT_NONE)` on those too: 25 -> 88"). 4,000 holes = 8,102 VMAs.
glibc does the same thing for `mmap`-threshold chunks (4,073 -- one `munmap`
per freed chunk, punching holes in the same way), so **at the half-freed
instant the two are comparable: umem is 2x glibc**. The difference is what
happens next:

- glibc's mmap threshold is dynamic: after the first free of a 1 MiB
  `mmap`ped chunk it raises the threshold, so the refill round comes from
  the `brk` heap and the arenas -- its VMA count stays at 4,073 and its
  "full" count on round 1 *is* 4,073, then flat. And when everything is
  freed it is back to 74.
- umem refills the holes from the freed spans (good: "post-free alloc"
  lands inside the reservation, `0xfffd...`), but the reservation has
  already been fragmented by the PROT_NONE remaps, and re-committing a hole
  RW with `mmap(MAP_FIXED)` does not merge it with its RW neighbours (the
  replacement-mapping rule). Full again, umem is back to 103 -- **only
  because the kernel did merge them**, which contradicts the rule. So the
  RW commit *does* merge here where it did not in P6.1, and the difference
  is that here the neighbours were created by the same `_vmem_extend_alloc`
  reservation; P6.1's 128 KiB slabs were each their own top-level
  reservation. Either way: after all is freed, 1,303 VMAs remain for 8,000
  objects, 6,151 for 40,000 -- PROT_NONE fragments that were never
  coalesced back into one reservation.

**The ceiling this produces.** At 136 KiB (the first oversize size), 40,000
live-then-half-freed objects cost 40,102 VMAs = 61 % of `vm.max_map_count`.
**A program holding ~65k oversize objects and freeing a scattered half of
them exhausts the map count** -- 8.7 GB at 136 KiB, and any process with
large numbers of 128 KiB-2 MiB buffers (network I/O buffers, image tiles,
database pages above 128 KiB) is in this regime. glibc reaches the same
VMA count for the same pattern, but its dynamic threshold exits the regime
after the first round, and umem has no such escape. The oversize path is
also **20-100x slower per call**: worst 1.1-1.5 ms per `umem_alloc` vs
12-236 us for glibc, because every oversize allocation is an `mmap` syscall
plus a vmem span create; glibc's `mmap` is the same syscall but with no
segment bookkeeping.

**Required fix.** Two independent halves:
(1) Stop punching PROT_NONE holes on every oversize free. `vmem_mmap_free`
should `madvise(MADV_DONTNEED)` (or `MADV_FREE`) the span and leave it RW
and mapped -- RSS returns identically (measured: RSS after free is already
~0 because of the remap, and DONTNEED gives the same), the VMA does not
split, and the next commit is a no-op instead of another `mmap`. The
security argument for PROT_NONE (a freed span faults on use-after-free) is
real; if it is kept, it should be for spans above a size threshold (say
>= 16 MiB, where VMA count cannot matter) and DONTNEED below. Regression:
`probe_objsize 139264 40000 2` must stay under ~200 VMAs at the half-freed
point.
(2) Cache oversize spans below a size ceiling instead of returning them to
the mmap heap on every free: an `umem_oversize_arena` with a `qcache_max` of
a few MiB (it is created with `0`, `umem.c:5787`) turns 136 KiB-2 MiB
allocations into slab-cached objects with no syscall, and closes the
20-100x per-call gap at the same time. With P6.1's fix in place the qcache
slabs would be >= 4 MiB spans. This is the same lever as glibc's dynamic
mmap threshold, done statically.

**STATUS: (1) FIXED (`ab8a73d`); (2) not done, and with a reason.**

(1) `vmem_mmap_free` now `MADV_DONTNEED`s spans below `vmem_mmap_guard_min`
(16 MiB, `UMEM_OPTIONS=mmap_guard=N`, 0 = never guard) and `PROT_NONE`-remaps
at or above it. The threshold is a *policy* choice, recorded as such in the
source: fault-on-use-after-free is worth most on large buffers and costs
nothing there (4,000+ simultaneously-freed 16 MiB spans to reach the cap);
below it the VMA cost lives and small spans are re-committed soon anyway.
Evidence (`c7i.2xlarge`):

| | pre `4f0feae` | post `ab8a73d` |
|---|---|---|
| `test_oversize_vma`: new VMAs from 1,000 frees of 136 KiB | **1,999** (two per hole: the NONE region and the split neighbour) | **0** |
| guard arm (`mmap_guard=4096`, same run) | -- | 1,999 (the `PROT_NONE` path exists) |
| RSS returned by the free | 132 of 132 MB | 132 of 132 MB |
| `probe_objsize 139264 40000 2`, VMAs after all freed | 1,303 | **74** |
| `test_heap_ceiling` 4 KiB / 512 B arms | 73 / 992 VMAs | 73 / 992 (unchanged) |
| reclaim regressions | PASS | PASS |

This is also what closed P6.4's destroy leak (freed descriptor pages took the
same path). Scope: every span free of any kind goes through this function.

(2) An oversize quantum cache was **not added**. With `cf3f762`'s 4 MiB floor
on `UMC_QCACHE` slabs, a 2 MiB oversize class would be 2 objects per 4 MiB
slab and a 136 KiB class 30 -- the floor would have to be lifted for this
arena, which reintroduces the P6.1 VMA-per-slab arithmetic at exactly the
sizes where it bit. And the per-call gap (1) was measured against is now a
different number: with DONTNEED instead of a `PROT_NONE` remap plus a
`MAP_FIXED` re-commit, an oversize alloc/free pair is one `madvise` and one
`mmap`, not two `mmap`s. Not re-measured here; the comparison run will.

### P6.4 Cache count: 50,000 caches -- FINE on allocation stall; MEDIUM on create cost and per-cache footprint
`umem.c:884-893` (`umem_cache_applyall`, holds `umem_cache_lock` for the
whole walk); `umem.c:4849` (`csize = UMEM_CACHE_SIZE(umem_max_ncpus)`);
`umem.c:5124-5135` (two `mmap()`s of `umem_max_ncpus * 64 B` per cache for
the depot arrays); `umem.c:5165` (one `mmap()` for `cache_rseq`);
`umem.c:5196` (create takes `umem_cache_lock` to link)

Provenance: `a2548b8`, `c7g.2xlarge` (8 vCPU, `umem_max_ncpus` = 8),
`probe_caches 10000 --fork` and `probe_caches 50000`. The 192-CPU figures
are P6.4b below, from `intel-hi`.

| | 10,000 caches | 50,000 caches |
|---|---:|---:|
| create wall | 1.21 s (121 us/cache) | **35.98 s (720 us/cache)** |
| worst single `umem_cache_create` | 3.0 ms | **39.8 ms** |
| RSS delta | 181 MB | 907 MB |
| per cache | **18.6 KB** | 18.6 KB |
| VMAs added | 318 (0.04/cache) | 1,577 (0.03/cache) |
| worst `umem_alloc` over 25 s (2+ `applyall` passes) | 0.1 ms, 0 stalls > 1 ms | **0.0 ms, 0 stalls** |
| create/destroy cycle while the walk may run, worst | 8.4 ms | 42.1 ms |
| destroy all | 0.23 s | 1.16 s |
| RSS after destroying all | 29.6 MB (from 4.5) | 130.6 MB (from 4.4) |
| VMAs after destroying all | **5,930** | **29,159** |
| `fork()` with 10k caches | 80-83 ms (x3) | not run |

**Fine:** the thing item 4 asked about most -- does `umem_cache_applyall`
holding `umem_cache_lock` across a 50k-cache walk stall allocation -- is
answered no. The allocation fast path never takes `umem_cache_lock`, and
`umem_cache_update` takes each cache's own `cache_lock` briefly; 187M probe
allocations over 25 s with 50k caches saw a 0.0 ms worst. glibc has no cache
API; the comparison is not applicable and this entry is umem-only.

**Not fine, and growing with count:**

1. **Create cost is super-linear: 121 -> 720 us per cache from 10k to 50k,
   worst 3 -> 40 ms.** The 40 ms worst matches the `applyall` interval: a
   create that lands during the update thread's walk waits on
   `umem_cache_lock` for the whole walk (`umem.c:5196`), and the walk is
   O(caches) with a `cache_lock` + `cache_full.ml_lock` + reclaim scan each.
   The steady-state per-cache cost also grows: each create does three
   `mmap()`s and 3 x `umem_max_ncpus` `mutex_init`s, and `vmem_alloc` from
   `umem_cache_arena` for a 1.2 KB+ descriptor. So *cache creation* stalls
   behind the walk even though *allocation* does not. MEDIUM: a program that
   creates caches at runtime (per-connection, per-tenant) pays 40 ms tails.
2. **Per-cache footprint is 18.6 KB on an 8-CPU box, and most of it is
   `umem_max_ncpus`-proportional.** Descriptor `UMEM_CACHE_SIZE(8)` is ~1.2
   KB + 8 x 128 B `cache_cpu` = 2.2 KB; two depot arrays 8 x 64 B each in
   their own pages = 8 KB (two `mmap`s, page-rounded); `cache_rseq` 8 x 64 B
   in its own page = 4 KB; hash table 64 x 8 B = 512 B; one live object's
   slab 4 KB. **12 KB of the 18.6 KB is three page-rounded `mmap`s holding
   512 B each.** On 192 CPUs (`umem_max_ncpus` = 256) the descriptor becomes
   33 KB and the depot arrays 16 KB each -- see P6.4b.
3. **Destroy leaks VMAs: 5,930 after destroying 10k caches, 29,159 after
   50k.** Each destroy `munmap`s its three per-cache mappings, which punches
   holes in whatever the kernel had merged them into (adjacent
   `MAP_PRIVATE|MAP_ANON` mappings from consecutive `mmap(NULL, ...)` calls
   do merge, which is why create added only 0.03 VMAs/cache), and the
   `vmem_free` of the descriptor to `umem_cache_arena` retains the span.
   29,159 VMAs is 44 % of `vm.max_map_count` **left behind by caches that no
   longer exist**. A create/destroy churn of 100k caches over a process
   lifetime would hit the ceiling. MEDIUM-HIGH.

**Required fix.** (1) Allocate the depot arrays and `cache_rseq` from
`umem_cache_arena` (or one `mmap` per cache, carved) instead of three
page-rounded `mmap`s: 18.6 -> ~7 KB per cache on 8 CPUs, and no per-cache VMA
churn on destroy. (2) Move the create-time link (`umem.c:5196`) off the lock
the walk holds -- either publish via an RCU-style pointer swap, or have
`umem_cache_applyall` snapshot the list under the lock and walk the snapshot
unlocked (the per-cache work already takes `cache_lock`). Regression:
`probe_caches 50000` worst create < 5 ms and VMAs after destroy < 200.

**STATUS.** (1) **FIXED** (`e00fdf2`): the three arrays are carved from one
mapping sized to their contents (`cache_percpu_map`). Footprint 18,964 ->
**10,708 B/cache** on 8 CPUs (`test_cache_footprint`, 2,000 caches, pre
`76812d0` FAIL / post PASS on that arm). Predicted "~7 KB" was optimistic:
the remaining 10.7 KB is the descriptor (`UMEM_CACHE_SIZE(8)` = 2.2 KB), the
hash table, one live 4 KiB slab, and the carved mapping (1.5 KB, still
page-rounded to 4 KiB as a single mapping -- putting it *inside* the
descriptor's vmem allocation would recover that page and is the next step if
per-cache footprint matters further).

**The VMA leak on destroy is NOT this defect** and did not move with (1):
1,134 -> 1,216 for 2,000 destroys. `/proc/self/maps` after destroy: 1,236 of
1,295 VMAs are single 4 KiB `PROT_NONE` mappings -- freed cache
*descriptors*. `umem_cache_arena` (quantum 64) imports pages from
`umem_internal_arena`; when a descriptor's page becomes wholly free it flows
back to the heap and `vmem_mmap_free()` `mmap(PROT_NONE, MAP_FIXED)`s over
it, splitting the RW range. That is P6.2's mechanism at 4 KiB granularity,
on every span free of any kind. It is fixed where P6.2 is fixed;
`test_cache_footprint`'s VMA arm SKIPs with the live number until then and
then stands guard.

(2) **Characterised, left open.** The 40 ms worst-case *create* at 50k caches
is a create landing during the update thread's walk, which holds
`umem_cache_lock` for O(caches). Making the walk lock-free is not a local
change: `umem_cache_lock` is what keeps `cp` alive for the duration of
`func(cp)` against a concurrent `umem_cache_destroy`, for every list walker
-- `umem_cache_update`, `umem_cache_magazine_enable`, the fork handler,
`umem_inspect.c` (3 walks), `umem_introspect.c`. A snapshot-and-walk needs a
per-cache refcount or grace period so a snapshotted `cp` is not destroyed
underneath the walker. That is P1.4/P1.5's lifetime problem again, one level
up. The symptom is a cache-*create* tail, not an allocation stall
(allocation never takes this lock: 0.0 ms worst over 187M allocs at 50k
caches), and it needs 50k caches to reach 40 ms (3 ms at 10k). Recorded as
the honest boundary; a program creating caches per connection at that scale
should pool them.

### P6.3 Thread count: 16,000 threads -- FINE on capacity; HIGH on exit drain and per-thread footprint
`umem_ptc.h:107-123` (`umem_ptc_bin_t` = 128 slots x 8 B, `umem_ptc_t` =
28 bins + 28 magazines = **30.9 KB**, allocated per thread by
`umem_ptc_get()`, `umem_ptc.c:285`); `umem_ptc.c:539-611`
(`umem_ptc_destroy`: 28 x `umem_ptc_bin_flush_all` -> `_umem_cache_free` per
object, plus `umem_ptc_mag_flush_all` -> 56 depot `ml_lock` acquisitions);
`umem.c:5798-5818` (`umem_max_ncpus` -> 256 on 192 CPUs, `umem_cpu_mask`)

Provenance: `a2548b8`, `c7i.metal-48xl` (192 vCPU, `umem_max_ncpus` = 256,
377 GB), `probe_threads <t> 1000`: each thread does 1,000 alloc/free across
8 size classes (so its PTC and per-thread magazines are populated), parks on a
barrier while main samples RSS, then all exit at once while main times its own
allocations. 256 KiB stacks. glibc arm is the same binary against libc.

| threads | umem RSS/thread | glibc RSS/thread | umem exit drain (all threads) | glibc | main's worst alloc during the exit storm: umem / glibc |
|---:|---:|---:|---:|---:|---|
| 1,000 | **62.2 KB** | 20.6 KB | 32 ms (32 us/thr) | 11 ms (10.5 us/thr) | 64 us / 39 us |
| 4,000 | **56.8 KB** | 19.4 KB | **469 ms (117 us/thr)** | 45 ms (11 us/thr) | **914 us** / 56 us |
| 16,000 | **56.5 KB** | 17.5 KB | **6.77 s (423 us/thr)** | 185 ms (11.6 us/thr) | **36.6 ms** / 1.0 ms |

**Fine:** no cap, no wrap, no failure. `max_cpu_seen` reached 191 on every
run, i.e. `sched_getcpu()` returned every CPU and none exceeded
`umem_max_ncpus` (256, the power-of-two round-up of 192); the `CPU(mask)`
index cannot wrap on this box. `nthreads >> ncpus` (16,000 threads on 192
CPUs) does not cap anything. Spawn cost is 4x glibc (1.54 s vs 0.40 s for
16k) but linear.

**Per-thread footprint is 3x glibc and it is the PTC struct.** 56.5 KB per
idle thread against glibc's 17.5 KB (which is mostly the 256 KiB stack's
touched pages plus tcache's 576 B). `umem_ptc_t` is 30.9 KB: 28 bins each
sized for the *maximum* capacity of 128 slots (`PTC_NSLOTS`) even though bins
13-27 use 64 or 32 (`PTC_NSLOTS_MEDIUM`/`_LARGE`), so 20 KB of the 31 is
padding that is never indexed. The rest of the 56 KB is the 8 populated bins'
retained objects (up to 128 x 64 B ... 32 x 2 KB) and the two per-thread
magazines per bin. 16,000 threads x 39 KB of umem-only overhead = 620 MB
resident for idle threads; a 4,000-thread server carries 150 MB. `smaps` of
the parked 4,000-thread process: one 335 MB anonymous mapping holds it all
(the `umem_default` heap), nothing else above 2 MB. MEDIUM-HIGH: it is a
constant, not a cliff, but 3x glibc for idle threads is what a thread-pool
user notices first.

**Exit drain is super-linear and stalls other threads: 32 us/thread at 1k,
117 us at 4k, 423 us at 16k; glibc flat at 11 us.** Total 6.77 s for 16,000
exits, and during it main's `umem_alloc` saw a **36.6 ms** worst (glibc 1.0
ms). `umem_ptc_destroy` returns every retained object one at a time through
`_umem_cache_free` (up to ~600 objects per thread across the 8 populated
bins), each taking the per-CPU `cc_lock` for that cache -- so 16,000 exiting
threads on 192 CPUs contend for 8 caches x 256 `cc_lock`s and the depot
`ml_lock`s behind them, and the contention grows with exiting-thread count.
The yield every 64 objects (`umem_ptc.c:~595`) bounds each hold but not the
queue. glibc's `tcache` free-at-exit is a handful of pointer stores into
per-thread bins that the arena reclaims lazily. HIGH for anything that
creates and destroys threads at scale (per-request threads, thread-pool
resize); the 36 ms stall lands on unrelated allocating threads.

**Required fix.** (1) Size `umem_ptc_bin_t` per bin, not at `PTC_NSLOTS`
max: either a flexible layout with the 28 bins packed at their real
capacities (128/64/32 -> ~11 KB instead of 31 KB), or drop `PTC_NSLOTS` to
64 for all bins and measure the hit rate. Regression: `probe_threads 4000`
umem RSS/thread < 35 KB. (2) Bulk-return at exit: hand each bin's slots to
the depot as a *magazine* (they already are one in shape -- an array of
rounds) instead of `_umem_cache_free` per object, taking `ml_lock` once per
bin instead of `cc_lock` once per object. Regression: `probe_threads 16000`
exit drain < 20 us/thread and main's worst alloc during it < 2 ms.

**STATUS: FIXED (`b8c39e6`, `06559e5`), with two corrections to the entry
above.**

*Correction 1 -- the drain baseline was P8.2.* The 423 us/thread and 36.6 ms
stall were measured at `a2548b8`, before the CPU-hint fix (`ae86536`): every
exiting thread's `_umem_cache_free` hit `cache_cpu[0]`, one `cc_lock` for the
whole process. On 8 vCPU / 4,000 threads, pre- vs post-P6.3 drain is 27 vs
31 us/thread -- inside noise. The one-hand-off-per-bin exit path is kept
because it is exact (regression ledger: hand-offs == bins) and because the
per-object path is O(objects) lock acquisitions by construction; the 16k
metal re-measurement was lost when the agent doing it died (503) and is
recorded as not done.

*Correction 2 -- the bin capacities are not a free variable.* `b8c39e6` also
halved them (64/32/16). Alternating A/B, median of 9, `c7i.2xlarge` t=1:
bench `single` 512 B 5.72 -> 5.31 Mops (-7 %). Four more builds isolated
it: packing at the OLD capacities reproduced 5.31 exactly (so not the
packing); an offset layout did not help; line-separating the 16-byte bin
records recovered `multi` 16:1024 (5.76 -> 5.88) but not `single`. A pure
alloc-N-then-free-N microbench found the shape: **512 B, 157 Mpairs/s at
N=32, 63 at N=33, 5.5 at N=64 -- a 28x cliff at the bin boundary**, and the
pre-fix build has the identical cliff at 64/65. perf: 39 %
`pthread_mutex_trylock` + 34 % unlock. **The per-thread magazine "L2" behind
the bins is never primed** (P8.6 below), so anything past the bin pays up to
`UMEM_DEPOT_STEAL_MAX` failed trylocks then `cc_lock`, every op. Capacities
restored to 128/64/32 (`06559e5`); packing alone gives `sizeof(umem_ptc_t)`
31,616 -> 22,144 B and RSS/thread 55.6 -> 39.3 KB at 4,000 threads (target
was < 35 KB; the remaining 39 is the 22 KB struct plus ~600 cached objects
per thread in the probe, which is the cache doing its job).

*And the residual -7 % on bench `single` is not the allocator.* With
capacities restored it persisted (5.75 vs 5.31), so it was profiled: `perf
diff` puts **+4.21 % in `td_qsort`** -- the bench's own t-digest latency
histogram -- and **-0.54 % in `_umem_alloc`**; libumem's total share fell
5.11 -> 4.84 %, and per-op alloc latency fell p50 35 -> 33 ns, p999 41 -> 36.
The allocator got faster; the histogram got more expensive because the
latency distribution it was fed changed shape. `bench single` at t=1 spends
~70 % of its cycles in `td_*` and the vDSO clock and is **not a valid
allocator microbench**; recorded as a harness limitation (Phase 2 follow-up).
The pure microbench (no per-op clock, no histogram) shows +2 % (153 -> 156
Mpairs/s at 512 B).

Gate PASS both arches at `ede1849`; P1.3a stranded = 0, P1.3c ledger 0/12,
`test_main` clean, ASan clean on the drain path.

### P6.4b Cache count on 192 CPUs: 117 KB per cache, 415 ms creates, 3 s fork -- HIGH
Same mechanisms as P6.4; provenance `a2548b8`, `c7i.metal-48xl`,
`umem_max_ncpus` = 256.

| | 10,000 caches | 50,000 caches | 8-CPU (P6.4, arm) at 50k |
|---|---:|---:|---:|
| create wall | 2.14 s (214 us/cache) | **135.8 s (2.7 ms/cache)** | 36 s |
| worst single create | **45 ms** | **415 ms** | 40 ms |
| RSS per cache | **116.8 KB** | 116.8 KB | 18.6 KB |
| RSS at 50k | -- | **5.7 GB** | 907 MB |
| worst `umem_alloc` over 25 s | 3.1 ms (2 stalls > 1 ms) | 0.1 ms | 0.0 ms |
| create/destroy cycle worst | 90 ms | **430 ms** | 42 ms |
| `fork()` | **605-718 ms** | **2.99-3.03 s** | 80 ms (10k) |
| VMAs after destroying all | 10,431 | **51,803** | 29,159 |
| RSS after destroying all | 371 MB | **1.81 GB** | 131 MB |

Everything `umem_max_ncpus`-proportional scales 32x from 8 to 256 CPUs:
descriptor `UMEM_CACHE_SIZE(256)` = 256 x 128 B `cache_cpu` + header = 33 KB;
two depot arrays of 256 x 64 B = 16 KB each; `cache_rseq` 192 x 64 B = 12
KB; total **~80 KB of the 117 KB is per-CPU state, for a cache with one live
object**. 50,000 caches cost 5.7 GB. Creating one takes 3 x 256 `mutex_init`
+ 3 `mmap`s, and the 415 ms worst is the create waiting on
`umem_cache_lock` while `umem_cache_applyall` walks 50k caches each with 256
`cc_lock`s worth of `umem_depot_ws_update`. `fork()` takes 771 mutexes per
cache (`umem_lockup_cache`: 256 `cc_lock` + 2 + 512 depot + 1) = **38.5M
lock/unlock pairs, 3.0 s, per fork at 50k caches**; at 10k it is 0.6-0.7 s.
And after destroying all 50k, **51,803 VMAs remain (79 % of
`vm.max_map_count`) and 1.8 GB of RSS** -- the same destroy leak as P6.4,
scaled: the three per-cache `munmap`s punch holes in the merged region and
`umem_cache_arena` keeps the descriptor spans.

Allocation itself is still unaffected (0.1 ms worst at 50k). glibc: not
applicable.

**Required fix:** P6.4 (1) and (2), plus: (3) size `cache_cpu[]`,
`cache_depot_*[]` and `cache_rseq[]` by *online* CPUs (192) rather than the
power-of-two round-up (256) -- the mask indexing needs a power of two only
if `CPU(mask)` uses `&`; a modulo or a 192-entry indirection table costs
nothing on the fast path and saves 25 % of per-cache state. (4) Make
`umem_lockup` per-cache cost independent of `ncpus`: mark the cache "forking"
under `cache_lock` and have the per-CPU paths check it, or take only the
depot locks and re-init `cc_lock`s in the child (they are `USYNC_THREAD`
mutexes over per-CPU state the child owns outright). Regression:
`probe_caches 50000 --fork` on 192 CPUs: create worst < 20 ms, fork < 200
ms, VMAs after destroy < 500.

**STATUS.** (1) fixed as P6.4; on 256 slots the carve removes two 16 KB and
one 12 KB page-rounded mapping per cache -- predicted 117 -> ~75 KB/cache,
not re-measured on metal here. (3) and (4) **characterised, not done**:

(3) The power-of-two round-up (192 -> 256) existed because the CPU hint was
an arbitrary integer (`thr_self()` on Solaris, `pthread_self()` here) that
had to be masked into range. Since `ae86536` the hint is a real CPU id in
`[0, ncpus)` on Linux, so 192 slots would suffice **on that path** -- but
eight depot-steal sites (`umem.c:2473-2764`) do `(cpu + i) & (ncpus - 1)`
to walk stripes, `umem_cpu_mask` is used by the log layer, and the
non-Linux/no-rseq fallback hint is still a hash that needs masking. A
modulo on the steal loop is a `div` per step in a spin; an indirection
table is a load. Either is a measured change to the depot's hottest miss
path, and P6.3's agent is concurrently rewriting the PTC/depot hand-off. Not
attempted in this pass; 25 % of per-cache state on wide boxes, nothing on
8-CPU ones.

(4) `fork()` at 3 s for 50k caches is 771 mutexes per cache. Those locks are
what make the child's heap consistent: a parent thread mid-`_umem_cache_alloc`
under `cc_lock` has `cc_loaded`/`cc_rounds` half-updated, and the child has no
such thread to finish it. Skipping `cc_lock`s and re-initialising them in the
child (the plan's option) means the child must also *discard* every per-CPU
magazine's contents -- the objects in them are lost to the child, though not
corrupted. glibc and jemalloc take every arena lock at fork too; they have
8 x ncores arenas, not 50k. The cost here is structural to per-cache per-CPU
layering, and the honest levers are (3) and P6.3's fewer-locks-per-drain,
not a lock-skipping trick. A pre-fork server with 50k caches on 192 CPUs is
a shape this allocator does not serve well; recorded as such.

### P6.5 Fragmentation over time: 18 minutes of repaired `frag` churn -- FINE (plateaus); MEDIUM on the level
`test/bench/bench_framework.c:965-1071` (`frag_worker`, P2.2 live-byte
accounting: `peak_rss / live_bytes_at_peak` sampled together); `umem.c:1903`
(empty slab retained `SLAB_DIRTY` for reclaim rather than destroyed)

Provenance: `a2548b8`, `c7i.2xlarge`, `bench_main -a <umem|libc> -w frag -t
8 -n 4000000 -s 64:4096 -c -A -r <N>` -- 1,800 consecutive umem windows (1,097
s of workload, 18.3 min) and 984 libc windows (1,124 s, 18.7 min), each
window a full fill / free-half / refill cycle over a ~1.9 GB live pool with
a fresh RSS/live pair. Asked for 30 min; each window is ~0.6 s for umem and
the run was sized at 1,800 windows, so it came out at 18. Both arms ran
long enough to show the shape. `frag30` job, `/tmp/frag_{umem,libc}.csv` on
the box (not fetched: 1 MB each; the summaries below are from `awk` over
the full series).

| | umem | glibc |
|---|---:|---:|
| RSS at live-peak, window 0 | 2,221 MB | 1,951 MB |
| RSS at live-peak, window 150 | 2,276 MB | 1,968 MB |
| RSS at live-peak, window 600 | 2,283 MB | 1,953 MB |
| RSS at live-peak, window 1,650 / 900 | 2,287 MB | 1,814 MB |
| max RSS over the run | **2,287 MB** | 2,023 MB |
| RSS drift, first -> last window | **+50 MB (+2.2 %), all of it in the first 150 windows, then +11 MB over the next 1,650** | -- (tracks the live set, 1,814-2,023) |
| frag ratio (RSS / live at peak) | 1.13 -> 1.20 -> 1.29 -> 1.31-1.37 | **1.017-1.018, flat** |
| live bytes at peak | 1,958 -> 1,901 -> 1,772 -> 1,670-1,746 MB | 1,918 -> 1,782-1,934 MB |
| window time | 0.59-0.64 s | 1.12-1.26 s |
| p99 / p999 alloc latency (ns) | 590-670 / 850-970 | **9,500-10,800 / 13,000-14,600** |
| alloc failures | 0 | 0 |

**FINE: RSS plateaus.** umem's RSS at the live-set peak rises 50 MB over the
first 150 windows (90 s) as the slab pool and depot fill out, and then
**+11 MB over the following 1,650 windows (17 min)** -- 0.6 %/17 min, i.e. it
has stopped. There is no unbounded growth. glibc's RSS tracks its live set
down and up (1,814-2,023 MB) because it trims the top of the heap.

**MEDIUM: the level.** umem's fragmentation *ratio* climbs from 1.13 to
~1.33 while RSS is flat, because the **live set shrinks** (1,958 -> 1,670
MB) as the random-half frees leave more holes across the run while RSS
stays where the peak put it: retained empty slabs (`SLAB_DIRTY`, kept for
the reclaim delay) and depot magazines hold the pages. So the ratio's rise
is the denominator, not the numerator. Against glibc at 1.018 flat, umem
holds ~330 MB (17 %) more than its live bytes at the same instant. That is
the cost of the retention policy (`umem_reclaim_delay` 30 s, and see P6.8
for why the reclaim does not fire here either), not a leak. The 2x latency
and 2x window-time advantage over glibc on this workload is the other side
of the same policy.

**Required fix:** none for correctness. P6.8's reclaim fix would let the
ratio settle nearer glibc's on a long-lived process; whether to trade the
latency for it is a policy question, not a defect. Record the 18-minute
plateau as the baseline: a future run whose RSS at window 1,650 exceeds
~2,400 MB on this workload is a regression.

### P6.6 Fork with a 4 GB heap -- FINE
`umem_fork.c:187-228` (`umem_lockup`: walks every cache, `ncpus + 2 * depot
+ 3` mutexes each); `umem_fork.c:230-330` (`umem_do_release`)

Provenance: `a2548b8`, `c7i.2xlarge`, `probe_fork 4 6` (4 GB heap: 524,288 x
4 KiB + 2,048 x 1 MiB, all touched; forks 3-5 with two allocating threads
running), and `probe_caches 10000 --fork` on `c7g.2xlarge`.

| | umem | glibc |
|---|---:|---:|
| heap RSS before fork | 4.04 GB, 77 VMAs | 4.02 GB, 54 VMAs |
| `fork()` latency, quiet parent (#1, #2) | 23.6, 23.4 ms | 21.8, 21.0 ms |
| `fork()` latency, 2 churn threads (#3-5) | 23.6, 23.0, 22.7 ms | 23.6, 21.6, 21.3 ms |
| first fork (page-table warm-up, both) | 36.7 ms | 35.4 ms |
| child RSS at entry -> after 100k allocs + 1 MiB | 4139.5 -> 4139.9 MB (+0.4 MB) | 4116.7 -> 4117.3 MB (+0.6 MB) |
| child work time | 1.3 ms | 1.4 ms |
| `fork()` with 10,000 caches (arm) | 80-83 ms | n/a |

**The atfork handlers cost ~2 ms over glibc at 4 GB** (the difference is
constant, not proportional to heap size: it is the lock walk over the ~40
default caches, not the pages). The kernel's page-table copy is the 21 ms
both share. Under allocation load the handlers do not stall: 22.7-23.6 ms
with two threads allocating, the same as quiet. The child's first
allocations touch 0.4 MB of parent pages (copy-on-write), less than glibc's
0.6 MB. Child-side release works: every child ran to completion and exited
0, six times, with and without concurrent allocators in the parent.

With 10,000 caches the handler walk is visible -- **80 ms, of which ~60 ms
is umem** (each `umem_lockup_cache` takes 8 + 2 + 16 + 1 = 27 mutexes on 8
CPUs; 270k lock/unlock pairs per fork). On 192 CPUs that is 256 + 2 + 512 +
1 = 771 per cache, so 10k caches would be ~7.7M lock operations per fork;
see P6.4b. MEDIUM only for cache-heavy programs that also fork; the
common case is FINE.

**Required fix:** none for the default configuration. For the cache-heavy
case, the same fix as P6.4 (2): `umem_lockup` need not take every per-CPU
`cc_lock` if the child is going to re-initialise them anyway; taking
`cache_lock` + the depot locks and marking the cache "forking" would cut the
per-cache count from `2*ncpus+ncpus+3` to `2*ncpus+3` (still ncpus-bound via
the depot arrays) or, with per-cache depot arrays reduced to one lock, to a
handful.

### P6.7 Kernel knobs: clean NULL+ENOMEM everywhere -- FINE; no recovery after failure -- MEDIUM; `max_map_count`=4096 confirms P6.1 -- BLOCKING
`vmem_mmap.c:108-150` (`vmem_mmap_alloc`, errno preserved per P5.5);
`vmem.c:1047` (`vk_fail`, NULL return); `umem.c:3832` (oversize NULL path)

Provenance: `a2548b8`, `c7g.2xlarge` (16 GB), `probe_rlimit <as|data|none>
<mb>` -- 4 KiB objects until failure, then 1 MiB until failure, every
returned pointer written, then one 64 B allocation to see whether the
allocator still works. Signals trapped and reported. `rlimit`, `knobs` jobs.

| knob | umem: first failure | umem: errno | glibc: first failure | glibc: errno | umem 64 B after failure | glibc |
|---|---:|---|---:|---|---|---|
| `RLIMIT_AS` 1 GB | 987 MB | 12 ENOMEM | 1015 MB | 12 | **NULL (12)** | ok |
| `RLIMIT_AS` 4 GB | 3994 MB | 12 | 4075 MB | 12 | **NULL (12)** | ok |
| `RLIMIT_DATA` 1 GB | 983 MB | 12 | 1019 MB | 12 | **NULL (12)** | ok |
| `RLIMIT_DATA` 4 GB | 4004 MB | 12 | 4079 MB | 12 | **NULL (errno 0)** | ok |
| `overcommit_memory=2`, CommitLimit 7.65 GB | 5239 MB | 12 | 5332 MB | 12 | **NULL (12)** | ok |
| `overcommit_memory=2`, 1000 x 64 B (init) | ok, 4.4 MB | -- | ok, 2.0 MB | -- | -- | -- |
| `max_map_count=4096`, 4 KiB + 1 MiB to 3 GB | none, 107 VMAs | -- | none, 74 | -- | ok | ok |
| **`max_map_count=4096`, 512 B** | **NULL at 1,022,464 objects = 500 MB** | 12 | none at 2 GB | -- | -- | -- |

**FINE:** every hard cap produces a clean `NULL` with `errno = ENOMEM` and
no abort, no signal, no corrupted pointer, on both the slab path and the
oversize path. The P5.5 errno fix holds on all five forcing mechanisms.
umem reaches the cap 30-90 MB earlier than glibc (its reservation
granularity is coarser), which is not a defect. Under strict overcommit
(`mode 2`, where `MAP_NORESERVE` is *not* exempt) libumem initialises and
runs a small heap normally, and fails at the CommitLimit exactly like glibc:
it does not over-reserve at startup.

**MEDIUM: after the first failure, libumem cannot allocate 64 B while glibc
can.** In four of five capped runs the post-failure `umem_alloc(64)` returned
NULL. The heap is at the cap and every cache's magazine layer is empty, so a
64 B request needs a new slab, which needs a new 4 KiB span, which needs a
128 KiB va-arena qcache slab import, which needs `mmap` -- and that is what
is capped. glibc's 64 B comes from a bin inside memory it already has. The
one `errno 0` case (`RLIMIT_DATA` 4 GB) is a stale-errno report on a real
failure -- the failing path there did not set it, so **one errno hole
remains**: probably the `umem_alloc_retry`/`umem_reap` loop returning NULL
after a reap without a fresh syscall. Needs a `RLIMIT_DATA` regression
alongside the `RLIMIT_AS` one in `test_errno_preserved`. A process that
handles ENOMEM by logging and continuing will find that libumem has nothing
left for the log line; glibc typically does.

**BLOCKING (confirms P6.1 under a tighter cap):** with `vm.max_map_count`
lowered to 4,096, 4 KiB and 1 MiB objects reach 3 GB at 107 VMAs -- the
`3f2e67c` fix holds for its class -- but **512 B objects fail at 500 MB**,
1,668 VMAs at 400k objects, 4,096 at ~1M. glibc reaches 2 GB. Same mechanism
as P6.1 (one VMA per 128 KiB qcache slab); the tighter cap just moves the
cliff from 8.2 GB to 500 MB. Containers routinely run with lowered
`max_map_count`.

**Required fix.** (1) P6.1's qcache slab floor removes the `max_map_count`
cliff for 512 B. (2) For post-failure recovery: keep a small emergency
reserve (one qcache slab's worth, 128 KiB-4 MiB) that `umem_alloc` may draw
on only after a backend failure, released back on the next successful
import -- glibc gets this for free from its existing arena; libumem has to
choose to. (3) Find and close the `RLIMIT_DATA` errno-0 path; add
`RLIMIT_DATA` to `test_errno_preserved`.

### P6.8 Reclaim under pressure: a freed 4 GB heap stays resident indefinitely -- BLOCKING
`umem.c:4084-4100` (`umem_cache_reap` -> `umem_depot_ws_reap`, the only
path that returns depot magazines to the slab layer, reached only via
`UMU_REAP`); `umem.c:4743-4813` (`umem_reap`: the only thing that sets
`UMU_REAP`, and only when a *caller* invokes it or a backend allocation
fails); `umem.c:4565-4655` (`umem_cache_update`, the periodic pass: hash
rescale, `umem_depot_ws_update`, magazine resize, `umem_cache_reclaim_pages`
-- **no `UMU_REAP`**); `umem.c:4416-4435` (`umem_cache_reclaim_pages`
touches only slabs with `slab_refcnt == 0`); `umem.c:40-56` (the file header
documents this and calls it a "nuance")

Provenance: `a2548b8`, `c7g.2xlarge` (16 GB). `probe_reclaim <gb> <window>
<mix>`: build the heap (all pages touched), free all of it in one pass, then
sample RSS every second for 100 s while doing one 32 B alloc/free per second
(a live process, not a parked one). `reclaim`, `reclaim_mt2`, `reclaim_gdb5`
jobs.

| run | peak RSS | RSS 1 s after free | RSS at 30 s | at 60 s | at 100 s | glibc at 100 s |
|---|---:|---:|---:|---:|---:|---:|
| 4 GB: 2 GB x 4 KiB + 2 GB x 1 MiB | 4,141 MB | 2,098 MB | 2,098 | 2,098 | **2,098** | 2,062 (same shape: the 4 KiB half stays) |
| 2 GB x 4 KiB, single-threaded | 2,090 MB | 2,095 MB | 2,095 | 2,095 | **2,095** | -- |
| 2 GB x 4 KiB, `umem_reap()` called once before the fill so the update thread exists | 2,090 MB | 2,095 MB | 2,100 | 2,100 | **2,100** | -- |
| 2 GB x 4 KiB, `umem_reap()` called every 10 s during the window | 2,090 MB | 2,095 MB | 2,100 | 2,099 | **2,095** (-5 MB) | -- |

**The oversize half works:** the 1 MiB objects' 2 GB is back with the kernel
within a second of the free (4,141 -> 2,098), because `vmem_xfree` returns
each span to the mmap heap and `vmem_mmap_free` `PROT_NONE`s it (P6.2 shows
the VMA cost of that). glibc does the same via `munmap`.

**The slab half never comes back.** 2 GB of freed 4 KiB objects is still
resident 100 s later with the update thread running (`gdb`: thread 2 in
`umem_update_thread` at `umem_update_thread.c:160`, `umem_update_thr` set,
`umem_reap_interval` 10, `umem_reclaim_delay` 30, `umem_reclaim_enabled` 1,
`umem_update_next` advancing every 10 s). Even calling `umem_reap()` every
10 s -- the file header's documented advice for Linux -- returned 5 MB of
2,095 in 100 s. glibc keeps its 2 GB too (its interior holes are below the
`brk` top and `M_TRIM_THRESHOLD` only trims the top), so **at the 100 s mark
umem and glibc are equal** -- but glibc has no mechanism it is claiming to
run, and libumem does. The reclaim feature (P1.4/P1.5, `umem_reclaim_delay`
30 s, `MADV_DONTNEED` on idle slabs) does not fire on this, the most ordinary
shape of "free a big heap".

**Mechanism, read from the live process with `gdb` 14 s after the free:**

```
cache=umem_alloc_4096 slabsize=65536 buftotal=524288 slab_create=32768 slab_destroy=0 magsize=31
full.ml_total=0  empty.ml_total=0
per-cpu depot: full=16911  empty=0
```

All 524,288 freed objects are in **16,911 full magazines of 31 rounds on
the per-CPU depot lists** (16,911 x 31 = 524,241; the rest are in the CPU
layer's loaded/previous magazines). From the slab layer's point of view every
one of those objects is *allocated*: `slab_refcnt` is nonzero on every one of
the 32,768 slabs. `umem_cache_reclaim_pages()` is running every 10 s exactly
as designed, and it skips every slab (`if (sp->slab_refcnt != 0) continue;`,
`umem.c:4418`), so nothing ever becomes `SLAB_DIRTY`, the 30 s delay never
starts, and `MADV_DONTNEED` is never issued. The pages are not held by the
slab layer's retention policy; they are held by the **depot**, one layer up,
and the only code that drains the depot is `umem_depot_ws_reap`, which is
reached only through `umem_cache_reap`, which is reached only through
`UMU_REAP`, which only `umem_reap()` sets -- and `umem_reap()` is called only
by the application or by a *failed* backend allocation
(`vmem_xalloc` -> `vmem_reap` -> `umem_reap`, `vmem.c:571`). The periodic
update pass does `umem_depot_ws_update` (the working-set *bookkeeping*:
`ml_reaplimit = ml_min; ml_min = ml_total`) but never the reap that acts on
it. **So on a process that never runs out of memory, the depot is never
reaped and freed memory is never returned, and the 30 s reclaim delay is
unreachable for anything that went through a magazine.** The `umem_reap()`
call *does* reach `umem_depot_ws_reap`, but `umem_maglist_ws_reap` reaps
`MIN(ml_reaplimit, ml_min)` magazines per list per pass, i.e. the working-set
minimum of the last interval -- and with `umem_maglist_mark_excess` capping
`ml_min` at `UMEM_DEPOT_PERCPU_MAX` = 8, that is **at most 8 magazines per
per-CPU list per reap**: 8 lists x 8 x 31 x 4 KiB = 8 MB per 10 s. That is
the 5 MB measured. Draining 2 GB at that rate takes ~40 minutes of an
application calling `umem_reap()` every 10 s.

The file header (`umem.c:48-53`) says as much -- "On Linux umem will not
return memory back to the OS until umem fails to allocate a chunk ... your
code will need to call `umem_reap()` periodically" -- and calls it a nuance.
With `reclaim=1` as the default, `umem_reclaim_delay` as a documented
tunable, and P1.4/P1.5 shipped as "background page reclamation", a user
reasonably expects freed pages to go back. They do not, and calling
`umem_reap()` does not materially help either.

**glibc comparison:** for this shape glibc also keeps interior freed pages
(2,062 MB stays), but it (a) `munmap`s the large half at once, same as umem,
(b) trims the top of the heap when the free is at the top -- which a
LIFO-ish free order gets for free -- and (c) never claims otherwise. umem's
failure is not "worse than glibc's number"; it is "the feature that exists
to be better than glibc does not run".

**Required fix.**
(1) Make the periodic pass reap the depot: `umem_cache_update()` should
request `UMU_REAP` (or call `umem_depot_ws_reap` directly) when the depot
holds more than the working set -- the `ml_reaplimit`/`ml_min` bookkeeping
it already maintains is exactly that signal. Then freed magazines return
their objects to the slabs, `slab_refcnt` hits 0, `umem_cache_reclaim_pages`
sees `SLAB_DIRTY` slabs, and the existing 30 s / 60 s machinery does what
it was written to do.
(2) Lift the `UMEM_DEPOT_PERCPU_MAX`-derived 8-magazines-per-pass cap in
`umem_maglist_ws_reap` when the list is far above its working set: reap down
to `ml_min`, not `MIN(reaplimit, min)`, or at least a fraction of the excess
per pass so a 2 GB surplus drains in a few intervals rather than 40 minutes.
(3) Regression: `probe_reclaim 2 100 small` must show RSS below 25 % of
peak by t = 70 s (delay 30 + one interval + slack) with no `umem_reap()`
call from the application; today it shows 100 % at t = 100 s. A second arm
with `reclaim_delay=0` should show the drop within two intervals.
(4) Rewrite the `umem.c:40-56` header: it describes the defect as a
platform nuance, and after (1) it will be false.

**STATUS: FIXED (`147d5ff`, `9bbe58b`) -- and the diagnosis above was one
layer short.** Implementing (1)+(2) as written (`147d5ff`) produced *no
change*: 135 MB stayed 135 MB. `gdb` on the repro: **one task in the
process**; a breakpoint on `umem_cache_update` never fired. The periodic
pass did not merely fail to reap the depot -- **it did not run**, because the
update thread did not exist. Its only creator was `umem_reap()`
(`umem.c:4855-4864`), i.e. an application call or a *failed* backend
allocation. The P6.8 measurement above had a thread only because its 4 GB
fill hit the VMA ceiling (P6.1), failed a backend allocation, and thereby
called `umem_reap()`. The reclaim feature's own tests drive the pass by hand
(`repro_reclaim_reuse.c:85`, `update_pass()`), which is how a never-started
thread stayed unseen through P1.4/P1.5/P1.6. Every feature that documents
itself as "background" or "periodic" -- hash rescale requests, magazine
resize, depot working-set bookkeeping, slab reclaim -- was dead in a process
that never ran out of memory.

`9bbe58b`: `umem_init()` creates the thread after the caches exist and before
`READY`. Isolation on `c7i.2xlarge`, `test_reclaim_returns` (128 MB of
4 KiB, `reap_interval=1,reclaim_delay=2`, no `umem_reap()`):

| build | result |
|---|---|
| pre (`a3aa023`) | FAIL: 135 -> 135 MB in 20 s |
| (1)+(2) only (`147d5ff`) | FAIL: 135 -> 135 MB -- no thread to run them |
| thread-at-init only | FAIL: pass runs, never requests `UMU_REAP` |
| thread-at-init + (1), cap kept | PASS: 8 MB at t = 6 s -- the cap did not bite here, because the `ws_excess` gate fires only once `reaplimit` already reflects the full surplus |
| **all three (`9bbe58b`)** | **PASS: 8 MB at t = 6 s** |

The cap (2) matters for the *other* path: an application calling
`umem_reap()` directly, which was the documented advice. `reclaim=0`,
`umem_reap()` every 1 s: cap removed 135 -> 6 MB in 12 s; cap restored
135 -> 126 MB in 12 s (~3 MB/s -- the agent's 5 MB/100 s at a 10 s
interval). Both arms are now real.

Thread count in a plain `umem_alloc` process: 1 -> 2. `LD_PRELOAD` on
`/bin/ls` and `python3`: works (thread created inside the first `malloc`).
`make check` 34/31/3/0. Follow-up recorded: **fork children still lose the
thread** (`umem_fork.c:238` zeroes `umem_update_thr`; nothing recreates it
until the child calls `umem_reap()` or fails an allocation) -- P6.9.

### P6.9 Fork children have no update thread -- HIGH (follow-up to P6.8)
`umem_fork.c:230-250` (`umem_do_release` as child: `umem_update_thr = 0`,
`umem_reaping = UMEM_REAP_DONE`); no corresponding recreation.

A forked child inherits the heap but not the update thread (threads do not
survive `fork`). The child's periodic maintenance is therefore in exactly the
pre-`9bbe58b` state: dead until `umem_reap()` or an allocation failure. For a
pre-fork server model (nginx-style, one long-lived child per worker) that is
every worker. Not yet measured; the mechanism is read from source.

**Required fix:** recreate the thread in the child's release handler (it can
call `umem_create_update_thread()` after dropping the locks it holds), or
lazily on the child's first `umem_cache_update`-worthy event. Regression: fork
after a fill, free in the child, RSS must return in the child without
`umem_reap()`.

**STATUS: FIXED (`cceae1d`).** `umem_do_release(as_child=1)` recreates the
thread after every allocator lock is released and the interposer/introspection
child hooks have run, guarded on `umem_ready == UMEM_READY` and on the parent
having had one. Regression `test_fork_child_reclaim` (parent initialises,
forks; the child frees 64 MB and watches its own RSS, no `umem_reap()`):
pre `fe0b48f` FAIL (child RSS flat), post PASS, both `c7i.2xlarge`; gate PASS
both arches 36/33/3/0 default, 36/36 introspect; ASan arm PASS.
`LD_PRELOAD` on `python3` with `os.fork()`: child has 2 tasks, allocates, exits 0.

*Aside, recorded so nobody chases it:* under `--enable-asan` on aarch64 LSan
reports one 56-byte direct leak from `umem_init -> umem_stacktrace_init ->
backtrace_warm` (libgcc's one-time `dl_iterate_phdr` state). Identical at
`3b170bc`, before any of this; unrelated to the update thread.

## Phase 7 — Hardening properties: what is actually established

**Origin:** the open items README listed after v3.1.0, plus a re-review of
Phase 5. Position codes as in §7a of `AGENTS.md`. Two agents dispatched on
this phase died on provider content filters; the coordinator did it directly.

| Item | State | Evidence |
|---|---|---|
| P7.1 P5.4 mangling not independently tested | **CLOSED** | `inslab` case: FAIL with `-DUMEM_NO_LINK_MANGLE`, PASS default (`c8d83bd`) |
| P7.2 `UMEM_OPTIONS=abort` documented but nonexistent | **FIXED** | `test_abort_option.sh`: pre rc=0, post SIGABRT (`1c604df`) |
| P7.3 P1.3c ledger false positive | **FIXED** | stale-tail split 0/254/127; drain-disabled 635/1016/635 (`64ca9af`) |
| P7.4 `umem_may_own()` convex hull | **OPEN, characterised** | below |
| P7.5 leading-component symlinks | **OPEN, by design** | below |
| P7.6 update thread never started | **FIXED** (found via P6.8) | `9bbe58b` |

### P7.1 -- see P5.4 and `docs/results/2026-09-23-p54-which-control-blocks.md`.

### P7.2 The documented escape hatch did not exist
`malloc_interpose.c:611` (old comment), `README.md:337`, `envvar.c`

The interposer clears `umem_abort` so foreign pointers under `LD_PRELOAD` are
logged rather than fatal, and both the source comment and the README told users
`UMEM_OPTIONS=abort=1` restores the abort. No option table had an `abort`
entry; only `noabort` existed, an `ITEM_CLEARFLAG` in the `UMEM_DEBUG` table
(a different variable), and `abort=1` would have been rejected by the flag
parser for carrying a value. So a preloaded program had no way back to
glibc-like abort-on-invalid-free, and the user who set the documented option
believed they had one. Position D: the difference between a forged free that
kills the process and one that logs to an in-memory buffer nobody reads
(`umem_output` is 0 by default, so `umem_err_recoverable` writes nothing to
stderr).

Fixed: `abort` is an `ITEM_FLAG` in `umem_options_items`, secure-**safe**
(arming can only turn continued execution into a crash). The first attempt put
it next to `noabort` in the `UMEM_DEBUG` table and the regression caught it:
`UMEM_OPTIONS=abort` parsed nothing, arm still exited 0. Regression
`test/security/test_abort_option.sh`: control arm (default) refuses and
completes; test arm (`UMEM_OPTIONS=abort`, same forged free) dies with
SIGABRT. Pre-fix `d8a0984`: FAIL; post `1c604df`: PASS.

### P7.3 -- see the `umem_ptc_probe_shell_free` comment in `umem.c` and the
CHANGELOG entry. Recorded here because it is a hardening-*evidence* defect:
an exact oracle that fires on a non-defect is worse than a statistical one,
because it is believed.

### P7.4 `umem_may_own()` is a convex hull -- characterised, left open
`malloc.c:475-500` (`hull_refresh`, `umem_may_own`); `malloc.c:578,698`
(`process_free` call sites)

**What it does.** `[umem_heap_lo, umem_heap_hi)` is the min/max over
`vmem_heap`'s spans, refreshed on a miss. A pointer strictly between two spans
-- a gap the kernel gave to someone else -- passes.

**What that buys an attacker (position D, the only one where this matters).**
They need writable memory *inside the hull*, *outside every span*, whose
address they can pass to `free()`. With ASLR and the heap reserved in large
`PROT_NONE` ranges, the gaps are the kernel's choice, not theirs; the realistic
case is a large `mmap` of their own that landed in a gap, which requires the
heap to have grown around it. Given that, they forge a `MALLOC_MAGIC` header
(the magic is a fixed constant) and a plausible size, and `process_free`
accepts it and calls `_umem_free(base, size)`. For `size <= UMEM_MAXBUF` that
is `umem_cache_free(cp, buf)` -> PTC bin push -- **no validation on that
path** -- and the attacker's address is later returned by `malloc()`. That is
a chosen-pointer return, the primitive P5.4 also defends against, reached
without touching a slab.

**Why it is not closed here.** The exact check is `vmem_contains(vmem_heap,
addr)` -- a segment-hash lookup under `vm_lock` -- on **every** `free()`,
because the hull passes the in-between case, so a check only on hull misses
does not help. That is a lock on the interposer's free path, which P8.1 just
took ~390x back from. The alternative, validating at the slab layer
(`umem_slab_free`'s `UMEM_SLAB(cp, buf)` reads `sp->slab_cache` from a page the
*pointer* names, so a forged page with `slab_cache = cp` passes), is the same
class of problem.

**What glibc does.** Nothing: `free()` reads the chunk header unconditionally
and applies consistency checks; for the main arena there is no range check at
all. jemalloc's `rtree` lookup **is** an exact ownership check (it is how
jemalloc finds the extent), and scudo's checksummed header is an
unforgeable one. So: strictly more than glibc, less than jemalloc/scudo.

**What would close it.** Either a header that cannot be forged (a per-process
secret folded into `malloc_stat`, the approach scudo takes -- this changes the
on-heap format and the introspection tools that read it) or an exact
ownership structure cheap enough for the free path (a radix over span bases,
which is what jemalloc's `rtree` is). Both are real projects; neither is a
line. Recorded as the honest boundary of the interposer's hardening:
**a forged header inside the hull but outside every span is accepted.**

### P7.5 Leading-component symlinks -- by design, stated
`umem_open_write()` (`misc.c`); callers in `umem_profile.c`, `umem_inspect.c`

`umem_open_write()` opens the **final** component with `O_NOFOLLOW|O_EXCL`
semantics and verifies it is a regular file with one link owned by the
effective uid. It does not resolve the directory path component by component,
so `/tmp/attacker-symlink/out.log` follows the symlink into whatever
directory it names and creates `out.log` there.

This is the same boundary glibc's `MALLOC_TRACE`/`mtrace` and every
"write a file at a user-supplied path" facility has, and the same one
`O_NOFOLLOW` itself has by specification. The complete fix is `openat()`
walking each component with `O_NOFOLLOW|O_DIRECTORY`, or refusing paths
whose directories are writable by others. In secure mode these options are
already **ignored** (P5.2), which is the case where a hostile path matters
(position A/C). For an unprivileged process writing where its own
environment says, the process owner controls both the path and the
symlink. Left as documented behaviour.

### P7.6 -- see P6.8. Recorded here because it is a hardening property too:
every background self-check, rescale and reclaim documented as "periodic" was
not running in any process that had not yet failed an allocation.

### Property inventory vs. alternatives

What is actually enforced, on which path, and how it is tested. "Tested"
means a regression that fails when the control is removed.

| Property | libumem | glibc | jemalloc | scudo |
|---|---|---|---|---|
| Header outside heap rejected before read | hull (`umem_may_own`), tested | no | rtree, exact | checksum, exact |
| Header inside heap gap rejected | **no** (P7.4) | no | yes | yes |
| Freelist link integrity (free chunk) | mangle + align + containment, each tested (P5.4, P7.1) | safe-linking (mangle only) | n/a (bitmap) | n/a |
| Double free detected | buftag (`UMEM_DEBUG`) only | tcache key, default | opt | quarantine, default |
| Env tunables ignored when setuid/AT_SECURE | yes, side-effect options, tested (P5.2) | yes | yes | yes |
| Abort on detected corruption | default on; interposer off; `abort` re-arms, tested (P7.2) | on | on | on |
| Writer file paths: symlink final component | refused, tested (P5.3) | n/a | n/a | n/a |
| Writer file paths: symlink in directory | followed (P7.5) | n/a | n/a | n/a |
| Control socket path predictable/shared | euid-private dir, `lstat`, rename reclaim, tested (P5.6) | n/a | n/a | n/a |
| Stack-walk bounds | `pthread_getattr_np`, tested (P5.9) | n/a | n/a | n/a |
| `PATH`-resolving exec in library | none, tested (P5.1) | none | none | none |

## Phase 8 — Performance gaps

**Origin:** the 2026-09-23 allocator comparison,
`docs/results/2026-09-23-allocator-comparison.md`. Nine allocators (glibc,
libumem through its API, libumem through its `LD_PRELOAD` interposer,
jemalloc, tcmalloc, mimalloc, snmalloc, scudo, rpmalloc), four boxes
(`c7i.2xlarge`, `c7g.2xlarge`, `c7i.metal-48xl`, `c8g.metal-48xl`), fixed
total work per point, two replicate processes per arm alternating at the
innermost loop, and **a null control at every grid point** (libumem against a
relabelled copy of itself). Only gaps that clear that null are listed. Sha
`f2a8267` (allocator sources identical to `v3.1.0`); the P8.1 fix is
`a74065e`, measured separately.

**The distinction this phase rests on.** The comparison measured two libumem
arms and they are different things:

| arm | `multi` 16:64, 192 threads, `c7i.metal-48xl` | vs glibc (431 Mops) |
|---|---|---|
| `umem_alloc` API (what `-lumem` callers get) | **394 Mops** | -9 % |
| `LD_PRELOAD=libumem_malloc.so` at `f2a8267` (what drop-in users got) | **0.82 Mops** | -99.8 % |
| `LD_PRELOAD=libumem_malloc.so` at `a74065e` | **314 Mops** | -27 % |

Every earlier comparison in this repository measured only the first row.

Design costs that are **accepted, not tasks** (category (a) in the results
doc): the 1.2-1.6x RSS/live-set ratio at 64 B and up and 2.6x at 16-63 B
against glibc's 1.0-1.9x (size-class rounding plus warm object caches and
magazines -- scudo, the other size-class allocator with a header, lands at
the same 2.6x; §5.5 of the results doc); the 8-byte `malloc_data_t` header
on the interposer path (it is how `free()` learns the size; glibc pays the
same). Known-open items measured but owned elsewhere: the ~5 GB heap ceiling
(P6.1; the probe here reproduced it at 4.45-4.55 GB with 6.2-6.9M
`alloc_failures` before `3f2e67c` landed).

**Status as of 2026-09-23**

| Item | Gap (measured, null-controlled) | State |
|---|---|---|
| P8.1 interposer global mutex on every `free()` | preload 0.8 Mops vs API 394 at 192 t (500x); negative thread scaling on all 4 boxes | **FIXED** `a74065e`; A/B 0.79 -> 314 Mops (396x) at 192 t, null +/-3.5 % |
| P8.2 API path collapses at 1k:4k under threads | 0.06x glibc at 64+ t on both metals, -78..-96 % vs best, p999 32-68 us; deterministic (umem@null identical) | **FIXED** `ae86536`: CPU hint was `pthread_self() & mask == 0` -- one `cc_lock` per process; 1.4 -> 16.4 Mops at t=8 |
| P8.2b 1k:4k cliff at t>=128 on both metals (hidden by P8.2) | at `d6f04ab`: x86 105 -> 91 Mops t=64 -> 128 (libc 165 -> 253); arm 277 -> 77 (libc 255 -> 200); null falls with it; umem p999 11 us vs libc 0.4 | **FIXED** `ad72787` (PTC bins through 8192 B; `eb68575` 63-round magazines is inside the null): `c8g.metal` t=64/128/192 275/128/179 -> 314/352/429 Mops, 0.42x -> 1.24x libc at t=128, p999 2.8 us -> 39 ns; `c7i.2xlarge` t=8 0.69x -> 1.22x. x86 metal not re-measured (no capacity) |
| P8.6 PTC per-thread magazines never primed | 28x cliff at the bin boundary, single thread (157 -> 5.5 Mpairs/s at N=64 -> 65 for 512 B); 39 % trylock + 34 % unlock | **FIXED** `a2177b9`: one magazine allocated on the first free-side miss; cliff 26.9x -> 1.16x (x86), 26.3x -> 1.05x (arm); depot trylocks per 200 rounds 25,600 -> 0; inside the bin within null |
| P8.3 interposer per-call residual after P8.1 | preload/API 0.69-0.81 at every thread count, flat | open; re-measured at `d6f04ab`: 0.74-0.91, flat; profile attributes it |
| P8.4 API `multi` 16:64 8-24 % behind best at 128-192 t on x86_64 metal | -20 %/-24 % at t=128/192 (null sd 8.5 %); inside null on aarch64 | **CLOSED** by `ae86536` (P8.2's fix): at `d6f04ab` x86 t=192 umem 528.8 = null 527.8 > libc 486.8 -- PTC misses had also gone to `cache_cpu[0]` |
| P8.5 `frag` 20-42 % behind size-class allocators at t=1..8; **8-19x behind sustained at 192 threads, p999 6-10 ms** | all four boxes; sustained frag 16:64 at 192 t: umem 2.0 / 1.2 Mops vs 15-24 for every other allocator incl. glibc; perf: 13 % of cycles in depot mutex trylock/unlock | **PARTIAL** `0532c38`: at t=8 the cost was steal SCANS locking empty stripes (98.6 % of 17M pops), not steals (dep_remote/dep_local 0.11); unlocked head check -> dep_conten 434k -> 0, sustained 0.57x -> 0.72x (x86) / 0.77x (arm) glibc; p999 19 us unchanged (slab layer, plan's (1)); t=192 not re-measured (no metal) |

### P8.1 The `LD_PRELOAD` interposer took a global mutex on every `free()` -- FIXED

`malloc_interpose.c:321-343` (`is_libc_pointer`: `libc_ptr_lock` + 512-slot
scan, unconditional), `:393-427` (`interpose_owner_of`: that scan, then
`process_free(ptr, 0, ...)` to classify), `:636-683` (`free`: classify, then
`umem_malloc_free` -> `process_free(ptr, 1, ...)` decoding the same header
again). At `f2a8267`.

**Measured** (`c7i.metal-48xl`, `multi` 16:64, 20M ops, 2 replicates,
alternating with 10 other arms): 3.03 / 2.02 / 1.61 / 1.06 / 0.89 / **0.82**
Mops at 1 / 8 / 32 / 64 / 128 / 192 threads. Same build's API arm: 6.70 /
37.8 / 140.7 / 181.5 / 277.7 / **393.6**. glibc 431.5 at 192. Preload-arm
null control at these points sd 3-5 %. The same shape on `c8g.metal-48xl`
(1.15 vs 487.5), `c7i.2xlarge` (1.5 vs 31.6 at t=8) and `c7g.2xlarge` (1.5
vs 50.6). The table only ever holds bootstrap-phase `memalign` pointers and is
empty for the whole steady-state life of every process; the lock was taken to
search nothing.

**Fixed in `a74065e`** (malloc_interpose.c only): an atomic live-entry count
gates the scan; `free()` goes to `umem_malloc_free()` in one pass once READY.
Regression `test/stress/repro_interpose_free_scaling` (1- vs 8-thread
aggregate throughput: pre 0.29x, post 4.10x; gated in `make check`).

**A/B that closes it** (`scripts/ec2/interpose_ab.sh`, `c7i.metal-48xl`,
pre `5513c81` vs post `a74065e` vs an independent rebuild of pre as the null,
3 alternating pairs per point, `docs/results/2026-09-23-interpose-ab/`):
null t=192 median -0.5 % (-3.5..+1.7); pre -> post 2.0x / 14.9x / 72x / 145x
/ 262x / **396x** at 1 / 8 / 32 / 64 / 128 / 192 threads; prodcons 64:256
t=192 p999 2.73 ms -> 50 us. Zero `ops_floor_raised`, zero `alloc_failures`.

### P8.2 The `umem_alloc` path collapses at 1k:4k object sizes under threads

`umem_ptc.c:46` (`umem_ptc_maxsize = 2048`), `:85-93` (`ptc_size_classes`
ends at 2048 with two zero pads), `:199-213` (`umem_ptc_bin_table[idx] = -1`
above `umem_ptc_maxsize`); `umem.c:3699-3813` (`_umem_alloc`: `bin < 0`
falls straight to `_umem_cache_alloc`), `:3157-3235` (`_umem_cache_alloc`:
rseq fast path serves zero hits, then `cc_lock`), `:549-558` (`umem_magtype`:
chunks 2560-4096 get **31-round** magazines, 5120 gets 15; the PTC-covered
classes get 63-127).

**Measured** (API arm, `multi` 1024:4096, all four boxes; the wrapper's
16-byte header maps a 1024..4095 request onto caches 1280..5120, and ~2/3 of
the range lands above 2048):

| box | t | glibc | umem | best | umem/glibc | umem p999 ns | null here |
|---|---|---|---|---|---|---|---|
| `c7i.2xlarge` | 8 | 23.5 | 6.0 | 30.9 jemalloc | 0.26 | 10,173 | +1.3 % |
| `c7g.2xlarge` | 8 | 29.6 | 8.6 | 39.6 jemalloc | 0.29 | 5,992 | +3.4 % |
| `c7i.metal-48xl` | 8 | 28.5 | 5.8 | 38.6 rpmalloc | 0.20 | 9,413 | -1.0 % |
| `c7i.metal-48xl` | 64 | 146 | 9.0 | 204 tcmalloc | **0.06** | 50,824 | +0.1 % |
| `c7i.metal-48xl` | 192 | 326 | 23.0 | 458 tcmalloc | 0.07 | 49,154 | -8.0 % |
| `c8g.metal-48xl` | 64 | 215 | 14.9 | 309 mimalloc | 0.07 | 42,548 | +0.0 % |
| `c8g.metal-48xl` | 192 | 337 | 33.7 | 412 snmalloc | 0.10 | 41,682 | -11.0 % |

t=1 is fine (0.97-1.10x glibc). The umem@null arm reproduces the collapse
to within 1 % (5.98 vs 5.94 Mops at `c7i.metal` t=8): deterministic.
`prodcons` and `frag` at 1k:4k do **not** show it (the object survives long
enough to amortise the depot round trip).

**Mechanism.** Above 2048 B the PTC is bypassed by construction, every
operation takes `cc_lock` on the per-CPU magazine layer, and on `multi` each
thread draws a different random size class per iteration, so one CPU cache's
loaded/previous pair is churned across 4-5 classes with 31-round magazines.
A 31-round magazine drains in 31 allocations; each drain is a blocking
`umem_depot_alloc()` round trip. The 8-vCPU contention dump for the
neighbouring 1024/1280 classes shows the shape (`dep_remote` 26,177 vs
`dep_local` 39 -- nearly every reload steals cross-CPU). Competitors' thread
caches extend to 32 KB (tcmalloc, jemalloc) or are the whole allocator.

**Diagnose.** Run `multi` 1024:4096 t=8 with `UMEM_OPTIONS=ptc_maxsize=8192`
(the knob exists, `envvar.c:245`). If the cliff moves to 4k:16k the mechanism
is proven in one run. Then `bench_contention -w multi -s 2048:4096` to
confirm `cc_alloc` and `full_reload` carry the traffic.

**Fix as proposed above.** (1) Extend `ptc_size_classes` through 8192;
(2) raise `mt_magsize` for the 2048..8192 band; (3) regression at t=8.

**STATUS: FIXED (`20999ee`, `ae86536`) -- and the mechanism above was
wrong.** The one-run diagnostic proposed above was run first, and it did not
move the cliff: `tcache_max=8192` took 1k:4k from 10.9 to 10.5 Mops at t=8
on `c7i.2xlarge`. So the PTC-bypass story was at best incomplete. Per-class
at t=8: 1536 B (PTC-served) 31.8 Mops, 2560 B 1.4, 4096 B 1.5 -- and
`bench_contention` on the 2560 class showed `cc_alloc` = 999,992 with
**zero** depot reloads. Not the depot, not magazine size: pure `cc_lock`
contention. Counting per-CPU caches with `cc_alloc != 0` after 8 threads:
**1** (occasionally 2).

**The real mechanism** (`umem.c:641`, `umem_impl.h` `get_cached_cpu_hint`).
On Solaris `CPUHINT()` is `thr_self()`, a small integer thread id, so
`hint & cache_cpu_mask` spreads threads over `cache_cpu[]`. This port
defined it as `pthread_self()` cast to `int`: a page-aligned stack address
whose low bits are always zero. `hint & mask == 0` for **every thread**, and
the value was cached in TLS once, forever (the comment claimed it was "reset
on magazine reload to detect CPU migration"; nothing reset it). The rseq
`cpu_id` that the function preferred was never consulted for the cached
value: a thread's first `_umem_cache_alloc` computed `CPU_CACHED(mask)` on
its first line, *before* the rseq block below registered the thread. So
every operation reaching the magazine layer -- every size above
`tcache_max` and every PTC miss below it -- serialised on one `cc_lock` for
the whole process. A port bug older than every phase of this plan; the PTC
had been hiding it for every size it covers.

**Fix.** `get_cached_cpu_hint()` on its one miss registers rseq if the caller
has not and takes the kernel `cpu_id`; else `sched_getcpu()`; else a
thread-id hash of the bits that vary (`>> 12`). `CPU()` (the log path) goes
through the same function. A first version re-read the rseq `cpu_id` on
every call to track migration and cost 5 % at t=1 (4.16 -> 3.94 Mops,
2560 B, median of 7, alternating builds); reverted to read-once-and-cache
(4.06, within noise of 4.16). Spread is the property; post-migration
exactness is not.

**Evidence** (`c7i.2xlarge`, `test_cpu_hint_spread`: 8 threads, 2560 B,
count of `cache_cpu[]` slots with `cc_alloc != 0`):

| build | slots used of 8 | 2560 B t=8 | 4096 B t=8 | 1k:4k t=8 | 16:64 t=8 |
|---|---|---|---|---|---|
| pre `a7bcdc4` | **1, 1, 1** | 1.4 | 1.5 | 10.9 | 27.4 |
| post `ae86536` | **8, 8, 8** (also 8 with `rseq=0`) | 16.4 | 17.2 | 17.4 | 25.8 |
| glibc same box | -- | 22.8 | 21.0 | 22.8 | 26.8 |

12x on the affected classes at t=8; the (3) target of >= 0.8x glibc at t=8
is met at 0.72-0.82x. The metal numbers (0.06x glibc at 64-192 t) are not
re-measured here; the mechanism predicts they scale with the fix, and that
is a claim for the next comparison run, not this entry. The original (1) and
(2) -- PTC classes through 8 KB, larger magazines for 2-8 KB -- remain as
possible *further* gains; they are no longer the fix. Gate PASS both arches,
default / `--enable-introspect` / `--disable-rseq`.

### P8.6 The per-thread magazine layer is never primed

`umem.c:3782-3868` (`_umem_alloc`: bin empty -> `mag->rounds`,
`mag->prounds`, then `umem_depot_alloc_trylock(cp, &cp->cache_full)`, then
fall to `_umem_cache_alloc`); `:3994-4068` (`_umem_free`: bin full -> mags
-> `umem_depot_alloc_trylock(cp, &cp->cache_empty)`, then fall to
`_umem_cache_free`); `umem.c` `umem_depot_alloc_trylock` (local stripe +
`UMEM_DEPOT_STEAL_MAX` neighbours + global, all trylock, all fail on a
list that has never been populated).

**Measured** (`c7i.2xlarge`, one thread, 512 B, alloc N then free N,
Mpairs/s, median of 5): N=32 157.8, N=48 158.0, N=63 145.7, **N=64 145.9,
N=65 84.3, N=96 8.0, N=128 5.5**. A 28x cliff at the PTC bin's capacity.
perf at N=64 (with the halved bins): 39 % `pthread_mutex_trylock`, 34 %
`pthread_mutex_unlock`, 7.6 % `umem_depot_pop_trylock` -- the allocator is
spending its time failing to lock empty lists.

**Mechanism.** The PTC's magazines take from the depot by trylock only and
never allocate a magazine. The CPU layer publishes an *empty* magazine to
the depot only when its own loaded magazine drains -- which a steady
alloc/free workload never does -- and a *full* one only when its loaded
magazine fills, likewise. So on a cache that has only ever seen one thread
cycling objects, the depot lists stay empty forever, and every op past the
bin does 1 + min(ncpus, `UMEM_DEPOT_STEAL_MAX`) + 1 trylock/unlock pairs
that all fail, then takes `cc_lock` and does the work there anyway. This is
the same class as "rseq magazines are never populated" (README, open since
v2.7.0): a fast layer that exists in the code and is never fed.

**Fix.** Prime on first miss: when both PTC magazines are NULL and the depot
trylock finds nothing, allocate one empty magazine from `mt_cache` (the CPU
layer does exactly this at `umem.c` `_umem_cache_free` "no empty magazines
in the depot, so try to allocate a new one") and adopt it as `mag->loaded`.
One allocation per thread per size class, ever. Then the free side fills
it, the alloc side drains it, and the depot round trip happens once per
`magsize` objects instead of the bin's worth. Regression: the microbench
above must not have a cliff between N=64 and N=128 (ratio < 2x, today
26x). Interaction: the P1.3b/P1.3c invariants (capacity from the magazine,
nothing discarded with objects in it) already govern `umem_ptc_mag_return`;
the new magazine goes through the same function. Not done in this pass --
it is a fast-path change and needs its own A/B with the null control.

**STATUS: FIXED (`a2177b9`, out-of-line in `a1901c8`).**  On the free side,
when both per-thread magazines are full (or absent) and the depot's empty
list has nothing, `umem_ptc_mag_prime()` allocates one magazine from
`cp->cache_magtype->mt_cache`, exactly as `_umem_cache_free` does for the
CPU layer, and the caller adopts it with `umem_mag_capacity()` (P1.3b).  No
magtype-changed retry: a stale shell is drained and freed by
`umem_ptc_mag_return*` when it is handed back (P1.3c).  The alloc side is
unchanged -- when both magazines are empty and the depot has no full one,
there is nothing to prime with, and falling to `_umem_cache_alloc` is
correct.

**Regression** `test/integration/test_ptc_mag_primed` (in `make check`,
probe build): one thread, alloc `2*cap` then free `2*cap` for 200 rounds at
512 B (bin 64) and 64 B (bin 128), counting `umem_depot_alloc_trylock()`
calls from the PTC paths via a new `UMEM_PTC_RESIZE_PROBE` ledger
(`umem_ptc_probe_depot_trylocks`).  Bound `2 * rounds + 8 = 408`.

| build | 512 B attempts | 64 B attempts | result |
|---|---|---|---|
| parent `356354e` (test only), both arches | **25,600** (= one per object past the bin) | **51,200** | FAIL |
| fix `a2177b9`, both arches | **0** | **0** | PASS |

Zero, not "a handful": once the L2 holds a magazine the workload never
fills it (2*cap <= magsize), so the depot is never consulted again.

**The cliff** (`bench_pairs`, one thread, alloc N then free N, Mpairs/s,
`verify-isolated` at each sha):

| box | build | 512 B N=64 | N=65 | N=96 | N=128 | N=256 | N=64/N=128 | 64 B N=128 | N=256 |
|---|---|---|---|---|---|---|---|---|---|
| `c7i.2xlarge` | `356354e` | 160.5 | 93.0 | 8.8 | **5.97** | 4.0 | **26.9x** | 156.6 | 5.98 |
| `c7i.2xlarge` | `a2177b9` | 160.0 | 158.2 | 146.1 | **137.4** | 133.1 | **1.16x** | 159.9 | 135.6 |
| `c7g.2xlarge` | `356354e` | 102.3 | 57.3 | 5.7 | **3.89** | 2.6 | **26.3x** | 104.3 | 3.89 |
| `c7g.2xlarge` | `a2177b9` | 102.2 | 99.6 | 98.7 | **97.6** | 95.2 | **1.05x** | 104.7 | 100.0 |

**A/B with the null control** (`scripts/ec2/hotpath_ab.sh`: three arms
from `git archive` -- pre `356354e`, post `a1901c8`, and an independent
rebuild of pre as the null -- 9 alternating pairs per point, median delta,
ONE `bench_pairs` binary for every arm; instructions per pair from `perf
stat` at t=1):

| point | x86 null | x86 A/B | arm null | arm A/B | insn/pair x86 | insn/pair arm |
|---|---|---|---|---|---|---|
| 512 B N=1 t=1 | +2.2 % (-4.4..+5.8) | +6.1 % (+1.2..+8.4) | +0.0 % | -0.2 % | +0.0 % | +0.0 % |
| 512 B N=64 t=1 (bin edge) | +0.4 % | -0.2 % | +0.0 % | -0.2 % | +0.1 % | +0.0 % |
| 512 B N=128 t=1 | +0.3 % | **+2234 %** | +0.0 % | **+2401 %** | **-90.2 %** | **-90.0 %** |
| 16:64 N=1 t=1 | +0.1 % | -3.0 % (-4.7..-0.4) | -0.1 % | +0.5 % | +0.1 % | +0.0 % |
| 16:64 N=64 t=1 | -0.1 % | -0.2 % | -0.0 % | +0.4 % | +0.0 % | +0.0 % |
| 16:1024 N=1 t=1 | -0.2 % | -1.9 % (-2.7..-0.9) | +0.0 % | +0.2 % | +0.0 % | +0.0 % |
| 16:1024 N=64 t=1 | -0.2 % | +1.7 % | +0.0 % | -0.3 % | -0.1 % | +0.0 % |
| 512 B N=1 t=8 | -0.1 % | -0.1 % | -0.5 % | -0.1 % | | |
| 512 B N=128 t=8 | -19 % (-98..+182) | +141 % (+15..+524) | +92 % (-70..+243) | +286 % (+94..+290) | | |
| 16:64 N=1 t=8 | -0.4 % | +0.1 % | -0.1 % | +0.6 % | | |
| 16:64 N=64 t=8 | -0.2 % | +0.0 % | +0.0 % | +0.5 % | | |
| 16:1024 N=1 t=8 | +0.0 % | -0.5 % | -0.0 % | +0.2 % | | |
| 16:1024 N=64 t=8 | +0.4 % | -0.0 % | +0.0 % | -0.0 % | | |

Inside the bin (N <= 64) and at every 16:64 / 16:1024 point the change is
within the null on arm and within 3 % on x86 with instruction counts
identical to 0.1 %; 16:64 t=8 -- the brief's no-regression point -- is
+0.0 % / +0.5 %.  Past the bin the L2 now serves what the depot scan used
to fail at: 10x fewer instructions per pair.  The t=8 N=128 point is
bimodal in BOTH arms (the null's own spread is -98..+182 %): with eight
threads cycling 128 objects each the pre-fix depot lists are sometimes fed
by another thread's CPU magazine draining and sometimes not, and the
post-fix figure depends on how many threads primed before the 1 s window.
It is reported, not claimed.

**What the first A/B got wrong, and how it was found.**  The first two
runs of the rig built `bench_pairs` per arm and reported 512 B N=1 t=1 at
**-8.9 %** and **-7.7 %** (null +0.3 %) with instructions per pair
unchanged to 0.1 %.  Two layout variants (`cold`, `aligned(64)`) and a
padding control (pre + a dead copy of the helper) did not explain it -- the
padding control was inside the null.  Cross-pairing library x bench binary
did: same library, bench from pre vs bench from post, **-13.1 % / -14.9 %**;
same bench, library pre vs post, **+7.8 % / +3.2 %**.  A printf change in
`bench_pairs.c` between the two refs had moved the bench's own `worker` by
0x20 bytes.  The rig now uses one bench binary (from post) for every arm
and logs its sha256 (`aa8c2ba`); the comment that had attributed the loss
to inlining was corrected (`71422fe`).  The A/B above is the corrected run.

**Oracles** at `a2177b9`, both arches: `test_ptc_thread_exit_drain_probe`
stranded 0; `test_ptc_resize_no_loss_probe` x12 all PASS, objects lost 0;
`test_cpu_hint_spread` 8/8; `test_ptc_footprint` PASS.  Gate runs recorded
at the end of Phase 8.

### P8.2b The 1k:4k class has a second cliff at 128+ threads, which P8.2 was hiding

`umem.c` `_umem_alloc`/`_umem_free` (sizes above `tcache_max` bypass the PTC
entirely); `_umem_cache_alloc` (`cc_lock`, then a **blocking**
`umem_depot_alloc()` on magazine exhaustion); `umem_magtype` (2560-4096 B
chunks: 31-round magazines).

**Measured** at `d6f04ab` (2026-09-24 comparison, `multi` 1024:4096, API,
Mops/s, median over replicates; before = `f2a8267`):

| box | t | before | after | null after | libc after | best after |
|---|---:|---:|---:|---:|---:|---:|
| `c7i.metal` | 64 | 9.4 | **105.2** | 87.4 | 164.6 | 207.1 |
| `c7i.metal` | 128 | 12.4 | **90.6** | 69.0 | 252.5 | 326.3 |
| `c7i.metal` | 192 | 27.7 | 113.1 | 83.3 | 344.2 | 494.8 |
| `c8g.metal` | 64 | 15.3 | **277.3** | 277.3 | 254.7 | 328.0 |
| `c8g.metal` | 128 | 26.8 | **77.3** | 75.4 | 200.3 | 416.4 |
| `c8g.metal` | 192 | 36.3 | 273.3 | 281.9 | 390.1 | 535.8 |

Up to t=64 P8.2 is fixed (umem within 15 % of glibc). From t=64 to t=128
throughput **falls** on both metals while every other arm keeps scaling; the
null falls with it, so it is deterministic and in the allocator. Latency
shape at arm t=128: umem p50 156 ns, p99 4,454, p999 10,959; libc p50 55,
p999 372; jemalloc p999 42.

**Mechanism.** These sizes never touch the PTC, so every op is `cc_lock`, and
every 31 ops per CPU is a blocking depot round trip -- with 128 threads now
correctly spread over 128 per-CPU slots (the very thing `ae86536` fixed), 128
CPUs each take that trip into a stripe that is empty because the objects were
freed elsewhere, and the cross-stripe steal is under `ml_lock`. At 64 CPUs
the depot keeps up; at 128 it convoys. This was the *second half* of the
original P8.2 diagnosis; the CPU-hint bug made it unobservable because
everything was on one lock anyway. The x86 lo box shows the same shape one
step down (t=8: umem 16.1, null 22.3, libc 23.2, `unstable` on both umem
replicates, p999 1-2 us vs libc 0.16).

**Fix** is the original P8.2 (1) and (2), now with a live pre-fix number:
extend PTC classes through 8 KB so 2.5-8 KB objects have per-thread bins and
magazines (and fix P8.6 so those magazines actually work), and raise
`mt_magsize` for the 2048-8192 band from 31 to 63. **Regression:** `multi`
1024:4096 at t=128 on metal must be >= its t=64 figure (monotone) and
>= 0.6x libc; today 0.36x (x86) / 0.39x (arm). The contention-dump phase of
`allocator_comparison.sh` should include 1024:4096 so the next run has the
depot counters at this point (a rig gap: `CONTENTION_SIZES` covers three
size ranges and not this one).

**STATUS: FIXED on the lo boxes (`ad72787` part a, `eb68575` part b);
metal below.**  Two commits, measured separately.

(a) `ad72787`: `PTC_NBINS` 28 -> 36, a fourth tier `PTC_NSLOTS_XLARGE` of
16 slots for 2560..8192 B, `umem_ptc_maxsize` default 2048 -> 8192.
`size_to_bin_table` was `PTC_NBINS * 32` = 896 entries indexed by
`size / 8`; 8192 / 8 = 1024 would have overrun it, so it is now sized to
the largest class.  `sizeof (umem_ptc_t)` 22,144 -> 24,000 B, under
`test_ptc_footprint`'s 24 KB (24,576).  `test_cpu_hint_spread` moves its
PTC-bypassing size from 2560 to 10240 and `test_fork_mt_load` its cc_lock
classes from 8192 to 12288; `umem_ptc_test`'s class table gains the eight
classes and passes (720,654 / 2,839,870 checks on x86 / arm).  With P8.6
fixed these bins have a working L2.

(b) `eb68575`: `umem_magtype` rows {15, 4096-8192}, {31, 2048-4096},
{63, 1024-2048} become one {63, 1024-8192} row (two rows with the same
magsize would create a duplicate `umem_magazine_63` cache at init).

**Lo-box regression** (`multi` 1024:4096 t=8, `matrix.sh`, 10M ops, 5
runs, 3 alternating replicates per arm; `verify-isolated` at each sha;
median Mops and p999 ns):

| box | build | libc | umem | umem@null | umem/libc | umem p999 | libc p999 | unstable |
|---|---|---|---|---|---|---|---|---|
| `c7i.2xlarge` | pre `71422fe` | 23.9 | **16.6** | 20.6 | **0.69** | **921** | 146 | 3/3 + 3/3 |
| `c7i.2xlarge` | (a) `ad72787` | 23.8 | **29.3** | 29.2 | **1.23** | **152** | 152 | 0/3 + 0/3 |
| `c7i.2xlarge` | (a)+(b) `eb68575` | 24.6 | **29.9** | 29.5 | **1.22** | **130** | 141 | 0/3 + 0/3 |
| `c7g.2xlarge` | pre `71422fe` | 29.7 | 31.1 | 31.2 | 1.05 | 107 | 112 | 0/3 + 1/3 |
| `c7g.2xlarge` | (a) `ad72787` | 30.2 | **39.3** | 38.8 | **1.30** | **41** | 103 | 0/3 |
| `c7g.2xlarge` | (a)+(b) `eb68575` | 30.1 | **39.4** | 39.0 | **1.31** | **42** | 101 | 0/3 |

x86: 0.69x -> 1.23x libc, p999 921 -> 152 ns, `unstable` gone; target
was >= 0.8x and p999 < 500 ns.  arm had no t=8 deficit before and gains
27 % anyway.  (b) on top of (a) is inside the null at t=8 on both boxes,
as expected: with the PTC covering the band, the magazine layer sees only
PTC overflow, and (b)'s effect is on the depot trip rate at scale.

**Per-class bare loop** (`bench_pairs`, N=1, Mpairs/s; the PTC boundary
was between 1536 and 2560):

| box | build | t | 1536 | 2560 | 3072 | 4096 | 5120 | 8192 |
|---|---|---|---|---|---|---|---|---|
| `c7i.2xlarge` | pre | 8 | 602 | **22.9** | 66.3 | 62.2 | 23.0 | 30.4 |
| `c7i.2xlarge` | (a) | 8 | 589 | **590** | 590 | 590 | 590 | 577 |
| `c7i.2xlarge` | (a)+(b) | 8 | 619 | 609 | 616 | 614 | 616 | 604 |
| `c7i.2xlarge` | pre | 1 | 154 | **22.8** | | 22.9 | | 22.9 |
| `c7i.2xlarge` | (a)+(b) | 1 | 159 | **159** | | 159 | | 159 |
| `c7g.2xlarge` | pre | 8 | 800 | **37.3** | 15.6 | 36.7 | 34.6 | 89.5 |
| `c7g.2xlarge` | (a)+(b) | 8 | 800 | **800** | 805 | 806 | 801 | 799 |
| `c7g.2xlarge` | pre | 1 | 100 | **15.2** | | 15.2 | | 15.1 |
| `c7g.2xlarge` | (a)+(b) | 1 | 100 | **99.8** | | 101 | | 101 |

Above the old ceiling the per-class cost was 6.6x (x86) / 6.6x (arm) that
of the class just below it at t=1 -- `cc_lock` plus the rseq registration
check on every op -- and 10-26x at t=8; the band is now level with 1536 B.
`bench_pairs` 1040:4112 (the bench's post-header range) t=8 N=1: 59.9 ->
478 (x86), 150 -> 577 (arm).

**Oracles at `ad72787` and `eb68575`, both arches**:
`test_ptc_mag_primed` PASS, `test_ptc_thread_exit_drain_probe` stranded 0,
`test_ptc_resize_no_loss_probe` x12 rc 0, `test_cpu_hint_spread` 8/8 (at
10240 B), `test_ptc_footprint` 24,000 <= 24,576 PASS.

**Metal** (`c8g.metal-48xl`, 192 vCPU; `c7i.metal-48xl` had no capacity in
any us-east-2 AZ for the hour this ran, so the x86 metal row is not
re-measured here).  `multi` 1024:4096, 60M ops, 3 runs, 3 alternating
replicates per arm, `verify-isolated` at each sha, same box, one after the
other; median Mops and p999 ns:

| build | t | libc | umem | umem@null | umem/libc | umem p999 | libc p999 | unstable (umem / null) |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| pre `71422fe` | 64 | 249.7 | 275.4 | 276.0 | 1.10 | 86 | 80 | 0/3, 0/3 |
| pre `71422fe` | 128 | 304.0 | **127.5** | **140.0** | **0.42** | **2,773** | 89 | 2/3, 3/3 |
| pre `71422fe` | 192 | 361.6 | 179.0 | 280.4 | 0.49 | 1,337 | 363 | 2/3, 3/3 |
| (a) `ad72787` | 64 | 248.8 | **313.6** | 315.7 | 1.26 | **39** | 82 | 0/3, 0/3 |
| (a) `ad72787` | 128 | 285.3 | **352.4** | 355.5 | **1.24** | **39** | 117 | 1/3, 1/3 |
| (a) `ad72787` | 192 | 368.0 | **428.9** | 437.6 | 1.17 | **39** | 291 | 0/3, 1/3 |
| (a)+(b) `eb68575` | 64 | 250.8 | 314.1 | 314.9 | 1.25 | 39 | 82 | 0/3, 0/3 |
| (a)+(b) `eb68575` | 128 | 317.7 | **359.0** | 346.4 | **1.13** | **39** | 84 | 2/3, 1/3 |
| (a)+(b) `eb68575` | 192 | 351.0 | **438.6** | 420.4 | 1.25 | 39 | 344 | 0/3, 0/3 |

The pre row reproduces the 2026-09-24 comparison's cliff at HEAD-of-then
(277 -> 77 there; 275 -> 128 here, the null 276 -> 140, p999 2.8 us):
the same shape, deterministic, in the allocator.  With (a) the curve is
monotone -- 314 -> 352 -> 429 -- at 1.17-1.26x libc at every point, and
p999 is 39 ns at all three thread counts against libc's 82-344.  The
targets (t=128 >= t=64; >= 0.6x libc) are met with room.  (b) on top of
(a) is inside the null at every point: once these sizes are PTC-served,
the magazine layer sees only PTC overflow, and a 31- vs 63-round magazine
behind a 16-slot bin is not where the time goes at this thread count.  (b)
is kept because it is what the Solaris table would have said for a 16-
object slab and costs nothing here, but it is not what fixed P8.2b -- (a)
is, and (a) works because P8.6 gave those bins a working L2.

`multi` 16:64 at t=192 (the no-regression point for the PTC table growth):
pre umem 521.4 / null 538.7 / libc 576.9; (a) 530.5 / 578.7 / 555.6;
(a)+(b) 541.7 / 548.7 / 558.2.  Inside the null's own spread (-20..+40
Mops between replicates of the same binary) at every sha.

The `bench_contention` dump for this point (`CONTENTION_SIZES` gap noted
above) was attempted in the same job and produced no rows -- the tool
needs `LD_LIBRARY_PATH=.libs` and the job did not set it; recorded here so
the next comparison run does not repeat it.  The throughput and latency
rows above are the regression's evidence; the depot counters at this
point remain to be captured.

**STATUS: FIXED (`ad72787`).**  Exit criterion 2 of Phase 8 for this item
(pre-fix demonstration, fix, post-fix A/B with null on a metal box) is
met on `c8g.metal-48xl`; `c7i.metal-48xl` awaits capacity.

### P8.3 Interposer per-call overhead after P8.1

`malloc_interpose.c` `free()` fast path at `a74065e` (`is_static_pointer`,
`is_bootstrap_pointer` via PLT into `libumem.so`, `is_libc_pointer`, then
`umem_malloc_free`); `malloc.c:736-754` (`umem_malloc_free` calls
`is_bootstrap_pointer` **again**), `:535-731` (`process_free`:
`__errno_location()`, `umem_may_own()` twice, header decode, poison store).

**Measured** (post-fix, same build, API vs preload alternating, 3 pairs per
point, `c7i.metal-48xl`, `multi` 16:64): preload/API = 0.814 / 0.694 / 0.741
/ 0.782 / 0.810 / 0.811 at 1 / 8 / 32 / 64 / 128 / 192 threads, i.e. a flat
**19-31 % per-operation cost with no thread dependence**. `perf` at t=192
(`docs/results/2026-09-23-interpose-ab/perf-post-umem-preload-t192.flat.txt`):
`is_bootstrap_pointer` 5.9 %, `process_free` 4.4 %, `_umem_free` 2.6 %,
`umem_malloc` 2.0 %, `umem_may_own` 1.7 %, `free` 1.7 %, `is_libc_pointer`
0.7 %, `umem_malloc_free` 0.7 % -- versus the API arm's `_umem_alloc` +
`_umem_free` + wrapper at ~6 % total. `hull_refresh`/`vmem_walk` appear in no
profile at any thread count: there is no second lock.

**Fix.** (1) Call `is_bootstrap_pointer` once: `umem_malloc_free` is reached
from the interposer only after the interposer has already checked, so give
the interposer an entry that skips it (or inline the check -- it is one load
and one compare, but it is a load of `buf[-1]` on an arbitrary pointer, which
is the read P5.8 exists to avoid; the hull test should come first and the
bootstrap magic test only on a hull miss). (2) Inline `umem_may_own`'s hull
hit (two compares against two atomics) into `process_free`; keep the
out-of-line refresh for the miss. (3) Drop the `__errno_location()` save /
restore on the success path -- `process_free` sets `errno` only on the
failure paths, so read it lazily there. Target: preload/API >= 0.95 at t=1
and t=192 under the P8.1 A/B protocol; regression is that protocol's
reference table.

### P8.4 API `multi` 16:64 is 8-24 % behind the best allocator at 128-192 threads on x86_64 metal -- DIAGNOSIS

`c7i.metal-48xl`, `multi` 16:64, 20M ops: umem 37.8 / 140.7 / 181.5 /
277.7 / 393.6 Mops at 8 / 32 / 64 / 128 / 192 threads vs mimalloc 42.8 /
164.3 / 210.0 / 348.3 / 504.9 and glibc 38.4 / 128.7 / 177.7 / 298.5 /
431.5: -12 / -14 / -14 / **-20 / -24 %** vs best. Null at these points -5.5 /
+9.5 / +0.3 / +8.0 / +6.1 %, pooled sd 8.5 %, so only t=128 and t=192 clear
the band on their own; the sign is the same at all five thread counts and at
64:256 / 256:1024 (-5..-18 %). On `c8g.metal-48xl` the same gap is 3-6 % at
every thread count -- inside the null -- so it is x86_64-specific or
implementation-on-x86-specific, not a design property.

**Mechanism not established.** The `multi` contention dumps show zero depot
traffic and zero `cc_alloc` at these points: every operation is a PTC hit,
so this is per-operation cost on the hit path itself. The available profile
(`perf-post-umem-t192.flat.txt`) puts `_umem_alloc` + `_umem_free` at
4.8 %, but the benchmark's own t-digest (`td_qsort`, `td_add`,
`td_compress`) is 30+ % of cycles and drowns the signal.

**Diagnose** (this is the task; no fix is proposed until it lands): (1)
`perf stat -e instructions,cycles,L1-dcache-load-misses` for umem vs mimalloc
vs glibc on this exact point with `bench_main`'s histogram disabled (add a
`-q` that skips `td_add`; the framework has no such switch today). (2)
`perf annotate` of `_umem_alloc`'s PTC block (`umem.c:3699-3730`): the
candidates are the `umem_introspect_break_armed` load on every alloc, the
`thread_ptc` TLS access, and the `ptc_bin_capacity()` call on every free
(`umem.c:3911`). (3) The same on `c8g.metal-48xl` where the gap is absent,
to see which counter differs.

**STATUS: CLOSED (`ae86536`, measured at `d6f04ab`).** Not by a P8.4 fix but
by P8.2's: a PTC *miss* (bin full or empty) at these sizes also went to
`cache_cpu[0]`, and at 128-192 threads that was a measurable fraction of
operations. 2026-09-24 comparison, `multi` 16:64 on `c7i.metal-48xl`: t=128
umem 357.7 = libc 356.6 (best); t=192 umem 528.8, null 527.8, libc 486.8,
best 583.4 mimalloc (umem/best 0.91, inside the run's null spread). Same on
`c8g.metal`: t=192 602.1 vs null 560.6 vs best 612.3. 64:256 and 256:1024 at
t=192 gained 28-36 % identically.
`docs/results/2026-09-24-allocator-comparison.md` §2.

### P8.5 `frag` is 20-33 % behind the size-class allocators at 16..1024 B, and 2-3x behind under sustained load

`umem.c:1686-1745` (`umem_slab_alloc`: `cache_lock`, one object per
acquisition), `:1839-1926` (`umem_slab_free`: same lock), `:2435-2500`
(`umem_depot_alloc_trylock`: bounded stripe scan), `umem_ptc.h:46-48`
(PTC bin capacity 128 / 64 / 32), `umem.c:3328` (`umem_cache_alloc_batch`,
exists and is unused by the PTC refill path).

**Measured** (`frag`, all four boxes, null +/-3-11 %): umem 4.2 / 3.9 / 3.4
Mops at t=1 for 16:64 / 64:256 / 256:1024 vs the best 6.1 / 5.7 / 4.9
(`c7i.2xlarge`), -27..-42 % at t=1 on metal; -20..-36 % at t=2..8. At 1k:4k
the gap is 8-15 %, at the band's edge; above t=32 on metal every allocator
converges (the benchmark's `memset` saturates). Sustained `frag` 16:64,
matched work, 4 x 20 s windows:

| box | threads | umem Mops | field | umem p999 | field p999 |
|---|---|---|---|---|---|
| `c7i.2xlarge` | 8 | 8.7 | 19-22 (glibc 19.3) | 22 us | 0.4-8 us |
| `c7g.2xlarge` | 8 | 7.7 | 19-25 (glibc 21.5) | 22 us | 0.3-5 us |
| `c7i.metal-48xl` | 192 | **2.0** | 14-16 (glibc 15.4) | **6.0 ms** | 25 us-0.7 ms |
| `c8g.metal-48xl` | 192 | **1.2** | 22-24 (glibc 23.0) | **9.9 ms** | 0.3 us-0.2 ms |

umem@null reproduces every row to within 5 %. At 192 threads this is the
worst point in the whole comparison: 8-19x behind everything, glibc
included, and the window ran 18-31 s where the field took 1.6-2.4 s. umem
beats glibc on the *matrix* `frag` (1.2-2.3x) because glibc's `free()`
consolidation has a 10-16 us tail, but that is not the comparison that
matters.

**Mechanism.** The workload holds `budget/4` live objects per thread (5M in
the matrix, 11M sustained) and frees a random half each round, so half of
each round's allocations cannot come from anything recently freed and must
reach the slab layer, one object per `cache_lock` acquisition, and half the
frees are of objects the PTC/magazine layers never saw. The freed objects
land on the *freeing* CPU's depot stripe and are wanted next by whichever
CPU allocates, so at N threads the depot is an N-way all-to-all exchange
through per-stripe mutexes. Evidence at both scales:

- 8 threads (`contention-umem-frag-t8-16_64.txt`): `dep_conten` (depot
  trylock failures) 45,736-58,261 per size class against `dep_local`
  ~41,000 -- **more than half of all depot attempts fail the trylock** --
  and `cc_alloc` 500-1,100 magazine-layer allocations that bypassed the PTC.
- 192 threads (`c7i.metal`, `contention-umem-frag-t192-64_256.txt`):
  `dep_remote` 4,800-13,900 vs `dep_local` 2,000-5,000 per class -- **2-3
  of every 4 reloads steal from another CPU's stripe** -- and `dep_conten`
  2,000-6,100. `perf` on the same point
  (`perf-umem-frag-t192-64_256.flat.txt`): **`pthread_mutex_trylock` 10.0 %
  + `pthread_mutex_unlock` 2.9 % of all cycles**, callers `umem_depot_alloc`
  / `umem_depot_pop_trylock`, plus 1.2 % in the kernel's `osq_lock` under
  `mmap` (slab creation contending on `mmap_lock`). glibc's profile on the
  same point has no user-space lock at all (its 20 % `osq_lock` is the same
  `mmap_lock`, and it still finishes 8x sooner).

jemalloc/mimalloc/snmalloc/rpmalloc return a freed object to the *owning*
thread's page or segment (a lock-free push), not to the freeing CPU's
stripe, so their remote-free cost is O(1) with no lock and no scan.

**Diagnose.** `perf record` umem `frag` 16:64 at t=1 (no contention) and
t=8, split between `umem_slab_alloc`/`umem_slab_free` (lock + list), `umem_slab_create`, and the PTC miss path. The metal `perf-umem-frag-*`
captures from the comparison run are the first cut.

**Fix.** (1) Batch the slab layer: `umem_slab_alloc` hands out one object per
`cache_lock`; when a PTC magazine needs refilling and the depot is empty, take
the lock once and fill the whole magazine from the slab freelist
(`umem_cache_alloc_batch` is the shape, `umem.c:3328`). Same on the free
side for a full PTC magazine whose depot push fails. (2) The all-to-all
stripe traffic is the structural cost: consider returning a full magazine
to the stripe of the CPU that *allocated* its objects (the slab knows; a
magazine does not, but `umem_slab_t`/`bufctl` are per-object and the first
round's slab is a good-enough hint), or a per-cache global full list that
`umem_depot_alloc_trylock` tries **before** the bounded 8-stripe steal scan
(`UMEM_DEPOT_STEAL_MAX`, `umem.c:576`) so a hot cache's full magazines are
found in O(1) rather than after 8 failed trylocks. (3) Measure first: the
`perf` capture says 13 % of cycles are in the mutexes themselves, which
bounds what (1)/(2) can recover to ~15 %; the remaining 8x must then be
lock *wait*, which `perf` attributes to the caller -- take a `perf record
-e sched:sched_switch` or `off-cpu` profile of the same point before
choosing between (1) and (2). (4) Regression: sustained `frag` 16:64 at 8
threads >= 0.8x glibc and p999 < 5 us on `c7i.2xlarge` (today 0.45x and
22 us); at 192 threads on metal >= 0.5x glibc (today 0.13x).

**STATUS: PARTIAL FIX (`0532c38`); the diagnosis above was wrong for the
8-thread case, and the 192-thread case is not re-measured.**

**Re-measured first**, as the brief required, after P8.6 (`a2177b9`) and
P8.2b (`ad72787`, `eb68575`) landed -- either could have moved this, since
PTC misses are what reach the depot.  Neither did.  frag 16:64 t=8,
`verify-isolated`, `c7i.2xlarge` / `c7g.2xlarge`:

| build | matrix frag umem / null / libc (Mops) | sustained frag umem / libc (Mops, 4 x 20 s) | umem p999 / libc p999 (us) | dep_local | dep_remote | dep_conten | conten per reload | perf trylock + unlock |
|---|---|---|---|---:|---:|---:|---:|---|
| `356354e` x86 | 22.1 / 21.6 / 19.8 | **10.4** / 20.4 | 19.2 / 2.2 | 75,322 | 17,140 | 437,045 | 4.7 | 22 % + 8 % |
| `eb68575` x86 | 22.1 / 22.1 / 20.7 | **11.6** / 20.5 | 18.9 / 2.1 | 71,790 | 8,058 | 433,667 | 5.4 | 17 % + 7 % |
| `356354e` arm | 21.6 / 22.4 / 20.7 | **10.7** / 20.3 | 14.1 / 1.6 | | | | | |
| `eb68575` arm | 20.6 / 24.4 / 21.9 | **12.7** / 22.4 | 15.1 / 1.6 | | | | | |

The matrix frag point (20M ops, no sustained live set) has umem level with
or ahead of glibc on both boxes, as the 2026-09-24 report said.  The
sustained run is the deficit: 0.51x / 0.48x glibc, p999 9x / 9x worse.

**What the contention dump says, against what the entry above says.**
The mechanism written above -- and confirmed at HEAD in the 2026-09-24
report from the t=192 metal dump -- is cross-stripe *stealing*: "95 % of
depot reloads steal from another CPU's stripe".  At t=8 on 8 CPUs that is
not what the counters show: `dep_remote / dep_local` is 0.11-0.23, so
nine of ten successful reloads are LOCAL.  What is large is `dep_conten`,
434k against 80k successful reloads: 5.4 times per reload, a thread's
blocking `umem_depot_pop()` on its OWN stripe found the lock held and
slept.  Who holds it?  A `perf` with DWARF unwinding (frame pointers are
off in the release build) attributes 15 % of all cycles to
`pthread_mutex_trylock` called from `umem_depot_alloc_trylock` (8.2 %)
and `umem_depot_alloc` (5.1 % via `umem_depot_pop_trylock`, 1.7 % via
`umem_depot_pop`) -- the steal SCANS, not the steals.  Every alloc-side
miss trylocked all 8 stripes on the PTC path and then all 8 again on the
blocking path, and both pop primitives took the lock BEFORE looking at
the list.  On frag the live set grows faster than frees return magazines,
so nearly every stripe is empty nearly all the time: the scan was 16
lock/unlock pairs per miss to read 16 NULLs, and eight threads doing that
concurrently held each other's local stripe locks.  The probe build's
`test_depot_empty_scan` counts it: **98.6 % (x86) / 98.7 % (arm) of
17-19M depot pops acquired a stripe lock and read an empty list.**

**Fix `0532c38`.**  `umem_depot_pop()` and `umem_depot_pop_trylock()` read
`ml_list` unlocked first and return NULL if it is NULL.  The read is racy
and that is the right semantics: a magazine pushed a moment later is found
on the next call, which is exactly the outcome of a failed trylock.  Two
lines in the two primitives every depot path goes through; no new list, no
new hashing.  The plan's (1) batch-from-slab and (2) overflow list /
steal-by-freeing-CPU were not taken: (2) addresses stealing, which the t=8
counters say is not the cost, and (1) is a different mechanism (slab-layer
lock rate) that the post-fix profile below now exposes as the next one.

**After** (`0532c38`, same protocol):

| box | sustained frag umem / libc (Mops) | ratio | umem p999 / libc p999 (us) | dep_local | dep_remote | dep_conten | locked-empty pops (probe) | perf trylock + unlock |
|---|---|---:|---|---:|---:|---:|---|---|
| x86 before (`eb68575`) | 11.6 / 20.5 | 0.57 | 18.9 / 2.1 | 71,790 | 8,058 | 433,667 | 17,114,748 of 17,360,206 | 17 % + 7 % |
| x86 after | **14.9** / 20.6 | **0.72** | 19.1 / 2.1 | 76,114 | 2,844 | **0** | **19** of 26,749,400 | **0 % + 1.2 %** |
| arm before (`eb68575`) | 12.7 / 22.4 | 0.57 | 15.1 / 1.6 | | | | 19,268,300 of 19,513,852 | |
| arm after | **18.0** / 23.3 | **0.77** | 13.0 / 1.4 | | | | **5,661** of 24,100,671 | |

+28 % (x86) and +42 % (arm) sustained; `dep_conten` 434k -> 0;
`pthread_mutex_trylock` gone from the profile.  The matrix frag point is
inside the null (x86 22.4 vs null 22.1; arm 24.0 vs null 21.5, null
spread 21.0-24.8).  **The p999 did not move**: 19 us / 13 us against
libc's 2.1 / 1.4.  The post-fix profile says where the rest is: `frag_worker`
24 % + `td_qsort`/`td_add`/`td_compress` 14 % + `umem_free_wrapper` 15 %
are the bench's own memset, histogram and header check (the same 45-50 %
overhead the team brief warns of at t=1); of what is left, kernel
`native_queued_spin_lock_slowpath` 5.6 % (futex wake under `mmap_lock`,
slab creation) and `umem_slab_alloc` 3.2 % + `pthread_mutex_lock` 2.1 %
(one object per `cache_lock` acquisition) are the allocator.  That is the
plan's (1), and it is the tail: a slab-create under `mmap` is the 19 us.
Target (4) -- >= 0.8x glibc and p999 < 5 us at t=8 -- is not met: 0.72x /
0.77x and the tail unchanged.

**Not done.**  The t=192 metal re-measurement (the 95 % cross-stripe
figure, sustained 3x behind) -- `c7i.metal-48xl` had no capacity and
`c8g.metal` was used for P8.2b and terminated per the brief; the brief
said not to launch metal for this alone.  The fix here removes lock
traffic that the t=192 profile (59 % in `pthread_mutex_trylock`) also
shows, but whether stealing or scanning dominates there is a claim for the
next metal run, with the contention dump beside it.  (1), the slab-layer
batch, is the remaining lo-box mechanism and is not attempted in this
pass.

### Phase 8 exit criteria

1. P8.1 closed (it is). The README's Performance section states the
   interposer-vs-API distinction with both numbers.
2. P8.2 and P8.5 each have a pre-fix demonstration (the rows above), a fix,
   and a post-fix A/B with the null control on at least one metal box.
3. P8.3's target met, or the residual re-measured and accepted with its
   number in the README.
4. P8.4 has a mechanism or is closed as "inside noise" with the counters
   that show it.
5. No new number in the README without a null control beside it.

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

### Resolved since the first gate run

- **`prop_fragmentation`'s abort is fixed**, and the gate no longer exempts
  anything. It needed three fixes, each a separate defect: `vmem_populate()`
  aborted on an unsupported `VM_SLEEP` instead of reporting it; the test itself
  passed `VM_SLEEP` (11 sites across two files, against a flag `vmem.c`'s own
  header documents as unsupported); and it freed every allocation with
  `umem_free(ptr, 0)` under a comment claiming the size was tracked internally.
  The last one only became reachable once the `QCC_getValue()` fix made the
  property execute at all. All property tests now pass on both architectures.

### Known-open at the gate, measured on every run rather than hidden

- **The ~5 GB Linux heap ceiling** (`vm.max_map_count`). Still open, and an
  attempt to fix it **failed** — three hypotheses, none of them the cause,
  all reverted (`553d42e`). The failure narrowed it usefully: `strace` shows
  64,275 `mprotect` calls, 64,270 of them exactly 4096 bytes, for 60,000
  allocations. The slab layer takes one page-sized span per 4 KiB object from
  `umem_va_arena`, so the mapping is split **upstream of the mmap backend** and
  no change there can help. The fix belongs in span sizing (the va-arena's
  quantum/`qcache_max`, or batching spans for small size classes). Two
  independently-proven fixes were kept from the attempt: `errno` is no longer
  erased over a real `mmap()` failure (`errno=0 Success` became `errno=12
  ENOMEM`), and unsupported-flag misuse is reported rather than aborted.
  `test/integration/test_heap_ceiling` measures it every run and reports SKIP
  with live numbers; flip its two `rc = 77` returns to `rc = 1` when span sizing
  is fixed. This is what still blocks calling libumem production-ready for
  general use, and it leads the README's limitations.
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

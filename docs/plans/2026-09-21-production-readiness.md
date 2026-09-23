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

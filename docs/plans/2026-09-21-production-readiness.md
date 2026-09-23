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

### P6.1 Object count: 100M live 64 B objects -- FINE on overhead, HIGH on VMAs and stalls
`umem.c:1540-1620` (`umem_slab_create`, one span per 64 KiB slab via
`umem_va_arena`); `vmem.c:1555-1567` (the va-arena qcaches);
`umem.c:4218-4262` (`umem_hash_rescale`)

**Tested to 100M objects, both architectures. No failure, no cliff in
per-object overhead.** Provenance: `a2548b8`, `c7i.2xlarge` and
`c7g.2xlarge`, `probe_objcount 100000000 64`.

| | umem x86 | umem arm | glibc x86 | glibc arm |
|---|---:|---:|---:|---:|
| RSS at 100M | 7368 MB | 7350 MB | 8395 MB | 8394 MB |
| bytes/object (incl. driver ptr) | 69.2 | 69.0 | 80.0 | 80.0 |
| alloc phase | 47.4 s (444 ns mean) | 63.4 s (601 ns) | 9.0 s (61 ns) | 9.5 s (62 ns) |
| worst single alloc | **94.9 ms** | **41.2 ms** | 0.25 ms | 1.6 ms |
| allocs > 1 ms | 19 | 21 | 0 | 1 |
| VMAs at 100M | **48,218** | **48,215** | 54 | 74 |
| free phase | 25.1 s, worst 42 ms | 34.5 s, worst 15 ms | 5.8 s | 6.7 s |
| RSS after freeing all | **9022 MB (+1.65 GB)** | **9005 MB (+1.65 GB)** | 8395 MB (flat) | 8394 MB |
| stall while parked 25 s (update thread ran 2x) | 0.1 ms | 3.8 ms | 0.0 ms | 0.2 ms |

What is fine: 64 B objects cost 61 B each (69.2 minus the 8 B driver pointer),
**less than glibc's 72**, flat from 20M to 100M. Nothing on the update
thread's periodic walk stalls the allocation path at this scale: the parked
probe, which spans two `umem_cache_update` passes over a 7 GB heap, saw at
worst 0.1 ms on x86 (3.8 ms once on arm). `umem_hash_rescale` is not
involved -- a 64 B cache is not `UMF_HASH`, so it has no hash table; the
objects live in 4 KiB single-page slabs with embedded bufctls.

Three things are not fine:

1. **VMA growth: 1 VMA per ~2,074 objects, 48,218 at 100M, 74 % of the
   default `vm.max_map_count` (65,530).** At this rate the process fails at
   ~136M live 64 B objects (~8.4 GB), exactly the class of cliff `3f2e67c`
   removed for 4 KiB objects. Mechanism, read from `/proc/pid/maps` on the
   live process: 1,470 `rw-p` VMAs of **132 KiB each**, adjacent, not merged.
   `umem_va_arena` is created (`umem.c:5466`) with `qcache_max = 8 *
   pagesize`, so 32 KiB-and-under span requests from the slab layer are
   served from the va-arena's qcaches, whose slabs are 128 KiB
   (`vmem_create` -> `umem_cache_create(..., UMC_QCACHE)`, `bestfit =
   MAX(1 << highbit(3 * qcache_max), 64)`). Each 128 KiB qcache slab is one
   `heap_alloc` -> `vmem_mmap_alloc` -> `mmap(MAP_FIXED, RW)` over the
   PROT_NONE reservation, plus a 4 KiB `umem_slab_t`-holding page, giving the
   132 KiB stride. Adjacent RW anonymous mappings created by separate
   `MAP_FIXED` `mmap()` calls over a `MAP_NORESERVE` reservation **do not
   merge** on this kernel (6.x, Amazon Linux 2023) -- the `anon_vma` differs
   per call once pages are touched. `test/integration/vma_merge_probe.c`
   (another agent's uncommitted probe) measures the same thing. glibc has 54
   VMAs because it grows one `brk` heap and a few 64 MB arenas with
   `mprotect`, which does merge.

2. **Allocation stalls that grow with heap size: 9 ms at 10M, 25 ms at 30M,
   52 ms at 50M, 74 ms at 70M, 95 ms at 90M (x86); 3 -> 41 ms on arm.** glibc's
   worst is 0.25 ms and does not grow. The stalls are on the *alloc* path
   during the fill (not while parked), they scale with the number of slabs
   already created, and the parked-heap probe shows the update thread is not
   the cause. Candidate: `vmem_hash_rescale` on `umem_default_arena` /
   `umem_va_arena` (`vmem.c:1671`), which rehashes every allocated segment
   under `vm_lock` -- at 50M objects the va arena holds ~24k qcache slab
   segments and the default arena holds ~1.2M 4 KiB span segments, and each
   power-of-two rescale walks all of them while every span import waits. The
   stall count (19 over 100M) and the doubling pattern of when they happen
   are consistent with this; **not yet confirmed by a stack sample**. A
   `reclaim=0` control run is in `oc_ctl` (see below).

3. **RSS grows by 1.65 GB while freeing.** Freeing 100M 64 B objects takes RSS
   from 7368 MB to 9022 MB; glibc stays flat. The free path for a non-HASH
   cache writes the mangled freelist link into the buffer tail
   (`umem_slab_free`, `umem.c:~1747`), and the slab's `umem_slab_t` is at the
   end of the page -- neither is a new page. The 1.65 GB is 22 % of the heap;
   the likely source is the magazine layer (`umem_magazine_t` for 100M
   objects flowing through depot magazines of 143 rounds: 700k magazines x
   1.2 KB = 840 MB) plus the empty-slab retention list. **Unconfirmed;** the
   free phase also took 25 s versus glibc's 5.8 s.

**glibc comparison:** shares none of these. 54 VMAs, 0.25 ms worst, flat RSS
on free.

**Required fix.**
(1) VMAs: the va-arena qcache slabs must come from larger spans. Raise
`umem_va_arena`'s import granularity so one `mmap(MAP_FIXED)` covers many
qcache slabs (e.g. import 2-4 MB from `heap_arena` at a time and carve the
128 KiB slabs from that), or make `vmem_mmap_alloc` commit with `mprotect` on
an already-RW-then-PROT_NONE'd region so the kernel merges. Either way the
regression is `probe_objcount 100000000 64` asserting VMAs < 1,000; the
number to beat is 48,218. This is `3f2e67c`'s sibling, one layer up.
(2) Stalls: confirm with `perf record -g` on the fill phase; if
`vmem_hash_rescale` it is, make the rescale incremental (rehash a bounded
number of buckets per call, or double the table off-lock and swap) so no
single import waits on a 1.2M-entry walk. Target: worst alloc < 1 ms at 100M.
(3) RSS-on-free: `perf` the free phase and read `/proc/pid/smaps` before and
after; if it is magazine metadata, cap depot growth relative to live objects
(`umem_depot_ws_reap` already exists and should be running -- it is not
keeping up, or is not reached because the free phase finishes inside one
`umem_reap_interval`).

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

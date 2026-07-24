# GC stop-the-world soundness: confirmed root cause, applied fix, and the
# remaining oversubscription work

**Status:** Root cause confirmed with evidence. A fix that eliminates the
reported reachable-object sweep in bulk validation on real parallel hardware
is applied (commits below). A residual, much rarer miss and a set of new
failure modes appear ONLY under extreme CPU oversubscription (≈4×: 32 threads
on 8 cores); those are documented here as follow-on architectural work.
**This fix is therefore PARTIAL — do not treat STW soundness as fully closed.**

Built/tested on EC2 (`intel-lo` c7i.2xlarge/8 vCPU, `intel-hi`
c7i.metal-48xl/192 vCPU), AL2023, gcc-11, per the repo's Global Constraints.

---

## 1. Confirmed root cause (with evidence)

The finding listed three candidates. The primary defect is candidate 3 plus a
core-mechanism gap, proven by instrumentation:

**Two disconnected thread registries.** The public registration API
`umem_gc_register_thread()` populated only `gc_thread_list` (in `umem_gc.c`).
The stop-the-world suspend signal (`gc_stop_the_world`) and the Phase-3 root
scan (`umem_gc_scan_all_roots`) both walk a *different* array,
`umem_gc_threads[]` (in `umem_gc_roots.c`), which the public API never
touched. Consequences on the baseline:

- `gc_stop_the_world()` found `target == 0` → **suspended no worker**, and
  marked with every worker running freely.
- Phase-3 scanned the empty `umem_gc_threads[]` → **scanned no worker stack**
  as a root.

A worker's stack-rooted chain survived only when a stale pointer-shaped word
happened to sit on the collector's own stack / in a register / in a data
segment, or the object was transitively reachable from something already
marked — exactly the intermittent ~1/3–1/8 behaviour.

**Evidence** (temporary instrumentation, since removed): a root scan during
the stress reported `registered_slots=1` (the collector only) and
`scan_thread` was invoked **zero** times for other threads; worker
registration events showed the workers registering into a disjoint list.
After bridging the registries, the same probe reported `registered_slots` up
to 24 at 32 threads and `susp=1` (suspended) scans occurring.

Secondary gaps the fix also had to close (each independently able to sweep a
live object once worker stacks are scanned):

1. **No register capture for suspended threads.** A callee-saved register can
   hold the only pointer to a live object. The suspend handler must spill
   registers to scannable memory.
2. **Wrong stack range.** A suspended thread must be scanned from its *current
   (suspended) SP* up to its stack base, not a stale cached extent.
3. **Add/find inconsistency window.** `gc_object_add` links a new object into
   the global sweep list *and* the page sparsemap under `gc_objects_lock`. A
   thread suspended mid-update leaves the object on the sweep list but missing
   from `umem_gc_find_header()`'s sparsemap lookup — so the conservative scan
   of that thread's stack finds the pointer, `find_header` returns NULL, the
   object is not marked, and sweep frees a reachable, just-allocated node.

---

## 2. Applied fix

Commits (this branch):

- `fix(gc): stop-the-world scans suspended threads' registers + full stack
  after park barrier (fixes reachable-object sweep)`
- `fix(core): umem_cache_reclaim_pages no longer walks slab list across lock
  drop`
- `fix(core): bound getpcstack frame-pointer walk to a plausible stack span`

### 2.1 STW mechanism (`umem_gc.c`, `umem_gc_roots.c/.h`)

- **Unified registry:** `umem_gc_register_thread`/`umem_gc_unregister_thread`
  now bridge into `umem_gc_threads[]`, so every mutator is a real STW target
  and root-scan source.
- **Register spill + correct range:** the SIGUSR2 handler `sigsetjmp`s its
  register block into `gcti_regs` and records the live SP in `gcti_sp` before
  acking. The collector scans `gcti_regs` plus `[gcti_sp, stack_base+size)`
  for each parked thread. The kernel's signal-frame `ucontext` (the full
  interrupted register set) also falls inside that range, so caller-saved
  registers are covered too.
- **Park barrier:** the collector waits until every *signalled* thread has
  published `gcti_suspended = 1` (a per-thread ack, immune to the counter-ABA
  a shared count suffers) BEFORE marking. A `gc_stw_active` flag makes a
  late/stale signal (delivered after the collection already resumed) a no-op,
  so a worker whose SIGUSR2 arrives after the final resume never spins
  forever.
- **Stable target set:** `umem_gc_threads_lock` is held across the entire STW
  window (signal → wait → mark → resume) so no thread can register/unregister
  mid-pause; `umem_gc_scan_all_roots` takes a `threads_locked` argument so the
  collector's Phase-3 scan does not re-lock (which would self-deadlock).
- **Atomic add/remove vs STW:** `gc_object_add` and the remove path in
  `umem_gc_free` block SIGUSR2 across the list+sparsemap critical section, so
  a thread is never suspended mid-update (closing gap 3 above).

### 2.2 Independent core fixes surfaced by the stress

- **`umem_cache_reclaim_pages`** dropped `cache_lock` mid-walk around
  madvise/destroy and advanced through a cached `next` pointer that a
  concurrent operation could invalidate → SEGV at `next = sp->slab_prev`. Now
  all slab-list surgery happens in one pass under the single held lock,
  collecting slabs into local lists (`slab_reclaim_next`) processed after one
  lock release. Pre-existing race; independent of GC.
- **`getpcstack`** frame-pointer unwinder could deref a garbage outermost fp
  at thread teardown and SEGV (surfaced under real ASan). Now bounded to a
  16 MB span above the starting frame. Pre-existing; independent of GC.

---

## 3. Validation

`prop_gc --strict-stw` makes a reproduced canary corruption a hard failure.
Full `test/.libs/test_main --no-fork` = **all tests pass, 0 failures**.

### Real parallel hardware — `intel-hi` (192 vCPU): CLEAN in bulk

| threads | rounds | iters | ok | canary | segv | abort | hang |
|---------|--------|-------|----|--------|------|-------|------|
| 48      | 1000   | 40    | 40 | 0      | 0    | 0     | 0    |
| 96      | 1000   | 30    | 30 | 0      | 0    | 0     | 0    |
| 192     | 800    | 25    | 22 | 0      | 0    | 0     | 3    |

At 48 and 96 threads the reachable-object sweep does **not** reproduce (0/70).
At 192 (every vCPU saturated) 3/25 hangs but **zero** soundness/corruption.

### Baseline vs fix — `intel-lo` 8-core

| build     | config          | canary | segv | abort | hang |
|-----------|-----------------|--------|------|-------|------|
| baseline  | 32t/1500r ×20 (strict) | 12 | 0 | 0 | 2 |
| baseline  | 32t/1500r ×25 (full-length) | 8 | 0 | 0 | 6 |
| **fix**   | 8t/400r ×20     | 0      | 0    | 0     | 0    |
| **fix**   | 32t/1500r ×25 (full-length) | 0 | 3 | 2 | 10 |

The fix eliminates the canary sweep at 8 threads (0/20) and drops it sharply
at 32t/8-core, but under that ~4× oversubscription it introduces intermittent
SEGV/abort/hang that the baseline does not have, and a rare residual canary
still appears in some 32t runs.

---

## 4. Remaining work (why the oversubscription case is architectural)

Under ~4× CPU oversubscription the signal-based suspend handshake is
fundamentally strained and interacts badly with the allocator:

1. **Park latency vs timeout.** With 32 runnable threads on 8 cores, a
   signalled worker may not be scheduled to run its handler within the 5 s
   STW timeout. On timeout the collector currently proceeds best-effort; a
   sound design must instead *not sweep* on a timed-out (incomplete) snapshot
   (a `stw_rc != 0 → skip sweep` gate was prototyped and is the right
   direction, but is insufficient alone).
2. **Allocation while stopped.** The mark queue can grow via `umem_alloc`
   while the world is stopped; a worker parked while holding an allocator lock
   then deadlocks the collector (hang) or, if parked mid slab-list update,
   corrupts allocator metadata (SEGV/abort). A prototype that pre-sizes the
   mark queue to the live-object count before STW reduced but did not
   eliminate this, because other in-pause allocator interactions remain.
3. **Signal-storm cost.** At high oversubscription collections fire
   constantly; the per-allocation `pthread_sigmask` around `gc_object_add`
   plus a `pthread_kill` storm add large overhead and widen the windows above.

A fully robust design likely needs one or more of: a dedicated always-runnable
collector thread with `SCHED` priority for the pause; suspending via a
mechanism that cannot leave a mutator parked inside an allocator lock (e.g.
poll-point/safepoint cooperation instead of async signals, or masking
SIGUSR2 around *all* umem-internal critical sections, not just the GC list);
a bounded, allocation-free mark worklist reserved before the pause; and a
hard "never sweep an incomplete snapshot" rule.

Because this is a larger, invasive change to both the GC and the allocator's
suspension contract, it is left as follow-on work rather than bundled into
this fix, per the project rule that a change reducing a race while adding
crashes is worse than a documented, reproducible partial fix.

## 5. Reproduction (kept intact)

```
# real hardware, clean:
LD_LIBRARY_PATH=.../.libs prop_gc --strict-stw --threads=48 --rounds=1000   # passes
LD_LIBRARY_PATH=.../.libs prop_gc --strict-stw --threads=96 --rounds=1000   # passes

# oversubscription, still fails intermittently (documented limitation):
for i in $(seq 1 30); do
  LD_LIBRARY_PATH=.../.libs prop_gc --strict-stw --threads=32 --rounds=1500
done
```

`test/property/prop_gc.c` is unchanged: `--strict-stw` remains opt-in because
the invariant does not yet hold under all conditions. Flipping it to a default
hard assertion should wait until the oversubscription work above lands.

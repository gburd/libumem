# Finding: GC stop-the-world soundness bug — reachable object swept while thread suspended

**Severity:** High (GC correctness). A live, reachable object can be
collected, leading to a use-after-free in user code.

**Discovered by:** Workstream H2, `test/property/prop_gc.c` STW stress, on
`intel-lo` (`c7i.2xlarge`, 8 vCPU), built `--enable-asan`.

## Symptom

Running the GC STW stress at high thread counts (workers oversubscribe the 8
cores), a worker thread's own stack-rooted chain occasionally comes back with
a corrupted canary after a concurrent `GC_gcollect()` from the main thread:

```
STW STRESS FAIL: reachable object corrupted (swept while thread suspended)
```

Each worker allocates a chain of GC nodes rooted **only** on its own stack,
writes a per-node canary `0xD1CEF ^ idx`, and re-reads the canary after
collections happen concurrently. A canary mismatch means the node's memory
was freed and reused — i.e. a reachable object was swept.

## Reproduction

```
$ ./configure --enable-asan && make
$ for i in $(seq 1 8); do
    ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD=$(gcc -print-file-name=libasan.so.6.0.0) \
      LD_LIBRARY_PATH=.libs ./test/property/.libs/prop_gc --threads=48 --rounds=1000
  done
```

Intermittent: reproduced roughly **1 in 3** runs at `--threads=32
--rounds=1500`, and **1 in 8** at `--threads=48 --rounds=1000`, on 8 cores.
Higher thread counts / more rounds raise the probability (use
`--stw-iters=N`). No ASan report accompanies it — the failure is a *logical*
use-after-free (the object is freed and legitimately reused), which is why an
explicit canary oracle is required to catch it; ASan alone would only fire if
the reused chunk happened to be poisoned at the moment of the read.

## Analysis

The object is reachable **only** from the worker thread's stack. For the
collector to sweep it, the stop-the-world root scan must have failed to treat
that worker's stack as a root at the moment of the mark. Candidates (in
`umem_gc.c` `gc_stop_the_world` / `gc_resume_the_world` and
`umem_gc_roots.c`):

- A worker thread that is mid-`GC_MALLOC` (or between registering itself and
  pushing its first frame) when STW snapshots stacks may not have its current
  stack bounds captured, so its live nodes are unmarked and swept.
- The SIGUSR2 suspend handshake (recently reworked to a spin loop, commits
  `0dd4b8c` "use spin loop instead of sem_wait" and `4ba6147` "Limit STW
  signal handler to Linux only") may resume/mark before every target thread
  has actually parked, i.e. a thread's registers/stack pointer are not yet
  flushed to scannable memory when its stack is scanned.
- A race between `umem_gc_register_thread` / `umem_gc_unregister_thread` and
  the thread-list walk in `umem_gc_scan_all_roots` could scan a stale or
  incomplete stack range.

The recent STW fixes reduced but did not eliminate this race.

## Status

**Update (2026-07-24, CLOSED):** Root cause confirmed and fixed — see
`docs/results/2026-07-24-gc-stw-fix-and-oversubscription.md`. The first
(partial) fix bridged the two thread registries, spilled suspended registers,
and added a park barrier, but the signal-based handshake stayed fragile under
~4× CPU oversubscription (hang/abort + a rare residual sweep). The final fix
replaces async-signal suspension with **cooperative safepoints**: mutators
poll a flag at allocation entry and park themselves outside all locks; the
signal is only a fallback; the collector **never marks/sweeps an incomplete
snapshot** (bounded park barrier, skip-the-cycle on timeout), and the dead set
is snapshotted under STW then reclaimed after resume. Validated 0 corruption /
0 hang / 0 abort at 4× oversubscription (intel-lo 32t×50) and on real 192-core
hardware up to full saturation (intel-hi 48t/96t×50, 192t×30). `--strict-stw`
is now the **default** hard assertion. A bounded throughput tail remains at ≥6×
oversubscription (global object-lock scalability, not a soundness bug).

`test/property/prop_gc.c` REPRODUCES the bug and reports it prominently. By
default a reproduced corruption does **not** fail the suite (the bug is in the
library's STW root scan, not the test, and the other four GC invariants —
soundness under single-thread collection, finalizer exactly-once,
finalizer at-most-once, and progress — are genuinely proven). Pass
`--strict-stw` to make it a hard failure, which is the gate a fix must clear.

Fixing the STW handshake is core-GC work (Workstream H/GC owners), tracked
separately from the four proven invariants.

## Note on the high-core roles

The 192-vCPU `intel-hi` / `arm-hi` roles could not run the GC at all because
of an unrelated pre-existing core bug (`umem_cpu_node[]` OOB in
`umem_depot_alloc`, see `2026-07-23-cpu_node-oob-finding.md`) that corrupts
the heap / aborts under ASan on every allocation when `umem_max_ncpus > 256`.
The STW soundness bug above was therefore characterized on `intel-lo` with
oversubscribed threads.

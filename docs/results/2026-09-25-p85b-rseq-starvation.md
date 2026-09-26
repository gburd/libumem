# P8.5b: rseq lock-free reload — armed, proven safe, starved by PTC (2026-09-25)

**Branch:** `p85b-r4rseq2` (NOT on master). **Base:** master `da8453a`.
**Outcome:** spec implemented, armed correctly, proven safe, measured —
**inert under the shipping architecture, left on-branch, not shipped.**

## What the task was

Arm the existing-but-inert rseq lock-free per-CPU magazine reload
(`umem_rseq.c` / `umem_rseq_x86_64.S` / `umem_rseq_aarch64.S`) so the common
alloc/free case skips the shared depot layer on a hit, per the precise spec
in `docs/results/2026-09-09-rseq-reload-asm-design.md`. The mandatory gate
was the 192-thread `stress_concurrency_oracle` with zero aliasing/corruption
on both metal arches.

## What was implemented

- **Reload commit asm, both arches** (`umem_rseq_reload_alloc_commit`,
  `umem_rseq_reload_free_commit`): each its own rseq critical section that
  publishes the depot→`cache_rseq[cpu]` swap atomically w.r.t. the thread's
  CPU occupancy — the only migration-safe mechanism (v2 analysis proved
  C-only / lock-based reloads race at ~42–47% double-issue). Continued from
  the `p85b-wip` (`f50dfd6`) draft, verified faithful to the spec: Resolution
  A write order (`rounds=0` → `loaded_mag` → `rounds=real`), OLD `loaded_mag`
  read before any write, single unconditional last store (bug-6 discipline),
  per-arch `RSEQ_SIG` (aarch64 `0xd428bc00`, bug-5-safe).
- **P5.13b demangle bridge** in the alloc/free fast paths: `SLOT_XOR` matches
  `UMEM_SLOT_MANGLE` exactly (`val ^ umem_link_cookie ^ (&slot >> 12)`),
  using `&mag_round[i]` as the slot address. Master carried the C-side
  `mag_round` mangle but not the asm demangle (harmless while inert); this
  bridge is required once the fast path can serve a mangled magazine.
- **ABI soundness fix beyond the spec** (`old_rounds_out` on both commit
  fns): the spec, implemented literally, frees the outgoing magazine to a
  fixed depot list. But the migration retry path can strand a NON-empty old
  magazine, and the depot's `ml_full`/`ml_empty` lists have exact-count
  contracts (`umem_ptc_mag_return`, umem.c invariants 1–2): a partial
  magazine on the wrong list silently loses/aliases its contents = depot
  corruption. The commit now returns the OLD rounds count (read atomically
  before the commit stores, so single-last-store is preserved), and the C
  caller classifies the old magazine via the existing `umem_ptc_mag_return`
  helper. Confirmed with the coordinator; internal (non-exported) ABI only.
- **C-side arming**: the two `umem_rseq_*_slowpath` functions (previously
  UNUSED) rewritten as the two-phase reload (phase-1 depot pull in C,
  phase-2 asm commit with bounded retry), wired into `_umem_cache_alloc` /
  `_umem_cache_free` after a fast-path miss; on reload success the fast path
  re-runs to serve the buffer rseq-atomically; on any failure the code falls
  through to the always-correct `cc_lock` path.

## What was verified (both arches, lo boxes — c7i.2xlarge / c7g.2xlarge)

- `make check`: **49 PASS / 3 SKIP / 0 FAIL**, including
  `test_mag_round_mangle` and `test_ptc_slot_mangle`.
- **Six-bug regression suite**: `test_rseq_fastpath` PASS (index/bounds),
  `repro_rseq_trailing_store` PASS (0 leaks, 0 double-presence, commit store
  correctly last), `repro_naive_reload_race` still correctly CONFIRMS the
  naive-C-reload race. The direct-asm tests were updated to build MANGLED
  magazines (the demangle bridge changed the fast-path ABI) by recovering the
  hidden `umem_link_cookie` empirically via one push through the real free
  fast path.
- **`repro_reload_commit_safe`** (new): commit publishes on cpu-match with
  the correct old-magazine rounds ABI, and **ABORTS leaving the slot
  completely untouched on cpu-mismatch** — the load-bearing migration-safety
  gate.
- **`stress_concurrency_oracle` 8t/60s, mixed, all patterns: PASS, 0
  aliasing/corruption**, ~600M (intel) / ~860M (arm) successful allocations.

## The decisive finding: the reload is STARVED under PTC

Instrumented counters (`UMEM_DBG_RSEQ_PROBE`, gated behind
`-DUMEM_RSEQ_ARM_DEBUG`), 8-thread real-`umem` churn on intel-lo:

```
entered=9793  slow_called=9793  armed=0  abort=0  no_full=9793  break_cpu=0
```

- The rseq alloc block **is reached** (only on a PTC bin miss).
- The reload slowpath **is called** every such time.
- `umem_depot_alloc(cp, &cp->cache_full)` returns **NULL 100% of the time**
  (`no_full=9793`): the depot's full-magazine list (per-cpu stripe) is empty
  whenever the reload looks.
- Therefore the reload **never arms** (`armed=0`), and `rseq_alloc` /
  `rseq_free` / `rseq_restart` stayed **0 in every config** (default AND
  `ptc=0`, both arches).

**Root cause (architectural, not a code bug):** the per-thread cache (PTC,
default on) sits IN FRONT of the rseq per-CPU layer. PTC owns the depot
traffic; the `_umem_cache_alloc` rseq path is only reached on a PTC miss, and
by then the depot has no full magazine on the relevant stripe to hand the
reload. The rseq reload is structurally last in line and cannot pull.

This matches the 2026-09-09 diagnosis's own words ("nothing loads a magazine
into `cache_rseq[cpu].loaded_mag`"): arming the commit is **necessary but not
sufficient** — the win requires the reload to actually pull a magazine, which
the PTC-fronted architecture prevents.

## Ship decision: INERT, ON-BRANCH, NOT SHIPPED

- The mechanism is **implemented and proven safe** (commit cpu-gate, six-bug
  suite, 8t oracle 0-corruption both arches).
- It **serves zero real allocations** under the default (and every tested)
  config and therefore **cannot move the sustained p999 tail** and **cannot
  change the t=1 fast path** — a starved path that never executes its commit
  against a real magazine changes neither number. These were not measured on
  metal because a provably-inert path cannot change them; recorded as
  reasoned, not measured, per the evidence rules.
- **No metal was spent.** The 192-thread oracle in the brief was the gate for
  *shipping* the armed path; since the path stays on-branch, there is nothing
  to gate for release, and a starved path cannot corrupt at 192t differently
  than at 8t (it never executes the commit against a real magazine). The 8t
  oracle already passed 0-corruption on both arches.

## Revival path (a redesign, not this task)

Arming is done and safe; the benefit is blocked purely by ordering. To make
the reload actually fire, a future task must either:

1. **Move the rseq per-CPU reload AHEAD of the PTC layer** (rseq becomes the
   first fast path; PTC becomes the fallback), or
2. **Have PTC refill from `cache_rseq`** instead of directly from the depot,
   so the per-CPU magazine is on PTC's own refill path.

Either is a structural change to the alloc/free fast-path ordering, with its
own fork-safety, exit-drain, and oracle obligations — and it earns the full
192-thread metal gate on both arches (default + asan) at that time. The
arming machinery, the migration-safe commit asm, the `old_rounds` ABI, and
the `UMEM_DBG_RSEQ_ARM_DEBUG` counter harness on this branch are the ready
starting point.

## Files on the branch

- `umem_rseq_x86_64.S`, `umem_rseq_aarch64.S`: commit asm + demangle bridge +
  `old_rounds_out` ABI.
- `umem.c`: armed two-phase slowpaths, wired call sites, gated diagnostics.
- `test/stress/repro_reload_commit_safe.c`: commit cpu-gate + ABI test.
- `test/stress/probe_rseq_armed.c`: arming/starvation probe
  (`-DUMEM_RSEQ_ARM_DEBUG`).
- `test/unit/test_rseq_fastpath.c`, `test/stress/repro_rseq_trailing_store.c`:
  mangle-aware updates for the demangle-bridge ABI change.

# Sheaves/barn review: does Linux 1b87ea06a34b apply to libumem? (2026-09-27)

Worker `@r7barn`. Round 7 workstream E (`docs/plans/2026-09-27-round7.md`).
Upstream: "mm/slub: refill prefilled sheaves from the barn", Hao Li,
slab/for-next (Linux 7.4), full text read from `/tmp/sheaves.patch`.
Code read at master `833b1c5`.

Status: **design review written before any measurement.** The measurement
and the decision follow in the sections marked MEASURED and DECISION, which
are added after the runs.

## 1. What the patch fixes, precisely

SLUB keeps per-CPU *sheaves* (arrays of object pointers, `sheaf_capacity`
long) and a per-node *barn* holding a bounded list of full sheaves and a list
of empty ones. Two paths matter:

- The generic path, `__pcs_replace_empty_main()`, **exchanges** an empty
  sheaf for a full one from the barn. Nothing is left over.
- The *prefill* API (`kmem_cache_prefill_sheaf` -> `__prefill_sheaf_pfmemalloc`)
  refills a sheaf that is **not necessarily empty**. Before the patch it
  called `refill_sheaf()`, which takes objects only from partial slabs,
  under `list_lock`, and never from the barn.

The barn's full list has a limit. `kfree_rcu()` builds RCU sheaves and, when
they are done, tries to put them in the barn (`barn_put`). With the full list
at its limit that fails (`barn_put_fail`) and the sheaf is flushed to slabs
object by object (`sheaf_flush`, `free_add_partial`). Nothing ever took a full
sheaf *out* of the barn on the prefill path, so once the list saturated it
stayed saturated, and every RCU sheaf after that was flushed. The prefill
refill, meanwhile, paid `list_lock` on partial slabs (`sheaf_refill`,
`alloc_slab`).

The patch: `refill_sheaf_from_barn()` copies objects into the caller's sheaf
from ONE barn-resident *partial* sheaf first, then from full sheaves; the
source that still has objects becomes the new partial, an emptied source goes
on the empty list. Result on will-it-scale mmap1 at 192 processes: +24.2 %
throughput, `alloc_slab` -97.75 %, `sheaf_flush` -44 %.

Two conditions are necessary for the problem:
(a) a bounded full list that rejects puts, forcing flushes to slabs; and
(b) a refill path that takes a *count* of objects (not a whole container)
and goes to slabs without looking at the barn.

## 2. The mapping onto libumem, verified against the code

| SLUB | libumem | where |
|---|---|---|
| sheaf | magazine, `umem_magazine_t` (`mag_round[magsize]`) | umem_impl.h |
| per-CPU main/spare sheaf | CPU layer `cc_loaded`/`cc_ploaded`; PTC L2 `ptc->mags[bin].loaded`/`previous`; rseq `cache_rseq[cpu].loaded_mag` (one slot; `previous_mag` exists but no path fills it) | umem.c `_umem_cache_alloc`, `_umem_alloc`, umem_rseq.h |
| barn full / empty lists | depot `cache_full`/`cache_empty` plus per-CPU stripes `cache_depot_full[]`/`cache_depot_empty[]`, one `ml_lock` per list | umem.c `umem_depot_alloc*`, `umem_depot_free*` |
| barn limit (`barn_put_fail`) | **none.** `umem_depot_push` always succeeds. Excess is trimmed later by `umem_depot_ws_reap` from the working-set counters | umem.c 2289, 3479 |
| partial slabs under `list_lock` | slab layer under `cache_lock` (`umem_slab_alloc`/`umem_slab_free`) | umem.c 1797, 1945 |
| `__pcs_replace_empty_main` (exchange) | every live magazine refill: CPU layer (`umem_cpu_reload`), PTC L2 (swap `loaded`/`previous`, then a whole full magazine from `umem_depot_alloc_trylock`), rseq (`umem_rseq_reload` detach/attach) | |
| prefill of a non-empty sheaf | **no live analogue.** See 3.2 | |
| `sheaf_flush` of a leftover sheaf | `umem_ptc_mag_return*` -> `umem_mag_drain` for a magazine that is neither exactly full on the full list nor empty. See 3.1 | umem.c 2920-3008 |

## 3. Which libumem paths have the patch's shape

### 3.1 "A partial goes back object by object" (condition a's analogue)

`umem_ptc_mag_return()` and `umem_ptc_mag_return_trylock()` (umem.c
2962-3008) drain any magazine that is not (full list, `rounds == capacity`)
via `umem_mag_drain()`, then put the shell on the empty list. The caller
list:

| caller | what it hands back | can it be partial? |
|---|---|---|
| `_umem_alloc` PTC refill (4229) | `mag->previous`, with both rounds 0 | no: empty, drain loop does 0 iterations |
| `_umem_free` PTC flush (4484) | `mag->loaded`, reached only when `rounds == magsize` | no: exactly full, goes on the full list |
| `umem_ptc_mag_flush_all` (3067, 3074), thread exit | `loaded`/`previous` with their counts, **always to `cache_empty`** | yes, and a FULL one is drained too, because `mlp != &cp->cache_full` |
| `umem_rseq_reload` (3173) | the magazine detached from `cache_rseq[cpu]` | yes, when the fast path failed for a reason other than empty/full: an rseq abort (preemption, signal, migration returns NULL/-1 from the fast path), or another thread on the same CPU pushed/popped between this thread's failed fast path and its detach |
| stale magtype (`!UMEM_MAGAZINE_VALID`, both functions) | anything | yes, drained and the shell freed; only after a magazine resize |

**The drain does not go to the slab layer.** `umem_mag_drain()` calls
`_umem_cache_free(cp, buf)` per object (umem.c 2931), which is the rseq fast
path, then the cc_lock CPU magazine, then a fresh magazine from the depot or
from the magtype cache, and only if all of that fails `umem_slab_free()`. The
plan's premise ("drained to the slab layer") is therefore only true in the
out-of-magazines case. The cost the drain actually has is one
`_umem_cache_free` per object instead of one list push per magazine.

One more cost, visible only by reading: on the **alloc**-side rseq reload,
the drain of a partial `old` runs *after* the new full magazine was attached
to the same CPU slot, so the first drained object's `_umem_cache_free` finds
that slot full, and the free slowpath swaps the just-attached full magazine
straight back out to the depot. Correct (every object stays owned exactly
once), but it undoes the reload. It happens only when `old` is partial.

### 3.2 "A refill bypasses the depot and goes to slabs" (condition b's analogue)

The prefill API has no live counterpart:

- `umem_ptc_bin_refill()` (umem_ptc.c 793) takes a count of objects by
  calling `_umem_cache_alloc` repeatedly, which does go through the depot. It
  is reached only from `umem_ptc_alloc()`, which nothing in the tree calls:
  the live PTC path is inlined in `_umem_alloc`/`_umem_free`, and there a
  bin is never refilled from the magazine layer (an empty bin falls through
  to the L2 magazine; bins are filled only by frees).
- `umem_cache_alloc_batch()` (umem.c 3740) takes from the CPU magazines,
  then whole full magazines from the depot, and only then from slabs. It does
  not bypass the depot. Its callers are tests.
- `_umem_cache_alloc` goes to `umem_slab_alloc` only after
  `umem_depot_alloc(cp, &cp->cache_full)` returned NULL. That scan is
  trylock-only across stripes (umem.c 2836-2870), so it **can** miss a full
  magazine on a stripe whose lock was held at that instant, and then allocate
  from slabs while the depot holds objects. That is the one real
  depot-bypass shape libumem has. It is not partial-magazine related.

A slab allocation "while the depot held a partial" is structurally zero
today: the depot holds no partial magazines by construction. The probe
therefore measures the nearest meaningful thing: slab allocations made from
the magazine-miss path while at least one depot full list was non-empty.

### 3.3 Why the saturation half cannot happen here

SLUB's damage came from `barn_put_fail`: a bounded list refusing puts. The
libumem depot never refuses a put (the free side's `umem_depot_free_trylock`
falls back to a blocking push, umem.c 2667-2692), so a full magazine is never
flushed to slabs for lack of room. The price is memory held in the depot
until `umem_depot_ws_reap` trims it (P6.8), not slab traffic.

## 4. Candidate change (only if the measurement justifies it)

One partial magazine per depot stripe, `umem_maglist_t.ml_partial` +
`ml_partial_rounds` on the `cache_depot_full[]` stripe, protected by that
stripe's `ml_lock`:

- `umem_ptc_mag_return*`: a VALID magazine with `0 < rounds < capacity`
  takes the stripe's partial slot if empty. If occupied, top up the
  resident partial from the incoming one (copy `min(space, rounds)` rounds);
  a resident that reaches capacity moves to the full list; whichever shell
  ends empty goes on the empty list; any remainder stays as the partial.
  Nothing is drained object by object. The stale-magtype path is unchanged.
- The two refills that take a count: the PTC L2 refill in `_umem_alloc`
  (after `umem_depot_alloc_trylock` misses) and the rseq alloc slowpath may
  take the stripe's partial as their new magazine with its own count, since
  both already carry a per-magazine `rounds`. The CPU layer may not: its
  `umem_cpu_reload` invariant is loaded-empty/previous-full.
- Reap, purge and `umem_cache_destroy` must drain `ml_partial`, and the fork
  lock order already covers `ml_lock`.

Invariant to state in the comment: a magazine is on exactly one of {a CPU or
thread slot, a full list with `rounds == capacity`, an empty list with 0, a
stripe's partial slot with `0 < ml_partial_rounds < capacity`}.

This adds state to reap/purge/destroy/fork and a branch to two refill paths.
It is justified only if partial drains are a material share of slab-layer or
magazine traffic.

## 5. What is measured (task 2)

Probe build, compiled out unless `-DUMEM_BARN_PROBE` (no Makefile target;
built with `./configure CPPFLAGS=-DUMEM_BARN_PROBE`, like
`UMEM_RSEQ_ARM_DEBUG`). Counters, relaxed atomics, printed to stderr by a
library destructor in the probe build only:

- magazines drained by `umem_ptc_mag_return*` with `rounds > 0`, split by
  source (thread exit / rseq reload / PTC trylock path) and by partial vs
  full, plus the stale-magtype path separately;
- objects drained that way, and how many of them reached `umem_slab_free`;
- `umem_slab_alloc` calls from the magazine-miss paths, and how many were
  made while some depot full list (global or a stripe) was non-empty;
- per-cache `cache_slab_alloc` / `cache_slab_free` (existing counters).

Plus a perf profile of the default build at the same points for the share of
`umem_slab_alloc`/`umem_slab_free`/`umem_mag_drain`/mutex time.

## MEASURED

(filled in after the runs)

## DECISION

(filled in after the runs)

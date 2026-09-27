# Sheaves/barn review: does Linux 1b87ea06a34b apply to libumem? (2026-09-27)

Worker `@r7barn`. Round 7 workstream E (`docs/plans/2026-09-27-round7.md`).
Upstream: "mm/slub: refill prefilled sheaves from the barn", Hao Li,
slab/for-next (Linux 7.4), full text read from `/tmp/sheaves.patch`.
Code read at master `833b1c5`.

Status: **done; negative. No code change ships.** The design review
(sections 1-5) was committed at `f7790b7` before any measurement. MEASURED
and DECISION were added after the runs. The probe that produced the numbers
is in the tree, compiled out by default (`-DUMEM_BARN_PROBE`, `4259e15` +
`f93672a`).

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

**What ran.** Library at `f93672a` (probe counters only; the allocator paths
are identical to `833b1c5`), shipped with `verify-isolated.sh` (git archive),
built twice in one job: default, and `./configure CPPFLAGS=-DUMEM_BARN_PROBE`.
The `nm` check lists 15 `umem_barn_*` symbols in the probe `.so` and 0 in the
default one. Counts come from the probe build. Perf comes from the default
build. rseq on (the default) in every run. Debian 13 (trixie), kernel
6.12.107, gcc 14.2.0, glibc 2.41. Scripts: `docs/results/r7barn/{build,
measure}.sh`, `summ.awk`. Raw output: `docs/results/r7barn/{lo-t8,hi-t192}/`
(each `*.err` carries the `BARN` lines, `provenance.txt` the library hashes).

- t=8: intel-lo `c7i.2xlarge` (x86_64, 8 vCPU).
- t=192: **arm-hi `c8g.metal-48xl`** (aarch64 Graviton4, 192 vCPU). intel-hi
  `c7i.metal-48xl` returned `InsufficientInstanceCapacity` in us-east-2a, 2b
  and 2c at ~20:00Z. The coordinator accepted one arch for the 192t
  decision. It asked for a retry on intel only if the arm result were
  borderline, and it is not.

Workloads, each its own process: `bench_main -w prodcons` (64:256) and
`-w frag` (t=8 16:4096, t=192 64:256, 25k-object pool per thread), each once
by op count (`-n`) and once in interval mode (`-D 10:4 -W 1`). Then
`sustained_load.sh umem` in continuous mode, prodcons then frag. That was
1+3 x 20 s at t=8 and **1+5 x 30 s at t=192, the depot-contention shape**
(the coordinator's point 2). The sustained rows count the whole process,
warm-up included.

**Columns.** *allocs* = `_umem_alloc` calls, the per-million denominator.
*rseq/exit partial* = objects drained from a magazine with
`0 < rounds < capacity` by `umem_rseq_reload` or by thread exit
(`umem_ptc_mag_flush_all`). The PTC trylock path handed back 0 partial
magazines in all 12 runs, as the code in 3.1 predicts. *exit full* = objects
in exactly-full magazines drained at thread exit, because
`umem_ptc_mag_flush_all` passes `cache_empty`. *stale* = drained because a
magazine resize made the shell invalid. **drained->slab** = drained objects
whose `_umem_cache_free` fell all the way through to `umem_slab_free`.
*slab_alloc/free* = `cache_slab_alloc/free` summed over the `umem_alloc_<N>`
caches, exact. *slab alloc w/ depot full non-empty* = slab allocations from
the magazine-miss path in `_umem_cache_alloc` made while some depot full list
(global or stripe) read non-empty, unlocked.

| box | run | allocs (M) | rseq partial objs (per M) | exit partial objs (per M) | exit full objs | stale objs | drained->slab | slab_alloc | slab_free (per M) | slab alloc w/ depot full non-empty |
|---|---|---|---|---|---|---|---|---|---|---|
| t=8 c7i.2xlarge | prodcons-n | 40.0 | 4045 (101.1) | 0 (0.0) | 8668 | 0 | **0** | 26861 | 0 (0.0) | 1 of 26861 |
| t=8 c7i.2xlarge | frag-n | 0.8 | 0 (0.0) | 7795 (9743.8) | 2850 | 0 | **0** | 198730 | 0 (0.0) | 1 of 198730 |
| t=8 c7i.2xlarge | prodcons-D | 231.4 | 20166 (87.2) | 0 (0.0) | 8668 | 0 | **0** | 28542 | 0 (0.0) | 4 of 28542 |
| t=8 c7i.2xlarge | frag-D | 292.3 | 9342 (32.0) | 10398 (35.6) | 2536 | 5292 | **0** | 233001 | 29610 (101.3) | 11 of 233001 |
| t=8 c7i.2xlarge | sustained prodcons | 483.3 | 30964 (64.1) | 0 (0.0) | 8668 | 0 | **0** | 27670 | 0 (0.0) | 7 of 27670 |
| t=8 c7i.2xlarge | sustained frag | 548.1 | 16113 (29.4) | 10528 (19.2) | 3679 | 5355 | **0** | 235468 | 29357 (53.6) | 7 of 235468 |
| t=192 c8g.metal-48xl | prodcons-n | 76.8 | 874 (11.4) | 815 (10.6) | 180882 | 0 | **0** | 529333 | 0 (0.0) | 97 of 529333 |
| t=192 c8g.metal-48xl | frag-n | 19.2 | 111 (5.8) | 103298 (5380.1) | 175908 | 0 | **0** | 4314500 | 0 (0.0) | 541 of 4314500 |
| t=192 c8g.metal-48xl | prodcons-D | 29.0 | 751 (25.9) | 1580 (54.6) | 194518 | 0 | **0** | 499288 | 0 (0.0) | 62 of 499288 |
| t=192 c8g.metal-48xl | frag-D | 451.9 | 1272 (2.8) | 179761 (397.8) | 57885 | 30607 | **0** | 4356078 | 21116 (46.7) | 569 of 4356078 |
| t=192 c8g.metal-48xl | sustained prodcons | 131.0 | 1028 (7.8) | 203 (1.5) | 191459 | 0 | **0** | 541906 | 0 (0.0) | 67 of 541906 |
| t=192 c8g.metal-48xl | sustained frag | 2191.5 | 2865 (1.3) | 178542 (81.5) | 51000 | 33274 | **0** | 4381538 | 27635 (12.6) | 425 of 4381538 |

The `-n` frag rows have a tiny denominator. The process is almost all pool
setup and thread exit, so its per-million figures describe teardown, not
steady state. Every run exited rc=0.

**Point (1), whether a drained object falls through to the slab layer when
the CPU's magazines are full.** It can, and the probe counts exactly that
case. `umem_mag_drain` hands each object to `_umem_cache_free`, which tries,
in order:

1. the rseq free fast path;
2. `umem_rseq_free_slowpath`, which arms an empty magazine from the depot;
3. under `cc_lock`: the loaded magazine, the previous one if empty, an empty
   magazine from `umem_depot_alloc(cp, &cp->cache_empty)` (the old loaded one
   goes to the full list), and a new magazine from the magtype cache.

Only when all of that fails does it reach `umem_slab_free` (umem.c,
`_umem_cache_free` tail). While a drain is running, the probe sets a
thread-local (`umem_barn_tl_drain_cp`) and counts every `umem_slab_free`
reached from that tail for the same cache (`drained_to_slab`). The count
was **0 in all 12 runs**, on both arches, including 4.4 M slab allocations
at t=192. A full CPU magazine does not send a drained object to the slab
layer: step 3 moves the full magazine to the depot and takes an empty one.
The depot never refuses a push (3.3), and a magazine shell costs one
magtype-cache allocation. (Whether that allocation ever failed was not
counted separately. Any failure that sent a drained object to the slab
layer would show in `drained_to_slab`, which is 0.)

**Slab traffic is not partial-drain traffic.** Slab frees occur only in the
frag runs (21-30k). None of them came from `umem_mag_drain`
(`drained_to_slab = 0`). The probe does not attribute them further. The
remaining paths to `umem_slab_free` are depot reap (`umem_magazine_destroy`,
P6.8), stale-magtype shells (`umem_depot_destroy_stale`) and `_umem_cache_free`
with no magazine available outside a drain; which of these carried them was
not measured.
Slab **allocations** are 4.3 M out of 19-2191 M allocs on frag. Those are
live-set growth: frag holds 25k objects per thread, 4.8 M at t=192. The miss
path found a non-empty depot full list for at most 0.025 % of them (the
worst row, t=192 prodcons-D: 62 of 499,288), and those reads are racy.

**Perf, default build.** *t=8 c7i.2xlarge.* libumem is 2.5 % (prodcons) and
4.7 % (frag) of inclusive samples, almost all of it `_umem_alloc`/`_umem_free`
and the two rseq fast paths. No `umem_slab_*`, `umem_depot_*`,
`umem_mag_drain`, `pthread_mutex_*` or futex symbol clears the 0.3 % report
cutoff. *t=192 c8g.metal-48xl.* The profile is the bench harness, not the
allocator. prodcons spends 79.6 % self in `__aarch64_cas8_relax` under
`producer_thread`/`ring_pop`: bench_main's one shared MPMC ring. frag spends
96.5 % in `__aarch64_ldadd8_acq_rel` under `frag_worker`: the one shared
`live_bytes` atomic, one `fetch_add`/`fetch_sub` per alloc and free by 192
threads (`test/bench/bench_framework.c` 1244, 1272). libumem symbols total
under 1 %. No slab, depot or mutex symbol clears 0.3 %. That is
interpretable for this question, since there is no `cache_lock` share to find.
It also means these 192t arm frag/prodcons workloads measure one contended
cache line in the harness for any allocator. That was reported to the
coordinator for the @r7cmp comparison and is not fixed here (not this scope).

## DECISION

**Not material. Do not build the partial-magazine refill.** The bar set
beforehand was partial drains above 1 % of slab-layer traffic, or a visible
`cache_lock`/`slab_alloc` share in perf. Neither was met:

- Drained objects that reached the slab layer: **0** in 12 runs (0 % of slab
  traffic, 0 per million allocations). The coordinator's borderline was
  0.1 % of allocations.
- Partial-magazine drains in steady state: 1.3-101 objects per million
  allocations. The steady-state rows are prodcons-D, frag-D and sustained;
  the `-n` frag rows are teardown. Each drained object costs one
  `_umem_cache_free`, the same call a `free()` makes, not a slab operation.
- No slab, depot or mutex symbol above 0.3 % in any profile.

Why libumem does not have the problem Linux fixed:

1. **No barn limit, so no forced flush.** SLUB's damage was
   `barn_put_fail`: a full list at its cap rejecting RCU sheaves and flushing
   them to slabs. `umem_depot_free*` never rejects; the trylock variant falls
   back to a blocking push. The cost of that choice is held memory, which
   `umem_depot_ws_reap` trims (P6.8), not slab traffic.
2. **No prefill of a non-empty container.** Every live refill exchanges a
   whole magazine (CPU layer, PTC L2, rseq attach), the analogue of
   `__pcs_replace_empty_main`, which the patch leaves alone because it leaves
   nothing over. The two count-based refills, `umem_ptc_bin_refill` and
   `umem_cache_alloc_batch`, have no production callers (3.2).
3. **A drained partial goes back through the magazine layer, not to
   slabs.** Measured above: 0 of 1,541,700 drained objects over the 12 runs
   (87,531 rseq-partial + 492,920 exit-partial + 886,721 exit-full + 74,528
   stale).
4. **The one depot bypass that exists is a race, not a policy.** A slab
   allocation made while a full list reads non-empty (at most 0.025 % of
   slab allocations in any run) is the trylock steal scan
   missing a stripe whose lock was held at that instant. A barn partial
   would not change it.

What the numbers do show, recorded rather than acted on:

- **Thread exit drains FULL magazines object by object.**
  `umem_ptc_mag_flush_all` passes `cache_empty` for both magazines, so
  `umem_ptc_mag_return` drains an exactly-full magazine instead of pushing
  it onto the full list. At t=192 that is 51k-195k objects per run, one
  `_umem_cache_free` each (the "exit full" column): 886,721 objects over the
  12 runs, more than all partial drains combined (580,451). It is a
  thread-exit cost, bounded by
  2 x capacity per bin per thread, and never reaches slabs here. A one-line
  change (pass `cache_full`; `umem_ptc_mag_return` already classifies by
  count) would avoid it. It is not made here: it is not what this item
  measured as material, and a thread-exit change needs its own P6.3-style
  exit-storm A/B. ponytail: left as found; take it if an exit-storm profile
  ever shows `umem_mag_drain`.
- **The alloc-side rseq reload undoes itself when `old` is partial** (3.1).
  Observed rate: 1.3-101 drained objects per million allocations. Each
  costs a reload plus a swap back. Not worth new state.

Follow-up if this is revisited: the only candidate change is the one in
section 4, and the evidence says it would remove nothing measurable. The
probe stays in the tree so a future workload can be checked in one build.

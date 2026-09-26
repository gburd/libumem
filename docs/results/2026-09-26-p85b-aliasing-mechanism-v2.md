# P8.5b aliasing: second reading of the mechanism (2026-09-26, @r5fix)

Written before any test, so the reasoning survives if this agent dies.
Status: TWO hypotheses, neither yet test-proven. Tests follow in this order.

## H1 (the brief, e622f65): stale `cache_rseq[cpu].magsize` after a resize

Reading the code, H1 produces a **LEAK, not aliasing**:

- Alloc reload commits `rounds = rc->magsize` (stale, smaller) on a full
  magazine of capacity `cap`. Rounds `[rc->magsize, cap)` are live buffers
  the slot does not count.
- On swap-out `umem_ptc_mag_return(cp, list, old, old_rounds)` re-derives
  `cap = umem_mag_capacity(old)` and puts a magazine on `cache_full` **only if
  `rounds == cap`** (umem.c ~2967). Otherwise it drains `[0, rounds)` and pushes
  the magazine to `cache_empty`. So the misclassification by the stale bound is
  corrected, and the full-list count contract holds.
- The untracked rounds ride onto the EMPTY list. Every consumer of an empty
  magazine (PTC free, cc_lock `umem_cpu_reload(.., 0)`, rseq free reload)
  starts at rounds 0 and overwrites the slots. `umem_depot_destroy_stale` and
  `umem_magazine_destroy` drain empty-list magazines with 0 rounds. So those
  buffers are never issued again: they are leaked (`cap - magsize` per reload
  after a resize, e.g. 2 x 64-128 KB per reload for a magsize-1 class grown to 3).
- The free-side bound is `rc->magsize <= cap` (magtypes only grow), so it
  under-fills and cannot overflow.

Prediction: a forced-resize test shows slab-outstanding growth (leak), with
zero double-issue.

## H2 (new): the reload commit is not all-or-nothing

An rseq abort does not roll back stores that already retired. Only the LAST
store in the window commits atomically; every earlier store must be
safe-to-waste (leave a consistent slot if the abort lands after it).

`umem_rseq_reload_alloc_commit` (both arches) stores, in order:
`rounds = 0`, `loaded_mag = new`, `rounds = new_rounds` (commit).
An abort (preemption, signal, migration) after `loaded_mag = new` retires
leaves the slot at `(new, 0)` and returns 0 ("aborted, nothing written"):

- The OLD magazine `M` and its `r` live rounds have lost their only owner
  (the C caller discards `*old_mag_out` on abort). That's a leak.
- The C caller still believes `fmp` is unpublished. On retry, the commit reads
  `old = fmp, old_rounds = 0`, publishes `fmp` again, and the caller hands
  `old == fmp` to `umem_ptc_mag_return(.., cache_empty, fmp, 0)`. **`fmp` is now
  both `cache_rseq[cpu].loaded_mag` (with `cap` live rounds) and on the depot
  empty list.** The next owner of the "empty" magazine pushes into slots the
  rseq layer will pop, and the magazine's buffers get two owners. That's
  ALIASING. The same happens if another thread on that CPU runs in the gap.
  If the caller instead gives up, it `umem_depot_free`s `fmp` to `cache_full`
  while the slot still points at it. Two owners again.

`umem_rseq_reload_free_commit` stores `loaded_mag = new_empty`, then
`rounds = 0` (commit). An abort between them leaves `(empty_mag, old_rounds)`:
the alloc fast path pops `old_rounds` STALE slots of an empty magazine, i.e.
buffers already handed out. That is direct double-issue. Its header comment
says the free side has "no section 4 transient-zero subtlety", which is wrong.

Why only 64-120 KB classes: those caches start at magsize 1 (and 3), so
nearly every alloc and free is a reload. The two-instruction window is then
crossed at the alloc rate instead of once per 127-255 ops. The small classes
reload about 100x less often. This also fits the ~2.5 %/10 s rate.

Why `repro_reload_commit_safe` missed it: it only tests the cpu-mismatch abort,
which exits BEFORE the first store. It never lands an abort between stores.

## Test plan

1. `test_rseq_commit_atomic`: pin to one CPU, SIGALRM storm, call each commit
   millions of times on a fake slot. Check: return 0 means the slot is
   unchanged, and return 1 means the slot is exactly `(new, new_rounds)`.
   Expect FAIL at e622f65 if H2 holds.
2. Forced-resize test for H1 on a >64 KiB class: expect a leak, not aliasing.

## Candidate fix for H2 (and H1 at once)

Make every commit a SINGLE store, using `loaded_mag == NULL` as "rounds and
magsize are don't-care":

- detach CS: read `(M, r)` into out-params, then store `loaded_mag = NULL`
  (the only store, so it commits).
- attach CS: require `loaded_mag == NULL`, store `rounds` and
  `magsize = umem_mag_capacity(mag)` (don't-care while NULL), then store
  `loaded_mag = mag` last (the commit).

The C slowpath runs detach, then attach, then returns the old magazine via
`umem_ptc_mag_return` (exact rounds, own capacity). Per-magazine `magsize`
also removes H1.

## H2b (found reading the asm): the success return value sits inside the window

In all four reload commits (x86_64 and aarch64, alloc and free), the
instruction that sets the return value (`movl $1, %eax` / `mov w0, #1`) comes
AFTER the committing store and BEFORE the `post_commit` label, so it is inside
the rseq-checked range. If an abort lands after the commit store retires, the
slot IS published but the abort handler returns 0 ("nothing written"). The C
slowpath then retries with the same `fmp`:

- same cpu: the commit reads `old = fmp` (its own publish, `cap` rounds) and
  hands it to `umem_ptc_mag_return(cache_full, fmp, cap)`. `fmp` is now in
  the slot AND on `cache_full`. The PTC L2 refill takes it with
  `rounds = cap` while the rseq fast path pops the same slots. That is aliasing.
- other cpu: `fmp` is published into two cpus' slots. That is aliasing.
- retries exhausted: `umem_depot_free(cache_full, fmp)` while the slot holds
  it. That is aliasing.

The fast paths do not have this defect: their commit store is immediately
followed by the label. Fix: set the success value before the commit store,
so the commit store is the last instruction in the window.

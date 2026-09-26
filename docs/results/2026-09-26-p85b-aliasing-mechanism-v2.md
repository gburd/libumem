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

## H2 + H2b PROVEN at 017520f (pre-fix), both arches

`repro_reload_commit_abort 20000000 5` (x3 runs each), built by
`verify-isolated.sh <role> 017520f prefix` on Debian 13 lo boxes
(intel-lo@r5fix c7i.2xlarge, arm-lo@r5fix c7g.2xlarge), `--enable-rseq
CFLAGS="-O2 -g"`. Every run FAILED (rc=1). Logs: `docs/results/r5fix/prefix-*.log`.

| box | run | alloc torn | alloc commit-reported-abort | free torn | free commit-reported-abort | aborts |
|---|---|---:|---:|---:|---:|---:|
| intel-lo | 1 | 2061 | 2824 | 301 | 828 | 52,753 |
| intel-lo | 2 | 1981 | 2677 | 244 | 811 | 55,066 |
| arm-lo | 1 | 797 | 414 | 400 | 404 | 31,124 |
| arm-lo | 2 | 825 | 410 | 416 | 409 | 24,963 |

The torn states observed are exactly the ones predicted:
- alloc `ret=0 slot=(OLD,0)`: abort after `rounds=0`. The old magazine's 3
  rounds are gone from the slot, and the caller did not get it back. Leak.
- alloc `ret=0 slot=(NEW,0)`: abort after `loaded_mag=new`. `NEW` is
  published while the caller believes it is not (H2). Double ownership.
- alloc/free `ret=0 slot=(NEW,7)` / `(NEW,0)`: the commit completed and the
  abort hit the `mov $1` after it (H2b). Double ownership on retry.
- free `ret=0 slot=(NEW,3)`: abort between `loaded_mag=empty` and `rounds=0`.
  The alloc fast path pops 3 stale slots of an empty magazine, which is
  direct double-issue.

The reload commits are not atomic. That is a sufficient cause of aliasing. H1
(stale `magsize`) still needs its own test; per the reading above it leaks and
does not alias.

## End-to-end repro (`repro_rseq_reload_alias`, d01f0a4, pre-fix): weak

Four threads pinned to one cpu, a SIGUSR1 storm, `umem_alloc(100000)`, and a
stamp oracle. Pre-fix: **1 failure in 70 runs** (arm-lo run 1 of 5 at 20 s:
`ALIAS: tid=4 ... found 0x3000004a8c766`, a buffer re-issued to tid 3 while
tid 4 held it. Then 0/30 at 15 s on both arches). That is real aliasing
through the public API, but at too low a rate to serve as the pre/post
regression. `repro_reload_commit_abort` (thousands of violations per run,
deterministic) is the regression. The e2e repro is supporting evidence, run
post-fix for many more runs than pre-fix.
Sweep at d813f7f (threads/ring/cpu-span `4 4 1`, `16 1 1`, `8 2 4`, `16 1 7`,
`32 8 7`; 6 x 10 s each, both arches): 0/60 per arch. The e2e pre-fix total
is 1/190. It stays as a post-fix smoke test only.

## H1 PROVEN pre-fix as a LEAK (`repro_rseq_resize_leak`, 2719794)

Both arches, 3/3 runs each, deterministic:
`cache=umem_alloc_114688 magsize 1 -> 3  stranded before=16 after=12010`.
After a forced resize, 12,010 buffers of that cache are walker-ALLOCATED at
quiescence: held by no magazine the walker knows about, freed by nobody.
The control (`before=16`, no resize) is not yet explained; under
investigation before this test is trusted as a pass/fail gate.
Logs: `docs/results/r5fix/preleak-*.log`.

### H1 control arms (5b0d145, intel-lo)

| arm | batch | before resize | after resize |
|---|---:|---:|---:|
| rseq on | 1 | 1 | 1 |
| rseq on | 8 | 8 | 4,006 |
| rseq on | 16 | 16 | 12,010 |
| rseq on | 32 | 32 | 36,014 |
| `norseq` | 1 | 1 | 1 |
| `norseq` | 16 | 16 | 16 |

`norseq` is flat across the resize and rseq-on grows with batch, so the leak
is the rseq layer's. The `before == batch` constant appears in every arm,
including norseq. It is unrelated to rseq, does not grow, and is **not
explained**. The gate is therefore "flat across the resize"
(`after <= before`), not "zero".

## FIX 4b30bff: detach/attach, per-magazine magsize. First results (lo, both arches)

Same tests, `verify-isolated.sh <role> 4b30bff fix1`, Debian lo. Logs:
`docs/results/r5fix/fix1-*.log`. The library build has no new warnings.

| test | pre-fix | post-fix intel-lo | post-fix arm-lo |
|---|---|---|---|
| `repro_reload_commit_abort` (3 x 20M/side) | FAIL every run (up to 2,824 violations) | PASS 3/3, 0 torn, 0 CRA, ~52k aborts/run | PASS 3/3, 0 torn, 0 CRA, ~20k aborts/run |
| `repro_reload_commit_safe` (ported, + attach-on-occupied) | PASS (blind to the defect) | PASS | PASS |
| `repro_rseq_resize_leak` batch 8/16/32 | 8->4006, 16->12010, 32->36014 | flat 8/16/32 | flat 8/16/32 |
| `repro_rseq_reload_alias` 3 x 10 s | 1/190 FAIL | PASS 3/3 | PASS 3/3 |

`repro_reload_commit_abort` was ported to the new entry points. The pre-fix
run drove the old swap commits, since the fix removed them, and the contract
checked (returned 1 means exactly the committed state, returned 0 means
exactly the prior state) is the same.

# P8.5b aliasing: mechanism (2026-09-26)

Recovered by the coordinator after the P8.5b agent (5064938e) died on a request
timeout. Its last message said it had found the mechanism by reading source; it
wrote nothing to disk. This is reconstructed from the code it was reading
(`umem.c` magtype/resize/purge paths, both `.S` commit windows) and checked
against the five captured failures in
`docs/results/2026-09-26-p85b-aliasing-rate.md`. It is a hypothesis with a
complete code path, **not yet demonstrated by a failing test**.

## The defect: `cache_rseq[cpu].magsize` goes stale on a magazine resize

- `cache_rseq[i].magsize` is set once, at cache creation, to the cache's
  initial magtype (`umem.c:5759`). Nothing ever updates it.
- `umem_cache_magazine_resize()` (update thread, triggered when
  `cache_depot_contention` rises by more than `umem_depot_contention = 2`
  failed trylocks per interval) calls `umem_cache_magazine_purge()`. The purge
  drains `cache_cpu[]` only: **zero references to `cache_rseq`**. Then it
  advances `cache_magtype`.
- The P8.5b agent's own writeup
  (`docs/results/2026-09-26-p85b-arch-rseq-feeds-ptc.md`, "Known bounded
  ceiling (not corruption)") knew `rc->magsize` goes stale, and argued it could
  only under-fill a magazine. That argument is wrong, as follows.

## Why a stale `rc->magsize` becomes aliasing

The rseq layer counts rounds against `rc->magsize` while the magazines it moves
in and out of the depot have the **new** magtype's capacity. Three consequences:

1. **Alloc reload under-reports rounds.** `umem_rseq_alloc_slowpath` pulls a
   full depot magazine (new magtype, `cap` real rounds) and commits it with
   `new_rounds = rc->magsize` (`umem.c:3151`). If `rc->magsize < cap`, rounds
   `[rc->magsize, cap)` are live free buffers the rseq layer does not know it
   holds.
2. **Swap-out misclassifies.** When that magazine is swapped out, the
   classifier is `old_rounds == rc->magsize ? cache_full : cache_empty`
   (`umem.c:3165`, `3219`), again against the stale value.
   `umem_ptc_mag_return` drains only `[0, old_rounds)` (`umem_mag_drain`), so
   the untracked buffers above that stay in the shell. The magazine then goes
   onto a depot list whose count contract it violates.
3. **A capacity-trusting consumer re-issues them.** The PTC L2 refill takes
   any magazine off `cache_full` and sets `mag->rounds =
   umem_mag_capacity(fmp)` (`umem.c:4249-4251`): it trusts the list, not a
   count. `umem_depot_destroy_stale` likewise drains a "full" stale shell by
   its capacity, not by a round count (`umem.c:2706-2717`). A buffer that one
   owner still holds, or that was already re-issued through another path, is
   handed out again. That is two owners.

## Consistency with the captured failures

- **All five failures are 64-120 KB.** Those caches start at the smallest
  magtypes (chunk > 64 KiB starts at magsize 1, and 32-64 KiB at 3), and they
  are the only `umem_alloc` classes whose magtype can still grow. The
  small-object classes start at magsize 127/255, the maximum, so they cannot
  resize. That is why the P8.5b agent's oracle runs, dominated by small
  objects, rarely hit it, and why its "default classes 8..256 are immune"
  note was true only for those classes.
- **Whole buffer overwritten from byte 0, both self- and cross-thread
  aliasing:** a buffer handed out twice, not a partial overrun.
- **~2.5 % per 10 s run at 8 threads:** the resize needs depot contention to
  cross its threshold during a run, then a reload to land on the stale bound.
- **0/100 with `norseq` and 0/100 before P8.5b:** the cc_lock layer reads
  capacity from the magazine it holds (P1.3b) and purge drains it; only the
  rseq layer carries a separate, never-refreshed bound.

## The fix (not yet applied)

Stop trusting a per-cache `rc->magsize`. The rseq layer must bound and count
against **the capacity of the magazine it actually holds**, exactly as P1.3b
made the cc_lock and PTC layers do:

- The alloc reload commits `new_rounds = umem_mag_capacity(fmp)`, not
  `rc->magsize`, and publishes that capacity into `cache_rseq[cpu]` in the same
  critical section (the free fastpath's `cmp CACHE_MAGSIZE_OFFSET` bound must
  describe the magazine in `loaded_mag`, so `magsize` becomes per-load, written
  before the committing `rounds` store; single-last-store preserved).
- The free reload does the same with `umem_mag_capacity(emp)`.
- Both swap-out classifiers use `old_rounds == umem_mag_capacity(old_mag)`.
- `umem_cache_magazine_purge()` must also drain `cache_rseq[]` (or be proven
  unnecessary once the bound is per-magazine). Draining another CPU's rseq
  slot from the update thread is not rseq-safe; the per-magazine bound is the
  change that removes the need.

## What must prove it

1. A regression that FAILS at `24bffbd` and PASSES after. Force the resize
   deterministically (a >64 KiB class, `umem_depot_contention` driven over its
   threshold, or a test hook calling `umem_cache_magazine_resize`), then run
   rseq alloc/free across the resize and check ownership. Plus the
   `oracle_rate.sh` arm at ~100 runs: 0 failures against the measured 5/200.
2. The 192t/60s oracle, default and ASan, both Debian metal arches, because
   the change touches both commit windows.
3. The 6-bug rseq suite, `repro_reload_commit_safe`, `make check`, the PTC and
   depot oracles, and the mangle tests.

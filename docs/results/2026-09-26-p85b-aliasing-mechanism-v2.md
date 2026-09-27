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

## Why the P8.5b gates passed with a real double-owner bug (a finding about the gates)

- **`repro_reload_commit_safe`** aborts only through a cpu-id mismatch, which
  jumps to the abort handler before the first store. It never landed an abort
  between stores or after the commit store, and those are the only places the
  defect lives. It tested the gate of the section, not the section's
  atomicity. (Ported to detach/attach, with a note saying what it does not
  cover. `repro_reload_commit_abort` covers the rest.)
- **`repro_rseq_trailing_store`** drives only the two fast paths, which were
  correct. It never called the reload commits.
- **192t/60s oracle, default and ASan, both metal arches: PASS.** The window
  is a few instructions per reload. An abort needs preemption, a signal, or a
  migration inside those instructions. At 192 threads on 192 vCPUs there is
  little preemption. Most allocations in the mixed/all run are small classes
  (magsize 127/255), which reload about once per 100+ ops, and PTC absorbs
  most of those. The bug needs a >64 KiB class (reload on nearly every op),
  an abort in the window, then a second owner that reuses the buffer while
  the first still holds it. The 8t/10s rate of ~2.5 %/run (5/200) means one
  60 s run at 192t can easily see zero: a PASS was never evidence at that
  rate. ASan cannot see this defect at all: both owners hold a valid, live
  allocation, so there is no overflow or use-after-free for it to report.
  Only the stamp oracle can see it.
- **The design spec's own safety argument** (asm-design §4) reasoned only
  about the fast path's view of a torn slot, never about the C caller's
  "0 = nothing written" assumption. The checklist it gave implementers
  therefore contained no test that aborts mid-section.

Lesson recorded: an rseq critical section needs a test that aborts it at
every instruction boundary (a signal storm on a pinned thread), with the
return-value/state contract checked on every call. A "migration abort" test
exercises only the entry gate.

## Sibling audit: every rseq critical section in both .S files

After the fix there are eight sections (4 per arch): `alloc_fastpath`,
`free_fastpath`, `reload_detach`, `reload_attach`. The rule: at most one
store that another path acts on, placed last before `post_commit`, and the
result register set before it.

| section | stores in window, in order | commit store last? | result set before commit? | verdict |
|---|---|---|---|---|
| alloc_fastpath (x86, arm) | `alloc_count++`, `rounds = r-1` | yes | yes (`%rax`/`x0` = popped buf) | OK |
| free_fastpath (x86, arm) | `mag_round[r] = buf`, `free_count++`, `rounds = r+1` | yes | yes (x86 `xorq %rax` after the label; arm `mov x0,#0` after the label; both are outside the window) | OK |
| reload_detach (x86, arm) | `*old_mag_out`, `*old_rounds_out`, `loaded_mag = NULL` | yes | yes (`mov $1` before) | OK |
| reload_attach (x86, arm) | `rounds`, `magsize`, `loaded_mag = new` | yes | yes (`mov $1` before) | OK |

Why the non-commit stores are safe to waste:
- `alloc_count` / `free_count` are advisory statistics. They are read only by
  `umem_cache_print_stats`-style reporting (umem.c ~1011) and the probes, and
  never feed a decision. An abort after the increment over-counts by one.
- `mag_round[r] = buf` (free fastpath): the slot is at index `rounds`, which
  no reader reads until `rounds` advances. An abort leaves it overwritten by
  the next push. (This is the bug-6 argument, and it still holds.)
- `*old_mag_out` / `*old_rounds_out`: caller stack memory, which the caller
  reads only when 1 is returned.
- attach's `rounds` / `magsize`: both fast paths test `loaded_mag == NULL`
  before they read either (alloc: `rounds` is read first, but a NULL
  `loaded_mag` then sends it to `alloc_empty` with nothing popped; free tests
  `loaded_mag` first). Every attach rewrites both before it publishes.
- `restart_count++` runs in the abort handler, outside the window.

The abort handlers set the result in the handler (`xorl %eax` / `mov w0,#0` /
`movq $-1`), outside the checked range, so an abort always reports "not
committed", and the commit store never retired in that case.

The fast paths had neither defect. The two removed swap commits had both.
No other file contains rseq critical sections (`grep post_commit`: only the
two `.S` files).

## Other readers of the per-cpu rseq slot (sibling grep)

`grep -rn "cache_rseq\|CACHE_MAGSIZE_OFFSET\|rc->magsize\|\.magsize"` over
C, headers, asm, and the gdb/lldb helpers:
- `umem.c` slowpaths: rewritten (detach/attach, per-magazine capacity).
- `umem.c` cache create: per-cache `magsize` init removed. Slots start zeroed
  and detached.
- `umem.c umem_cache_destroy`: reads the slot directly with no rseq section.
  That is legal only because destroy runs with no other user of the cache.
  It calls `umem_magazine_destroy()`, which freed the shell to the cache's
  CURRENT magtype and asserted `UMEM_MAGAZINE_VALID`. The per-cpu slot can
  hold a pre-resize shell there (purge does not touch `cache_rseq[]`), so
  that would free a 2-word shell into the 4-word magtype cache. **Fixed:**
  the shell goes back to its own source cache. The same function is used by
  `umem_cache_magazine_purge` and `umem_maglist_ws_reap`, and both paths
  were already valid-only. They are unaffected.
- `umem.c` stats (~1007): reads counters only.
- `umem_inspect.c` cached-set (~943): bounds by `UMEM_MAGAZINE_CAPACITY(mp)`
  (already per-magazine, CB-2) and never reads `rc->magsize`. Best-effort
  read without rseq, as documented. A NULL `loaded_mag` means it skips the
  slot, which is correct for a detached slot.
- `umem_rseq.h`: field comment now states that `magsize` is per-magazine and
  meaningful only while `loaded_mag != NULL`.
- Tests that set `rc.magsize` by hand (`test_rseq_fastpath`,
  `repro_rseq_trailing_store`, `repro_naive_reload_race`): they build private
  slots with `loaded_mag` set, which is still a valid state. No change.
- gdb-macros and tools/gdb, tools/lldb: no reference to `cache_rseq` (they
  read only `cc_magsize`).
- `umem_cache_magazine_purge` does not touch `cache_rseq[]`, deliberately.
  The comment on `umem_rseq_reload()` explains why: the bound is per-magazine,
  a stale-magtype shell drains on swap-out via `umem_ptc_mag_return`
  (`!VALID`), and a cross-cpu slot write from the update thread would not be
  rseq-safe.

## Lo gate at 4634100 (fix 4b30bff + gate script), Debian 13, both arches: ALL PASS

`verify-isolated.sh <role> 4634100 logate 'bash scripts/ec2/p85b_fix_lo_gate.sh 200'`,
`--enable-rseq CFLAGS="-O2 -g"`, `vm.max_map_count=65530`. Logs:
`docs/results/r5fix/logate-*.log`.

| gate | intel-lo c7i.2xlarge | arm-lo c7g.2xlarge |
|---|---|---|
| `make check` | 50 PASS / 3 SKIP / 0 FAIL | 50 PASS / 3 SKIP / 0 FAIL |
| `test_rseq_fastpath` (6-bug suite) | PASS | PASS |
| `repro_rseq_trailing_store` | PASS | PASS |
| `repro_reload_commit_safe` (ported) | PASS | PASS |
| `repro_rseq_resize_leak` 16, 32 | PASS (flat) | PASS (flat) |
| `repro_rseq_reload_alias` 30 s | PASS | PASS |
| `repro_reload_commit_abort` x 200 | 0 / 200 fail | 0 / 200 fail |
| oracle 8t/10s small/multi, mag/multi, mixed/all, large/churn | 4 / 4 PASS | 4 / 4 PASS |

The 3 SKIPs are the `test_introspect_*` trio, the same as pre-P8.5b (no
`--enable-introspect`). `make check` includes `test_ptc_slot_mangle`,
`test_mag_round_mangle`, `test_freelist_mangle`, `test_ptc_resize_no_loss(_probe)`,
`test_ptc_thread_exit_drain_probe`, `test_fork_ptc_drain_probe`,
`test_depot_empty_scan`, `repro_ptc_resize_capacity`, `oracle_fast`,
`oracle_control`, and `test_norseq`.

## Finding: the claimed 93.7 -> 46.5 us p999 baseline arm was probably NOT rseq-off

The P8.5b sustained A/B (`docs/results/jobs/intel-hi-r5rseq-sustained/cmd`)
selected its "BEFORE (rseq OFF)" arm with `UMEM_RSEQ_OFF=1`. That env var was
read only by a temporary gate compiled under `-DUMEM_RSEQ_ARM_DEBUG`, added in
2324ba2 and **removed in 545bbce** (the norseq commit). The sustained run did
build with `-DUMEM_RSEQ_ARM_DEBUG`, from `~/libumem-r5rseq`, a `run-remote.sh`
sync and not an isolated ref, so its source revision is unknown. If that tree
was at or after 545bbce, both arms ran with rseq ON and the 2x difference
comes from something else. The source that was measured cannot be recovered.
The number is therefore **unattributed**. The re-measure below uses
`UMEM_OPTIONS=norseq` (the shipped switch, verified by `test_norseq.sh`) on
the same binary, from an isolated committed ref.

## Finding: arm-hi sustained_load.sh SEGVs in umem_init -> dlopen (not the rseq path; attribution pending)

The metal default gate at 66fd33b on arm-hi (c8g.metal-48xl, Debian 13): the
192t/60s oracle PASSED (700,035,306 allocs, 0 failures), but every
`sustained_load.sh` bench window crashed with rc=139, rseq on AND `norseq`.
Reproduced by hand:
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=8388608 bench_main -a umem -w prodcons -t 192 -n 19200000 -s 64:256 -r 1 -W 0 -A -c`
gives SEGV. The backtrace (`docs/results/r5fix/mdef-arm-hi/crash-bt.txt`)
shows a thread-stack overflow in ld.so: `producer_thread -> _umem_alloc ->
umem_alloc_retry -> umem_init -> umem_setup_envvars (envvar.c:997) ->
dlopen -> ... stp x29,x30,[sp,#16]` right after `sub sp,sp,#0x440`.
`sustained_load.sh` exports that tunable itself (line ~59). The rseq reload
is not on the stack. Untested so far: whether it happens pre-fix (e622f65),
and whether it is the tunable or `-n`. Next: a control run.

### Attribution: PRE-EXISTING, not caused by this fix (arm-hi, e622f65)

Same box, `verify-isolated.sh arm-hi@r5fix e622f65 crashpre`, i.e. master
BEFORE the fix, same bench invocation (`docs/results/r5fix/crashpre-arm-hi.log`):

| `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=8388608` | runs | rc |
|---|---|---|
| set (what `sustained_load.sh` exports by default) | 3 | 139, 139, 139 |
| unset | 3 | 0, 0, 0 |

The crash reproduces on the pre-fix ref and depends on the tunable. It is
not in the rseq code. Mechanism (not fully proven): the first allocation in a
192-thread bench happens on a small producer thread stack. `umem_init`
`dlopen()`s from there, and with an 8 MiB optional static-TLS reservation
ld.so's frame overflows that stack. It is outside this task's scope and is
recorded as its own defect: `umem_init` should not `dlopen` on an arbitrary
caller's stack. The arm-hi sustained measurement below runs with
`GLIBC_TUNABLES` set to an empty tunable, and this is labelled. intel-hi does
not crash with the default.
The P8.5b **t=1 A/B** (`docs/results/jobs/intel-lo-r5rseq-t1ab/cmd`) selected
its off arm the same way (`UMEM_RSEQ_OFF=1`, non-isolated tree), so the claim
"t=1 not regressed" is also unattributed. It is re-measured with `norseq`.

## Metal, intel-hi c7i.metal-48xl (Debian 13), fix at 66fd33b: default gate

`docs/results/r5fix/mdef-intel-hi.log`. The oracle 192t/60s mixed/all
default PASSED: 370,252,584 allocs, 0 failures.

Sustained `sustained_load.sh umem 180 192`, prodcons 64:256, frag 64:256,
3 measured windows + 1 warmup, the SAME binary, rseq on vs `UMEM_OPTIONS=norseq`,
run sequentially (on first):

| prodcons | rseq ON w0 / w1 / w2 | median | norseq w0 / w1 / w2 | median |
|---|---|---:|---|---:|
| p999 (ns) | 29,737 / 32,278 / 26,355 | **29.7 us** | 41,673 / 41,735 / 39,079 | **41.7 us** |
| p99 (ns) | 4,062 / 5,555 / 3,681 | 4.1 us | 6,533 / 6,273 / 6,770 | 6.5 us |
| Mops/s | 1.697 / 1.837 / 2.183 | 1.84 | 1.958 / 1.882 / 1.901 | 1.90 |
| RSS@peak (MB) | 141 / 122 / 130 | 130 | 264 / 246 / 245 | 246 |

| frag | rseq ON median | norseq median |
|---|---:|---:|
| p999 | 1.97 ms | 2.00 ms |
| Mops/s | 8.80 | 9.17 |
| RSS@peak | 4.57 GB | 4.75 GB |

Read plainly: with the fix, rseq ON beats `norseq` on prodcons p999 by about
29 % (29.7 vs 41.7 us), cuts p99 by about 37 %, and roughly halves RSS. Both
arms are well under the old 85 us target, and so is `norseq` itself (41.7 us).
So the old "93.7 us rseq off" figure does not describe norseq on this build.
The rseq layer is not what brings the tail under 85 us. It adds a further
~12 us p999 reduction on top. Throughput is lower with rseq on: prodcons
1.84 vs 1.90 Mops/s (-3 %), frag 8.80 vs 9.17 (-4 %). n=3 windows per arm and
w0-w2 spread about ±10 %, so the throughput difference is within about one
spread and is not claimed as significant either way. The run order was not
alternated (on, then off), so drift is not controlled for.

## Metal, intel-hi: ASan gate + t=1 / t=192 A/B (fix, 3b82b59 = 66fd33b + scripts)

`docs/results/r5fix/masan-intel-hi.log`. The 192t/60s mixed/all oracle under
`--enable-asan` PASSED: 972,884,666 allocs, 0 failures, no ASan report.

`bench_main -w multi -s 16:256`, same binary, rseq on vs `UMEM_OPTIONS=norseq`,
alternating on/off, 5 runs each, medians:

| | ops/s ON | ops/s norseq | p50 / p90 / p99 / p999 ON (ns) | norseq (ns) |
|---|---:|---:|---|---|
| t=1 (20 M ops) | 7,197,211 | 7,088,259 | 21 / 22 / 23 / 25 | 21 / 22 / 34 / 36 |
| t=192 (96 M ops) | 256,965,460 | 259,344,872 | 31 / 50 / 190 / 409 | 31 / 48 / 192 / 416 |

At t=1, rseq on is not slower (+1.5 % ops/s, within run spread) and its tail
is lower (p99 23 vs 34 ns). At t=192 the two are equal within noise (-0.9 %
ops/s, p999 409 vs 416 ns). Neither arm regresses the other.
intel-hi terminated immediately after this job (i-0ef9c16cf90ea208d).

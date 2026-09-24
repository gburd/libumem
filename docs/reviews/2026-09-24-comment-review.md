# Comment review, 2026-09-24

Read-only review of comments added or modified in the last two years
(`git log --since='2 years ago'`, 522 commits from 8fb8b40; tip at start of
review: 1912fce). Standard: AGENTS.md §7 and the writing standard in
`docs/plans/2026-09-24-team-brief.md`. Code was not changed.

Classes: (1) CLAIM MISMATCH, (2) STALE MEASUREMENT, (3) NARRATION/CONFIDENCE,
(4) MISSING INVARIANT, (5) SOFTENED RECORD, (6) GOOD.

Method: `git blame -w --since='2 years ago'` per file to locate in-scope
comment lines, then read each hunk against the code it describes. Line
numbers are against 1912fce.

**Process note (this reviewer's own error).** The brief allowed `--amend` on
my own tip commit of this one file. Between my incremental commits, seven
commits by other agents landed on top (0fa829f, 71422fe, eb68575, 3b01b7a,
4e7ad1d, efe93c7, 8ea4215 as they now stand), and my `--amend --no-edit`
rewrote each of THOSE tips to add this file, changing their shas (e.g.
b1e5b0d -> 4e7ad1d, which the P1.8 plan entry already notes). Content of
their commits is otherwise unchanged and nothing was pushed. Recorded here
rather than repaired, per AGENTS.md §9: no reset, no further amend.

## Possible code bugs (comment right, code wrong)

_(filled in as found; see summary)_

### CB-1. `umem_impl.h:167-168` says "Nothing reset it"; `umem.c:3259` resets it on every CPU-layer magazine reload

`umem_impl.h:150-183` (commit 20999ee, 2026-09-23) states, of the cached CPU
hint: "The old comment here claimed the hint was 'reset on magazine reload to
detect CPU migration'. Nothing reset it." and later "So: read once, correctly,
and cache."

`umem.c:3228-3260` `umem_cpu_reload()` ends with `reset_cpu_hint_cache();`
(defined `umem_impl.h:244-248`, sets `cached_cpu_hint = -1`). It has eight
callers, all in `_umem_cache_alloc`/`_umem_cache_free` and their `_batch`
variants (umem.c:3352, 3370, 3470, 3484, 3581, 3599, 3704, 3718). So the hint
IS reset, on every CPU-layer magazine exchange, and the next `CPU(mask)` call
re-derives it via rseq `cpu_id` or `sched_getcpu()`.

Before 20999ee the reset was invisible because the re-derived value was again
`pthread_self() & mask == 0`. After 20999ee the reset is live: the hint tracks
migration at reload granularity, which is the behaviour the 2026-04-08 comment
described and the 2026-09-23 comment says does not exist and is not wanted
("read once ... and cache"; the 5 % t=1 cost cited is for re-reading on every
call, not per reload, so the actual cost of this reset is unmeasured).

Which is right is a decision for `@hot`: either delete the reset at umem.c:3259
(and `reset_cpu_hint_cache()`, now single-use) so the code matches "read once",
or keep it and correct umem_impl.h to say the hint is re-derived per CPU-layer
reload. Either way the "Nothing reset it" sentence is false as written and the
record should say so rather than be deleted (§7: nothing softened).

### CB-2 (lower confidence). `umem_inspect.c:880-889` bounds depot magazine reads by `cp->cache_magtype->mt_magsize`, not the magazine's own capacity

`umem.c:2213-2225` (`umem_mag_capacity`) says: "Use this, never
cp->cache_magtype->mt_magsize, to bound indexing into a magazine already in
hand (P1.3b)". `umem_inspect.c:cached_set_build_cache` reads every magazine on
`cache_full.ml_list` and `cache_depot_full[i].ml_list` for `magsize` rounds
where `magsize = cp->cache_magtype->mt_magsize`. After a resize, stale
smaller-capacity shells are still on those lists until popped and destroyed
(`umem_depot_destroy_stale`), so this reads past a 127-round shell as if it had
255. It runs under `cache_lock_all`, so no concurrent resize, but the stale
shells are already there. Effect: over-read into the next object (a magazine
shell is a slab object; the bytes past it are another shell or slab metadata),
adding garbage addresses to the cached set -- the cached set then wrongly
classifies whatever those garbage values happen to equal as CACHED. Not a
crash in the common case; a reporting error. The rseq branch (925-927) clamps
to `magsize` too, same issue. Verify against `umem_mag_capacity` by reading the
shell's own slab; the inspect file already has `umem_impl.h`.

## umem.c

In-scope comment lines: 1185 (blame, 2y). Read every hunk.

### Top-of-file block (lines 36-409): numbered claims vs code

Only 45-53 (Nuances) was touched in the window; the rest is the 2008 Sun text
and is listed here only because the brief asked for it to be checked claim by
claim. "old" = out of scope, pre-existing.

| § | Claim | Verdict |
|---|---|---|
| Nuances 45-53 | reclaim on by default; update pass reaps depot excess; slab reclaimer advises after reclaim_delay, destroys after 2x | TRUE: umem.c:599-600, 4728, 4789, 4538-4546 |
| 1 | "seven major areas of divergence" | Lists 7. OK |
| 2.1 | umem_startup is .init; calls pthread_atfork | TRUE via pthread_once -> umem_forkhandler_init -> umem_fork.c:368 |
| 2.2 | four paths into umem_init | old; plausible, not re-verified |
| 2.2.2 | umem_null_cache has 1-element cache_cpu, mask 0 | TRUE (umem.c:762-775) |
| 3 | "umem uses _lwp_self() as its hint" | FALSE (old). Hint is rseq cpu_id / sched_getcpu / thr_self>>12 (umem_impl.h:199-236). Whole §3 needs rewriting; not in scope but the new hint comment lives in umem_impl.h and nothing in this header points at it |
| 4.3 | umem_reap "at the time of heap growth" schedules UMU_REAP | Now also: update pass requests UMU_REAP on ws excess (4728), and umem_reap() returns early IN_UPDATE() (4913). Header silent on both |
| 4.4 | update thread created by umem_reap when multithreaded | INCOMPLETE: since 6103-6130 umem_init() creates it unconditionally. Header says nothing |
| 4.6 | fork handlers "lock up every mutex in every cache" | TRUE; umem_fork.c:84-104 |
| 6 | lock order: umem_cache_t's cc_lock, then ml_lock, then cache_lock | AGREES with umem_fork.c and with code (cc_lock held while umem_depot_alloc blocks on ml_lock: 3369). Omits: sbrk_faillock, vmem_segfree_lock, interposer locks (umem_fork.c:163 says those come BEFORE umem_init_lock), umem_ptc locks |
| 7 | UMEM_MAXBUF: update umem_alloc_table | old; not re-verified |

### Table

| line | class | comment excerpt | finding |
|---|---|---|---|
| 45-53 | 6 | "Returning memory to the OS ... umem_reap() remains available" | Nuances section corrected in place, says what changed and what now happens; matches umem.c:1911,4728,4789 |
| 190 | 1 | "Currently, umem uses _lwp_self() as its hint." | Pre-existing; now rseq cpu_id / sched_getcpu / thr_self>>12 (umem_impl.h:199). Out of scope (old) but the §3 block is now wrong end to end |
| 516-519 | 2 | "Size classes with ~1.25x spacing ... worst-case waste under ~25%" | Arithmetic claim only, no box/sha; acceptable as design note. Table shows 8->16 is 2x so "under ~25%" is false below 64 B. Reword or delete |
| 546-549 | 3 | "A 64-byte object now gets 127-slot magazines instead of 15-slot" | "now" is history-as-comment; table itself is the fact. Delete second sentence |
| 570-580 | 6 | umem_depot_steal_max: names the path, the cost, the reason, ponytail: with upgrade path | Model tunable comment |
| 599 | 3 | "background page reclamation via madvise" | Fine; terse tunable label |
| 650-655 | 6 | CPU(mask): names the bug and points at the measurement | Good cross-reference |
| 666-669 | 1 | "Per-thread cached CPU hint to reduce CPUHINT() syscall overhead. Initialized to -1 to force refresh" | CPUHINT() is pthread_self (no syscall); purpose is spread, not syscall cost -- contradicts umem_impl.h:150-183. Reword to point at get_cached_cpu_hint |
| 672-686 | 6 | umem_cpu_node table: states index range, sizing rule, OOB hazard, fallback | Good |
| 903-914 | 3 | "not a new hot-path cost. Safe to call from a benchmark after a run." | "Safe" without saying why (holds umem_cache_lock; walks list). Replace: "Takes umem_cache_lock; counters are read racily, totals are approximate" |
| 1116-1120 | 1 | "Deliberately weak ... so the debugger hook stays callable even when UMEM_INSPECT_EVENTS is off" | umem_inspect.c:98 defines it noinline,used -- NOT weak. Call at 1133 is unconditional so "stays callable" is true; delete "weak" |
| 1445-1460 | 6 | umem_link_cookie: zero sentinel, idempotence argument, ordering via cache_lock | Example of a publication-order comment done right |
| 1480-1486 | 6 | AT_RANDOM: why it is a pure function of the image | Good |
| 1514-1540 | 6 | umem_slab_link_valid: what is checked, what is not, ponytail: with ceiling and upgrade | Good |
| 1569-1571 | 6 | slab coloring "Protected by cache_lock" | Minimal but states the lock |
| 1704-1708 | 3 | "Prefetch slab metadata ... Medium locality (2)" | Restates __builtin_prefetch args. Delete |
| 1732-1735 | 3 | "Skip slabs being reclaimed (madvise in progress)" | Acceptable; but see 1761: state machine lives under cache_lock, and SLAB_RECLAIMING is set... verify |
| 1761-1776 | 6 | Reactivate idle slab: names the invariant the reclaim side maintains and the lock | Good; cites umem_slab_keeps_metadata() |
| 1788-1808 | 6 | Corrupted link: what is truncated, why refcnt is put back | Good |
| 1913-1916 | 6 | SLAB_DIRTY hand-off to update thread | OK |
| 2054-2058 | 3 | "Fast path: Use SIMD ... This optimization helps" | Confidence. Replace with "all-NULL magazines skip the loop" or delete |
| 2066-2071 | 3 | "Prefetch next 4 slots ... Low locality (1) ... helps pipeline" | Restates args. Delete |
| 2097-2099, 2109-2114 | 3 | "Uses C11 acquire ordering for safe publication" | Only 2 callers of these helpers remain; comment fine but "safe" is bare. Minor |
| 2126-2131 | 1 | "Select depot stripe based on thread ID and NUMA node. When NUMA is enabled, stripes are partitioned by node so threads on the the depot is a cold path" | Orphaned comment: 9640329 deleted the function, left a spliced header with no body under it. Delete |
| 2143-2148 | 2 | "saving ~50-100ns on the critical path" | Number with no box/sha. Delete number or cite |
| 2197-2205 | 6 | umem_mag_source_cache: why cache_magtype cannot be trusted | Good |
| 2213-2225 | 6 | umem_mag_capacity: ties to umem_cache_init 5585, states P1.3b hazard | Good |
| 2233-2249 | 6 | probe: "67 million samples across 96 threads on a 192-vCPU arm-hi box hit it zero times" | Names box and count; lacks sha. Minor |
| 2252-2272 | 6 | why the probe blocks; "holds no allocator lock" with the reason | Good |
| 2274-2289 | 6 | P1.3c ledger | Good |
| 2344-2367 | 6 | ledger WHERE/WHAT: records the false positive and the earlier wrong validation | Exemplary un-softened record |
| 2396-2405 | 6 | umem_ptc_mag_check: states the invariant and why it is placed at refill/flush | Good |
| 2417-2421 | 3 | "Never blocks on a mutex, eliminating p99 latency spikes" | Confidence claim with no measurement. Delete second sentence |
| 2469-2473 | 3 | "Tries per-CPU depots then global depot, all with trylock" | Fine (matches code) |
| 2502-2516 | 2 | "perf showed 82% of frag CPU in pthread_mutex_trylock" | No box/sha/date. Cite docs/results or drop the number |
| 2546-2549 | 3 | "Falls back to blocking push only if trylock fails on all targets" | Matches code (2575). OK |
| 2579-2598 | 1 | "Lock ordering: ... depot locks (ml_lock) are below cache_lock in the hierarchy. Holding cache_lock while acquiring ml_lock would invert the order" | (a) Block is detached: umem_depot_destroy_stale sits between it and umem_depot_alloc (2635). (b) "ml_lock below cache_lock" contradicts umem_fork.c:66 "cache_lock is below both"; fork file's sense (ml then cache) matches the code. Fix wording to "ml_lock is acquired BEFORE cache_lock" |
| 2600-2609 | 6 | umem_depot_destroy_stale: who produces stale magazines, what is_full means | OK, states contract |
| 2639-2643 | 6 | UMEM_CPU_NODE guard: sizing rule and fallback | OK, but macro is #defined inside the function body between decls -- odd placement, not a comment issue |
| 2669-2711 | 6 | full-breadth steal: records the earlier wrong fix (8-stripe cap), the box (intel-hi), the numbers, the two docs/results files | Model un-softened record; only missing the sha |
| 2762-2768 | 1 | "Lock ordering: caller must NOT hold cp->cache_lock. See umem_depot_alloc() for rationale." | Rationale block is the detached one at 2579; same inverted "below" wording |
| 2789-2795 | 6 | umem_mag_drain: why `cap` clamps | Good |
| 2812-2835 | 6 | umem_ptc_mag_return: two invariants, at the shared function, names what used to happen | Exemplary; this is the §7 "fix at the shared function" pattern written down |
| 2858-2863 | 3 | "Falls back to blocking only for stale magazine destruction (rare, only during magazine resize)" | OK |
| 2886-2907 | 6 | umem_ptc_mag_prime: why the depot empty list starves, what this does, box+config+method for the 9% | Good; sha missing (it is a1901c8's own change) |
| 2921-2926 | 6 | "Flush all per-thread magazines back to depot" + hand-off via umem_ptc_mag_return | Correct now (the old "flush all bins" bug is gone: loop covers PTC_NBINS) |
| 2956-2968 | 6 | UNUSED rseq slowpath: points at three docs/results files and the repro | Good, but 2026-08-06 file does not exist in docs/results (only the v2). Fix the reference |
| 2996-3000 | 6 | same for free_slowpath | OK |
| 3039-3044 | 6 | umem_depot_ws_excess "Read without ml_lock: a stale answer costs one interval" | States the racy read and why it is tolerable |
| 3080-3083 | 6 | "Caller must hold mlp->ml_lock" | Minimal and right |
| 3111-3124 | 6 | ws_reap: why destroy runs outside ml_lock, the self-deadlock, the sibling pattern | Good |
| 3147-3160 | 3 | UMEM_DEPOT_PERCPU_MAX / mark_excess header: "avoids destroying magazines that are still in the active working set" | Header still describes the intent the body says was inverted; body comment (3169-3181) corrects it in place. Header should point down |
| 3169-3181 | 6 | "Do NOT lower ml_min here ... (measured: 5 MB in 100 s; ~40 minutes to drain)" | Un-softened record of the inverted clamp. No box/sha for the 5 MB figure |
| 3232-3234 | 6 | "This function is always called under cc_lock." | States the lock. OK |
| 3241-3245 | 3 | "Prefetch cc_ploaded ... High locality (3) since we frequently swap" | Restates args. Delete |
| 3255-3258 | 1 | "Reset CPU hint cache on magazine reload to detect thread migration." | CODE DOES THIS (reset_cpu_hint_cache() at 3259, 8 callers of umem_cpu_reload). umem_impl.h:167-168 (20999ee, this week) says "Nothing reset it" and 178-183 says "read once ... and cache". Both cannot be true; the code resets on every CPU-layer reload. See top section |
| 3280-3285 | 3 | "RSEQ fast path: true lock-free per-CPU magazine access ... when we own the rseq registration (not glibc)" | Confidence ("true lock-free"). Also: AGENTS.md §6 says this asm "still serves zero magazine hits" -- if that is current, the comment should say what feeds cache_rseq magazines (nothing: the slowpaths at 2956/2996 are UNUSED). Reword to state what the path can and cannot serve |
| 3322-3326, 3559-3563 | 3 | "Prefetch the loaded magazine ... High locality (3) since we access this frequently" | Restates args. Delete |
| 3329-3330, 3566-3567 | 3 | "Decrement rounds. We hold the lock so this is safe." | Narration + bare "safe". Delete |
| 3430-3438 | 6 | alloc_batch: "Callers must not re-invoke the constructor" | States a contract |
| 3617-3621 | 3 | "Initialize the new magazine with SIMD ... efficiently using vectorized stores" | Confidence. Replace: "all slots NULL" or delete |
| 3800-3809 | 3 | "avoiding a pipeline stall on PTC miss under contention" | Perf rationale without a number; acceptable as design intent but "avoiding" is a claim. Soften to "so the load can overlap the PTC check" |
| 3874-3882 | 6 | retire empties: "Both are empty here (rounds == prounds == 0)" and delegates the stale case | Good |
| 3894-3903 | 6 | capacity from the magazine, not cache_magtype (P1.3b) | Good |
| 3997-4005 | 6 | "bin_table encodes -1 for debug caches, so PTC never bypasses buftag validation" | States the invariant that keeps debug correct |
| 4072-4080 | 6 | hand over the count with the magazine (P1.3c) | Good |
| 4113-4118 | 6 | "loaded was already donated, so fall through with none" | States resulting state |
| 4213-4219 | 1 | "We don't call it here because reclaim_pages drops and reacquires cache_lock internally, creating a race window" | Stated reason is now wrong: 4513-4530 says the walk no longer spans a lock drop (single pass under lock; drops once after). The decision (reap does not call reclaim_pages) may still be right, but the reason is stale. Also arrow glyph |
| 4221-4224 | 3 | "Reap the magazine shell cache too" | OK |
| 4379-4419 | 6 | umem_slab_keeps_metadata: what is inside the buffer region, why a zeroed page breaks each, ponytail: with upgrade path | Exemplary |
| 4427-4434 | 6 | umem_slab_reclaim "Advises only; the caller publishes SLAB_CLEAN under cp->cache_lock" | States ownership of the state write |
| 4445-4465 | 6 | madvise whole-page rule: records the old bug (metadata page discarded; EINVAL with colour) | Good record, un-softened |
| 4486-4501 | 1 | "Must be called with cp->cache_lock held. Drops and reacquires the lock around madvise and slab destroy calls." | Second sentence is the pre-fix behaviour; code (4513-4530, 4561) drops ONCE after the walk. Also "Serialized per cache by umem_cache_lock: every caller reaches here through umem_cache_applyall" -- true (only caller 4791 in umem_cache_update, under applyall's umem_cache_lock at 896) |
| 4513-4530 | 6 | single-pass rationale; records the old SEGV | Good |
| 4578-4590 | 6 | INVARIANT: slab_state only under cache_lock; records the prior unlocked publish as a data race | Exemplary |
| 4600-4620 | 6 | umem_cache_drain_slabs: why needed, preconditions (no locks, off list, magazines purged), what is left alone | Exemplary |
| 4705-4727 | 2/6 | "Measured: 2 GB of freed 4 KiB objects 100 % resident at t = 100 s, 16,911 full depot magazines" | Good record; no box/sha/date (docs/results has the sustained-load files; cite) |
| 4746-4754 | 3 | "Compute alloc_ops by summing per-CPU cc_alloc counters instead of using an atomic counter on the hot path" | OK, states why |
| 4786-4788 | 3 | "Reclaim pages from idle slabs." | Narration; the lock is visible. Fine |
| 4890-4912 | 6 | reap-from-update self-deadlock: the stack, why latent, why dropping is within contract | Exemplary |
| 5181-5187 | 2 | "4 MiB slabs cost 57x fewer at zero measured RSS cost" | No box/sha; umem_impl.h:759 cites docs/results/2026-09-22-umem-heap-ceiling-vma.md -- point here too |
| 5192-5202 | 6 | span-density floor: what best-fit is blind to, the ceiling, the exceptions | Good |
| 5275-5282 | 6 | per-CPU arrays from one mapping (P6.4), alignment invariant, ncpus power of 2 | Good |
| 5294-5305 | 6 | "The first version of this consolidation (e00fdf2) used one mmap() per cache ... 2,000 destroys left 940 orphaned VMAs" | Records the wrong first fix with sha and numbers. Exemplary |
| 5401-5409 | 6 | rseq magazine rounds returned before drain_slabs; names the old mislabel | Good |
| 5429-5435 | 6 | drain_slabs ordering vs descriptor free | Good |
| 5884-5886 | 3 | "Initialize NUMA support if available" | Narration. Delete |
| 5899-5905 | 3 | "true lock-free per-CPU magazine access with zero synchronization overhead. Falls back gracefully" | Confidence x3. Replace: "Registers rseq; umem_rseq_enabled/asm_safe gate the asm path in _umem_cache_alloc/free" |
| 5985-6001 | 6 | CPU->node table sizing | OK (duplicates 672-686; one could point at the other) |
| 6068-6071 | 1 | "This provides zero-lock access for sizes 8-448 bytes." | umem_ptc_maxsize defaults to 8192 (umem_ptc.c:45). 448 is years stale |
| 6077-6088 | 6 | P5.2 UMEM_PROFILE gate: states why the second check is not redundant | Good security comment: names what it covers |
| 6103-6126 | 6 | update-thread start: records that "background" features were dead, how the tests hid it (repro drove the pass by hand), why here, what failure means | Exemplary |

## umem_inspect.c

In-scope comment lines: 441. This file is the best-commented in the tree: the
C1-C5 contract block (156-206) is referenced by name from every locked region,
and each two-phase site says which phase it is and what lock it holds.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 35-41 | 6 | "umem_rseq.h DEFINES UMEM_RSEQ_AVAILABLE; it does not consume it ... silently compiled out the rseq magazine subtraction" | Records a real bug with the mechanism. Good |
| 70-75 | 3 | "Keep the function bodies trivial so the linker doesn't constant-fold them" | Confused: linkers don't constant-fold; the noinline,used attributes + asm barrier at 98-104 do the work. Reword or delete |
| 120-121 | 3 | "one cache-miss callout" | Perf claim, harmless. OK |
| 156-206 | 6 | C1-C5 contract: which lock protects what, what is consistent, what may be torn, debugger caveat | Exemplary. Verified C1 against for_each_cache (227-233) and collect_log (637-688): both hold umem_cache_lock whole-walk |
| 208-211 | 6 | UMEM_SNAP_TRIES ponytail: with upgrade path | Good |
| 217-220 | 6 | for_each_cache: lock + visitor rules | Good |
| 236-245 | 1 | cache_lock_all: "in THE ONE TRUE LOCK ORDER documented in umem_fork.c: per-CPU cc_lock ascending, then the depot maglist locks, then cache_lock" | Order agrees, but it takes cache_full.ml_lock and cache_depot_full[i] ONLY -- not cache_empty / cache_depot_empty. Fine for reading full lists, but comment says "Take every lock of one cache". Change to "every lock this read needs" |
| 276-277 | 6 | audit_record_size "fixed once umem_stack_depth is frozen; that happens during umem_init()" | States when the value is stable |
| 287-292 | 6 | cache_has_audit: why UMF_BUFTAG does not matter | Good |
| 296-300 | 6 | slab_buf_is_free: mangled links (P5.4) | Good; 316 demangles. Hash-chain walks at 452/1653 use raw bc_next, correct since only slab freelist links are mangled (umem.c:1640,1906) |
| 322-323 | 3 | "from a bufctl that the cache believes to be live" | OK |
| 358-365 | 6 | two-phase collection; pre-sizing under same locks | Good |
| 390-396, 411-413 | 6 | snap_collect: C2 reference at the exact realloc | Good |
| 425-428 | 6 | collectors run under umem_cache_lock, take own locks | Good |
| 506-508 | 6 | cache_is_user_visible: why QCACHE double-counts | Good |
| 543-549 | 6 | public walker contract | Good |
| 581-605 | 6 | log walk LOCKING: lh_lock protects rotation only; ponytail: with upgrade | Exemplary |
| 631-636 | 6 | umem_cache_lock outside lh_lock, cites the order in umem_fork.c, why held for the whole scan | Good |
| 738-753 | 6 | cached set STORAGE: why flat array; records the calloc-under-lock deadlock | Good |
| 846-870 | 6 | cached_set_build_cache: covered sites, NOT covered (PTC), locking per layer; rseq best-effort direction of error stated | Exemplary. One gap: uses cp->cache_magtype->mt_magsize as cap for depot magazines (883-889) -- same P1.3b hazard umem.c:2213-2225 warns about; a stale (smaller) shell would be over-read. Under cache_lock_all the resize cannot run concurrently, but pre-existing stale shells on the lists can. Flagged as possible code issue (CB-2) |
| 914-918 | 6 | rseq magazines "previously NOT subtracted" | Records the old defect |
| 952-964 | 6 | cached_set_has_unaccounted: what is missing and why; ponytail: | Good |
| 1106-1111, 1233-1239 | 6 | "outstanding allocations, not leaks" framing | Good |
| 1149-1173 | 6 | Phases 1-3 with lock state per phase; records the unlocked walk bug | Good |
| 1440-1447 | 6 | status_dump used to fprintf under both locks | Records old bug |
| 1468-1478 | 6 | cache_alloc_ops_now: why field is stale in default build (umem.c:4763 only under umem_magazine_tuning); "approximate by construction (C4)" | Good |
| 1565-1570 | 6 | INUSE semantics | Good |
| 1664-1670 | 6 | prefer user-visible caches over qcache | Good |
| 1707-1718 | 6 | CACHED vs ALLOCATED; records that UMEM_BUF_CACHED was never returned | Good |
| 1863-1886 | 6 | v2 on-disk format | Good spec comment |
| 1954-1959 | 6 | snapshot_collect_cache used to realloc under lock | Good |
| 2076-2079, 2186-2187 | 6 | P5.3: umem_open_write instead of fopen; says where `path` comes from | Good security comment; misc.c:229-237 confirms O_NOFOLLOW + fstat S_ISREG/nlink/euid |

## umem_introspect.c

In-scope comment lines: 401. Contract blocks B1-B5 (79-114), A1-A3
(1065-1084), P5.6 (918-957). Every claim checked against the code below it.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 22-36 | 3 | "ZERO-COST WHEN DISABLED ... the single hot-path check in _umem_alloc predicts not-taken" | Cost claim; the check is at umem.c:3813 (`!umem_introspect_break_armed`) -- a TLS-free global load per alloc, not zero. Say "one predictable load" |
| 79-114 | 6 | B1-B5 | Verified: B1 all brk_* under brk_lock (653, 676, 700); B2 armed set last in break_publish_locked (150-156) -- correct only if callers fill state first, they do (not shown; 600-640); B3 generation compare 748-760; B4 706-707; B5 763-787 called from umem_fork.c:314 |
| 117-118 | 6 | single predicate ponytail: | Good |
| 125-129 | 6 | "brk_lock is a LEAF lock ... must never be taken while holding an allocator lock" | States rule. Note realloc under brk_lock at 658 allocates while holding a leaf that the ALLOC path takes -> alloc path: brk_lock is taken AFTER allocation completes (break_check is called with buf in hand), so realloc->malloc->_umem_alloc->break_check->brk_lock would recurse only if break_armed; cmd_sig_add runs on server thread which B4 exempts. OK, but the comment should say this is why B4 also protects against self-deadlock on the realloc |
| 158-178 | 6 | CACHE LIFETIME + NOTE ON OUTPUT: two rules, and a ponytail: to unify | Good. Verified: every walk 292-509 holds umem_cache_lock throughout |
| 225-240 | 6 | is_allocated: mangled freelist, "Only the SLAB freelist is mangled", bounded iteration reason | Good; "umem_impl.h's UMEM_LINK_MANGLE comment lists this function as a reader" -- verify (see umem_impl.h section) |
| 455-460 | 6 | "held", not "allocated" | Good |
| 465-472 | 6 | walk_leaks: why cache_lock is dropped around the callback, with the lock-order reason | Good; matches 498-504 |
| 498-501 | 6 | "the slab cannot be destroyed while it holds this held buffer" | True: slab_refcnt > 0 blocks destroy (umem.c:4534) |
| 653-657 | 6 | realloc under brk_lock (B1) | Good |
| 678-680 | 6 | disarm under same acquisition as bump | Good |
| 687-693, 700-703, 748-753 | 6 | break_check: B4, B2, B3 cross-refs at the exact lines | Good |
| 766-777 | 6 | fork child reset: inherits armed + once, not thread; when it runs | Good; umem_fork.c:308-315 agrees |
| 817-827 | 6 | logtail: poll not hook, ponytail:, DISCONNECT DETECTION records the wedge | Good |
| 918-957 | 6 | P5.6 socket location: attacker position (other local user), both demonstrated failure modes, the test, the two-step fix, why the name stays predictable, the secure-mode gate | Exemplary security comment |
| 1006-1009, 1022-1027 | 6 | XDG only after check; mkdir atomicity is the defence | Good |
| 1065-1084 | 6 | A1-A3 | A1/A3 verified (1197-1210 lstat; rebind_over_stale). A2 "only the EFFECTIVE uid, or root" matches 1101 |
| 1086-1100 | 6 | P5.7: names the setuid attacker position, what the real uid could have done | Exemplary |
| 1122-1125 | 6 | no SO_PEERCRED: "say so rather than pretending otherwise" | Good |
| 1131-1147 | 6 | rebind_over_stale: why rename not unlink, what an attacker can achieve at worst | Good |
| 1241-1253 | 6 | SIGPIPE blocked per-thread, not SIG_IGN; why | Good |
| 1282-1293 | 6 | ONE CLIENT AT A TIME, the consequence for "continue", ponytail: | Good honest ceiling |

No CLAIM MISMATCH found in this file.

## umem_impl.h

In-scope comment lines: 290.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 137-139 | 3 | "Prefetch macros for performance optimization" | Narration. Delete |
| 149-186 | 1/6 | CPU-hint block (20999ee) | The bug description, the measurement (8-vCPU, 8 threads, 2560 B, 7-of-8 on slot 0) and the 5 % re-read cost (4.16->3.94 Mops, median of 7, alternating) are the standard the brief asks for, minus the sha. BUT "Nothing reset it" (167-168) and "read once, correctly, and cache" (183) are contradicted by umem.c:3259 -- see CB-1 |
| 189-193 | 3 | "rseq integration for CPU hint caching. When rseq is available and registered, we read the kernel-maintained cpu_id" | Redundant with 169-171; delete |
| 240-243 | 1 | reset_cpu_hint_cache: "Called during magazine reload to detect CPU migration." | TRUE of the code (umem.c:3259) and directly contradicts 167-168 in the same file, 70 lines above. One of the two must go (CB-1) |
| 346-384 | 6 | UMEM_LINK_MANGLE: what it protects, the glibc comparison, "This covers the SLAB FREELIST use of bc_next only", every in-tree reader listed, the control-arm build flag | Exemplary. Reader list verified: umem.c 1652 (build unwind), 1682 (create), 1796 (alloc), 1906 (free), destroy: umem_slab_destroy walks via bc_next? -- 4366 is hash rescale (plain, correct). umem_inspect.c 316, 500; umem_introspect.c 245. List is accurate |
| 414-420 | 6 | SLAB_* states | OK. Note 417: SLAB_RECLAIMING "do not allocate" matches umem.c:1736 |
| 430-432 | 6 | slab_state/idle_time/reclaim_next field comments | OK; "seconds empty (approx)" -- it is incremented by umem_reap_interval per pass (umem.c:4538), so "approx" is right |
| 468 | 4 | cc_lock "protects slow path (magazine reload)" | Understates: cc_lock protects cc_rounds/cc_loaded/cc_ploaded/cc_magsize on every non-rseq/non-PTC alloc and free (umem.c:3313-3330), not only reload. Reword: "protects every cc_* field; held across depot calls (see umem_fork.c order 6a)" |
| 485-497, 508-513, 539-548 | 1 | umem_tagged_ptr: "for lock-free stack operations ... Use umem_tagged_ptr_check() at init time" | Dead API: no caller of umem_tagged_ptr_check in umem.c; the depot moved to mutex lists (9640329). umem.c:2096-2124 keeps two helpers with zero callers. Comment describes a design that no longer exists in the file. Delete the block or mark UNUSED like umem.c:2956 does |
| 558-562 | 3 | "Each list is protected by its own mutex for simple, correct locking. The depot is a cold path" | "simple, correct" is confidence. Keep "ml_lock protects ml_*" only |
| 565-569 | 6 | ml_* field comments | OK |
| 610-613 | 1 | cache_mag_reloads "total magazine reloads" | Never incremented anywhere in the tree (only read: umem.c:4765, umem_profile.c:314, umem_introspect.c:301,377; test_umem_stats.c:409 says "not yet implemented", SKIPs). Field comment should say so, or the field should go. umem_introspect's `mag_reloads` line and umem_profile's report print a permanent 0 |
| 663-680 | 6 | per-CPU arrays from one mapping: the footprint numbers (12 KB of 18.6 KB; 29,159 VMAs from 50k destroys) | Good record; no box/sha |
| 689-695 | 3 | rseq layer field | OK |
| 699-702 | 3 | cache_numa_info "NUMA-aware depot info" | Only ever NULL (umem.c:760, no writer). Same class as cache_mag_reloads: dead field with a live-sounding comment |
| 707-708 | 3 | "cache-line aligned to prevent false sharing" | Fine |
| 747-784 | 6 | UMEM_MIN_SLAB_OBJECTS: the mechanism, the measured VMA count (65,532; 64,270 of 64,275 mprotects 4096 B), cites docs/results/2026-09-22-umem-heap-ceiling-vma.md, PORTABILITY argument with the check | Exemplary; "Measured on a small workload before/after in the commit that introduced this" (782-783) should name the commit |
| 788-832 | 6 | UMEM_MIN_QCACHE_SLAB: "the half the first one missed" -- records that the first fix was incomplete; illumos nqcache = 0 argument | Exemplary un-softened record |

## malloc_interpose.c

In-scope comment lines: 479. Very high standard throughout: ownership,
lifetime and lock rules are stated per structure and the two "the old comment
claimed X, it was wrong" corrections (476-495, 599-620) are kept in place.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 23-36 | 1 | "recommended in docs/PTHREAD_RESEARCH.md section 3" | File does not exist (docs/ has plans/, results/, reviews/, UMEMCTL.md). Dead reference; drop it |
| 99-122 | 6 | POINTER OWNERSHIP: four owners with LIFETIME per owner, "single classifier all three use" | Exemplary. Verified: free (717), realloc (855ff), malloc_usable_size (771) all call interpose_owner_of |
| 131-147 | 6 | static bump buffer: PERMANENT, why (P1.1), THREAD SAFETY: which lock, and why in_dlsym needs none | Exemplary |
| 162-165 | 6 | "never wraps, never reuses" | Good |
| 212-230 | 6 | libc pointer tracking: INVARIANT "recorded or handed to libc_free, never live-and-unrecorded"; records the append-only exhaustion bug | Exemplary |
| 239-266 | 6 | libc_ptr_live: box (c7i.metal-48xl), method (multi 16:64, null sd 3.75 %), numbers (3.0 -> 0.8 Mops/s vs 398/421), why the relaxed read is sound with the ordering argument | The best measurement comment in the tree. Missing only the sha |
| 269-289 | 6 | fork participation: which locks, why re-init not unlock, why the copied data is consistent | Exemplary; umem_fork.c:150-170 agrees |
| 309-315 | 6 | calloc_depth after fork | Good |
| 318-321, 348-352, 384-387 | 6 | track/is/untrack contracts ("caller MUST NOT return the pointer", "WITHOUT releasing the record") | Good |
| 334, 401 | 6 | "Count up BEFORE the pointer becomes findable" / "Count down AFTER" | Good: the ordering argument at the exact store |
| 362-366 | 6 | fast path: "is the whole fix for the negative thread scaling" | Good |
| 430-435 | 6 | interpose_owner_of contract | Good |
| 473-495 | 6 | calloc_depth per-thread; "The old comment claimed __thread could not be used ... That is true of the general-dynamic model ... not true of initial-exec" | Exemplary correction-in-place |
| 498-521 | 6 | calloc_zero_fill: GCC memset->calloc rewrite, the observed symptom (jmp calloc@plt, 100 % CPU, zero syscalls), why the pre-fix escaped | Exemplary |
| 544-565 | 6 | constructor: "Cross-.so note: numbered constructor priorities only order constructors WITHIN a single shared object ... The numeric priority is therefore a no-op across the boundary" | Corrects the IMPORTANT paragraph above it in place. Good, though the first paragraph (548-550) still asserts "We use priority 101 to ensure this runs first" -- reads as if both are true. Tighten: fold the correction into the claim |
| 574-577 | 6 | umem_malloc_is_interposing disables backtrace | Verified umem_stacktrace.c:147-152 |
| 585-621 | 6 | umem_abort = 0: "THIS IS NOT 'THE SAME AS GLIBC', and the comment here used to say it was"; what P5.8 changed; "abort=1" never existed and why | Exemplary. envvar.c:280 confirms "abort" is ITEM_FLAG |
| 625-629 | 3 | "Don't call umem_init() here - let it initialize naturally" | Duplicates 561-564. Delete one |
| 638-657 | 6 | malloc: dlsym static path; BOOTSTRAP->READY handoff | Good |
| 695-709 | 6 | free fast path: why one decode instead of two; which checks must stay first; why skipping pre-classification loses no safety (P5.8) | Exemplary |
| 719-723, 729 | 6 | OWN_STATIC no-op by design; OWN_LIBC release only as handed back | Good |
| 741-750 | 6 | unknown pointer disposition, cross-ref to why weaker than glibc | Good |
| 760-767 | 6 | malloc_usable_size must be interposed, why | Good |
| 778-789 | 6 | calloc: two consequences required by P1.1 | Good |
| 806-809, 820-824 | 6 | calloc static/recursion paths | Good |
| 844-851 | 6 | realloc ownership rule (P1.7c) | Good; verified: untrack happens in free(ptr) after memcpy (912-916) |
| 884-891 | 6 | OWN_UNKNOWN in realloc: why not guess length | Good |
| 935-943 | 6 | memalign bootstrap: why the pointer MUST be recorded | Good |
| 958-963 | 6 | last-resort bootstrap alignment limit | Good |
| 980-988 | 6 | posix_memalign: return the error, don't read errno | Good |
| 1019-1026 | 6 | aligned_alloc must be interposed; C11 UB accepted as glibc does | Good, names the glibc comparison |

One CLAIM MISMATCH (dead doc reference), no code-vs-comment conflicts.

## malloc.c

In-scope comment lines: 194.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 58-69 | 3 | "Based on jemalloc's arena 0 and tcmalloc's Arena pattern" | Provenance, not invariant. Harmless; the three bullets are fine |
| 90-94 | 6 | checked header addition: names the pre-fix failure (SIZE_MAX -> 15-byte total) | Good |
| 179-181 | 3 | "NOTE: malloc/free interposition is now handled by malloc_interpose.c ... The weak symbol pragmas have been removed." | History-as-comment. Delete |
| 184-188 | 1 | "On Linux, the PTC genasm code sets these function pointers after generating the per-thread-cache assembly. malloc() and free() check these" | No such pointers exist below the comment (it introduces nothing); genasm is not in this tree (umem.c:611 umem_genasm_supported = 0; only umem_ptc_bench.c mentions it). Delete |
| 190-196, 728-734 | 1 | "This is the function that PTC falls back to ... On non-x86, malloc is a weak alias to this function." | No weak alias exists (grep: no `weak` attribute/pragma in malloc.c); malloc() is defined in malloc_interpose.c and calls umem_malloc. Rewrite: "Called by malloc_interpose.c's malloc() once READY" |
| 206-210, 214-218 | 3 | "Based on jemalloc's arena 0 pattern"; "initial-exec TLS for single-instruction access" | Fine; second one states why |
| 282, 369-371 | 3 | "NOTE: calloc() is now in malloc_interpose.c" etc. | History; delete or keep one pointer at the top |
| 373-409 | 6 | umem_may_own: attacker position implied (foreign/wild pointer under LD_PRELOAD), WHAT IT IS (necessary not sufficient), WHY A CACHED HULL, monotone-widening argument, NO LOCK OF ITS OWN and why, ponytail: with upgrade | Exemplary security comment |
| 425 | 6 | "Widen lo downward / hi upward, never the other way" | Good |
| 448-453 | 1 | "vmem_walk() calls the callback with the arena lock held, so the callback must not allocate -- it does not." | Half-true: vmem.c:1440-1447 holds vm_lock across func() only when VMEM_REENTRANT is NOT in typemask; malloc.c:463 passes VMEM_SPAN alone, so yes, held. Comment is right for this call; say "without VMEM_REENTRANT" so a future caller does not generalize |
| 470-473 | 6 | umem_may_own contract: false yes possible, false no not | Good |
| 490-493 | 6 | miss -> refresh -> retest | Good |
| 509-531 | 6 | VALIDATION ORDER steps 1-5, then what pre-fix did at each step | Exemplary; verified 571-576 (step 2), 683-696 (step 4), poison writes after validate |
| 541-545 | 3 | bootstrap check "to avoid misinterpreting their headers" | OK |
| 558-560 | 6 | "Set by the switch, acted on only after validation (step 5)" | Good |
| 602, 655 | 6 | which tag carries state / both cleared after validation | Good |
| 683-696 | 6 | Step 4 rationale | Good |
| 741-744 | 3 | "Check if this is a bootstrap allocation" | Duplicates process_free's own check at 541; harmless |
| 756-760 | 3 | "NOTE: malloc(), free(), calloc() ... are now in malloc_interpose.c" | Third copy of the same note. Keep one |
| 762-770 | 1 | "_malloc and _free are the PTC trampoline entry points ... after PTC genasm activates, malloc()/free() call through the generated code directly via function pointers" | Describes Solaris genasm which this port does not have. Nothing calls _malloc/_free through function pointers. Either "legacy ABI symbols" (as umem.c:610 says of umem_genasm_supported) or delete |

## umem_ptc.h

In-scope comment lines: most of the file.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 33-43 | 3 | "Provides a zero-synchronization fast path ... Zero synchronization for cache hit" | Design summary; "(similar to jemalloc ptc)" -- jemalloc's is "tcache". Minor |
| 44-70 | 6 | Footprint (P6.3) + "THE CAPACITIES ARE NOT A FREE VARIABLE": records the halving that was wrong, the 28x cliff with numbers (157/63/5.5 Mpairs/s at N=32/33/64), the fix sha a2177b9, and "The capacities were not revisited after that fix ... has not been measured" | Exemplary un-softened record; the honest "not measured" closing sentence is the standard |
| 72-79 | 2/6 | bins through 8192 for P8.2b: "x86 metal 105 -> 91 Mops from t=64 to 128, arm 277 -> 77" | Numbers with box class but no sha/date/file. Cite the docs/results entry |
| 81-90 | 6 | "PTC_NBINS bounds size_to_bin_table ... test_ptc_footprint's 24 KB" | Good; ties constants to their consumers |
| 92-95 | 6 | PTC_TOTAL_SLOTS "umem_ptc_get() asserts that" | Verified umem_ptc.c:313 |
| 106-111 | 3 | umem_ptc_mag "eliminating cc_lock contention ... without taking any lock" | Confidence; "Only depot refill/flush takes a lock" is right. Trim first clause |
| 113-134 | 6 | magsize/pmagsize MUST come from the magazine (P1.3b); loaded/previous can differ | Exemplary field comment |
| 137-141 | 6 | UMEM_PTC_MAG_HAS_PMAGSIZE for regressions that build against both structs | Good |
| 152-161 | 6 | slots set ONCE by umem_ptc_get; only slots[0..count) meaningful, not zeroed | Good ownership/lifecycle statement |
| 165 | 3 | low_water "for auto-tuning (future)" | Never written or read anywhere (grep). Dead field with a promise; delete field or say "unused" |
| 166-171 | 6 | one record per cache line: measured 2 % (5.76 vs 5.88, median of 9, alternating) | Method stated; no box/sha |
| 174-177 | 6 | "Everything before `pool` is zeroed at creation; `pool` is written only below each bin's count" | Good; matches umem_ptc.c:297-313 |
| 180-183 | 3 | alloc_count/free_count/hits/misses "statistics" | Never incremented anywhere (grep ptc->hits etc.: none). Dead fields; 32 bytes per thread with a live-sounding label |
| 206-210 | 6 | ptc_bin_capacity tiers | Matches |
| 232-241 | 3 | umem_ptc_alloc/umem_ptc_free "caller should use slow path" | These two are not called from umem.c (the fast path is inlined); only the .c defines them. Say "not on the hot path; kept for tests" or delete |
| 263-267 | 6 | umem_ptc_bin_flush_all "Only correct at thread exit ... would lose its only reference" | Good |
| 275-292 | 2/1 | SBO: "no locking (~3-5ns)"; "When the buffer is full, it resets and all outstanding pointers from the previous generation become invalid" | (a) ~3-5ns has no box. (b) The constraint is stated honestly, but this is an exported API whose contract is use-after-reset UB, called by nothing but test/unit/test_sbo.c. Flag for coordinator: dead public surface with a by-design UAF |

## umem_ptc.c

| line | class | comment excerpt | finding |
|---|---|---|---|
| 46 | 3 | "enabled by default for performance" | Delete rationale |
| 48-52 | 6 | thread_ptc initial-exec, non-static for inlining | Good |
| 62-70 | 6 | size_to_bin_table: index rule, zero-init hazard ("a zero entry would silently alias every size to bin 0"), the ready flag | Good |
| 88-96 | 3 | "Size classes we cache (matching umem's small size classes) ... first PTC_NBINS entries in umem_alloc_sizes" | LP64 table has three 0 padding entries (bins 25-27) so it is NOT "the first PTC_NBINS entries"; comment at 96 explains the padding but the header doesn't. Minor mismatch |
| 158-162 | 6 | disable PTC for debug caches, why | Good |
| 178-193 | 6 | bin_table via backing cache's object size: the 176-byte example, what the gap cost | Good record |
| 209-212 | 3 | "Map through the backing cache's object size" | Restates 178-193; fine |
| 232 | 6 | "Publish after all tables are fully populated" | States the ordering; ptc_table_ready is a plain int -- no release fence. Readers in umem.c use umem_ptc_bin_table directly gated on... nothing (umem.c:3812 reads bin_table without checking ptc_table_ready). Populated in umem_init before READY, so fine, but the comment implies a publish protocol that isn't one. Say "umem_init() completes before any allocation can reach the table" |
| 298-303 | 6 | carve pool once; not zeroed; why | Good |
| 318-324 | 6 | pthread_setspecific failure: decline PTC rather than lose objects at exit | Good |
| 423-439 | 6 | flush policy `all`: what each does and why at exit; where objects go | Good |
| 440-458 | 6 | P1.3a probe: why an in-library count instead of comparing outstanding counts (flakiness record) | Exemplary |
| 461-466 | 6 | P6.3 ledger | Good |
| 512-521 | 6 | thread exit batch: "600 acquisitions per exiting thread ... 16,000 simultaneous exits ... 37 ms" | Numbers without box/sha |
| 542-544 | 3 | "Used only by umem_ptc_destroy()" | True (grep) |
| 605-615 | 6 | "Drain every bin COMPLETELY ... (The old comment here claimed 'Flush all bins', which the code did not do.)" | Exemplary: the §7 example, corrected in place |
| 622-636 | 6 | why NOT an ASSERT (rc=134 with no output); why the sched_yield went away | Good |
| 639-640 | 6 | stranded count before unreachable | Good |
| 663-666 | 3 | SBO "disabled when any debug flags are active on the smallest cache" | Code checks global umem_flags, not the smallest cache's flags. Reword to "when umem_flags has any debug flag" |

## umem_fork.c

In-scope comment lines: 35-80, 148-183, 300-333. The lock-order comment was
checked claim by claim.

### THE ONE TRUE LOCK ORDER (35-80), claim by claim

| # | Claim | Verdict |
|---|---|---|
| 1 | umem_init_lock first | umem_lockup 194 takes it after the interposer hooks (192) -- the comment at 163-167 says the interposer locks come BEFORE 1. The numbered list omits them. Add "0. interposer locks (weak)" |
| 2 | vmem_lockup(): vmem_list_lock, vmem_nosleep_lock, each vm_lock, vmem_segfree_lock; then vmem_sbrk_lockup(): sbrk_lock, sbrk_faillock | TRUE: vmem.c:1856-1867, vmem_sbrk.c:293-294 |
| 3-5 | umem_cache_lock, umem_update_lock, umem_flags_lock | TRUE: 209-211 |
| 6a | cc_lock ascending | TRUE: 90-91 |
| 6b | cache_full.ml_lock, cache_empty.ml_lock, then per-stripe full/empty interleaved ascending | TRUE: 93-98 |
| 6c | cache_lock | TRUE: 100 |
| 7 | lh_cpu[*].clh_lock ascending then lh_lock | TRUE: 129-132 |
| "6a before 6b ... _umem_cache_alloc() takes cc_lock and then, still holding it, calls umem_depot_alloc()/umem_depot_free(), which BLOCK on ml_lock" | TRUE: umem.c:3313 lock, 3375 depot_alloc, 3390 unlock; local pop and global fallback are blocking umem_depot_pop (2657, 2751) |
| "(The trylock-based cross-CPU steal does not change this; only the remote-stripe scan is non-blocking.)" | TRUE: 2718, 2737 |
| "umem_depot_alloc() -> umem_depot_destroy_stale() -> umem_slab_free() takes cache_lock while cc_lock and no ml_lock are held, so cache_lock is below both" | TRUE: 2626 -> umem_slab_free 1862 takes cache_lock; ml_lock released inside umem_depot_pop before return |
| "Nothing in the allocator takes cc_lock or ml_lock while holding cache_lock -- that is the documented contract on umem_depot_alloc()/umem_depot_free()" | Contract text is at umem.c:2594-2598 (detached; see umem.c table) and uses inverted "below" wording. The fork file's sense is the one the code follows |
| Solaris lineage paragraph | Not verified (no Solaris source here) |
| "A previous version ... acquired cache_lock -> ml_locks -> cc_locks ... ABBA ... Reproduced by test/integration/test_fork_mt_load.c" | Un-softened record. File exists |

Ordering NOT covered by the list, both taken from allocation paths: (i)
brk_lock (umem_introspect.c, leaf, taken after allocation completes -- fine);
(ii) umem_ptc has no locks. (iii) vmem_walk's vm_lock is taken from free()
via umem_may_own -> hull_refresh (malloc.c:463) with no allocator lock held --
consistent with 2 above cc_lock but worth a line since it is a new vmem
entry from the free path.

### Table

| line | class | comment excerpt | finding |
|---|---|---|---|
| 35-80 | 6 | THE ONE TRUE LOCK ORDER + derivation + old-bug record | Exemplary; omissions above |
| 89, 109 | 6 | "See THE ONE TRUE LOCK ORDER above: 6a, then 6b, then 6c." / "Release in reverse" | Good |
| 148-167 | 6 | interposer participation: which locks, why weak, ORDERING before umem_init_lock, why not a second pthread_atfork (glibc reverse order) | Exemplary |
| 172-183 | 6 | introspect child reset: why weak, what the child inherits | Good |
| 300-304 | 6 | "Released LAST, mirroring the acquisition order ... child variant re-initializes" | Good |
| 308-313 | 6 | disarm after locks released, why | Good |
| 317-333 | 6 | P6.9 update-thread recreation: what was dead, why here (pthread_create allocates), the two guards, failure non-fatal | Exemplary. Verified: 334-339 gated on umem_ready and cleanup_update; umem_update_thread.c:179 asserts MUTEX_HELD(umem_update_lock), held at 335 |
| 255-262 (old) | -- | "worst that can happen is a cache has its magazines rescaled twice" | Out of scope (2008 text), consistent with umem.c 4.6 |

## vmem_mmap.c

In-scope comment lines: 52-91, 116-182, 246-275. This file is where the
"grep for siblings" rule in AGENTS.md §7 came from, and both comments record
it in place.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 52-57 | 6 | "No PROT_EXEC ... The original Solaris code had PROT_EXEC for genasm ... removed" | Good: says why, names the platform constraint |
| 63-91 | 6 | MAP_FAILED / MAP_ANON / MAP_NORESERVE portability | Fine |
| 116-118 | 6 | "Failure: leave errno as mmap() set it. See below." | Good |
| 123-145 | 6 | "P5.5, and this function had the defect TWICE ... Only (1) was in the P5.5 report; (2) is the one that was actually on the measured heap-ceiling path ... so the v3.0.0 fix was not merely incomplete across functions but ineffective for the failure it was written for" | Exemplary un-softened record; cites docs/results file (exists) and the regression test (exists) |
| 151-182 | 6 | two ways to free a span: the VMA mechanism, measured counts (40,102 VMAs; 1,236 holes), what DONTNEED gives up, "The default is a policy choice recorded here, not a measurement" | Exemplary; the explicit "policy, not measurement" line is the standard. No box/sha for the counts |
| 246-275 | 6 | "AND FIXING IT HERE WAS NOT ENOUGH (P5.5) ... Measured at d22bf03 and 553d42e, both of which contain the comment you are reading" | Exemplary: names the shas at which the comment was true-but-insufficient. The ASSERT paragraph verified against vmem.c:604-628 |

No findings other than missing box/sha on the VMA counts.

## vmem.c

In-scope comment lines: 124, 604-621, 1525, 1594-1602.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 124 | 6 | `#include <errno.h> /* vmem_populate() reports ENOMEM on VM_SLEEP */` | Good |
| 604-621 | 1 | "(docs/results/2026-09-22-prop-fragmentation-vmem-abort.md)" | File was DELETED in 7ba9914 ("gate: remove the prop_fragmentation exemption; it passes now"). The comment's reference is dead; the record it cites is now only in git. Either restore the file (docs/ is durable per AGENTS.md §8 -- deleting a result because the symptom went away is the SOFTENED RECORD pattern) or change the reference to the sha. Class 5 as well |
| 604-621 (rest) | 6 | why ENOMEM not ASSERT; "Under NDEBUG the assertion vanished entirely ... strictly worse" | Good |
| 1594-1602 | 3 | vmem_xcreate description | Fine |

Class 4 note: vmem.c's header (lines 40-70, old) documents VM_SLEEP removal;
the lock-order list in umem.c §6 does not include vmem_segfree_lock, which
vmem_lockup takes (1867). umem_fork.c's list does. umem.c §6 should match.

## envvar.c

In-scope comment lines: 67. Every secure-unsafe mark carries its reason, in
the voice §7a asks for (the side effect and who chooses the environment).

| line | class | comment excerpt | finding |
|---|---|---|---|
| 50-59 | 6 | dead `#ifdef UMEM_NUMA_AVAILABLE` include: why it never fired, why removed not repaired | Good record; same class of bug as umem_inspect.c:35-41 |
| 106-115 | 6 | item_secure_unsafe: definition, who chooses the env, "Pure tuning options must NOT be marked" | Exemplary field contract |
| 119-123 | 6 | count-and-report-once: why not per option | Good |
| 166-168 | 6 | backend: "vmem_sbrk.c already refuses ... but only AFTER parsing. Refuse earlier" | Verified vmem_sbrk.c:314 |
| 232-234 | 6 | mmap_guard "Not secure-unsafe in either direction", reason | Good: states why NOT marked |
| 266-267, 275-276 | 6 | profile / introspect reasons | Good |
| 289-291 | 6 | abort "Arming the abort is the SAFE direction" | Good |
| 323-326 | 6 | verbose: "misc.c already refuses this write under issetugid(); do not even set the flag" | Verified misc.c:123 |
| 353-355 | 6 | noabort: attacker position (env of privileged target) | Good |
| 362-364 | 6 | mtbf: "umem.c also zeroes umem_mtbf post-parse under issetugid(); this refuses it earlier and also under AT_SECURE" | Verified umem.c:5890-5891 |
| 704-712 | 6 | single filter point: "a new one only becomes reachable in secure mode if someone leaves item_secure_unsafe clear" | Good: states the default-safe direction |
| 990-997 | 6 | report goes through log_message so "the warning must not become the disclosure" | Good |

No findings. `random` (367-370) and `checknull` have no secure mark and no
comment saying why -- both are pure tuning; a one-line "not secure-unsafe:
tuning only" would match the standard 232-234 sets.

## umem_hooks.c / umem_hooks.h

In-scope comment lines: 116 (.c); the L1-L5 contract in the .h.

| line | class | comment excerpt | finding |
|---|---|---|---|
| .h 11-46 | 6 | L1-L5: lifetime with two corollaries (unregister from own callback self-deadlocks), no user code under lock, stats not a snapshot, walk semantics incl. nested-walk self-deadlock, find is unreferenced | Exemplary contract. Verified L1 (unregister 160-190 waits refcnt==0 after hook_active=0), L2 (track_alloc 207-229 drops lock around hook_alloc; walk 440-448), L4 (walk_busy 437-441, per-hook gen) |
| .c 47-50 | 6 | cv / walk_busy / gen field comments cite L4 | Good |
| .c 53-71 | 6 | hook_hold: "refuse rather than wrap, because a wrap to 0 would let unregister free the hook while callers are still in it" | Good: states the failure the check prevents and what declining degrades to |
| .c 78, 86 | 6 | "Caller must hold hook_list_lock" | Good |
| .c 148-153 | 6 | unregister L1 | Good |
| .c 164-167 | 6 | racing double unregister: both callers get the guarantee | Good |
| .c 175-178 | 6 | "Close the door first" | Good |
| .c 364-370 | 6 | dump via walk (L2) with the interposition reason | Good |
| .c 394-398 | 6 | find L5 | Good |
| .c 415-427 | 6 | walk restart-safe iteration rationale | Good |
| .c 438 | 6 | "One walk at a time: hook_walk_gen is a single field per hook" | Good |

No findings.

## umem_rseq.c

In-scope comment lines: 47-92, 119-123, 153-156, 176-180, 197-201, 223-237,
279-283, 302-305.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 47-75 | 6 | RSEQ_SIG: what the kernel checks, when umem's own registration is reachable, why the value differs per arch, "(as verified on real Graviton hardware) ... force-kills the thread with SIGSEGV" | Exemplary; cross-checked umem_rseq_x86_64.S:27 (0x53053053) and umem_rseq_aarch64.S:31-38 (0xd428bc00) |
| 82-88 | 6 | weak __rseq_offset/__rseq_size detection | Good |
| 119-123 | 3 | umem_rseq_get_area | Fine |
| 153-156, 176-180 | 6 | availability: EBUSY/EINVAL/ENOSYS meanings | Good |
| 197-201 | 3 | detect glibc management | Fine |
| 223-237 | 1 | "Set FS-relative offset for the assembly fast path ... Otherwise, compute offset from umem_rseq_area TLS symbol" then `#else umem_rseq_asm_safe = 0` for non-x86 | On aarch64 without glibc rseq, asm is disabled (245) even though umem_rseq_aarch64.S exists and reads umem_rseq_fs_offset via tpidr_el0 (S:59-75, 139). Comment does not say aarch64 manual registration gets no asm path. umem.c:3291-3294 ("Uses assembly critical sections when we own the rseq registration (not glibc)") says the OPPOSITE of the code: asm_safe is set at 230 in the glibc branch and at 243 in the x86 manual branch. Fix umem.c's sentence |
| 279-283 | 3 | glibc-managed register: "copy cpu_id into our thread-local area" | Code stores a POINTER to glibc's cpu_id (umem_rseq_cpu_idp = &glibc_area->cpu_id), copies nothing. Reword |
| 302-305 | 6 | EBUSY -> glibc | Fine |

## umem_update_thread.c

In-scope comment lines: 38-58, 74-79, 222-231, 246.

| line | class | comment excerpt | finding |
|---|---|---|---|
| 38-58 | 6 | handshake object: who owns it (creator's stack), two predicates one mutex, why every mutation is under mtx + broadcast, the old lost-wakeup and destroyed-while-held bugs | Exemplary lifecycle comment |
| 74-79 | 6 | worker side of the handshake | Good |
| 225-231 | 6 | "The wait must NOT live inside ASSERT(): misc.h compiles ASSERT() out entirely under NDEBUG, which removed the wait from release builds" | Exemplary record of a real bug class (side effects in ASSERT) |
| 246 | 6 | "no worker exists, so nothing can hold obj.mtx" | Good |

No findings.

## test/ (header comments: what each test proves)

Read: test_ptc_resize_no_loss.c, test_ptc_thread_exit_drain.c,
repro_ptc_resize_capacity.c, test_heap_ceiling.c, test_fork_mt_load.c,
test_forged_free.c, test_stack_bounds.c, test_cache_footprint.c,
test_freelist_mangle.c, test_inspect_contracts.c, test_hook_contracts.c,
test_umem_stats.c, test_coverage.c, test_error_paths.c, repro_reclaim_reuse.c.

The regression tests written since 2026-09-21 share a header shape --
DEFECT (pre-fix, with the code excerpt) / HOW THIS DETECTS IT / what would make
the test vacuous / exit codes -- and that shape is the standard the rest of
the tree should be held to. The older unit tests do not have it.

| file:line | class | comment excerpt | finding |
|---|---|---|---|
| test_ptc_resize_no_loss.c 12-130 | 6 | DEFECT with the old code inline; PRIMARY ORACLE (exact ledger) vs CORROBORATING SIGNAL; "That control is WRONG for this defect, and an earlier version of this test used it and produced garbage ... 64, 64, and 1755"; "If the resize does not happen, the test reports INCONCLUSIVE, never PASS" | Exemplary. Records two wrong earlier designs (wrong control; gate opened too early, 273-284: "reported PASS on a build with the defect present") |
| test_ptc_resize_no_loss.c 458-466 | 6 | why exit 77 not 3: automake, observed on aarch64 | Good |
| test_ptc_thread_exit_drain.c 12-49, 165-177 | 6 | DEFECT / HOW / WHY AN INTERNAL CONTROL / EXACT ORACLE preferred, and why the comparison arms "made this test flaky" | Exemplary; matches Makefile.am:483-490 |
| test_ptc_thread_exit_drain.c 178-197 | 4 | probe arm returns 0 after exact oracle; the comparison arms below are then unreachable in the probe binary | Comment says "preferred"; the non-probe binary still runs the statistical comparison as its ONLY oracle. The header should say which binary the gate runs (exit_criteria_gate.sh runs the _probe variant per the team brief) so a reader does not take the plain binary's PASS as the P1.3a proof |
| repro_ptc_resize_capacity.c 12-40 | 6 | DEFECT with the two reads shown; what the over-read returns | Exemplary |
| test_heap_ceiling.c 12-40 | 6 | "Two things are asserted ... Checking only (1) would pass on a box whose limit had been raised by sysctl" | Exemplary: names the vacuity mode and closes it |
| test_heap_ceiling.c 13-16 | 1 | "the mmap heap's parent arena used a PAGE-SIZED quantum where Solaris uses 64 KiB" | The fix that landed is UMEM_MIN_SLAB_OBJECTS / UMEM_MIN_QCACHE_SLAB (umem_impl.h:747-833: slab density, not arena quantum). AGENTS.md §10 says the ceiling "remains open, with an honest record of a failed fix attempt". Header describes the mechanism the failed attempt targeted. Re-read against umem_impl.h and the plan and correct |
| test_fork_mt_load.c 1-40 | 6 | WHAT THIS CATCHES THAT umem_ptc_fork_test DOES NOT; the ABBA with both stacks; how depot traffic is forced | Exemplary |
| test_forged_free.c 10-62 | 6 | attacker position, three numbered defects with file:line, THE FIX, WHAT THIS ASSERTS A-E with E as CONTROL ("so A-D are not passing because free() rejects everything"), PRE-FIX DEMONSTRATION with the observed string | Exemplary. Line refs drift: "umem_impl.h:663" -> MALLOC_MAGIC is now :844; "malloc_interpose.c:552" -> umem_abort = 0 is now :623 |
| test_forged_free.c 80-86 | 6 | ASan skip: "Skip them rather than claim a result the run did not produce" | Good |
| test_stack_bounds.c 1-30 | 6 | defect with file:line ranges, why read-only ("confirmed by reading both walks line by line"), fix, WHY DIRECT CALL | Exemplary |
| test_cache_footprint.c 5-30 | 6 | defect, numbers (18.6 KB, 29,159 VMAs of 50k), what is tested and why N=2,000 | Good |
| test_cache_footprint.c 138-150 | 6 | "THE VMA ARM took two fixes, and the first one's regression test (this arm) is what showed it ... Pre-both: 1,134. After (a) only: 1,066. After both: see the RESULT line" | Exemplary correction-in-place (ede1849 rewrote the SKIP-branch rationale instead of deleting it) |
| test_freelist_mangle.c 5-60 | 6 | attacker position D named; attack + detect arms; "-DUMEM_NO_LINK_MANGLE ... this test then fails, which is what makes it a regression and not a tautology" | Exemplary |
| test_inspect_contracts.c 1-20 | 6 | "Each test names the contract clause (C1..C5) it defends"; PRE-FIX BEHAVIOUR per clause; "Reproduced on x86_64 within seconds (see the report in docs/results/)" | Good; the docs/results reference should name the file |
| test_hook_contracts.c 1-20 | 6 | same shape for L1-L5; "all four failed before the fix" | Good |
| test_umem_stats.c 1-12, 403-450 | 1 | header: "cache_mag_reloads counts magazine operations"; body: "NOTE: This counter is defined in the structure but not yet implemented ... Skip until implemented" then `return MUNIT_SKIP` | Header claims a property the test then SKIPs on. The field is never incremented anywhere (see umem_impl.h table). Either implement or drop the test and the field; today `make check` counts this as a pass-shaped SKIP for a dead counter |
| test_coverage.c 1063-1069 | 1 | "vmem_create with no import source crashes on destroy -- skip" with an unconditional `return MUNIT_SKIP` | A test whose whole body is SKIP, recording a crash as the reason (1e64034, 2026-04-22). Either the crash is real (then it is an open defect with no plan entry) or it is fixed (then the test is dead). Never-executed masquerading as SKIP -- AGENTS.md §6 |
| test_coverage.c 1079-1080, 1713 | 3 | `umem_cache_create(..., UMF_AUDIT)` / `UMF_FIREWALL` as cflags, `if (cp == NULL) return MUNIT_SKIP` | UMF_* are cache_flags, not UMC_* cflags; umem.c:5070 masks cflags with UMF_DEBUG so UMF_AUDIT (0x1) is passed through by accident and UMF_FIREWALL (0x40) too. The comment "Cache with UMF_AUDIT flag" is accurate only because of that mask. Fragile; note it |
| test_error_paths.c 1-10 | 3 | generic header | Says what categories it covers, not what a failure would mean. Pre-2026-09 style; no finding beyond "not to the standard" |
| repro_reclaim_reuse.c 17-40 | 6 | P1.5 three cases with the byte arithmetic that makes each shape reach the defect | Exemplary |

## scripts/ec2/

Read the header blocks of exit_criteria_gate.sh, oracle_matrix.sh,
sustained_load.sh, allocator_comparison.sh.

| file:line | class | comment excerpt | finding |
|---|---|---|---|
| exit_criteria_gate.sh 14-18 | 3/5 | "NOTE: every $? is captured IMMEDIATELY ... reported here as rc=0 ... Exit status is the number of gate failures" then line 18: "reported as rc=0 by this very script. Same defect class as oracle_matrix.sh's." | Line 18 is an orphaned fragment of the previous wording (an edit left half the old sentence). Delete line 18 |
| exit_criteria_gate.sh 42-44 | 6 | "No allowances here ... (prop_fragmentation used to be exempted for a vmem abort -- that was fixed, so the exemption is gone.)" | Good: says what changed. But the record it summarises (docs/results/2026-09-22-prop-fragmentation-vmem-abort.md) was deleted in the same commit -- see vmem.c table and Softened-record list |
| oracle_matrix.sh 5-9 | 6 | exit status contract: 0/1/77 and what each means; records the old `set -e` + unexamined status + silent unsanitized run | Exemplary; this is the §6 PASS/SKIP/FAIL/never-executed distinction written into a script |
| sustained_load.sh 9-30 | 6 | WHAT CHANGED 2026-09-22 and why older sustained.toml is not comparable: per-window rows, alternating A/B, matched op budgets ("Equalising wall-clock ... hands them different amounts of work and destroys the comparison"), provenance incl. sha | Exemplary method comment |
| allocator_comparison.sh 1-20 | 6 | "One job, one build, one box, so the identity of every number is the same"; missing competitor RECORDED never dropped; null control inside the matrix "is the rig's resolution; a cross-allocator delta inside it is noise" | Exemplary |

## umem_gc.c cross-check

Only that no live comment claims GC is present. `grep -rn "gc\b\|GC\b" --include=*.c --include=*.h` outside umem_gc*.c and test_gc/prop_gc: none in umem.c, umem_impl.h, malloc*.c, umem_ptc*, umem_fork.c. Clean.

## Summary

### Counts (in-scope comments, 2y blame window; "GOOD" rows are grouped, so the
count under-represents how many individual comments meet the standard)

| class | count | note |
|---|---|---|
| 1 CLAIM MISMATCH | 26 | 2 are possible code bugs (CB-1, CB-2); 1 is a contradiction between two live comments (umem_impl.h:167 vs :241); 5 are dead references (files/functions/aliases that do not exist) |
| 2 STALE MEASUREMENT | 8 | none superseded; all are "no box/sha/date". The best-cited numbers (umem.c:2669, 4705; umem_impl.h:757; vmem_mmap.c:123) point at docs/results files and lack only the sha |
| 3 NARRATION/CONFIDENCE | ~60 | concentrated in umem.c's 2026-04 prefetch/SIMD/"true lock-free" layer and umem_impl.h's tagged-pointer block; almost none in files touched after 2026-09-21 |
| 4 MISSING INVARIANT | 2 explicit + 3 noted inline | cc_lock's field comment understates its scope (umem_impl.h:468); umem.c §6 lock list omits vmem_segfree_lock, sbrk_faillock, interposer locks; ptc_table_ready has no publish protocol though the comment implies one (umem_ptc.c:232) |
| 5 SOFTENED RECORD | 2 | docs/results/2026-09-22-prop-fragmentation-vmem-abort.md deleted in 7ba9914 while vmem.c:617 still cites it and exit_criteria_gate.sh:43 summarises it; exit_criteria_gate.sh:18 is a half-deleted sentence. Everything else that was wrong was corrected in place -- umem_impl.h:167, umem_ptc.c:613, malloc_interpose.c:476/599, umem_fork.c:75, vmem_mmap.c:123/260, test_ptc_resize_no_loss.c:79/273, test_cache_footprint.c:138 are the models |
| 6 GOOD | ~240 rows | see per-file tables |

### Possible code bugs (comment right, code wrong) -- restated

1. **CB-1** `umem.c:3259` `reset_cpu_hint_cache()` in `umem_cpu_reload()`, 8
   callers. `umem_impl.h:167` (20999ee, yesterday) says "Nothing reset it" and
   :183 says "read once ... and cache". The reset is live and now re-derives
   the hint through rseq/sched_getcpu on every CPU-layer magazine exchange.
   Unmeasured cost; contradicts the design statement. `@hot` decides: delete
   the reset or correct the comment. Not both can stand.
2. **CB-2** `umem_inspect.c:880-889, 925-927` bound magazine reads by
   `cp->cache_magtype->mt_magsize`, the exact pattern `umem_mag_capacity`'s
   comment (umem.c:2213-2225) forbids. Stale smaller shells on depot lists are
   over-read. Reporting error, not a crash, but it is the P1.3b class in the
   one file that did not get the fix.

### Top ten CLAIM MISMATCHES, ranked by how wrong x how load-bearing

| # | where | why it ranks |
|---|---|---|
| 1 | umem_impl.h:167-183 vs umem.c:3255-3259 / umem_impl.h:240-243 | The CPU-hint comment is the single most-read explanation of this week's headline fix, and its central sentence is false. Two live comments in one header contradict each other 70 lines apart |
| 2 | umem.c:2579-2598 + 2762-2768 (depot lock order) | Says "ml_lock below cache_lock", umem_fork.c says cache_lock is below ml_lock; code follows umem_fork.c. This is the lock-order comment the fork file calls "the documented contract". Also physically detached from the function it describes |
| 3 | umem.c:4486-4489, 4213-4219 | reclaim_pages "drops and reacquires the lock around madvise and slab destroy" -- that is the pre-fix behaviour that SEGV'd; the fix comment 20 lines down says so. umem_reap's stated reason for not calling it is the same stale mechanism |
| 4 | umem_impl.h:610 cache_mag_reloads; test_umem_stats.c:403-450 | A counter three reporting tools print, never incremented anywhere, with a test that SKIPs on it and a header claiming it "counts magazine operations". Dead data presented as live |
| 5 | vmem.c:617 -> deleted docs/results file (7ba9914) | The only in-tree reference to a results record now points at nothing; the record was deleted because the symptom cleared. §8 says docs/ is durable |
| 6 | umem.c:3280-3285, 5899-5905, umem_rseq.c:223-245 | "true lock-free ... when we own the rseq registration (not glibc)" -- code enables asm in the glibc branch and x86-manual branch, never aarch64-manual. Opposite of the sentence. AGENTS.md §6 already flags the rseq path as serving zero hits; the comments still sell it |
| 7 | test_heap_ceiling.c:13-16 | DEFECT paragraph names the arena-quantum mechanism; the fix that landed and the file's own later table (43-60) describe slab density. A regression test's header is where the next reader learns what was wrong |
| 8 | malloc.c:184-196, 728-734, 762-770 | genasm/weak-alias/function-pointer dispatch that does not exist in this port, on the two entry points every interposed call goes through |
| 9 | umem_impl.h:485-548 tagged pointers | 60 lines describing a lock-free depot that 9640329 removed; two zero-caller helpers survive in umem.c:2096-2124 to keep it company. "Use umem_tagged_ptr_check() at init" -- nothing does |
| 10 | umem.c:1116-1120 "Deliberately weak"; umem.c:2126-2131 orphan header; malloc_interpose.c:34 dead doc ref; umem.c:6070 "8-448 bytes"; umem.c:2958 dead docs/results ref | Small, cheap, and each one teaches a reader something false |

### Suggested fix order

1. Resolve CB-1 (decision + one-line code or comment change; `@hot` owns the
   lines). Then correct umem_impl.h:149-186 so the record says the reset WAS
   there and what was decided about it -- do not delete "Nothing reset it";
   annotate it.
2. Fix #2: move the lock-order block onto `umem_depot_alloc()` and invert the
   "below" wording to match umem_fork.c. One comment, two sites.
3. Fix #3 (two sentences) and #6 (three sentences): pure comment edits in
   umem.c, no code.
4. Restore docs/results/2026-09-22-prop-fragmentation-vmem-abort.md with a
   STATUS: FIXED header (git show 7ba9914^:path), fixing #5 and the class-5
   finding at once.
5. Decide cache_mag_reloads / low_water / hits / misses / cache_numa_info /
   tagged-pointer helpers: implement or delete. Each is a field with a
   live-sounding comment and no writer. Deleting is the ponytail answer; the
   test that SKIPs on cache_mag_reloads goes with it.
6. CB-2: `@review-*` cannot touch source; hand to whoever owns umem_inspect.c
   with the two line ranges.
7. #7, #8, #10 and the class-3 sweep of umem.c's prefetch/SIMD comments
   (delete ~12 comments that restate `__builtin_prefetch` arguments).
8. Add box/sha to the eight class-2 numbers; most have a docs/results file to
   point at already.

### Calibration: what "good" looks like here

The standard is already met, and exceeded, in the files touched since
2026-09-21. Six comments a new contributor should read before writing one:

- `malloc_interpose.c:239-266` (libc_ptr_live): box, method, numbers, and a
  correctness argument for a lock-free read.
- `umem.c:2812-2835` (umem_ptc_mag_return): two invariants stated at the one
  shared function, with what used to happen.
- `umem_inspect.c:156-206` (C1-C5): which lock protects what, what is
  consistent, what may be torn, and the debugger caveat.
- `umem_introspect.c:918-957` (P5.6): attacker position, both failure modes
  demonstrated, the fix, and why the residual predictability is fine.
- `vmem_mmap.c:246-275`: "Measured at d22bf03 and 553d42e, both of which
  contain the comment you are reading" -- the fix-in-place record at its best.
- `umem_ptc.h:44-70`: the first version was wrong, here is the cliff, here is
  the fix sha, and "it has not been measured" for the part that was not.

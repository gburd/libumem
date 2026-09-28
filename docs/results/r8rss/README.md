# r8rss: why prodcons (cross-thread free) costs libumem RSS and throughput (O2-O5)

Agent @r8rss, 2026-09-28. This is a diagnosis. **No library code changed.**
Plan: `docs/plans/2026-09-28-round8.md`, O2-O5.

## Provenance

| box | instance | ref | what ran |
|---|---|---|---|
| `intel-lo/` | c7i.2xlarge, 8 vCPU, Debian 13, kernel 6.12.107, gcc 14.2.0, glibc 2.41, libjemalloc2 5.3.0 | `da6314a` (isolated `git archive`) | `scripts/ec2/r8rss_probe.sh` |
| `arm-lo/` | c7g.2xlarge, 8 vCPU, same stack | `da6314a` | same |
| `arm-hi/` | c8g.metal-48xl, 192 vCPU, same stack | `a8c3e1a` (script-only change on `da6314a`) | `scripts/ec2/r8rss_hi.sh` |

Library code at both refs is master after `2a5b850` (the comparison-v2 ref)
plus docs and scripts only. Default `./configure`. `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536`
for every arm. Every row is `bench_main -w prodcons -D 5:4 -W 1 -T 16` (metal: `-D 10:3`), 3 kept
intervals, in `*/rows.csv` (the first column is the tag). The retention snapshots
(`snap-*.txt`) come from `scripts/ec2/r8rss_retention.py`. That script uses gdb to attach
to the live run about 20 s in and walks every PTC through `umem_ptc_list`, plus every
`umem_alloc_*` cache's per-CPU, rseq and depot magazines, with all threads stopped.
`perf-*` is `perf record -g` output. `stat-*` is `perf stat` output.

## Step 1: reproduction (median of 3 intervals, Mops/s and peak RSS MiB; umem / libc / jemalloc)

| box | sizes | t=8 (= nproc) | t=32 (4x) | t=64 (8x) |
|---|---|---|---|---|
| x86 lo | 64:256 | 55.2 / 11.3 / 65.1 (14 / 8 / 9) | 59.0 / 149.0 / 129.5 (29 / 12 / 18) | 64.1 / 140.4 / 133.3 (45 / 19 / 29) |
| arm lo | 64:256 | 107.4 / 18.5 / 77.0 (13 / 7 / 8) | 106.1 / 157.4 / 88.2 (28 / 12 / 32) | 108.9 / 118.8 / 106.9 (49 / 18 / 49) |
| x86 lo | 1024:4096 | 25.2 / 17.0 / 29.3 (26 / 13 / 29) | 37.5 / 51.5 / 32.4 (109 / 50 / 611) | 36.4 / 42.3 / 32.5 (190 / 95 / 1007) |
| arm lo | 1024:4096 | 49.7 / 11.4 / 71.7 (27 / 15 / 25) | 60.4 / 46.6 / 59.3 (130 / 50 / 116) | 51.4 / 35.5 / 47.6 (181 / 94 / 228) |
| arm metal | 1024:4096, t=192 | 121-122 / 346-359 / 705-1066 (958-964 / 266-268 / 532-583) | | |

- **F1 reproduces** on both lo boxes: umem uses 2.0-2.7x glibc's RSS at t≥32. The gap
  to jemalloc reproduces only at 64:256 on x86. On x86 at 1-4 KB, jemalloc is the
  outlier (1007 MiB). On metal at 1-4 KB, umem is 3.6x glibc and 1.7x jemalloc.
- **F2 reproduces in shape**: umem stays flat from t=nproc to t=8·nproc (55→64 x86,
  107→109 arm), and glibc rises 12x (x86) / 8.5x (arm). The rise comes from glibc's
  **t = nproc number** (11.3 / 18.5 Mops/s, 5-6x below umem). It does not come from
  glibc doing something at 8x that umem cannot do.
- **F3 reproduces** at 64:256 on x86 lo only (jemalloc 2.2x umem at t=32). On arm lo,
  umem ≥ jemalloc in every cell. On metal at 1-4 KB it reproduces at full size: jemalloc
  5.8-8.8x umem.

## F1 / O2: where the RSS sits

Snapshot totals, in MB of object bytes. `slab_out` counts objects not on a slab freelist,
so it is live objects plus every cached free object.

| snapshot | VmRSS | slab_out | PTC bins | PTC mags | per-CPU cc mags | rseq per-CPU mags | depot full | slab free |
|---|---|---|---|---|---|---|---|---|
| x86 lo t=32 64:256 | 30.1 MiB | 14.7 | 2.9 | 6.2 | 0.6 | 1.8 | 2.3 | 0.02 |
| x86 lo t=64 64:256 | 48.0 MiB | 26.0 | 5.8 | 12.1 | 0.4 | 1.6 | 5.4 | 0.06 |
| arm lo t=32 64:256 | 30.1 MiB | 15.2 | 2.9 | 6.3 | 0.6 | 1.6 | 3.3 | 0.05 |
| x86 lo t=64 1-4 KB | 194.9 MiB | 180.7 | 16.3 | 80.9 | 1.1 | 13.8 | 51.4 | 0.2 |
| **arm metal t=192 1-4 KB** | **902 MiB** | **850.7** | **48.8** | **274.0** | **217.4** | **271.6** | **38.3** | **0.1** |

**Mechanism.** Almost none of it is slab fragmentation: the slab freelists hold less than
0.2 %. The RSS is free objects parked in retention layers that stack on top of one
another:

1. **Free-only threads park a full per-thread stock forever.** Exactly half the PTCs are
   non-empty: 16/32, 32/64 and 96/192, which are the consumers. On metal, 94 of the 96
   consumers hold exactly 208 bin objects plus 1079 magazine objects. That is every bin
   full (4 × 32 + 5 × 16 slots for the 1280..5120 classes) plus **one full magazine per
   class** (8 × 127 + 63). At 64:256 they hold 1088 + 2295, which is one full 255-round
   magazine for each of 9 classes. A consumer never allocates, so none of this stock is
   ever reused by its owner. Once the bins are full, every free bypasses them. The bins
   and the retained magazine are a permanent 3.36 MB per consumer at 1-4 KB (0.56 MB at
   64:256) until the thread exits. The bound is `sum(ptc_bin_capacity) + 2 × magsize` per
   class, and `magsize` grows under depot contention (the 1-4 KB caches were resized from
   63 to 127). Across 96 consumers that is 323 MB, **36 % of metal RSS**, and 68 % of
   `slab_out` on x86 lo at t=64.
   **Control:** `UMEM_OPTIONS=tcache=0` at t=32 64:256 cuts RSS from 29 to 19 MiB (x86) and
   from 28 to 19 MiB (arm). Throughput is unchanged: 58.0 vs 59.0 and 108.6 vs 106.1 Mops/s.
   In prodcons the PTC costs a third of RSS and buys nothing.
2. **Two per-CPU layers both hold stock.** Both the rseq per-CPU magazines and the
   classic `cc_loaded`/`cc_ploaded` magazines are populated: 272 + 217 MB on 192 CPUs,
   **54 % of metal RSS**, against 2 MB on 8 CPUs. That is why the absolute gap grows with
   core count. From the code, `_umem_cache_free` falls through to the cc_lock layer
   whenever `umem_rseq_free_slowpath()` finds no empty magazine in the depot
   (`umem_depot_alloc(cp, &cp->cache_empty)`, umem.c ~3415). In prodcons that is the
   free side's normal state: consumers only produce full magazines. Unlike the cc path,
   the rseq slowpath never allocates a fresh empty magazine, so the cc layer becomes a
   second per-CPU stock. I have not verified this path with a counter. The inference
   comes from the code and the populated cc rounds.
3. The depot full lists hold the rest: 4-28 % of the total.

**Caveat.** On metal the layers sum to 850 MB, which equals `slab_out` to within 1 MB. That
leaves about 0 for objects in flight in the 96 rings. The perf profile is producer-bound
(below), which fits near-empty rings, but I have not ruled out double-counting between
the cc and rseq layers.

**Fix proposals.**
- **F1-a: PTC garbage collection for live threads (class i: footprint defect).** A free-only
  thread's bins and its retained magazine are never reclaimed while it lives. Nothing
  outside the owner can drain a PTC, and `umem_reap` does not reach it.
  jemalloc bounds the same retention with `tcache_gc`. The proposal: in the bin-overflow
  slow path (already off the fast path), keep a per-bin low-water mark and flush the
  unused part every N overflows. For a free-only thread the low-water mark stays at
  capacity, so the whole stock goes.
  Expected effect: −0.56 MB per consumer at 64:256 and −3.36 MB per consumer at 1-4 KB
  (up to −36 % RSS on metal), with no throughput change, per the `tcache=0` control.
  Hot-path risk: the fast push/pop is untouched, but the overflow path is inside
  `_umem_free`'s inlined PTC block and is bound by the P1.3d fork-consistency rules.
  It needs its own failing-before test (a free-only thread's PTC bytes stay above a
  bound) and a hotpath A/B. It is **not small**, so it is proposed here and not
  implemented.
- **F1-b: stop the cc layer from filling behind rseq (class i: footprint).** In
  `umem_rseq_free_slowpath`, when the depot has no empty magazine, allocate one (as
  `umem_ptc_mag_prime` and the cc path already do) instead of returning 0. Expected
  effect: the cc stock (−217 MB, −24 % on metal at 1-4 KB) goes away, and the rseq
  layer's own bound is unchanged.
  Risk: this is a slow path, but it runs once per magazine per CPU, and one more shell
  allocation per miss is extra `cache_mag_total` growth. Before implementing, confirm with
  a counter that the cc rounds come from this fall-through. The change is about 6 lines
  plus a test. I did not implement it: the verification would not fit in this task's
  60-call budget.

## O5: `umem_free_wrapper`'s cost is the header load, a cache miss on a line another core wrote

`perf annotate` of `umem_free_wrapper` (t=32 64:256):
- **x86:** 99.06 % of the function's samples fall on `cmp %rax,%rcx`, the magic compare.
  That is the first use of the `header->magic` load, so the load's stall is charged to
  the compare.
- **arm:** 68.3 % + 28.7 % fall on the `mov/movk #0xcafebabe` right after the load. On
  arm, perf skids the load's stall onto those instructions.

It is not the size lookup and not the free path. `_umem_free` is 3.8 % self (x86) and
5.2 % (arm). `umem_free_wrapper` is 22.1 % (x86 lo), 9.6 % (arm lo) and 19.7 % (arm
metal, 1-4 KB). The producer just `memset` the object and wrote the header, so the
consumer's first read of the header is a cross-core coherence miss.

This is not purely a harness artifact. The `LD_PRELOAD` drop-in's `umem_malloc_free`
(malloc.c:1064) reads the 8-byte `malloc_stat` tag in front of the buffer in the same
way. The native API `umem_free(buf, size)` needs no read, and jemalloc's `free` never
touches the object: its rtree metadata and tcache pointer arrays live elsewhere.

**Fix proposals.**
- **Harness (neither class):** the consumer already gets `sz` from `ring_pop`, so the
  umem API arm could call `umem_free(ptr, sz + header)` and skip the read. That measures
  the API as it was designed. Keep the header arm, because it models the drop-in.
- **Drop-in (class ii):** a sizeless free that finds the cache from the slab address
  (a page map) instead of an in-band tag. This is a large change, and it is **not
  proposed for this phase**.

## F2 / O3: flat scaling under oversubscription

**Mechanism: the jump is glibc's, not a libumem ceiling.** glibc at t = nproc is 5-6x
*below* umem on lo (11.3 vs 55.2 x86, 18.5 vs 107.4 arm), and oversubscription lifts it
back up. glibc's arm t=32 profile is lock atomics: `__aarch64_swp4_rel` 13.3 %,
`__aarch64_cas4_acq` 10.7 %, `_int_malloc`/`_int_free_chunk` 15.6 %. That fits arena-lock
ping-pong between a producer and a consumer running concurrently on different cores,
which time-slicing removes. **I did not profile glibc at t = nproc**, so the collapse
mechanism is an inference and has not been measured.

umem is flat because it is already saturated at t = nproc. A co-scheduled
producer/consumer pair gains nothing, because objects never go directly from consumer
to producer: they pass through the consumer's PTC, then a magazine, then the depot or
rseq layer.

**Refuted: lock contention.** At t=32, umem's `pthread_mutex_lock` (0.33 % x86),
`umem_depot_pop_trylock` (0.32 % / 0.83 %), `umem_depot_alloc` (1.4 % arm) and
`umem_rseq_*_slowpath` (1.3 % arm) together stay under 3 %. `sched_yield` in the ring
is 1.0 % (x86) and 1.9 % (arm) for umem, against 2.6 % / 2.5 % for glibc. At t=64 over
20 s, umem does 4.74 M context switches against glibc's 2.78 M and jemalloc's 4.52 M
(x86), and 4.00 / 2.66 / 2.38 M (arm). umem's consumers find their ring empty more
often, which means the producers are the bottleneck.

**Unexplained, needs its own diagnosis:** `UMEM_OPTIONS=norseq` is **+44 %** on x86 lo at
t=32 (84.7 vs 59.0 Mops/s) and +10 % on arm (116.3 vs 106.1). With the PTC also off
(`tcache=0,norseq`) the result is 67.8 / 82.7. At oversubscription the rseq layer costs
throughput in prodcons. I have not attributed the cause: candidates are restarts on
preemption and the extra layer's handoffs.

**Fix:** none for libumem at the O3 level; the finding is about glibc. The norseq
observation is a class (ii) performance question. Do not headline t=384 prodcons.

## F3 / O4: jemalloc ahead in cross-thread free

**Mechanism: object locality, not allocator code.** The metal t=192 1-4 KB profile for
umem is `__memset_sve_zva64` 33.9 %, `umem_free_wrapper` 19.7 %, `ring_pop` 16.3 %,
`__aarch64_cas8_relax` 8.1 % and `__schedule` 5.0 %. libumem's own symbols are about
1 % each (`umem_depot_pop_trylock` 1.02 %). jemalloc's profile is memset 25.3 % and
`producer_thread` 17.1 %.

Per operation, umem's producer memset costs about **10x** jemalloc's: 0.339 / 121 M
against 0.253 / 886 M. The code is identical, so umem's producers are writing to
cold or remotely owned lines and jemalloc's are not. Two measured contributors:

1. umem's consumer pulls every object's first line to its own core (the O5 header read).
   jemalloc's consumer never touches the object.
2. umem's recycle path is anonymous. A consumer's full magazine goes to its CPU's depot
   stripe or rseq slot, and then to whichever producer asks next. In jemalloc 5.3,
   `tcache_bin_flush` returns objects to their slab's arena bin, and that arena belongs
   to the thread that allocated them. The same producer reallocates them while they are
   still in its cache.

The umem pool also cycles through about 850 MB (F1), far larger than the LLC. The 10x is
derived from perf share and throughput. **I did not confirm it with miss counters**:
`perf c2c` or `mem_access` per op is the next check.

On 8 vCPU arm, umem beats jemalloc, which fits a mechanism that depends on the memory
hierarchy.

**Fix (class ii, not implemented):** return-to-owner recycling, such as tagging
magazines with the producing CPU or node and preferring them on refill. F1-a also
shrinks the circulating pool that has to fit in cache. That is a side benefit and has
not been measured.

## Summary

| finding | mechanism (evidence) | proposed fix | class |
|---|---|---|---|
| F1 / O2 RSS | free objects parked in stacked layers. Free-only consumers hold full bins + 1 full magazine per class forever (36 % metal RSS; `tcache=0` −35 % RSS, ±0 throughput). cc + rseq per-CPU layers both populated (54 % metal RSS) | F1-a PTC GC for live threads; F1-b rseq free slowpath allocates an empty magazine instead of falling to the cc layer | (i) both. Neither is small enough to land here; proposed only |
| O5 wrapper 37.6 % | the `header->magic` load, a coherence miss on a line the producer wrote (annotate: 99 % / 97 % on the instruction after the load) | harness: free with the ring's size for the API arm; drop-in: page-map sizeless free | harness / (ii) |
| F2 / O3 flat at 2x | glibc collapses at t = nproc (11-18 Mops) and recovers when oversubscribed; umem is already saturated at nproc. Lock contention refuted (< 3 %) | none; `norseq` +44 % x86 is an open question | (ii) |
| F3 / O4 jemalloc 1.5-9x | producer memset ~10x more per op on metal: cold objects (consumer header touch + anonymous recycling vs jemalloc's return to owner). Derived, not confirmed with miss counters | return-to-owner magazine routing | (ii) |

**Implemented:** nothing in the library. Scripts: `scripts/ec2/r8rss_probe.sh`,
`r8rss_hi.sh` and `r8rss_retention.py`.

**Incident:** one of my shell commands ran `git stash` after a rejected push. It
stashed another agent's uncommitted edits to `test/integration/test_first_alloc_small_stack.{c,sh}`.
The stash matched what that agent then committed (`f649c03`, `git diff` = 0), and I
popped and dropped it. Nothing was lost, but that command form must not be reused.

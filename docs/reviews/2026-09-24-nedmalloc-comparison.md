# Review: what, if anything, libumem should take from nedmalloc (2026-09-24)

Read-only source comparison. No builds, no EC2. libumem at `a1901c8`
(master, 2026-09-24); nedmalloc at the tree in `~/src/nedmalloc`
(nedmalloc v1.10 beta, dlmalloc 2.8.4 embedded as `malloc.c.h:7`,
Boost licence, `nedmalloc.h:1-27`). Line numbers below are into those
two trees.

## What nedmalloc is, in one paragraph

nedmalloc is dlmalloc 2.8.4 built with `MSPACES=1 ONLY_MSPACES=1 FOOTERS=1`
(`nedmalloc.c:108-117`) plus two layers on top: a per-thread cache of freed
blocks in power-of-two bins up to 8 KB (`THREADCACHEMAX 8192`,
`nedmalloc.c:191`; nine bins, `THREADCACHEMAXBINS (13-4)`, `:213`), and a
small pool of dlmalloc mspaces, each guarded by its own mutex, which threads
pick by trylock (`FindMSpace`, `:1589-1655`). Everything above the
threadcache is dlmalloc: boundary tags, coalescing, `mmap` above 256 KB,
`mspace_trim`. There is no per-CPU structure, no lock-free path other than
the threadcache hit, and no object-cache or constructor concept.

## Verdict table

(a) = libumem has it or an equivalent; (b) = libumem lacks it and it would
land on a named open gap; (c) = libumem lacks it and it would not help or
conflicts with a commitment.

| # | nedmalloc mechanism | nedmalloc.c | verdict | libumem equivalent / reason |
|---|---|---|---|---|
| 1 | Per-thread free-block cache, TLS lookup, bins by size | `threadcache_t` `:794-811`; `GetThreadCache` `:1910-1938`; `threadcache_malloc` `:1375-1460`; `threadcache_free` `:1478-1541` | (a) | PTC: `umem_ptc_t` `umem_ptc.h:178-186`, inlined hit in `_umem_alloc` `umem.c:3811-3826` and `_umem_free` `:4007-4021`. libumem's is a pointer array per exact size class (28 classes); nedmalloc's is a doubly-linked intrusive list per power of two (9 bins), which makes a hit a dependent pointer chase and stores link words in the freed buffer (worse than libumem on both counts). |
| 2 | Threadcache size ceiling 8 KB | `THREADCACHEMAX 8192` `:191` | (b) | libumem's ceiling is 2 KB (`umem_ptc_maxsize = 2048`, `umem_ptc.c:45`). P8.2b's fix path is already "extend PTC classes through 8 KB" (`production-readiness.md:2014-2017`). nedmalloc's number is a confirmation, not a new idea; see §B1. |
| 3 | Bump-up-a-bin on miss (serve a 48 B request from the 64 B bin) | `:1413-1422` | (c) | libumem's bins are exact size classes feeding exact slab caches; an object from the 64 B cache handed to a 48 B request would be freed to the 48 B cache by `_umem_free(buf, 48)` (`umem.c:3989-3995`, cache chosen from the caller's size) and corrupt both. Structurally impossible here, and nedmalloc's version already wastes up to 2x per object. |
| 4 | Threadcache full → age-based flush (`freeInCache >= 1 MB`, evict blocks older than N frees, halving N) | `ReleaseFreeInCache` `:1462-1477`; `RemoveCacheEntries` `:1167-1205` | (c) | libumem bounds per-bin count (128/64/32 slots, `umem_ptc.h:69-71`) and flushes half a bin as one batch (`umem_ptc_bin_flush`, `umem_ptc.c:531`; called from the full-bin path at `:405-406`). nedmalloc bounds total bytes and walks nine lists from the old end on every overflow. Byte-bounded is arguably better for footprint at 8 KB objects (32 slots x 8 KB = 256 KB per bin per thread), but libumem's `low_water` field is declared for exactly this and unused (`umem_ptc.h:165`, "for auto-tuning (future)"; no reader in `umem.c` or `umem_ptc.c`). Not a gap on the P8 list; note under footprint, §C. |
| 5 | Freeing thread keeps the block (no owner return); realloc/free of a foreign-mspace block goes to the block's own mspace via FOOTERS | `threadcache_free` `:1478` (freeing thread's `tc`); `CallFree(0, ...)` `:453-492`, `mspace_free` locates mstate from footer, `malloc.c.h:5630` | (a)/(c) | Same shape as libumem: a remote free lands in the freeing thread's PTC bin (`umem.c:4019-4022`), and on overflow in the freeing CPU's depot stripe. This is the very mechanism P8.5 names as the structural cost (`production-readiness.md:2129-2150`: "freed objects land on the freeing CPU's depot stripe"). nedmalloc has nothing to teach here; it has the same defect one level down, and its FOOTERS-based "free in the originating mspace" is a lock on a shared mutex, not an owner-return. |
| 6 | Pool of N mspaces per `nedpool`, thread → mspace by `threadid % N` at cache creation, trylock-scan on contention, create-new up to `p->threads` (default 4, max 16) | `AllocCache` `:1366-1367`; `FindMSpace` `:1589-1655`; `GetMSpace` `:1876-1885`; limits `:187,201` | (c) | This is a weaker version of libumem's per-CPU cache array (`UMEM_CPU_CACHE(cp, CPU_CACHED(cp->cache_cpu_mask))`, `umem.c:3278`, `:3450`). It is also the exact P8.2 bug: `abs(threadid) % end` at `:1366` where `threadid` is `pthread_self()`, which on glibc is a page-aligned pointer, so `% 4` is nearly always the same residue -- the CPU-hint defect `ae86536` fixed in libumem (`production-readiness.md:1802`, `:1896-1900`). The trylock-scan fallback rescues it but at a lock per miss. Nothing to import. |
| 7 | Per-pool (`nedpool`) heaps with user value | `nedcreatepool` `:1671`; `nedpsetvalue`/`nedgetvalue` `:1806-1819` | (a) | `umem_cache_create` with `cache_private` (`umem_impl.h:624`) and vmem arenas give a stronger form; nedmalloc's pool is an untyped dlmalloc heap. |
| 8 | Large allocations: dlmalloc `mmap` above 256 KB, `mremap`-based in-place resize, `M2_RESERVE_MULT(n)` address-space over-reserve so `realloc` grows without copy | `DEFAULT_MMAP_THRESHOLD` `nedmalloc.h:1588`; `mmap_resize` `malloc.c.h:4131-4164`; `nedprealloc` defaults `M2_RESERVE_MULT(8)` `nedmalloc.c:2078-2091`; Linux path ignores reserve and uses `mremap(MAYMOVE)` `malloc.c.h:1698`, `nedmalloc.h:340-341` | (b), narrow | libumem's oversize path is `vmem_alloc(umem_oversize_arena)` (`umem.c:3909`) and realloc is copy-always (`malloc_interpose.c:899-911`). For >128 KB blocks an `mremap` step in the interposer's `realloc` is the only realloc idea from nedmalloc that survives the slab constraint; see §B2. The Windows reservation machinery (`win32direct_mmap` `malloc.c.h:1796-1935`) is irrelevant. |
| 9 | Small realloc: threadcache-first, copy always; shrink-within-1 KB is a no-op | `nedprealloc2` `:1967-2029`, no-op test `:1990-1996` | (a) | For sizes in the threadcache nedmalloc is copy-always too, exactly like libumem. libumem's `realloc` already short-circuits `size == old_size` (`malloc_interpose.c:871-872`); nedmalloc's "within 1 KB smaller" no-op is a slightly wider version of the same thing. See §B2 for whether widening it is worth doing. |
| 10 | dlmalloc `internal_realloc`: shrink in place, extend into `top`, `mmap_resize`; else malloc-copy-free | `malloc.c.h:4739-4813` | (c) | Note what it does NOT do: dlmalloc 2.8.4 does not coalesce into a free *next* chunk on realloc (`:4752-4778` has only the three cases above). The "realloc is cheap if the next chunk is free" property belongs to glibc's ptmalloc, not to this dlmalloc. So the coalescing advantage the question posits is smaller than assumed even for nedmalloc itself. libumem cannot coalesce slab objects at all; irrelevant. |
| 11 | `independent_calloc` / `independent_comalloc`: N objects in one call, guaranteed adjacent | `nedpindependent_calloc` `:2201-2219`; `ialloc` `malloc.c.h:4903-5010` | (c) | Adjacency is the point of dlmalloc's version (one chunk split N ways). libumem's `umem_cache_alloc_batch` (`umem.c:3405`) gives N objects under one lock but not adjacent, and adjacency across slabs is impossible. A public "N from one cache" API has no user in the tree today and the internal batch is itself unused by the PTC refill path (`production-readiness.md:2099-2100`); that internal use is P8.5's fix (1), already planned. |
| 12 | `nedtrimthreadcache` / `neddisablethreadcache`: per-thread cache drain, required manually at thread exit or memory leaks | `:1821-1857`; Readme "B2: Memory Leakage" | (a) | libumem drains at thread exit via `pthread_key_create(&ptc_key, umem_ptc_cleanup)` (`umem_ptc.c:133`), full drain in `umem_ptc_destroy` (`umem_ptc.c:587-640`), with the P1.3a stranded-count oracle. nedmalloc documents the leak and asks the application to avoid it. libumem is ahead. |
| 13 | `nedmalloc_trim(pad)` → `mspace_trim` per mspace; passive trim every 4095 frees above 2 MB free | `:2142-2153`; `MAX_RELEASE_CHECK_RATE` `malloc.c.h:494-499` | (a) | `umem_reap()` (`umem.c:4890`) plus the periodic update pass with `MADV_DONTNEED` after `umem_reclaim_delay` (`umem.c:599-600`, `:4389-4460`). libumem's is time-based and automatic; nedmalloc's is free-count-based and per-mspace. Equivalent in kind. |
| 14 | Foreign-block detection so `free()` can accept system-malloc pointers: heuristic header sniffing with a stated ">0.01 % but real chance of segfault", or 3-word magic headers costing "20-50 %" of memory | `nedblkmstate` `:494-625`; `USE_MAGIC_HEADERS` `nedmalloc.h:185-194` | (c) | libumem's `interpose_owner_of` (`malloc_interpose.c:437-471`) classifies static / bootstrap / libc / umem by address range and its own metadata, refuses `OWN_UNKNOWN` after READY rather than guessing (`:882-896`), and P5.8 specifically exists to avoid the `buf[-1]` read nedmalloc does unconditionally. nedmalloc's approach is a security finding under attacker position (D) -- it reads attacker-adjacent memory to decide who owns a pointer -- and is what libumem's P8.3 is trying to make *cheaper*, not adopt. |
| 15 | Windows DLL patcher (`winpatcher.c`, 1107 lines) to redirect every loaded module's `malloc` | `winpatcher.c`; `REPLACE_SYSTEM_ALLOCATOR` `nedmalloc.h:217-236` | (c) | Not applicable: libumem's drop-in is `LD_PRELOAD` (`libumem_malloc.so`). Windows support in libumem is via the CRT, not import-table patching. |
| 16 | Operation log (CSV, optional stack walk) | `LogOperation` `:1040`; `logentry` `:753-766`; `nedflushlogs` `:1206` | (a) | libumem's transaction log (`UMEM_DEBUG=audit`, `umem_log`), alloc-site capture, and `umem_inspect_snapshot()` `.ums` are strictly more capable and queryable live (`README.md:66-70`). nedmalloc's is compile-time-gated, per-thread page-sized, and needs `nedflushlogs` which is "NOT threadsafe" (`nedmalloc.h:577`). |
| 17 | Statistics: `nedmallinfo`, `nedmalloc_footprint`, `nedmalloc_stats` (sum of `mspace_mallinfo`) | `:2102-2176` | (a) | Per-cache stats via `umem(1)` / `cmd_stats` (`umem_introspect.c:285`), `umem_cache_stats`, live attach. Ahead. |
| 18 | Double-free detection: head-of-bin compare only | `:1510-1514` | (a) | libumem: `UMERR_DUPFREE` under `UMEM_DEBUG` (`umem.c:1211`), and buftag validation. nedmalloc catches only the immediate repeat. |
| 19 | Large-page support (`ENABLE_LARGE_PAGES`, Readme "10-15 %") | `nedmalloc.h:49-55`; Readme B4 | (c) | vmem arenas already allow a hugepage-backed source; no measurement in libumem's record says TLB is a bottleneck, and the Readme figure carries no box, sha, or workload. Not on the P8 list. |
| 20 | `malloc2`/`realloc2` flag API (`M2_ZERO_MEMORY`, `M2_PREVENT_MOVE`, alignment on realloc) | `nedmalloc.h:355-443` | (c) | `M2_PREVENT_MOVE` ("try to grow in place, never move") is the one interesting flag, but in-place growth is impossible for a slab object; only the >128 KB `mremap` case in §B2 could honour it. `M2_ZERO_MEMORY` = `umem_zalloc`. A new public API with no caller is YAGNI. |
| 21 | Per-thread utilisation counter and the "disable the threadcache below 80 % hit rate" rule | `tc->mallocs/frees/successes` `:800`; Readme B3 | (a) | `umem_ptc_t.hits/misses` (`umem_ptc.h:183-184`). The disable rule is manual in nedmalloc; libumem's PTC never needs disabling because a miss falls through in a bounded number of steps now that P8.6 is primed (`umem_ptc_mag_prime`, `umem.c:2908-2919`, called at `:4090`). |

Count: (a) 10, (b) 2 (one of which only confirms an existing plan), (c) 9.

## (b) items

### B1. Threadcache ceiling: 8 KB is nedmalloc's number too -- confirms P8.2b's fix path, adds no new mechanism

**Gap.** P8.2b (`production-readiness.md:1978-2022`): sizes above
`umem_ptc_maxsize` (2048, `umem_ptc.c:45`) bypass the PTC, so every op is
`cc_lock` and every 31 ops per CPU is a blocking depot round trip; at
t>=128 on both metals throughput falls 105 -> 91 Mops (x86) and 277 -> 77
(arm) while glibc keeps scaling.

**What nedmalloc does.** `THREADCACHEMAX 8192` (`nedmalloc.c:191`) with a
compile-time warning that the bin count must move with it (`:193-198`).
That is the whole mechanism. The Readme gives no measurement for the choice
of 8 KB; it is not evidence, it is a coincidence with tcmalloc (32 KB),
jemalloc (32 KB), and the P8.2b plan (8 KB).

**Mapping.** Already specified: extend `ptc_size_classes` through 8192
(`umem_ptc.c:84-93` ends at `2048, 0, 0` on `:92`), which adds bins for 2560, 3072, 3584,
4096, 5120, 6144, 7168, 8192 (`umem_alloc_sizes`, `umem.c:531-532`), and
raise `mt_magsize` for the 2048-8192 band from 31/15 to 63
(`umem_magtype`, `umem.c:553-554`). `PTC_NBINS` 28 -> 36; `int8_t`
`umem_ptc_bin_table` still fits. Now that P8.6 is primed (`a2177b9`), the
per-thread magazine behind each new bin will actually hold objects.

**Cost.** Per-thread PTC struct grows by 8 bins x 32 slots x 8 B = 2 KB of
slots plus 8 x 64 B bin headers plus 8 `umem_ptc_mag_t`; ~3 KB on the
current 12 KB. Cached bytes per thread at the top bin: 32 x 8 KB = 256 KB
in the bin, plus up to 2 x 63 x 8 KB = 1 MB in the L2 magazines. At 192
threads that is up to ~240 MB of parked 8 KB objects in the worst case
(every thread's bins and both magazines full of the top class). This is
where nedmalloc's byte-bounded `THREADCACHEMAXFREESPACE` (table row 4) is
the relevant contrast: it caps a thread's cache at 1 MB total regardless of
class. libumem would want a per-bin capacity that shrinks with object size
(the current 128/64/32 tiering is the shape; add a fourth tier of 8 or 16
for >2 KB) rather than an age walk.

**Risk.** Footprint, measured by `test_ptc_footprint`, and the `umem_ptc.h:53-66` lesson that bin capacities are not a free variable: the
P6.3 halving moved a 28x cliff. Any new tier's capacity needs the same
alloc-N-free-N sweep at the new sizes. Owner: `@hot` (umem.c 2400-4100).

### B2. Realloc: what survives the slab constraint

**Gap.** Not a numbered P8 item. `realloc` is copy-always on any size
change (`malloc_interpose.c:899-911`), and the interposer is the P8.3
surface.

**The premise needs correcting first.** The question posits dlmalloc's
"realloc is cheap if the next chunk is free". dlmalloc 2.8.4's
`internal_realloc` (`malloc.c.h:4739-4813`) has exactly three in-place
cases: (i) new size fits in the old chunk (shrink, split remainder,
`:4758-4767`); (ii) old chunk is adjacent to `top` (`:4768-4778`); (iii)
chunk is `mmap`ped, `mmap_resize` (`:4756-4757`). It does **not** absorb a
free following chunk. So nedmalloc's coalescing realloc advantage exists
only for the most recently allocated chunk in an mspace and for >256 KB
blocks. And below 8 KB nedmalloc does not even reach `internal_realloc`:
`nedprealloc2` tries `threadcache_malloc` first and copies (`:1998-2018`),
so its small realloc is copy-always exactly like libumem's.

**What survives.**

1. **Shrink is a no-op.** nedmalloc returns `mem` unchanged when the new
   size is smaller by less than 1 KB (`:1990-1996`). libumem does this only
   for `size == old_size` (`:871-872`). Because libumem's `malloc` header
   records the `umem_alloc` size, i.e. request + header (`malloc_data_t`,
   `malloc.c:170-176`; written at `:253-262`) and `_umem_free` picks the
   cache from that recorded size, a shrink that stays inside the same
   `umem_alloc_table` index can be served by rewriting the header size in
   place -- no copy, no cache change. Widening the no-op to "same `umem_alloc_table`
   index" is a few lines in the `OWN_UMEM` case, and unlike nedmalloc's
   fixed 1 KB it is exact. Growth within the same class is the same
   rewrite. Cost: one table lookup. Risk: the header rewrite must keep the
   `UMEM_MALLOC_ENCODE` field consistent (`umem_impl.h:852`, `malloc.c:254`),
   and `process_free` (`malloc.c:535`) must still decode the buffer to the
   right cache afterwards; a regression is `realloc(p, n-1)` returning `p`
   at every class boundary and `free` of the result landing in the original
   cache.
   Small, safe, and worth doing; not from nedmalloc specifically (glibc
   does it too), but this is the item the question asked about.

2. **`mremap` for oversize blocks.** Above `UMEM_MAXBUF` (128 KB,
   `umem_impl.h:741`) a block is a `vmem_alloc(umem_oversize_arena)`
   (`umem.c:3909`), which on Linux is ultimately an `mmap` span. nedmalloc
   grows these with `mremap(MREMAP_MAYMOVE)` (`malloc.c.h:1698`, used at
   `:4145`), which moves pages instead of bytes. libumem's copy of a 100 MB
   buffer on `realloc` is the case where this matters. Mapping: in
   `realloc`'s `OWN_UMEM` case when both old and new sizes are above
   `UMEM_MAXBUF`, `mremap` the span and `vmem_xalloc`/`vmem_free`
   bookkeeping to move the arena's record. Cost: a Linux-only branch; the
   arena's span accounting has to be told the address changed, which is not
   a vmem operation that exists today. Risk: moderate -- it touches vmem
   internals and the VMA-count ceiling work (`README.md:214-232`); a
   `MAP_FIXED` slab span adjacent to the block would break the assumption
   that the kernel may extend in place. Defer unless a workload shows
   oversize `realloc` in a profile; none in the record does.

3. **Address-space over-reserve** (`M2_RESERVE_MULT(8)`, `nedprealloc`
   `:2085-2089`) is a Windows mechanism; on Linux nedmalloc itself ignores
   it (`nedmalloc.h:340-341`). Nothing to take.

**Nothing to take for slab-served sizes** beyond item 1. A slab object's
neighbours are other live objects of the same class; there is no chunk to
absorb.

## What libumem does better (honest both ways)

nedmalloc is a 2005-2010 design (`nedmalloc.h:2`) built when the state of
the art was ptmalloc2 and Hoard; it predates tcmalloc's per-CPU rseq work,
jemalloc 4+, mimalloc, and snmalloc, all of which return remote frees to
the owning heap lock-free. nedmalloc does not. On every axis on the P8 list
libumem is ahead or equal:

- **Thread → heap mapping.** nedmalloc: `pthread_self() % 4` at cache
  creation (`:1366`), the exact bug libumem shipped and fixed as P8.2, with
  a trylock scan and a hard cap of 16 mspaces (`:201`) after which threads
  block on a shared mutex. libumem: per-CPU cache array sized to the
  machine, corrected hint, and the depot beneath it.
- **Fast path shape.** nedmalloc hit = TLS read + bin index + doubly-linked
  unlink writing into the buffer (`:1428-1433`). libumem hit = TLS read +
  `b->slots[--b->count]` (`umem.c:3826`). No pointer chase, no link words
  in user memory (which is also the P5.4 freelist-mangling concern
  libumem's slab layer has and nedmalloc's threadcache has worse).
- **Thread exit.** libumem drains automatically and has an oracle
  (`umem_ptc_destroy`); nedmalloc leaks unless the application calls
  `neddisablethreadcache` per pool per thread (Readme B2).
- **Foreign pointers.** libumem classifies by owner and refuses unknowns;
  nedmalloc sniffs headers with a documented segfault probability
  (`nedmalloc.h:452-453`).
- **Introspection and debugging.** Not comparable: nedmalloc has a
  compile-time CSV log; libumem has live attach, audit mode, snapshots.
- **Object caches with ctor/dtor, vmem.** nedmalloc has no analogue.

Where nedmalloc is ahead, and it is minor:

- **Byte-bounded threadcache** (1 MB total, `:218`) vs libumem's
  count-bounded bins, which matters once bins hold 8 KB objects (§B1).
- **Shrink-realloc no-op window** (§B2 item 1), trivially matched.
- **`mremap` for very large realloc** (§B2 item 2), a Linux `mmap`-backend
  property, not a nedmalloc design idea.
- **Threadcache reach to 8 KB** -- which libumem already plans (P8.2b).

**Bottom line.** Nothing in nedmalloc addresses P8.5 (cross-stripe steal
under `frag`) or P8.2b's convoy mechanism; it has the same freeing-thread-
keeps-it structure that causes both, one level down in a shared mutex.
P8.3 (interposer cost) is a problem nedmalloc has in worse form
(`nedblkmstate` on every free). The one design point worth carrying is the
observation that once the per-thread cache reaches 8 KB objects, the cache
bound should be in bytes (or a steeply smaller slot count per tier), not a
flat slot count -- and that is a footprint note for the P8.2b
implementer, not a nedmalloc import.

## Not verified here

- No claim above about libumem's runtime behaviour was re-measured in this
  review; every libumem number cited is quoted from
  `docs/plans/2026-09-21-production-readiness.md` with its own provenance.
- nedmalloc's Readme performance claims ("10-15 %" for large pages, "90-99 %
  lock avoidance") carry no box, version, or workload and are reported as
  claims, not facts.
- P8.6 was closed at `a2177b9`/`a1901c8` while this review was being
  written; the plan table at `production-readiness.md:1804` still says
  "open". This review treats it as fixed on the strength of the commit,
  not of a gate run.

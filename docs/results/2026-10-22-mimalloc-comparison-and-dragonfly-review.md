# libumem vs mimalloc, and what DragonflyDB's use of mimalloc teaches us

Date: 2026-10-22. Two questions, answered together:

1. Benchmark libumem against mimalloc v2.5.2 -- both stock and with the three
   patches DragonflyDB applies -- plus jemalloc and glibc, on EC2 metal.
2. Review how DragonflyDB (`~/src/dragonfly`) actually uses mimalloc and those
   patches, and extract what libumem can learn from the choice.

Provenance for the benchmark: `c7i.metal-48xl` (192 vCPU, us-east-2, Debian 13,
gcc 14.2), libumem master `76d2283`, mimalloc `v2.5.2`
(sha256 5cf9d2a1...bdab), jemalloc 5.3.0. Method: one `bench_main` process per
(allocator, workload), `-D 60:10` continuous intervals, first 4 dropped as
warm-up, 6 measured; `numactl --interleave=all`; 3 GiB total live set (`-L
3072`); sizes 64:256; `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536` for
every arm. umem and both mimalloc builds ran twice (own run-to-run gap on the
record); jemalloc and glibc once. Raw CSV + summary: the run was captured to
`mimalloc-compare.csv` / `-summary.txt` (archived with this analysis).

## Part 1 -- Benchmark results

### prodcons (pure alloc/free throughput, no retained live set)

| allocator | Mops/s (run1 / run2) | median RSS MB |
|-----------|----------------------|---------------|
| umem         | 650 / 603 | ~223 |
| mimalloc stock   | 530 / 548 | 64 |
| mimalloc patched | 646 / 449 | 64 |
| jemalloc     | 1014 / 1013 | 85 |
| glibc        | 1096 / 1064 | 57 |

**prodcons does not separate umem from mimalloc, and the reason is itself the
finding.** Each individual process is internally dead-stable (mimalloc's two
runs were a flat 647M and a flat 449M across all six measured intervals -- not
warm-up drift), but whole processes differ up to 44 % from one launch to the
next. That is start-up placement variance on a 192-core box (first-touch NUMA +
thread scheduling), not allocator speed. Under the non-overlap rule umem
(603-650) and mimalloc (449-648) overlap, so prodcons shows **no reliable
throughput difference** between them. jemalloc and glibc are genuinely faster
on this contention-free hammer (tight, non-overlapping ~1013 and ~1080).

### frag (retains a 3 GiB live set; measures RSS and fragmentation)

This workload is stable run-to-run (umem 648.6 vs 648.8 Mops between its two
processes), so differences here are trustworthy.

| allocator | Mops/s | RSS MB | frag = RSS / live |
|-----------|--------|--------|-------------------|
| umem             | 649 | 4298 | 1.78 |
| mimalloc stock   | 856 | 3808 | 1.58 |
| mimalloc patched | 856 | 3808 | 1.58 |
| jemalloc         | 1120 | 3802 | 1.60 |
| glibc            | 431 | 3896 | 1.63 |

Under sustained fragmenting churn at 192 threads:
- **umem is the most memory-hungry** of the modern allocators here: 4298 MB for
  a 3 GiB live set (frag 1.78) vs mimalloc/jemalloc at ~3800 MB (frag ~1.58).
  ~13 % more RSS for the same live set. This is consistent with F1 from the v2
  comparison (umem holds more cross-thread working set); the magazine/depot
  layers trade memory for lock-free speed.
- **umem throughput (649) sits between glibc (431) and mimalloc (856)** on frag:
  faster than glibc, slower than mimalloc and jemalloc.
- **jemalloc wins frag outright** (fastest AND near-lowest RSS) on this box.

### Stock vs patched mimalloc: identical, as predicted from the diffs

| metric (frag) | stock | patched |
|---------------|-------|---------|
| Mops/s        | 856.2 | 856.4 |
| RSS MB        | 3808.0 | 3808.0 |
| frag          | 1.580 | 1.582 |

Within noise on every axis. The built libraries differ by exactly **3720 bytes
of .text** -- the size of the one function the patches add. This is the
empirical confirmation of the static reading in Part 2: the DragonflyDB patches
do **not** touch mimalloc's allocation hot path. They add an introspection
function that an application calls during its own defragmentation pass; a pure
malloc/free benchmark never calls it, so stock and patched measure identically.
Benchmarking "mimalloc with the patches vs without" on raw throughput/RSS can
only ever show no difference -- the patches' value lives in the *application's*
defrag loop, not the allocator.

## Part 2 -- How DragonflyDB uses mimalloc, and why

DragonflyDB (`~/src/dragonfly`, the Redis/Memcached-compatible datastore -- not
DragonFly BSD) vendors mimalloc **v2.5.2**, fetched by `ExternalProject_Add` in
`src/external_libs.cmake`, built **static** with `-O3 -g -DMI_STAT=0 -DNDEBUG`,
`MI_NO_PADDING=ON`, and crucially **`MI_OVERRIDE=OFF`**: mimalloc is NOT an
LD_PRELOAD global `malloc` replacement. It is used through its explicit API
(`mi_heap_malloc_aligned`, `mi_free`, `mi_usable_size`, `mi_heap_visit_blocks`),
one `mi_heap_t` per shard.

### The architecture that the allocator serves

Dragonfly is shared-nothing: each shard is pinned to one proactor thread with a
**thread-local mimalloc heap** (`ServerState::tlocal()->data_heap()`,
`src/server/engine_shard.cc`). `MiMemoryResource` (`src/core/mi_memory_resource.
{h,cc}`) wraps a `mi_heap_t` as a `std::pmr::memory_resource` so STL/absl
containers allocate from the shard's heap. Cross-thread frees are deliberately
routed back to the owning shard (`docs/zero_copy_get.md`) so per-shard memory
accounting stays exact. mimalloc was chosen for: fast thread-local allocation,
per-heap isolation, `mi_usable_size`, and heap-walking introspection
(`mi_heap_visit_blocks`, used by `MEMORY` commands in `src/server/memory_cmd.cc`).

### The three patches, and the single feature they implement

All three patches in `patches/mimalloc-v2.5.2/` serve one purpose: **active
defragmentation driven by page-utilization visibility.**

- `0_base.patch`: CMake back-compat (3.18->3.16) + adds
  `bool mi_heap_page_is_underutilized(heap, p, ratio)` in `src/alloc.c`. It
  reads mimalloc's internal `mi_page_t` for the object `p`: the page is a
  defrag candidate if it belongs to this heap, is not `in_full`, is not the
  head of the page queue (`page->prev != NULL`, i.e. not the page currently
  serving mallocs), and is sparse (`page->used <= capacity * ratio`).
- `1_add_stat_type.patch`: adds `mi_page_usage_stats_t` + `MI_DFLY_PAGE_*`
  flag bits to `include/mimalloc/types.h`.
- `2_return_stat.patch`: changes the function to **return the struct** with
  detailed per-page stats when asked, keeping a cheap boolean-ish fast answer
  otherwise.

### How Dragonfly consumes it

`src/core/page_usage/page_usage_stats.{h,cc}` wraps the patched call in a
`PageUsage` class with a **cycle-based time quota** (`CycleQuota`, default
150 us). `src/core/compact_object.cc` walks live data-structure objects
(`DefragIfNeeded` / `ReallocIfNeeded` for StringMap, listpack, intset, SortedMap,
hash/set/zset/list, search indices) and for each asks "is this object sitting on
an under-utilized page?" If yes, it **reallocates** the object -- copying it to
a denser page -- so the sparse page empties and mimalloc can return it to the OS.
This is how Dragonfly keeps RSS from ballooning under the fragmenting churn of a
long-lived cache (exactly the `frag` workload above), without the global
stop-the-world compaction a moving GC would need. The patches exist because
mimalloc does **not** expose page occupancy in its public API; Dragonfly had to
reach into `mi_page_t`.

## What libumem can learn

1. **The thing Dragonfly had to patch into mimalloc, libumem has natively.**
   mimalloc's public API hides page occupancy, so Dragonfly added
   `mi_heap_page_is_underutilized` to read `page->used <= capacity*ratio`. In
   libumem's slab allocator that quantity is a first-class concept: every
   `umem_slab_t` tracks `slab_refcnt` against `slab_chunks`. "Is this object on
   an under-utilized slab, and which slab?" is answerable from
   `umem_cache`/`umem_slab` without patching the allocator. libumem already
   ships live-heap introspection (`umem_inspect`, the gdb/mdb walkers,
   `umem_dump_contention`). **A supported "slab occupancy + is-this-object-
   relocatable" query is a natural, differentiating libumem feature** -- the
   same capability an application like Dragonfly wants, offered as API rather
   than a private patch. This is worth a design note; it plays to the slab
   model's strength.

2. **Application-driven defrag needs an allocator that exposes placement, not
   one that hides it.** Dragonfly deliberately did NOT pick a moving/compacting
   allocator; it kept pointers stable and did selective copy-and-free in the
   application under a microsecond quota, using the allocator only as an
   oracle. libumem's non-moving slab design is compatible with exactly this
   pattern. Document it as a supported use (an allocator that answers "should I
   relocate this?") rather than attempting internal compaction.

3. **`MI_OVERRIDE=OFF` + per-thread heaps is the high-performance embedding
   mode**, not LD_PRELOAD. libumem's strongest story is the same: linked and
   used through its API (per-CPU magazines, `umem_cache_create` for typed
   caches), where it keeps its introspection and typed-cache advantages. The
   LD_PRELOAD `libumem_malloc.so` path is the compatibility shim, not where
   libumem should expect to win -- and the benchmark agrees: on the pure
   prodcons hammer glibc/jemalloc lead, while libumem's edge is structural
   (typed caches, introspection, the slab model), which a flat malloc/free
   benchmark cannot show.

4. **On raw malloc throughput and RSS under churn, umem is currently behind
   mimalloc and jemalloc** (frag: 649 vs 856 vs 1120 Mops; RSS 4298 vs 3808 vs
   3802 MB). That is an honest standing: libumem trades memory and some
   throughput for lock-free per-CPU magazines and live introspection. The
   ~13 % RSS overhead vs mimalloc/jemalloc on a fragmenting live set is the
   clearest number to target if footprint becomes a goal (it echoes F1 in the
   v2 comparison -- the magazine/depot working set). Not a regression; a
   known trade recorded with a figure.

5. **Patches that only add an introspection hook cost nothing on the hot path
   -- and that is the right shape for a hardening/observability feature.**
   mimalloc patched vs stock measured byte-identical in RSS and within noise in
   throughput because the new code is never on the alloc/free path. libumem's
   own debug-hook work (T1/T8: divert to the slow path only when armed, +0.00 %
   while disarmed) already follows this principle; the mimalloc/Dragonfly split
   is independent corroboration that "add an off-path query, don't instrument
   the hot path" is how to add allocator observability without a tax.

## Honest limits of this comparison

- One box (x86_64 c7i.metal-48xl), one size range (64:256), two workloads. The
  arm half and other size ranges are not run here; the conclusions about
  stock==patched and umem's RSS trade are robust (stable, repeated), the
  prodcons throughput ranking between umem and mimalloc is *not* resolved
  (process placement variance swamps it).
- mimalloc was built **shared** so the bench could dlopen it; Dragonfly builds
  it **static**. The allocation code is identical; only the link form differs.
- This measures mimalloc as a *malloc*, which is not how Dragonfly uses it
  (per-heap API + application defrag). The benchmark answers "how do the
  allocators compare as allocators"; Part 2 answers "why Dragonfly chose this
  one", and those are deliberately different questions.

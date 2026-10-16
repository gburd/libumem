---
title: Slab and vmem
parent: Design
nav_order: 1
permalink: /design/slab-and-vmem/
lede: >-
  The two layers underneath everything: object-caching slabs over a vmem
  virtual-address allocator.
---

libumem is two allocators stacked. **vmem** manages ranges of a resource
(virtual addresses, by default) and hands out spans. The **slab layer**
carves those spans into fixed-size objects grouped by an *object cache*.
Everything else -- magazines, the per-thread cache, the rseq layer -- is a
caching front end over this pair.

## vmem: resource arenas

A vmem arena allocates and frees *segments* of an integer resource. The
root arena's resource is the process virtual address space, backed by
`mmap`; a child arena can carry any resource (DMA regions, NUMA-bound
address ranges, an id space). Arenas nest: a child imports from its parent
in large chunks and sub-allocates, so the parent's lock is touched rarely.

Two properties make vmem more than a `malloc` for addresses:

- **Quantum caching.** Allocations at or below a *quantum* (a small
  multiple of the arena's base unit) are served from a per-size *quantum
  cache* -- itself an object cache -- so the common small-span request
  never reaches the arena's segment list or its lock. Objects of 512 bytes
  and under take this path in libumem.
- **Constant-time coalescing.** Freed segments are merged with their
  neighbours through boundary tags and a segment hash, so an arena does not
  fragment into a long free list the way a naive address allocator does.

See [`umem_cache_create(3)`]({{ '/reference/api/umem_cache_create/' | relative_url }})
and the vmem sources (`vmem.c`, `vmem_mmap.c`, `vmem_sbrk.c`).

## Slabs: object caches

An **object cache** (`umem_cache_t`, created by
[`umem_cache_create(3)`]({{ '/reference/api/umem_cache_create/' | relative_url }}))
owns objects of one size and, optionally, one *type*: a constructor runs
once when a buffer is first carved, a destructor once when the slab is
finally released. Between those, the buffer cycles through alloc/free
*already constructed*, so a workload that allocates and frees the same
shape of object repeatedly amortizes its initialization across every reuse.
This is the headline feature no size-class allocator offers.

A **slab** is one span from vmem, divided into equal buffers plus a small
amount of control state (a `umem_slab_t`, and per-buffer `umem_bufctl_t`
records for the hashed path). The cache keeps slabs on three lists:
complete (all buffers free), partial (some in use), and -- transiently --
empty slabs awaiting reclaim. Allocation prefers a partial slab; a free
returns the buffer and, if the slab becomes complete, makes it a reclaim
candidate.

### Reclaim

Idle complete slabs are returned to vmem, and their backing pages released
to the OS with `madvise(MADV_DONTNEED)`. Debug builds must preserve the
per-buffer metadata (buftags, free patterns) that a later integrity check
reads, so reclaim on a HASH or multi-page debug slab is handled carefully
(P1.5): `slab_state` is published under the right lock, and metadata that a
corruption check depends on is not discarded by `MADV_DONTNEED`.

## Why a slab allocator at all

The size-class allocators (jemalloc, tcmalloc, mimalloc) also segregate by
size and cache per-thread. What slab adds is the *object* abstraction:
ctor/dtor type stability, and the vmem resource arenas beneath it that make
address-space management a first-class, hierarchical thing rather than a
single global heap. If your workload is "billions of anonymous small
mallocs," a size-class allocator is the better fit. If it is "a
long-running service with object lifecycles, pools, and address regions,"
that is what slab was built for -- see [Lineage]({{ '/philosophy/lineage/' | relative_url }}).

{: .note }
> The Linux port originally built far smaller slabs than Solaris because
> Linux's heap quantum is 4 KiB where Solaris's is 64 KiB, and each slab
> span is its own `mmap`. That is the root of the heap-ceiling story; see
> [The Linux heap ceiling]({{ '/design/heap-ceiling/' | relative_url }}).

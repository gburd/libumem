---
title: "ADR-0001: GC removed"
parent: Decisions (ADRs)
nav_order: 1
permalink: /adr/0001-gc-removed/
---

# ADR-0001: Garbage collection / the unused reclamation layers removed

- **Status:** Accepted.
- **Decision:** libumem is a manual allocator; the inherited speculative
  reclamation/GC-style machinery that never ran is removed, and slab reclaim
  is the one real reclamation path.

## Context

The portable fork carried several reclamation-adjacent subsystems that read
as "garbage collection" aspirations -- a NUMA *policy* layer, a per-CPU
caching module, and assorted update-thread machinery -- much of which either
never ran or did not build. libumem's actual memory model is manual:
`umem_free` returns a buffer to its cache; idle complete slabs are returned
to vmem and their pages released with `madvise(MADV_DONTNEED)` by the
reclaim path and the maintenance thread. There is no tracing collector and
no intent to add one.

## Decision

Keep exactly one reclamation story -- slab reclaim plus the maintenance
thread -- and remove the dead layers rather than carry them as aspiration.
The `umem_numa.[ch]` policy layer (which never ran) was removed 2026-09-21;
`--enable-percpu-caching` (which did not build) and the HTM module were
quarantined in `attic/` the same day.

## Consequences

- The memory model is simple to reason about and to document.
- Reclaim correctness is a real, tested property (P1.4/P1.5): destruction
  drains retained slabs; debug metadata a corruption check depends on is not
  discarded by `MADV_DONTNEED`; `slab_state` is published under lock.
- Users wanting automatic reclamation under pressure use the cache
  `reclaim` callback, not a collector.

## Alternatives considered

- **Keep the dormant layers "for later."** Rejected: non-working code
  advertised as a feature is worse than its absence. `attic/README.md`
  records why each removed piece is unfinished in a way that could not be
  fixed by finishing it.

---
title: "ADR-0003: Remote-free declined"
parent: Decisions (ADRs)
nav_order: 3
permalink: /adr/0003-remote-free-declined/
---

# ADR-0003: snmalloc-style message-passing remote-free declined

- **Status:** Accepted (2026-10-16).
- **Decision:** do **not** adopt snmalloc's message-passing remote-free. The
  cross-thread locality gap is characterized and deferred, not fixed.

## Context

A free performed by a thread that does not own the buffer's per-CPU cache
costs more than a local free. Round 9 measured this directly: the
cross-thread cost is real (15.7x LLC misses). snmalloc's answer is elegant:
a non-owning free is batched into a per-allocator MPSC message queue and
recycled by the *owner*, warm. The question was whether libumem should adopt
it.

## Decision

Decline it. Two reasons:

1. **It is a fundamental re-architecture, not an adoption.** snmalloc's
   remote-free needs a *per-allocator identity* and an MPSC return queue.
   libumem has no per-allocator identity today -- its cross-thread path is
   the magazine/depot hierarchy, not a set of addressable allocators.
   Grafting message passing on changes the cross-thread architecture of the
   allocator. The project guardrail is explicit: *if remote-free
   fundamentally changes libumem, don't do it.* libumem stays a Solaris-slab
   port.
2. **The cheaper alternative was already shown not to help.** Round 9's
   stripe-rotation prototype did not reduce remote traffic (it was unchanged
   at stripe granularity), so there is no incremental win sitting between the
   status quo and the full re-architecture.

## Consequences

- The cross-thread-free RSS and throughput gap at high thread counts
  ([limits]({{ '/reference/limits/' | relative_url }})) stays an **open,
  documented** item -- characterized, with the mechanism understood, not
  silently carried as a to-do.
- libumem's architecture stays comprehensible as the Bonwick & Adams design.

## Alternatives considered

- **Full snmalloc remote-free.** Rejected per the guardrail above.
- **Stripe rotation / other stripe-granularity schemes.** Rejected: Round 9
  measured no change in remote traffic.

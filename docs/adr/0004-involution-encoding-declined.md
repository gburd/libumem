---
title: "ADR-0004: Involution encoding declined"
parent: Decisions (ADRs)
nav_order: 4
permalink: /adr/0004-involution-encoding-declined/
---

# ADR-0004: snmalloc's involution freelist encoding declined (LIFO conflict)

- **Status:** Accepted (2026-10-16).
- **Decision:** keep libumem's forward-only cookie mangling; do **not** adopt
  snmalloc's doubly-encoded (involution) freelist, because libumem's
  magazines and per-thread-cache bins are LIFO stacks and the encoding
  requires FIFO queues.

## Context

snmalloc's `docs/security/FreelistProtection.md` describes encoding *both*
directions of a doubly-linked free list: a forward transform `f(a) = a ^ k0`
and a backward transform `g(a, b) = (a ^ k1) * (b ^ k2)`, with the invariant
`x.next.prev == x`. This catches a forged *backward* link, which forward-only
mangling cannot. It is a genuinely stronger scheme -- where it applies.

## Decision

Decline it. snmalloc states the hard precondition plainly: the backward-link
encoding "can only use queues, and not stacks," because construction order
and consumption order must **match**.

libumem's structures violate that precondition. Confirmed from the code:
magazines are LIFO stacks -- alloc pops `mag_round[--rounds]`, free pushes
`mag_round[rounds++]` (`umem.c`), the last freed is the next allocated. The
per-thread-cache bins are stacks too. LIFO is not incidental; it is why a
freed buffer comes back cache-hot, a core performance property of the
magazine design.

Turning the magazines and bins into FIFO queues to accommodate the encoding
would trade that locality property away -- a re-architecture of the hot path,
not a security add-on.

## Consequences

- libumem keeps its forward-only cookie mangling (P5.4/P5.13/P5.13b), which
  carries a per-process `AT_RANDOM` cookie and is therefore **stronger than
  glibc's cookieless safe-linking**. See
  [Freelist mangling]({{ '/design/freelist-mangling/' | relative_url }}).
- The specific class involution would catch -- a forged *backward* link -- is
  not defended, because libumem's free structures are singly-linked LIFO
  stacks that have no backward link to forge.

## Alternatives considered

- **Convert magazines/bins to FIFO queues and adopt involution.** Rejected:
  loses the LIFO cache-locality property.
- **Scope the encoding to only the paths that happen to be queues.**
  Rejected: libumem's protected free structures are the LIFO magazines and
  bins; there is no queue-shaped free path to scope it to.

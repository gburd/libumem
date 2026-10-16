---
title: "ADR-0005: sparsemap removed"
parent: Decisions (ADRs)
nav_order: 5
permalink: /adr/0005-sparsemap-removed/
---

# ADR-0005: The vendored sparsemap removed

- **Status:** Accepted (v4.0.0).
- **Decision:** remove the vendored sparsemap (`sm.c`/`sm.h`, upstream
  v5.5.0) and its 86 test helpers; the soname becomes `libumem.so.2`.

## Context

The tree vendored a copy of `sparsemap` (namespaced
`-DSPARSEMAP_PREFIX=umem_` so it could not collide), used by machinery that
did not survive the revival. Later upstream releases were reviewed for
whether to update the pin: v5.6.0 was found to have regressions and the pin
was held at v5.5.0; v5.7.0 was reviewed and **not taken** (see the
`docs/reviews/2026-09-27-sparsemap-*` notes).

## Decision

Rather than carry, update, or re-justify a vendored dependency nothing in the
live allocator needed, remove it entirely in v4.0.0. The sparsemap-era test
helpers that nothing built go with it. The soname moves to `libumem.so.2`;
`libumem_malloc.so.1` is unchanged.

## Consequences

- One fewer vendored dependency to track for upstream security and
  correctness regressions.
- A soname bump (`libumem.so.1` -> `libumem.so.2`), which is the correct
  signal for the removed symbols.
- The reviews of v5.6.0/v5.7.0 remain in `docs/reviews/` as the record of why
  updating was not the answer.

## Alternatives considered

- **Update the pin to v5.7.0.** Rejected: the review found no reason to take
  it, and the dependency was not needed.
- **Keep v5.5.0 vendored.** Rejected: carrying an unused vendored library is
  a standing maintenance and audit cost for no benefit.

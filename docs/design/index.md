---
title: Design
nav_order: 2
has_children: true
permalink: /design/
lede: >-
  How libumem works, one mechanism per page -- lifted from the commit
  record, the production-readiness plan, and the measured results, with
  the open gaps stated as open.
---

Each page here documents one mechanism of the allocator: what it is, why
it is shaped that way, and -- where libumem differs from the modern
size-class allocators -- what the difference costs and what it buys.

- [Slab and vmem]({{ '/design/slab-and-vmem/' | relative_url }}) -- the two
  layers underneath everything: object-caching slabs over a vmem virtual
  address allocator.
- [Magazines, depot, and the per-thread cache]({{ '/design/magazine-depot-ptc/' | relative_url }})
  -- the per-thread/per-CPU caching hierarchy that keeps the fast path off
  the shared locks.
- [The rseq per-CPU layer]({{ '/design/rseq-percpu/' | relative_url }}) -- the
  restartable-sequence lock-free reload wired in behind the per-thread
  cache (P8.5b).
- [Freelist mangling]({{ '/design/freelist-mangling/' | relative_url }}) -- the
  cookie-encoded free pointers (P5.4/P5.13), and why libumem keeps
  forward-only mangling rather than snmalloc's involution encoding.
- [The span table]({{ '/design/span-table/' | relative_url }}) -- exact heap
  ownership for `free()` (P7.4), replacing the old convex-hull range check.
- [Introspection]({{ '/design/introspection/' | relative_url }}) -- the
  live-heap debugging surface (T1-T14): `umem`, `umemctl`, and the gdb/lldb
  integrations. libumem's distinctive feature.
- [The Linux heap ceiling]({{ '/design/heap-ceiling/' | relative_url }}) -- the
  VMA-density fix that removed the ~5 GB `vm.max_map_count` wall.
- [Security hardening]({{ '/design/hardening/' | relative_url }}) -- the Phase 5
  posture, framed honestly.
- [Platform & architecture abstraction (PAL/AAL)]({{ '/design/pal/' | relative_url }})
  -- the design for collecting scattered platform code behind two
  interfaces. This is also the **porting contract**: the required-symbol
  surface a new platform or architecture must satisfy.

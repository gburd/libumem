---
title: Lineage
parent: Philosophy
nav_order: 1
permalink: /philosophy/lineage/
lede: >-
  Jeff Bonwick's slab allocator, the Bonwick & Adams magazine and vmem
  layers, Solaris libumem, and the long road to this port.
---

libumem is not a new allocator. It is a thirty-year-old one, revived. The
design is Jeff Bonwick's, and understanding where it comes from explains why
it is shaped so differently from jemalloc or mimalloc.

## 1994 -- the slab allocator

Bonwick's [*The Slab Allocator: An Object-Caching Kernel Memory
Allocator*](https://www.usenix.org/legacy/publications/library/proceedings/bos94/bonwick.html)
(USENIX 1994) introduced the idea that structures the whole library: an
allocator organized around **object caches**, not size classes. A cache
holds objects of one type, with a constructor that runs once per buffer and
a destructor once at the end -- so the expensive part of initialization is
paid once and amortized across every reuse. SunOS 5.4 shipped it in
production.

## 2001 -- magazines and vmem

Bonwick and Adams' [*Magazines and Vmem: Extending the Slab Allocator to
Many CPUs and Arbitrary
Resources*](https://www.usenix.org/legacy/event/usenix01/full_papers/bonwick/bonwick.pdf)
(USENIX 2001) added the two layers that make the slab allocator scale:

- **Magazines** -- per-CPU stacks of pre-allocated objects, so the common
  alloc/free never touches a shared lock. This is the
  [magazine/depot/PTC]({{ '/design/magazine-depot-ptc/' | relative_url }})
  hierarchy.
- **vmem** -- a general resource allocator (virtual addresses, or any
  integer resource) with quantum caching and constant-time coalescing, made
  a first-class subsystem beneath the slabs. See
  [slab and vmem]({{ '/design/slab-and-vmem/' | relative_url }}).

## ~2002-2005 -- Solaris libumem

Solaris 9 shipped **libumem** as a userspace port of the kernel slab
allocator: the same algorithms, running in-process. Solaris 10 and
OpenSolaris released it broadly, and `mdb`'s `::findleaks` and `::umem_log`
dcmds became the gold standard for userspace heap debugging -- the capability
this port works hardest to bring back (see
[Introspection]({{ '/design/introspection/' | relative_url }})).

## ~2007-2010 -- the portable fork

Wez Furlong (Message Systems) ported libumem to Linux and Windows for the
Ecelerity MTA, released as `portableumem`. OmniTI later took over the
portable fork; steady but minimal maintenance followed.

## 2014-2023 -- fallow

A long quiet period. The original Solaris source stayed preserved in
illumos-gate; the portable fork accumulated rot -- features that no longer
worked, documentation that described things the code did not do.

## 2024-2025 -- this revival

A substantial rework: the allocator hot paths rebuilt, modernization to
C11/C17, portability fixes for Linux/FreeBSD/macOS, new features (the
per-thread cache, scoped arenas, ownership tracking, profiling), and -- the
headline -- the runtime introspection tools that bring `mdb`-class debugging
back on non-Solaris platforms. What was added, fixed, and removed is on the
next page: [What this port changed]({{ '/philosophy/what-changed/' | relative_url }}).

{: .note }
> The lineage is also why libumem's licence is CDDL (the OpenSolaris
> lineage), not the BSD/MIT licences common to the newer allocators.

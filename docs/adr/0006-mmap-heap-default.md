---
title: "ADR-0006: mmap heap by default"
parent: Decisions (ADRs)
nav_order: 6
permalink: /adr/0006-mmap-heap-default/
---

# ADR-0006: The mmap heap is the default, illumos included

- **Status:** Accepted (2026-11-27, maintainer decision).
- **Decision:** the heap comes from the mmap backend on every platform,
  illumos included.  The sbrk heap, illumos libumem's default, stays
  available with `UMEM_OPTIONS=backend=sbrk`.

## Context

`init_lib.c` chose the heap backend under `#if defined(sun)`.  configure
builds with `-std=c17`, which does not define the bare `sun` macro, so the
illumos branch never compiled and illumos got the mmap heap like every other
platform.  Once that test became `__sun`, the question was whether illumos
should go back to sbrk, as illumos libumem does.

The review measured both heaps on OpenIndiana (EC2 c5.9xlarge, gcc 14.3, at
a0df1ed), against illumos libumem (illumos-gate f71cb08e8c):

- **Freed memory.**  After freeing 1 GiB of 256 KiB objects and a reap, RSS
  is 6,184 KB on the mmap heap and 1,070,396 KB on the sbrk heap (illumos
  libumem: 1,068,712 KB).  The sbrk heap never gives memory back.
- **Per-thread cache separation**
  ([P5.12]({{ '/design/hardening/' | relative_url }})).  The mmap heap keeps
  internal slabs and user slabs in separate mappings.  The sbrk heap is one
  contiguous run, so an internal slab can sit directly below a user slab.
- **Use after free.**  Spans the mmap heap returns read as zeros
  (MADV_DONTNEED), and spans of at least `mmap_guard` fault (PROT_NONE).  On
  the sbrk heap freed memory stays live.  (On illumos the first half was
  not true when this was decided: its MADV_DONTNEED keeps the data.  See
  Consequences.)
- **Speed.**  The sbrk heap is faster.  A one-thread gcc compile runs 11.2%
  slower than illumos libumem on the mmap heap and 2.4% slower on the sbrk
  heap; MADV_DONTNEED accounts for most of that difference.  The sbrk heap
  leads by 9-18% on glibc bench-malloc-thread at 4 to 32 threads, and by
  23-33% on larson.

## Decision

mmap, the option the review recommended: the fork exists for its
hardening, and the sbrk heap gives up RSS return, P5.12's separation, and
the use-after-free properties of returned spans.  No code changed for this;
the default was already mmap.  On illumos, `backend=sbrk` is refused when
libmapmalloc is loaded, as in illumos libumem.

## Consequences

- **Later finding (b643f47).**  illumos's MADV_DONTNEED is not a discard:
  it writes the dirty pages to swap and keeps their data, so on illumos a
  freed span below the guard was still readable through a stale pointer,
  and the call was what made the compile slow.  The discard is MADV_PURGE
  there now.  On UltraSPARC the compile under LD_PRELOAD went from
  297-302 s to 248-252 s (-16%, 3 alternating pairs); the x86 numbers above
  were not re-measured.  This strengthens the decision rather than changing
  it: the mmap heap now has the use-after-free property on illumos too.
- Whatever compile gap remains on x86 is unmeasured.  The next candidates
  are skipping the discard below some span size or batching it.
- The sbrk heap stays tested: `make check` runs it in
  `test_errno_preserved_sbrk.sh` and, on illumos, `test_sbrk_foreign_brk`.

## Alternatives considered

- **sbrk on illumos** (one line in `init_lib.c`, plus dropping the mmap pin
  from `test_oversize_vma` and `test_ptc_adjacency`).  Rejected for the
  costs above.
- **mmap with the discard skipped for small spans.**  Not measured; it
  would give up RSS return for small spans to win back compile time.

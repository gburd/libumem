---
title: The Linux heap ceiling
parent: Design
nav_order: 7
permalink: /design/heap-ceiling/
lede: >-
  The VMA-density fix that removed the ~5 GB vm.max_map_count wall --
  diagnosed through three wrong attempts to the right cause, and fixed in
  two halves.
---

Until v3.2.0, libumem on Linux hit a wall at roughly **5 GB**: `umem_alloc()`
returned NULL while glibc on the same box reached 96 GB. The cause was not
the mmap backend -- three attempts there failed and are documented -- it was
**slab density** colliding with a kernel limit on the number of memory
mappings.

## The mechanism

Every slab span is its own `mmap(MAP_FIXED)`, and the kernel does **not**
merge adjacent `MAP_FIXED` mappings into one VMA (virtual memory area). The
process has a hard cap on VMAs: `vm.max_map_count`, default **65530**.

Under Linux's 4 KiB heap quantum, libumem built far smaller slabs than it
did under Solaris's 64 KiB quantum -- so it needed many more slab spans to
hold the same bytes, and each span is a separate VMA. At ~5 GB the process
ran out of VMAs and allocation failed. glibc, which uses `brk` and large
`mmap` arenas, never approaches the VMA cap.

## The fix, in two halves

The fix restores Solaris-era slab density with two floors, each measured:

1. **`UMEM_MIN_SLAB_OBJECTS`** -- a floor on objects-per-slab for the hashed
   best-fit path. At 2 GB of 4 KiB objects: **16,283 VMAs -> 75**.
2. **`UMEM_MIN_QCACHE_SLAB`** -- a floor for the quantum-cache path that
   objects of 512 B and under take. At 2 GB: **15,702 VMAs -> 274**, at zero
   measured RSS cost.

The first floor was shipped as a complete fix and was *not*: it covered only
the hashed path. The second came from a hard-limit hunt that tested the size
class the first fix's own regression did not exercise -- the AGENTS.md
"grep for siblings before claiming the class is closed" rule, learned here
the expensive way.

## What it is gated by

Both arms are gated by regressions (`test_heap_ceiling`, the `..._512.sh`
variant), and a 64 KiB-quantum simulation (`test_slab_floor`) checks that
both floors are **no-ops on illumos**, where the native 64 KiB quantum
already gives dense slabs. The full diagnosis, including the three failed
mmap-backend attempts, is in
`docs/results/2026-09-22-umem-heap-ceiling-vma.md`.

Measured result: an 8-11 GB live set of 1-4 KiB objects -- the point that
previously crashed or failed 39 % of allocations -- now completes with
**zero failures** on every test box, at the default `vm.max_map_count`.

## If you run an older release

Raise the kernel limit:

```sh
sysctl -w vm.max_map_count=1048576     # or a value suited to your heap
```

{: .note }
> This whole class of problem is Linux-specific and quantum-driven. It does
> not arise on Solaris/illumos (64 KiB quantum) and informs the PAL design's
> treatment of the page quantum as a per-platform constant; see
> [PAL/AAL]({{ '/design/pal/' | relative_url }}).

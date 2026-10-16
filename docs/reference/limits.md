---
title: Limits
parent: Reference
nav_order: 3
permalink: /reference/limits/
lede: >-
  The measured ceilings, the known trade-offs, and the open gaps -- stated
  as open, with where each number comes from.
---

This page collects what has been **measured** about libumem's edges. Numbers
were taken on EC2 through `scripts/ec2/verify-isolated.sh` (committed content
only), never on the dev host, with a null control at every grid point. A
delta inside the null band is noise, not a result.

{: .note }
> "Status" means *what has been demonstrated*, not what is expected to work.
> libumem is **not production-ready on any platform** at the current commit:
> the 2026-09-21 review found reachable correctness defects in default paths
> (now fixed) and measurement defects that invalidated earlier numbers. See
> the production-readiness plan for the exit criteria.

## The Linux heap ceiling -- fixed

The ~5 GB `vm.max_map_count` wall is removed, in two halves
(`UMEM_MIN_SLAB_OBJECTS`, `UMEM_MIN_QCACHE_SLAB`). An 8-11 GB live set of
1-4 KiB objects now completes with zero failures at the default
`vm.max_map_count`. Full mechanism: [heap
ceiling]({{ '/design/heap-ceiling/' | relative_url }}).

## Memory overhead -- accepted trade

libumem holds roughly:

- **1.5-1.6x** its live set at 64-1024 B objects, **2.6x** at 16-63 B;
- against glibc's 1.07-1.25x / 1.85x and jemalloc/mimalloc's 1.1-1.2x /
  1.65x. scudo lands where libumem does.

This is the size-class + warm-cache trade: warm magazines and bins hold
freed buffers rather than returning them. About a third of the 16-63 B
overhead is the size header pushing requests one class up; the rest is slab
and magazine retention. Accepted as the slab-allocator trade.

## Throughput and tail latency -- mixed, measured

At 192 threads on Debian metal (median of three 15-minute runs; see the
README's heavy-load section for the full table and null controls):

- **Single thread:** at or above glibc everywhere (0.97-1.24x), within
  3-13 % of the fastest allocator; never the slowest.
- **`multi` scaling to 192 threads at 16-1024 B:** within the null of the
  best allocator on both metals.
- **Tail latency:** lowest or tied-lowest p99 in five of eight 64-256 B
  cells; 4.5-11x lower cross-thread p999 than glibc at 1-4 KB. jemalloc has
  the lowest cross-thread p999 on arm.
- **Fragmentation churn (`frag`) sustained at 192 threads:** still the
  slowest in the field by 2-3x (down from 8-19x). **Open** (P8.5): the slab
  layer's one-object-per-lock tail is architectural.
- **384 threads cross-thread free:** glibc becomes fastest and libumem
  slowest; the cause is **not yet diagnosed** -- treat that row as
  unexplained.

## Fork and large cache counts -- structural, closed with a decision

- A 50k-cache fork and the per-cache footprint are **structural**; their only
  "fixes" are a hot-path cost for a pathological workload or a provably
  unsound lock skip. Closed as a decision, not a guess.
- The 16k-thread drain is a kernel `mmap_lock` property, not an allocator
  lock (measured and refuted).
- Per-thread retention is ~280 KB, not the feared 5.4 MB (measured).

## Free-only thread PTC retention -- open

A thread that only *frees* (the consumer in a producer/consumer split)
retains free stock in its per-thread-cache bins. A trim on the
magazine-overflow slow path was attempted and **reverted** (it added
fast-path cost and its RSS effect was regime-dependent). The documented
upgrade path is an off-fast-path reaper, not an inline overflow check
(F1).

## Platforms

| Platform | Arch | Status |
|---|---|---|
| Linux | x86_64 | Best covered (CI on every push: normal/ASan/UBSan/gcov; EC2 benches). Not production-ready. |
| Linux | aarch64 | Builds and tests pass by hand; the nightly CI job exists but is not armed. |
| Linux | riscv64 | Cross-compiles via Nix+QEMU; no hardware validation. |
| FreeBSD | amd64 | Ported, W^X/`MAP_ANON` fixes verified; no CI, no recent run. |
| illumos | x86_64 | Manually validated at one point in time. |
| illumos | SPARCv9 | Compiles; no recorded run on hardware. |
| macOS | x86_64, arm64 | `dladdr`/`backtrace` and lldb exercised by hand; not continuous. |
| Windows | x64 (MSVC, MinGW) | Experimental. |

Reproduce any number with `scripts/ec2/allocator_comparison.sh` on your own
target rather than trusting a table; workload and hardware move them.

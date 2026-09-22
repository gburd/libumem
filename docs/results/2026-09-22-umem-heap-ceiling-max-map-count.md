# umem's ~5 GB heap ceiling: `vm.max_map_count` exhaustion (2026-09-22)

**Status: diagnosed, not fixed.** The fix is in `vmem_mmap.c`, which is not
this workstream's file set. Evidence and mechanism below; assignment needed.

**Category: (b) a configuration/sizing limit, with a named knob.** Not a
correctness defect, and not correct-fail-fast either — see "why (c) was ruled
out".

---

## What was observed

The `alloc_failures` column added for P2.2/P2.5 revealed that `umem_alloc`
returns NULL at scale where glibc `malloc` does not:

| | libc | umem |
|---|---|---|
| frag workload, 192 threads, matched budget | 36,540,723 ops, **0 failures** | ~16,000,000 ops, **~10,500,000 failures (~39%)** |
| `probe_alloc_failure`, 192 threads × 250k live | 48,000,000 ok, **0 failures** | 3,960,223 ok, **44,039,777 failures (91.7%)** |
| peak `VmHWM` reached | **96,023,092 kB (~96 GB)** | heap capped at **5,314,641,920 B (~4.95 GiB)** |

Same box (`c7i.metal-48xl`, 192 vCPU, ~380 GB RAM), same workload, same budget.
The allocator using *less* memory is the one failing, which rules out genuine
memory exhaustion immediately.

---

## Root cause

**The process ran out of VMA slots, not memory.**

`test/bench/probe_heap_ceiling.sh` sampled `/proc/PID/maps` while the heap grew:

```
peak VMA count observed = 65532
vm.max_map_count        = 65530      (100%)
heap at ceiling         = 5,023,784,960 B (4.68 GiB) over 65,532 VMAs
                        = ~75 KiB per VMA
```

Linux caps a process's VMA count at `vm.max_map_count` (default 65530) and
returns `ENOMEM` from `mmap()` once it is reached — **with RAM still free**.
That is the exact shape observed: failure at ~5 GB on a box with ~380 GB.

The chain:

1. `init_lib.c:79-84` — the default backend is **mmap** (`VMEM_BACKEND_MMAP`).
2. `vmem_mmap.c:203-204` — on Linux `MAP_ALIGN` does not exist, so
   `CHUNKSIZE = pagesize` (4096), and the arena's quantum is a page rather
   than the 64 KiB used where `MAP_ALIGN` is available.
3. `vmem_mmap_top_alloc()` (`vmem_mmap.c:139`) grows the heap with a fresh
   `mmap()` per extension. Non-adjacent spans cannot be coalesced into an
   existing VMA, so **each span costs a VMA**.
4. At ~75 KiB of usable heap per VMA, 65,530 VMAs is ~4.7-5 GiB. That is the
   wall, and it is independent of available RAM.

glibc reaches 96 GB on the same box because it grows via `brk` plus a small
number of large `mmap`s — a handful of VMAs total, not one per span.

### Why the failure was invisible

`vmem_mmap_top_alloc()` saves `errno` on entry and restores it on **every**
exit path, including both failure paths (`vmem_mmap.c:143`, `183`, `190`):

```c
int old_errno = errno;
...
} else {
        ASSERT((vmflags & VM_NOSLEEP) == VM_NOSLEEP);
        errno = old_errno;      /* erases the mmap ENOMEM */
        return (NULL);
}
```

So the genuine `mmap` `ENOMEM` is erased before any caller can observe it. The
probe recorded `errno=0` at the first failure — **that zero is an artifact of
this code, not evidence the OS succeeded.** Any operator diagnosing a NULL from
`umem_alloc` on Linux gets no errno to work with, by construction.

---

## Alternatives ruled out, with evidence

| hypothesis | ruled out by |
|---|---|
| **Genuine exhaustion / OOM** | libc reached 96 GB `VmHWM` on the same box while umem failed at 4.95 GiB. Failing allocator used 19× less memory. |
| **`vmem` segment pool (`VMEM_SEG_INITIAL`, `vmem_populate`)** | Heap arena reported `free=0` with `total` pinned at ~5.0-5.3 GB across arms — the arena could not *grow*, which is a span/VMA problem, not a segment-structure shortage. A `vmem_populate` failure would not cap total span at a VMA-count-derived number. |
| **Size-class / per-cache limit** | Failures are **uniform across all nine size buckets** (8-16 B through 2048-4096 B, each failing at a similar ratio). A per-cache limit would cluster. Also all sizes here are ≪ `UMEM_MAXBUF` (131072), so no oversize path is involved. |
| **(c) Correct fail-fast under transient pressure** | Arm C (`umem_reap()` then retry) barely helped: 91.7% → 89.9% at 192 threads, 67.8% → 51.6% at 16 threads. Retrying does not rescue the allocations because the VMA limit is not transient pressure — it is a hard, sticky ceiling. Had failures gone to ~0, this would have been a benchmark-fairness finding instead. |

`sbrk` was also considered and is not involved: the default backend is mmap
(`init_lib.c:79-84`), so `vmem_sbrk.c`'s ceiling never applies.

---

## What this invalidates (independent of the P2.2 ratio defect)

**Every previously published umem `frag` and sustained-`frag` number was
measured over a run in which a large fraction of allocations failed, and nothing
in the output said so.** The old harness counted a NULL as nothing at all — not
an operation, not an error — so the effect surfaced only as a lower `total_ops`,
which reads as "slower", plus a handful of stderr lines in a log nobody diffs.

This is separate from, and additional to, the fragmentation-ratio defect:

- the ratio defect made the *memory* number wrong;
- this makes the *throughput* number on the same runs incomparable, because the
  two allocators did different amounts of work.

Fixed here: `alloc_failures` is now a column in every CSV and TOML row, nonzero
counts are called out in the human-readable output, and
`test_bench_accounting` asserts the count is zero against a mock allocator that
cannot fail, so a miswired counter fails loudly.

---

## For whoever fixes it

The knob that removes the symptom immediately:

```sh
sysctl -w vm.max_map_count=1048576     # or higher
```

That is an operator workaround, not a fix, and it must be documented if umem is
to be used with multi-GB heaps on Linux. Candidate real fixes, none attempted
here:

1. **Grow in larger chunks.** `CHUNKSIZE = pagesize` on Linux is the direct
   cause of ~75 KiB per VMA. A larger minimum extension (64 KiB as on
   `MAP_ALIGN` platforms, or larger and geometric) would raise the ceiling
   proportionally. Cheapest change, biggest effect.
2. **Reserve and commit.** One large `PROT_NONE` reservation carved with
   `mprotect`/`MAP_FIXED` keeps the VMA count near constant regardless of heap
   size.
3. **Encourage coalescing.** Request extensions adjacent to the existing heap so
   the kernel merges them into one VMA.
4. **Stop erasing `errno`** on the failure paths of `vmem_mmap_top_alloc()`, and
   surface a distinguishable "cannot grow heap" condition. Independent of the
   above, and necessary for anyone to diagnose this in the field.
5. **Report it.** Nothing in umem's statistics or log output says "heap cannot
   grow"; a `vmem` failure counter surfaced through `umem_inspect` would have
   made this visible years ago.

## Reproduction

```sh
# on a high-core box, after make
./test/bench/probe_alloc_failure 192 250000   # arms: libc / umem / umem+retry
./test/bench/probe_heap_ceiling.sh            # samples VMAs vs heap growth
```

Evidence: `docs/results/jobs/intel-hi-bench-probe/out.log` (sha `58bf76e`),
`docs/results/jobs/intel-hi-bench-ceiling/out.log` (sha `2942b85`), both
isolated builds on `c7i.metal-48xl`.

# umem's ~5 GB heap ceiling on Linux: `vm.max_map_count` exhaustion

**Date:** 2026-09-22
**Found by:** the Phase 2 evidence workstream, via a benchmark column that did
not exist before (`alloc_failures`)
**Status: OPEN.** Root-caused, not fixed. The fix belongs in `vmem_mmap.c`.
**Severity: high for any process wanting more than ~5 GB from umem on Linux.**

## Symptom

On `c7i.metal-48xl` (192 vCPU), the fragmentation workload at 192 threads with
a matched work budget:

| Allocator | Ops completed | Allocation failures |
|---|---:|---:|
| glibc | 36,540,723 | **0** |
| umem | ~16,000,000 | **~10,500,000 (~39% of attempts)** |

`umem_alloc()` returned NULL for about two in five attempts, in every
measurement window, while glibc on the same box with the same budget never
failed once.

The tell that this was not ordinary memory exhaustion: **umem was using *less*
memory than the allocator that succeeded** — ~5 GB peak RSS against glibc's
~9.4 GB. An allocator failing while consuming half as much as its competitor is
hitting an internal ceiling, not the machine's limit.

## Root cause

Address-space fragmentation against the kernel's per-process VMA limit,
`vm.max_map_count`, whose default is **65530**.

Measured at failure: **peak VMA count 65,532 of 65,530 — 100 % of the limit.**

The mechanism is in `vmem_mmap.c`:

```c
#ifdef MAP_ALIGN
#define CHUNKSIZE  (64*1024)    /* 64 kilobytes */
#else
static size_t CHUNKSIZE;        /* ... set to pagesize on Linux */
#endif
```

Linux has no `MAP_ALIGN`, so `CHUNKSIZE` becomes `pagesize` (4 KiB) where
Solaris — the platform this code was written for — used 64 KiB. The mmap heap's
quantum is therefore 16x smaller on Linux, so the heap accumulates roughly one
VMA per ~75-80 KiB of mapped address space instead of per ~1.2 MiB.

Arithmetic check: 65530 VMAs x ~76 KiB ≈ **4.7 GB**, which matches the observed
~5 GB ceiling.

glibc reaches 96 GB on the same box because it does not fragment its address
space this way.

## What was ruled out first

Each of these would have fully explained the symptom, and each was eliminated:

1. **Genuine memory exhaustion** — the box had far more memory free, and umem
   failed while using less than glibc.
2. **The vmem segment-structure pool** (`VMEM_SEG_INITIAL`, `vmem_populate()`) —
   not the limiting resource here.
3. **A per-size-class limit** — the workload's 16..4096 B requests are all
   cache-backed, well under `UMEM_MAXBUF` (131072), and failures were not
   clustered at one class.
4. **Correct fail-fast behaviour.** `umem_alloc()` with `UMEM_DEFAULT` is
   contractually permitted to fail where glibc works harder. This was the most
   likely benign explanation, so it was tested directly: adding reap-and-retry
   moved the failure rate only 91.7 % -> 89.9 %. Retrying does not help,
   because the address space — not a free list — is what is exhausted.

## A second, aggravating defect

`vmem_mmap_top_alloc()` saves `errno` on entry and **restores it on its failure
paths**:

```c
int old_errno = errno;
...
errno = old_errno;     /* on failure */
```

So the real `ENOMEM` from the underlying `mmap()` is erased before the caller
can see it. A caller gets NULL with a stale, unrelated `errno`. That is why this
presented for years as "umem is slower on this workload" rather than "umem
could not get memory": nothing in the failure path told anyone why.

## Why this went unseen

The benchmark harness counted allocation failures as **nothing at all** —
neither successes nor errors. A run that failed 39 % of its allocations
therefore appeared as a run that simply completed fewer operations, which reads
as *slower*, not *broken*. It surfaced only once the Phase 2 work added an
`alloc_failures` column.

**Consequence for published results:** every previously published umem
fragmentation and sustained-fragmentation *throughput* number was measured over
a run that was failing roughly two in five allocations, with nothing in the
output disclosing it. This is independent of, and additional to, the
fragmentation-ratio defect (max-ratio selection bias) documented separately.

## Suggested fix, and why it was not done here

The obvious change is to give the Linux mmap heap a larger quantum — restore a
64 KiB-class `CHUNKSIZE` on platforms without `MAP_ALIGN`, so the heap consumes
VMAs at Solaris-like density. That is a one-line-looking change with real
consequences for address-space layout, small-heap footprint, and the
`vmem_mmap_alloc` / `vmem_mmap_free` contract, so it needs its own regression
(a test that drives the heap past ~5 GB and asserts no failures) and its own
before/after RSS measurement.

It was deliberately left unassigned rather than rushed: `vmem_mmap.c` was not
the Phase 2 workstream's file, and a core address-space change landing without
a dedicated test is exactly the pattern this plan exists to stop.

Separately and independently worth fixing: stop restoring `errno` over a
genuine failure in `vmem_mmap_top_alloc()`.

## Reproducing

```sh
export AWS_PROFILE=hotdog
./scripts/ec2/launch.sh intel-hi@heap && ./scripts/ec2/bootstrap.sh intel-hi@heap
./scripts/ec2/verify-isolated.sh intel-hi@heap HEAD heap 3600 \
  './scripts/ec2/clean-regen.sh && make -j$(nproc) && \
   test/bench/.libs/bench_main -a umem -w frag -t 192 -n 40000000 -s 16:4096 -c'
# watch: alloc_failures in the CSV, and
#   grep VmPeak /proc/<pid>/status ; wc -l < /proc/<pid>/maps
./scripts/ec2/terminate.sh intel-hi@heap
```

## 2026-09-22 follow-up: three wrong hypotheses, and where the cause actually is

An attempt to fix this failed. Recording it in full, because the measurements
narrow the search considerably and the next person should not repeat them.

**Headline number:** VMAs at a fixed 2 GB of 4 KiB allocations —

| Build | VMAs at 2 GB |
|---|---:|
| before the attempted fixes | 16,283 |
| after all three | 16,487 |

No improvement. All three were reverted (`553d42e`).

### What was tried, and why each was wrong

1. **Raise `CHUNKSIZE` to 64 KiB** (matching Solaris, per the original analysis
   above). Measured 9.3 VMAs/MB afterwards against 8.01 before. The span
   **count**, not their size or the heap quantum, is what consumes VMAs — so the
   quantum was never the binding constraint. The original hypothesis in this
   document was wrong.

2. **Over-map and trim for alignment.** Raising the quantum broke
   `_vmem_extend_alloc()`'s alignment assertion, because `mmap()` only promises
   page alignment. Over-mapping and trimming both ends fixed the assertion and
   made the ceiling *worse*: the trimmed tail leaves an unmapped hole between
   reservations, so spans can never merge — 4,593 mappings each separated by a
   64 KiB gap.

3. **`mprotect()` instead of a `MAP_FIXED` `mmap()`, plus large contiguous
   reservations.** The mechanism here is real, and isolated cleanly:

   | Operation | VMAs |
   |---|---:|
   | 64 MiB reserved `PROT_NONE` | 24 |
   | 64 contiguous 128 KiB spans `mprotect`ed RW | 25 (they merge) |
   | `MADV_DONTNEED` on alternating spans | 25 (no change) |
   | `mprotect(PROT_NONE)` on those too | **88** |

   So a replacement mapping cannot merge, and a protection change on free splits
   what did merge. Both true — and both irrelevant here.

### Where the cause actually is

`strace` on 60,000 4 KiB allocations:

```
64,275 mprotect   <-- one per ALLOCATION
   259 mmap
     2 munmap
```

with 64,270 of those `mprotect` calls being **exactly 4096 bytes**. The slab
layer requests one page-sized span per 4 KiB object from `umem_va_arena`, so the
mapping is split upstream of anything `vmem_mmap.c` chooses. No change to the
mmap backend can fix that; the fix belongs in span sizing — `umem_va_arena`'s
quantum and `qcache_max`, or the slab layer's decision to take a fresh span per
object at this size class.

`umem_va_arena` is created with a `pagesize` quantum and `8 * pagesize`
`qcache_max` (`umem.c`, near the `vmem_create("umem_va", ...)` call). A 4 KiB
object's slab is one page, which is why every such allocation reaches the
source. Raising the va-arena quantum, or making the slab layer batch spans for
small size classes, is the direction — with this document's VMA-at-2 GB
measurement as the before/after check.

### What was kept from the attempt

Two fixes that stand on their own evidence:

- **`errno` is no longer erased over a genuine `mmap()` failure.** Measured
  directly: `FIRST FAILURE at 8269MB (errno=0 Success)` became
  `(errno=12 Cannot allocate memory)`. This is the defect that made the ceiling
  look like a performance problem rather than an allocation failure.
- **`vmem_populate()` reports an unsupported `VM_SLEEP` instead of aborting.**
  The assertion crashed the process with no indication the caller's flags were at
  fault, and vanished entirely under `NDEBUG`, continuing into a path the code
  says is not allowed.

### Status

`test/integration/test_heap_ceiling` measures this on every `make check` run and
reports **SKIP** with the live numbers (failure point, VMAs vs limit). Flip its
two `rc = 77` returns to `rc = 1` when span sizing is fixed — that is what the
test is for.

Workaround unchanged: raise `vm.max_map_count`.

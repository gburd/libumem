# Finding: `umem_cpu_node[]` out-of-bounds read in `umem_depot_alloc` on high-core machines

**Severity:** High (core allocator, not experimental). Every allocation on a
machine where `umem_max_ncpus > UMEM_MAX_DEPOT_CPUS` reads out of bounds.

**Discovered by:** Workstream H2 (GC STW stress) on `intel-hi`
(`c7i.metal-48xl`, 192 vCPU) and `arm-hi` (`c8g.metal-48xl`, 192 vCPU),
built `--enable-asan`.

## Reproduction

On any Linux box with a high core count (192 vCPU here), built with ASan:

```
$ ./configure --enable-asan && make
$ ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD=$(gcc -print-file-name=libasan.so.6.0.0) \
    LD_LIBRARY_PATH=.libs ./a.out      # a.out does a single umem_alloc(16, 0)
```

A single `umem_alloc()` aborts:

```
==NNN==ERROR: AddressSanitizer: global-buffer-overflow on address 0x... at pc 0x...
READ of size 4 at 0x... thread T0
    #0 umem_depot_alloc  umem.c:2152
    #1 _umem_cache_alloc umem.c:2653
    #2 _umem_alloc       umem.c:3205
    ...
0x... is located 0 bytes to the right of global variable 'umem_cpu_node'
  defined in 'umem.c:658:12' of size 1024   (== 256 ints)
SUMMARY: AddressSanitizer: global-buffer-overflow umem.c:2152 in umem_depot_alloc
```

The standalone reproduction is `test/unit/repro_cpu_node_oob.c` (a single
`umem_alloc`/`umem_free`), which is expected to abort under ASan on machines
with `umem_max_ncpus > 256` and pass everywhere else.

## Root cause

- `umem.c:657` `#define UMEM_MAX_DEPOT_CPUS 256`
- `umem.c:658` `static int umem_cpu_node[UMEM_MAX_DEPOT_CPUS];` — a **fixed
  256-entry** CPU→NUMA-node table.
- `umem.c:4234` sets `cp->cache_depot_ncpus = umem_max_ncpus`, where
  `umem_max_ncpus` is the machine's CPU count rounded up to a power of two
  (`umem.c:4892`) and is otherwise capped only at `CPUHINT_MAX() == INT_MAX`.
- `umem_depot_alloc` (`umem.c:2132`, `:2152`, `:2170`) and the depot free
  path index `umem_cpu_node[cpu]` / `umem_cpu_node[other]` with
  `cpu, other ∈ [0, cache_depot_ncpus - 1]`.

When the machine has enough CPUs that `umem_max_ncpus > 256` (a 192-vCPU
metal instance rounds/detects above 256), the depot NUMA-stealing loop reads
`umem_cpu_node` past its 256-entry bound. The `umem_cpu_node` table is only
ever *populated* for indices `[0, min(umem_max_ncpus, 256))`
(`umem.c:4919-4921`), confirming the author intended a 256 cap that the
depot indexing does not honor.

The per-CPU depot magazine arrays themselves (`cache_depot_full/empty`) are
correctly `mmap`'d to `umem_max_ncpus` entries; only the `umem_cpu_node`
NUMA-node lookup table was left at a fixed 256.

## Impact

- Out-of-bounds read on the hot allocation path for any process on a
  machine with `umem_max_ncpus > 256`.
- The read lands in adjacent globals (`umem_cpu_mask` and beyond), so the
  NUMA-node comparison uses garbage, degrading the depot stealing heuristic
  (a correctness-of-heuristic issue, not a crash without ASan — but it is a
  real OOB that ASan/hardened builds will trap, and a latent
  read-past-page hazard if the array is near a page boundary).

## Suggested fix (core / Workstream D, out of scope for H)

Any one of:
1. Size `umem_cpu_node` dynamically to `umem_max_ncpus` (like the depot
   magazine arrays), populated at init; or
2. Clamp the depot indexing: treat CPUs `>= UMEM_MAX_DEPOT_CPUS` as node 0
   (`(idx < UMEM_MAX_DEPOT_CPUS) ? umem_cpu_node[idx] : 0`) at the three
   use sites; or
3. Cap `cache_depot_ncpus` at `UMEM_MAX_DEPOT_CPUS` (changes depot fan-out
   on >256-CPU boxes, so option 1 is preferred).

## Effect on Workstream H2

The GC STW stress cannot run at very high core counts under ASan until this
core bug is fixed, because `umem_gc_init` (and every GC allocation) goes
through `_umem_alloc` → `umem_depot_alloc`. The GC invariant + STW-stress
suite (`test/property/prop_gc.c`) is proven under ASan on `intel-lo`
(8 vCPU, `umem_max_ncpus <= 256`) up to 16 threads / 400 STW rounds, and the
high-core roles reproduced this **pre-existing core defect** rather than any
GC-specific fault.

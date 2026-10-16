# PAL carve-out, part 2 -- memory syscalls behind umem_pal_* (r11 task 4)

Date: 2026-10-16. Branch `r11pal`, commit `5f4ed85` on top of master
`d375e0f` (`374f6cc..5f4ed85` is exactly the 7 files below; the getpcstack
commits `f03b004`/`374f6cc` beneath are @r11pc's, not this change).
Validation box: `intel-lo@r11` (c7i.2xlarge, Debian, x86_64) and
`arm-lo@r11pal` (c7g.2xlarge, AL2023, aarch64), AWS profile `kiwi`.

## What moved

A behaviour-preserving relocation of the memory-management syscalls out of
the scattered call sites into one auditable header, `pal/pal_mem.h`, behind
the `umem_pal_*` memory contract the PAL design doc names
(`reserve`/`reserve_aligned`/`commit`/`decommit`/`release`, plus `map_rw` for
the one-shot anonymous RW map). Each op is a `static inline` that emits the
*exact* syscall and flag set the site emitted before, so the generated code is
byte-for-byte identical and the malloc/free fast path (which makes no syscall)
is untouched. The arena *policy* -- the guard-vs-dontneed decision, the errno
discipline, the vmem bookkeeping -- stays with the callers; only the syscall
primitives and their per-OS flag sets moved.

### Sites migrated

`vmem_mmap.c` (9 platform sites):
1. `vmem_mmap_alloc` commit: `mmap(MAP_FIXED, RW)` -> `umem_pal_commit`.
2. `vmem_mmap_free` guard branch: `mmap(PROT_NONE, MAP_FIXED)` ->
   `umem_pal_decommit(..., GUARD)`.
3. `vmem_mmap_free` small-span branch: `madvise(MADV_DONTNEED)` ->
   `umem_pal_decommit(..., DONTNEED)`.
4. `vmem_mmap_top_alloc` MAP_ALIGN reserve -> `umem_pal_reserve_aligned`.
5. `vmem_mmap_top_alloc` plain reserve: `mmap(0, PROT_NONE, MAP_NORESERVE)`
   -> `umem_pal_reserve`.
6. `vmem_mmap_top_alloc` rollback: `munmap` -> `umem_pal_release`.
7-9. The `ALLOC_PROT`/`FREE_PROT`, `ALLOC_FLAGS`/`FREE_FLAGS`,
   `MAP_NORESERVE`/`MAP_ANON`/`MAP_FAILED` portability shims (and the Windows
   VirtualAlloc/VirtualFree fallbacks) -> `pal/pal_mem.h`.

`umem.c` (2 PAL-shaped sites):
10. `umem_slab_reclaim` lazy page reclaim: the
    `#if _WIN32 / __FreeBSD__ (MADV_FREE) / else (MADV_DONTNEED)` block ->
    `umem_pal_decommit(..., LAZY)`. Kept distinct from `vmem_mmap_free`'s
    DONTNEED path because the FreeBSD reclaim deliberately used MADV_FREE and
    must stay so.
11. The CPU->node table `mmap(NULL, PROT_RW, MAP_PRIVATE|MAP_ANON)` ->
    `umem_pal_map_rw`.

`getpcstack.c` and the rseq `.S` were left untouched (owned by @r11pc / the
AAL rseq work).

### Why one header with capability `#if`, not pal_linux.c/pal_freebsd.c/...

The design doc lists per-OS override files, but the only differences the
memory ops actually have are compile-time capability tests: `MAP_NORESERVE`
absent on FreeBSD (defined to 0), `MAP_ALIGN` present on illumos, `MADV_FREE`
vs `MADV_DONTNEED` for lazy reclaim, and the Windows fallbacks. These are
exactly the `#if` the call sites already used, which is the capability-not-OS
model the design doc asks for ("so the core can `#if` on capability not on OS
name"). Splitting them into near-empty per-OS files would add indirection and
risk for no behaviour change, against the carve-out's own rule. The contract
is enumerated in `pal_concept.h`; the smoke test
(`test/port/pal_aal_smoke.c`) now runs a self-contained one-page
reserve/commit/decommit/release + map_rw round-trip, so a port missing any
symbol fails to link.

## A. make check IDENTICAL to baseline, per-test, both arches

verify-isolated.sh against committed refs (git archive; no worktree leakage).

| arch    | ref (post / base)       | TOTAL | PASS | SKIP | FAIL | XFAIL | XPASS | ERROR |
|---------|-------------------------|-------|------|------|------|-------|-------|-------|
| x86_64  | 5f4ed85 (post)          | 73    | 60   | 13   | 0    | 0     | 0     | 0     |
| x86_64  | d375e0f (master base)   | 73    | 60   | 13   | 0    | 0     | 0     | 0     |
| aarch64 | 5f4ed85 (post)          | 73    | 62   | 11   | 0    | 0     | 0     | 0     |
| aarch64 | d375e0f (master base)   | 73    | 62   | 11   | 0    | 0     | 0     | 0     |

Not just the totals: a per-test `diff` of the sorted `PASS/SKIP/FAIL/...`
lines between base and post is **IDENTICAL** on both arches -- no test
changed state. The migrated paths are directly exercised and green:
`test_reclaim_returns.sh`, `test_oversize_vma.sh`, `test_heap_ceiling*`,
`test_fork_child_reclaim.sh`.

Logs: `docs/results/jobs/intel-lo-r11-palx86{post,base}/out.log`,
`docs/results/jobs/arm-lo-r11pal-palarm{post,base}/out.log`.

## B. hotpath_ab.sh -- fast path +0.00 %, both arches

`PRE=374f6cc POST=5f4ed85` (isolates this change: pre has the getpcstack
commits but not the PAL carve-out; `374f6cc..5f4ed85` is the 7 files of this
commit alone). One `bench_pairs` binary from post serves every arm; pre/post
`libumem.so` confirmed byte-different; soname unchanged. 9 replicate pairs per
point, null (pre vs pre2) measured first.

**Instructions per pair at t=1 (load-independent -- the authoritative gate):
every A/B point sits inside its own null on both arches.** On x86_64, four of
the seven points read exactly +0.00 %; three (two x86 s=512 N=1/N=128, one arm
s=16:64 N=64) read -0.06..-0.07 %, which is a single 0.1-instruction tick out
of ~147 in the *favorable* direction and is covered by the null's own spread
(the arm point's A/B median -0.06 % equals its null median -0.06 % exactly --
pure rig drift). No point moved the fast path in the costly direction, and the
fast path in fact touches none of these ops. Throughput deltas at t=1 and t=8
all sit inside the null's min..max spread (noise), on both arches.

(An earlier run used `PRE=d375e0f`, which also measured the getpcstack delta
beneath this change; it too came out +0.00 % insn/pair. The `374f6cc` run
isolates this change and is the one reported. Logs:
`docs/results/jobs/*-palx86ab2/`, `*-palarmab2/`; the broader `d375e0f` run is
in `*-palx86ab/`, `*-palarmab/`.)

## C. riscv64 + arm32 + aarch64 still cross-compile

Cross `./configure --host=<triple> CC=<triple>-gcc --enable-frame-pointers`
then `make libumem.la test/port/pal_aal_smoke umem_test umem_test2`, run under
qemu-user on `intel-lo@r11` (cross toolchains + qemu present).

| arch    | libumem.la | pal_aal_smoke (mem round-trip) | umem_test/2 |
|---------|-----------|--------------------------------|-------------|
| riscv64 | BUILD-OK  | `mem reserve/commit/decommit/release OK`, smoke rc=0 | rc=0 |
| arm32   | BUILD-OK  | `mem reserve/commit/decommit/release OK`, smoke rc=0 | (built) |
| aarch64 | BUILD-OK  | `mem reserve/commit/decommit/release OK`, smoke rc=0 | (built; native make check 73/62/11 above) |

The new PAL memory ops did not break the new arches. Logs:
`docs/results/jobs/intel-lo-r11-palcross/out.log` (riscv64) and
`docs/results/jobs/intel-lo-r11-palcross2/out.log` (arm32, aarch64).

## Merge decision

A + B hold on both arches; C holds. Mergeable. Fast-forward master to the PAL
commit (`5f4ed85`) plus this results/CHANGELOG commit.

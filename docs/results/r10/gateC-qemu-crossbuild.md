# r10 PAL/AAL refactor — Gate C: QEMU-user cross-build + run

The NEW portability coverage the PAL/AAL carve-out buys. For each of the three
new/validated AALs the refactor names — riscv64, armhf (arm32), aarch64 — we
cross-build the libraries AND the test binaries, then run a representative
test subset under `qemu-user-static`.

## Provenance

- **Ref:** master tip `7e5ed07` (`git archive HEAD`, committed content only).
- **Box:** `intel-lo@palci` (kiwi 830118047484, i-0116023c931bc22bc), x86_64
  host, Debian 13 trixie, 8 vCPU.
- **Toolchains:** Debian cross-gcc 14.2.0 — `riscv64-linux-gnu-gcc`,
  `arm-linux-gnueabihf-gcc`, `aarch64-linux-gnu-gcc`; sysroots under
  `/usr/<triple>/`.
- **Runner:** `qemu-{riscv64,arm,aarch64}-static` with `-L /usr/<triple>` and
  `LD_LIBRARY_PATH` on the freshly built `.libs/`.
- **Driver:** `scripts/ec2/gateC-driver.sh` (validation only — does NOT modify
  library source). Per-arch logs: `docs/results/r10/gateC-logs/`.
- **Each arch built twice:** DEFAULT configure (rseq auto → `yes`, the header
  is present in every cross sysroot) and `--disable-rseq`, to confirm both
  paths cross-build and run.

Stages recorded per arch: **COMPILES** (libumem.la builds), **LINKS** (the
test binaries build), **RUNS** (the subset passes under qemu).

## Result table

| arch | triple | compiles | links | runs | notes |
|---|---|:---:|:---:|:---:|---|
| riscv64 | riscv64-linux-gnu | ✅ | ✅ | ✅ | smoke OK; umem_test 3/3; umem_test2 3/3 (4-thread). rseq auto=yes AND `--disable-rseq` both build+run. `getpcstack depth=0` here → **FIXED in r11: depth=3** with the new per-arch frame walk (`docs/results/r11/getpcstack-newarch.md`). |
| arm32 (armhf) | arm-linux-gnueabihf | ✅ | ✅ | ✅ | smoke OK; umem_test 3/3; umem_test2 3/3 (4-thread). rseq auto=yes AND `--disable-rseq` both build+run. 32-bit (`ptr=32 word=32`). `getpcstack depth=0` here → **FIXED in r11: depth=1** (one real caller frame; `docs/results/r11/getpcstack-newarch.md`). |
| aarch64 | aarch64-linux-gnu | ✅ | ✅ | ✅ | smoke OK; umem_test 3/3; umem_test2 3/3 (4-thread). Real rseq `.S` present. `getpcstack depth=4` (per-arch frame walk works). |

All three arches reach **RUNS** in **both** rseq configurations. No arch was
`#error`-blocked or link-blocked; the AAL abstraction holds on each.

## `pal_aal_smoke` output per arch (DEFAULT configure)

```
riscv64:  AAL: RISCV64 (ptr=64 word=64)
          AAL: getpcstack depth=0
          PAL: linux features=0x0019
          PAL: page_size=4096 ncpus=8 cpu_hint=2
          PAL: getentropy rc=0
          PAL/AAL smoke: OK

arm32:    AAL: ARM32 (ptr=32 word=32)
          AAL: getpcstack depth=0
          PAL: linux features=0x0019
          PAL: page_size=4096 ncpus=8 cpu_hint=7
          PAL: getentropy rc=0
          PAL/AAL smoke: OK

aarch64:  AAL: AARCH64 (ptr=64 word=64)
          AAL: getpcstack depth=4
          PAL: linux features=0x0019
          PAL: page_size=4096 ncpus=8 cpu_hint=0
          PAL: getentropy rc=0
          PAL/AAL smoke: OK
```

`features=0x0019` = HAS_RSEQ | AT_SECURE_GATE | HAS_AUXV (same bitmask as the
native x86_64/aarch64 runs in Gate A). The identity string names the correct
AAL on each target, and `ptr/word` widths are right (riscv64/aarch64 64-bit,
arm32 32-bit) — the P7.4 "no hard-coded 48-bit VA" rule holds: structures size
from `UMEM_AAL_PTR_BITS`.

Core unit tests under qemu (identical on all three arches):
```
umem_test:   Test 1 basic 32B alloc PASS; Test 2 PTC ptc_enabled=1 PASS;
             Test 3 post-init 64B alloc PASS.  All tests passed (3/3)
umem_test2:  Test 1 4000 alloc/free single-thread PASS; Test 2 PTC PASS;
             Test 3 4-thread × 4000 alloc stress PASS.  All tests passed (3/3)
```

## AAL gaps found (follow-up items, NOT fixed here — docs/validation only)

1. **`getpcstack()` returns depth 0 on riscv64 and arm32.** `getpcstack.c`
   has per-arch frame walks only for x86_64, i386 and aarch64; riscv64/arm32
   fall to the generic path, which does not walk and returns 0. The core
   allocator does not depend on it, but libumem's leak tracking / audit stack
   capture (`UMEM_DEBUG` bufctl backtraces, `findleaks`) will show empty stacks
   on these arches. A real per-arch frame walk (or `__builtin_frame_address`
   loop guarded for frame-pointer availability) is owed before claiming
   debugging-tool parity on riscv64/arm32. This is a genuine AAL gap the
   refactor exposed, recorded honestly — it is not a correctness defect.

   **FIXED in r11** (`feat(getpcstack): per-arch frame-pointer walks for
   riscv64 and arm32`, commit `f03b004`): both arches now have a frame-pointer
   walk modelled on the x86_64/aarch64 paths, with the P5.9 stack-bounds
   guard. Under QEMU with `--enable-frame-pointers`, `pal_aal_smoke` reports
   `getpcstack depth=3` (riscv64) and `depth=1` (arm32), both > 0; aarch64
   stays depth=4; the x86_64 preprocessed output is byte-identical to master
   (zero behaviour change). Evidence: `docs/results/r11/getpcstack-newarch.md`.

2. **rseq on riscv64/arm32 is a no-op fallback, not a real critical section.**
   The DEFAULT configure enables rseq (the `linux/rseq.h` header is present),
   but there is no `umem_rseq_riscv.S` / `umem_rseq_arm.S`; the x86_64 and
   aarch64 `.S` files self-guard (`#if defined(__arch__) && __linux__`) and
   assemble to empty objects on the other arches, and `umem_rseq.c`'s per-arch
   `#else` branches return the classic per-CPU fallback. So the build links and
   runs, but these arches get the lock-based per-CPU cache, not the lock-free
   rseq fast path. This is the behaviour the AAL contract documents (rseq is an
   optional capability, not a required AAL symbol); a native rseq critical
   section for riscv64/arm32 is a performance follow-up, not a correctness gap.

Neither gap blocks the "compiles / links / runs" result; both are the honest
"this AAL is complete enough to run, not yet complete enough for every tool /
every fast path" status the carve-out was meant to make visible.

## Scope / what this is NOT

- A representative subset under emulation, not the full `make check` suite
  (property tests, stress oracle, the gdb/lldb e2e scripts and perf tests are
  excluded — qemu-user is slow and some need native tooling). It proves the
  libraries and the exercised unit paths build and run on each arch under
  emulation; it does not prove the comprehensive suite passes, nor anything on
  real riscv64/arm32 silicon.
- qemu-user emulates the CPU, not a kernel-faithful rseq / NUMA / CPU-affinity
  surface; `cpu_hint`/`ncpus` reflect the x86_64 host under emulation.

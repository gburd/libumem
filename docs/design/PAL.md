---
title: "Platform & Architecture Abstraction Layer (design)"
nav_order: 20
parent: Design
permalink: /design/pal/
---

# PAL / AAL design

Status: DESIGN (2026-10-16). Implementation + cross-platform validation on
EC2 (kiwi) is Phase B. This doc is the contract an implementer and a porter
work against. It is a **behaviour-preserving refactor**: it relocates platform
and architecture code behind two interfaces; it does not change the allocator.

## Why

Today libumem's platform and arch code is scattered: `vmem_mmap.c`,
`getpcstack.c`, `init_lib.c`, `envvar.c`, `umem_fail.c`, `umem_fork.c`,
`malloc_interpose.c`, `freebsd_pthread_hooks.c`, `x86_subr_compat.c`,
`i386_subr_sol.s`, `sparc_subr_sol.S`, `umem_rseq_{x86_64,aarch64}.S`,
`atomic.h`, plus `#if __linux__ / __FreeBSD__ / __sun / __APPLE__ / _WIN32`
and `#if __x86_64__ / __aarch64__ / __arm__ / __riscv / __sparc / __powerpc`
sprinkled through `umem.c`, `umem_rseq.c`, `misc.h`. The port surface is
tribal; the "compiles on SPARC, never run" honesty in the README exists
because nothing makes the surface auditable or testable.

Three models studied:
- **snmalloc** `src/snmalloc/pal/` (~21 OSes) + `aal/` (arch): a clean
  two-layer split, the PAL defined by a C++20 Concept so a missing method is a
  compile error; partial PALs (`PALPOSIX`, `PALBSD`) let a POSIX-ish OS be a
  few lines. `docs/PORTING.md` documents the contract and a memory state
  machine (using / using-readonly / not-using).
- **libdb** `src/os/os_<operation>.c` (40 files: map, atomic, clock, csprng,
  cpu, yield, errno, stack, …) with per-OS override dirs (`os_qnx`,
  `os_windows`) and a full porting guide. One small file per abstracted
  operation; the common POSIX impl in `src/os/`, overrides elsewhere.
- **libumem today**: already has the *nucleus* — per-arch `.S` for rseq,
  `sparc_subr_sol.S`, `atomic.h`, `vmem_mmap.c` — just not organized.

## The combination chosen for libumem (C, not C++)

libumem is C17, so there is no Concept. The C equivalent of a checkable
contract is **one header that enumerates every required symbol**, plus a
compile-time `#error` cascade that fails a port that forgot one, plus a PAL
smoke test (a tiny program that references every required symbol so a missing
one is a link error on the porting box).

Layout (libdb's per-operation granularity, snmalloc's two-layer PAL/AAL split):

```
pal/
  pal.h              -- selects the active PAL by platform macro; the contract
  pal_concept.h      -- the required-symbol list + #error cascade (C "concept")
  pal_posix.c/.h     -- the common POSIX base (mmap/munmap/mprotect/madvise,
                        sysconf page size, clock, yield, getentropy fallback)
  pal_linux.c        -- Linux: getrandom, MAP_NORESERVE, rseq availability,
                        madvise(MADV_FREE/DONTNEED), /proc for cpu count
  pal_freebsd.c      -- FreeBSD overrides (freebsd_pthread_hooks folds in here)
  pal_illumos.c      -- illumos/Solaris: the historic home; getpcstack via
                        sparc/i386 subr, SOLARIS socket libs, the .so.1 SONAME
  pal_macos.c        -- Apple (mach vm, no MAP_NORESERVE)
  pal_netbsd.c pal_openbsd.c  -- as needed, inherit pal_posix
aal/
  aal.h              -- selects the active AAL by arch macro; the contract
  aal_concept.h      -- required-symbol list + #error cascade
  aal_x86_64.c/.S    -- getfp/pcstack, pause, prefetch, rseq asm (x86_subr,
                        i386_subr, umem_rseq_x86_64.S fold in)
  aal_aarch64.c/.S   -- rseq asm (umem_rseq_aarch64.S), pac if present
  aal_arm32.c/.S     -- armhf: frame walk, atomics (NEW)
  aal_riscv.c/.S     -- riscv64 (and rv32 if trivial) (NEW)
  aal_sparcv9.c/.S   -- UltraSPARCv9 (sparc_subr_sol.S folds in) (NEW focus)
  aal_powerpc.c      -- ppc64 (page size 64K) if cheap
  atomic.h           -- stays, becomes the AAL atomics surface
```

## The PAL contract (required of every platform)

Each is a libumem-prefixed C function (`umem_pal_*`). The common POSIX impl
satisfies most; a platform file overrides only what differs.

Memory (the core of the allocator's demand on the OS):
- `void *umem_pal_reserve(size_t)` — reserve address space, NOT yet usable
  (snmalloc's `not_using` state). mmap(PROT_NONE|MAP_NORESERVE) on Linux.
- `void *umem_pal_reserve_aligned(size_t, size_t align)` — if the OS can give
  strong alignment (FreeBSD/NetBSD MAP_ALIGNED); else PAL advertises it can't
  and the core over-reserves and trims (snmalloc's pattern).
- `int  umem_pal_commit(void *, size_t, int zero)` — make usable
  (`notify_using`); mprotect(RW) + optional zero.
- `void umem_pal_decommit(void *, size_t)` — return to OS
  (`notify_not_using`); madvise(MADV_FREE|DONTNEED) or mprotect(NONE).
- `void umem_pal_release(void *, size_t)` — munmap.
- `size_t umem_pal_page_size(void)` — may exceed the hardware page (ppc 64K).
Entropy / identity / scheduling:
- `int  umem_pal_getentropy(void *, size_t)` — getrandom/getentropy/arc4random/
  /dev/urandom fallback (feeds the AT_RANDOM cookie, P5.4).
- `long umem_pal_ncpus(void)` — online CPU count (today's 64K-stack
  `proc_stat` reader, O7-shrunk, lives here).
- `void umem_pal_yield(void)` — sched_yield.
- `int  umem_pal_cpu_hint(void)` — current CPU id (rseq-backed where present;
  the AAL provides the rseq primitive, the PAL the fallback).
Diagnostics / lifecycle:
- `void umem_pal_abort(void)` — the controlled abort (umem_do_abort folds in).
- fork hooks (umem_fork.c's pthread_atfork wiring; FreeBSD's hook variant).
- `pal_features` bitmask: HAS_RSEQ, HAS_MADV_FREE, STRONG_ALIGN,
  AT_SECURE_GATE, etc., so the core can `#if` on capability not on OS name.

## The AAL contract (required of every architecture)

- `aal_name` (X86_64/AARCH64/ARM32/RISCV64/SPARCV9/PPC64) and word/pointer
  width constants (NO hard-coded 48-bit VA — snmalloc's rule, and libumem's
  P7.4 span table must size from this).
- `umem_aal_pause()` — cpu-relax (PAUSE / YIELD / WFE).
- `umem_aal_prefetch(addr)` — the UMEM_PREFETCH_* macros fold in.
- `getpcstack`/`getfp` — frame walk (the O7/T9 work; per-arch, honest about
  frame-pointer dependence on x86_64).
- rseq critical-section asm (umem_rseq_<arch>.S) — the lock-free per-CPU
  reload; the AAL exposes "is rseq usable here" and the commit primitive.
- 128-bit / pointer-width atomics surface (atomic.h).

## Required targets (user)
illumos/UltraSPARCv9; RISC-V (illumos|bsd|linux); arm32 (armhf); aarch64;
plus the existing x86_64; keep Linux/FreeBSD/macOS. New AALs: arm32, riscv64,
sparcv9 (SPARC asm exists but is unproven). New/!tested PAL: illumos.

## Porting contract doc
`docs/porting/` (libdb's porting-guide structure: audience, scope, layout,
build-target, test-port, certify) + the required-symbol list from
`pal_concept.h`/`aal_concept.h`. A new OS that is POSIX-ish:
`pal_myos.c` including `pal_posix.c` and overriding only what differs. A new
arch: implement the AAL contract; the `#error` cascade names what's missing.

## Validation owed (Phase B, EC2 kiwi, Debian + QEMU + illumos if reachable)
1. Behaviour-preserving: full `make check` + exit_criteria_gate on x86_64 and
   aarch64 BEFORE and AFTER the refactor — identical pass set, and
   `hotpath_ab.sh` inside the null (the fast path must not move; the refactor
   is not allowed to cost anything).
2. Cross-build + `make check` under QEMU-user for riscv64, armhf, aarch64.
3. A real illumos run (SPARCv9 if a VM/host is reachable; else "compiles under
   cross-tool, unrun" recorded honestly — do NOT claim a SPARC run that did
   not happen).
4. The `#error` cascade: deliberately omit a required symbol on a scratch
   build and confirm it fails to compile with a message naming the symbol.
5. The PAL smoke test links every required symbol.
NOTHING here is "done" until 1 passes (identical gate + null A/B) on both
primary arches; the cross-arch builds are the new coverage the refactor buys.

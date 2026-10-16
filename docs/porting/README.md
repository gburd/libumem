---
title: "Porting libumem (PAL/AAL contract)"
nav_order: 40
parent: Design
permalink: /porting/
---

# Porting libumem

This is the contract a porter works against when bringing libumem to a new
operating system or CPU architecture. It is the companion to the design in
[`../design/PAL.md`](../design/PAL.md); that document says *why* the two-layer
split exists, this one says *what you must provide* and *how to prove it*.

The split follows snmalloc (`src/snmalloc/pal/` + `aal/`) and libdb
(`src/os/os_<operation>.c`): a **PAL** (Platform Abstraction Layer) for the OS
surface, an **AAL** (Architecture Abstraction Layer) for the CPU surface. C17
has no C++20 Concept, so the checkable contract is a header enumerating every
required symbol plus a compile-time `#error` cascade and a link-time smoke
test.

## Audience

You are porting libumem to an OS it does not build on, or a CPU it does not
have an AAL for. You do not need to understand the slab/magazine/PTC core;
you need to satisfy the two contracts below.

## Scope: what the PAL and AAL cover

| Layer | Covers | Files |
|---|---|---|
| PAL | OS syscalls the allocator needs: memory reserve/commit/decommit/release, page size, entropy, online CPU count, yield, cpu hint, controlled abort, fork hooks, capability bitmask | `pal/pal.h`, `pal/pal_concept.h`, `pal/pal_posix.c`, `pal/pal_<os>.c`, `vmem_mmap.c` (the mmap arena) |
| AAL | CPU primitives: frame walk (`getpcstack`/`getfp`), cpu-relax (`umem_aal_pause`), prefetch, the rseq critical section where present, the atomics surface | `aal/aal.h`, `aal/aal_concept.h`, `aal/<arch>.{c,S}`, `atomic.h` |

## Layout

```
pal/
  pal.h            selects the active PAL by platform macro; declares umem_pal_*
  pal_concept.h    required-symbol list + #error cascade (the C "concept")
  pal_posix.c      common POSIX base (page size, ncpus, yield, cpu hint, entropy)
  pal_<os>.c       per-OS overrides: include pal_posix.c, redefine only what differs
aal/
  aal.h            selects the active AAL by arch macro; arch identity + pause
  aal_concept.h    required-symbol list + #error cascade
  <arch>.{c,S}     per-arch frame walk / rseq asm (umem_rseq_<arch>.S, *_subr_*)
  atomic.h         the atomics surface
```

## The PAL contract (every platform provides)

`unsigned int umem_pal_features(void)` — capability bitmask (`UMEM_PAL_HAS_RSEQ`,
`UMEM_PAL_HAS_MADV_FREE`, `UMEM_PAL_STRONG_ALIGN`, `UMEM_PAL_AT_SECURE_GATE`,
`UMEM_PAL_HAS_AUXV`), so the core keys on capability, not OS name.

- `size_t umem_pal_page_size(void)` — the page size (may exceed the hardware
  page; ppc 64K).
- `int umem_pal_getentropy(void *, size_t)` — AT_RANDOM / getrandom /
  getentropy / arc4random / `/dev/urandom`; feeds the P5.4 link cookie. Return
  −1 if no source produced anything (the caller keeps its own fallback mixing).
- `long umem_pal_ncpus(void)` — online CPU count.
- `void umem_pal_yield(void)` — `sched_yield`, errno-neutral.
- `int umem_pal_cpu_hint(void)` — a current-CPU id for striping (rseq-backed
  via the AAL where present; −1 if unavailable — the core masks it).
- `void umem_pal_abort(void)` — the controlled abort (not `abort(3C)`, which
  flushes FILEs through `free()`).
- The mmap arena: `vmem_t *vmem_mmap_arena(...)` in `vmem_mmap.c` is the memory
  entry point (the PROT_NONE reserve, MAP_FIXED commit, MADV_DONTNEED/PROT_NONE
  decommit, munmap release). A POSIX-ish OS inherits it unchanged.
- Fork hooks: the `pthread_atfork` wiring in `umem_fork.c` (FreeBSD's hook
  variant in `freebsd_pthread_hooks.c`).

A POSIX-ish OS is a few lines: `pal_myos.c` that `#include "pal/pal_posix.c"`
and overrides only what differs. If the OS is unknown to `pal.h`, the `#error`
in `pal_concept.h` tells you to add a `UMEM_PAL_<os>` block.

## The AAL contract (every architecture provides)

- Arch identity in `aal.h`: `UMEM_AAL_<arch>`, `UMEM_AAL_NAME`,
  `UMEM_AAL_PTR_BITS`, `UMEM_AAL_WORD_BITS`. **No hard-coded VA width** — the
  span table sizes from `UMEM_AAL_PTR_BITS` (P7.4).
- `umem_aal_pause()` — cpu-relax (PAUSE / YIELD / Zihintpause / membar), inline.
- `int getpcstack(uintptr_t *, int, int)` — the frame walk (`getpcstack.c`),
  honest about frame-pointer dependence on x86_64.
- `void *getfp(void)` — **illumos only** (sparc/x86 subr); on Linux/BSD the
  walk uses `__builtin_frame_address` and there is no `getfp` symbol.
- The atomics surface (`atomic.h`): `atomic_cas_ptr`, `atomic_add_64`,
  `membar_producer`/`membar_consumer`, etc.
- Optional: the rseq critical section (`umem_rseq_<arch>.S` + `umem_rseq.c`),
  gated by `ENABLE_RSEQ` at build and `UMEM_RSEQ_AVAILABLE` at run. An arch
  *without* rseq is still complete; the PAL advertises the gap via
  `UMEM_PAL_HAS_RSEQ`.

If the arch is unknown to `aal.h`, the `#error` in `aal_concept.h` names the
block and the implementation unit you must add. Do **not** stub a missing
symbol silently — name the gap in the cascade.

## Build-target

Wire the per-OS/per-arch files into `Makefile.am` by platform conditional
(`SOLARIS_BUILD`/`FREEBSD_BUILD`/`ARCH_SPARC`/`ENABLE_RSEQ`), the way the
existing `RSEQ_SOURCES` / `SPARC_ASM_SOURCES` / `X86_ASM_SOURCES` variables do.

## Test-port

1. The `#error` cascade: build on the new target. An incomplete `aal.h`/`pal.h`
   block fails to compile naming what is missing.
2. The smoke test: `make check` builds and runs `test/port/pal_aal_smoke`,
   which references every required symbol (link failure = missing symbol) and
   prints the selected PAL/AAL identity and feature bitmask.
3. `make check`: the behaviour contract. It must match the baseline pass set
   for the target (or document any delta with cause).

## Certify

A target is **certified** only when `make check` passes on real hardware (or a
faithful emulator) and `hotpath_ab.sh` shows the fast path unmoved against the
previous commit. A target that only cross-compiles is recorded honestly as
"cross-compiles, unrun on hardware" — never claimed as a run that did not
happen (`AGENTS.md` §6).

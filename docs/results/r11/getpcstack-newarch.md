# r11 — getpcstack per-arch frame walks for riscv64 and arm32

Closes the r10 Gate C gap #1: `getpcstack()` returned depth 0 on riscv64 and
arm32 because `getpcstack.c` had per-arch frame-pointer walks only for
x86_64, i386 and aarch64; riscv64/arm32 fell to the generic path and returned
0, leaving `UMEM_DEBUG=audit` bufctl backtraces and `findleaks` with empty
stacks on those arches.

## Change

Two new `#elif` branches in `getpcstack.c`, modelled on the existing
x86_64/aarch64 frame-pointer walks and carrying the same P5.9 stack-bounds
guard (`umem_stack_bounds()` + `umem_frame_readable()`), so a wild or
data-holding frame pointer (a caller built without frame pointers) cannot
fault the allocator — every dereference is bounds-checked against this
thread's stack and the chain must strictly increase.

- **riscv64** — fp is `s0`/`x8`. RISC-V ELF psABI puts the saved register
  pair BELOW the frame pointer: saved `ra` at `s0-8`, previous `s0` at
  `s0-16`. The readable range checked is `[fp-16, fp)`. (x86/aarch64 put the
  pair at `fp[0]/fp[1]`; riscv differs, so this is a distinct arm.)
- **arm32 (armhf, AAPCS)** — fp is `r7` in Thumb, `r11` in ARM; selected by
  `__thumb__` at compile time. Verified against GCC codegen on armhf
  (default Thumb-2): `push {r7, lr}; add r7, sp, #0` makes `r7` point at
  `{ saved_fp, saved_lr }`, i.e. `fp[0]/fp[1]` — the same shape the walk
  reads. Defensively bounds-checked because ARM has never mandated one frame
  layout.

Both set `UMEM_HAVE_REAL_PCSTACK`, so only these two arches gain the walk;
the generic/`backtrace(3)` fallback is bypassed on them and nowhere else.
Full depth requires `-fno-omit-frame-pointer` (`--enable-frame-pointers`),
exactly as the x86_64 walk already documents — a default `-O2` build omits
frame pointers and the walk keeps a single frame (the honest `first_internal`
fallback) rather than presenting an empty or fabricated stack.

## Validation (kiwi, box `intel-lo@r11`, Debian 13 trixie, 8 vCPU x86_64)

- **Ref:** branch `r11pc`, commit `f03b004` (`feat(getpcstack): per-arch frame
  walks for riscv64 and arm32`); cross build ran against `374f6cc` which has
  `f03b004` as an ancestor (verified `git merge-base --is-ancestor`). Shipped
  as committed content via `verify-isolated.sh` (`git archive`).
- **Toolchains:** Debian cross-gcc — `riscv64-linux-gnu-gcc`,
  `arm-linux-gnueabihf-gcc` (armhf/Thumb-2), `aarch64-linux-gnu-gcc`.
- **Configure:** `--host=<triple> CC=<triple>-gcc --enable-frame-pointers`
  (the AAL requests `-fno-omit-frame-pointer`, confirmed `UMEM_HAVE_FRAME_POINTERS 1`
  in each `config.h`).
- **Runner:** `qemu-{riscv64,arm,aarch64}-static -L /usr/<triple>`.
- Logs: `docs/results/jobs/intel-lo-r11-cross{,2}/`,
  `docs/results/jobs/intel-lo-r11-x86native/`.

### getpcstack depth: before → after

| arch | before (r10 Gate C) | after | note |
|---|:---:|:---:|---|
| riscv64 | 0 | **3** | fp=s0/x8, pair at fp-16/fp-8; real caller frames |
| arm32 (armhf) | 0 | **1** | fp=r7 (Thumb-2), pair at fp[0]/fp[1]; one real caller frame then stock libc has no fp chain |
| aarch64 | 4 | **4** | unchanged — no regression |

depth=1 on arm32 is a *real* captured frame, not the single-frame fallback:
the walk returns `depth>0`, which the code only does after writing a
non-internal return address (the smoke test calls `getpcstack` from `main`;
the walk captures `main`'s return into stock libc, whose frames carry no
frame pointer, so the chain correctly stops there). It is honest depth, not
fabricated.

### Smoke + core tests under qemu (all three arches)

`pal_aal_smoke` → `PAL/AAL smoke: OK` on each. `umem_test` 3/3 and
`umem_test2` (4-thread) 3/3 on each. Full `pal_aal_smoke` output:

```
riscv64:  AAL: RISCV64 (ptr=64 word=64)
          AAL: getpcstack depth=3
          PAL/AAL smoke: OK

arm32:    AAL: ARM32 (ptr=32 word=32)
          AAL: getpcstack depth=1
          PAL/AAL smoke: OK

aarch64:  AAL: AARCH64 (ptr=64 word=64)
          AAL: getpcstack depth=4
          PAL/AAL smoke: OK
```

### x86_64: no regression

- Native x86_64 `make check` on the same box: **TOTAL 73, PASS 60, SKIP 13,
  FAIL 0, XFAIL 0, XPASS 0, ERROR 0** (`intel-lo-r11-x86native`). The skip
  set is environment-dependent (introspect-socket, heap-ceiling, and the
  `--enable-frame-pointers`-gated T9 leak-stack test); no new fails or skips
  from this change.
- **Preprocessed proof of zero x86_64 behaviour change:** `gcc -E` of
  `getpcstack.c` for `__x86_64__` is byte-identical between `master` and
  `r11pc` — the two new branches are guarded by `__riscv`/`__arm__` and the
  x86_64 compiler never reaches them.

## Scope / what this is NOT

- QEMU-user emulates the CPU, not kernel-faithful stacks beyond the ELF
  layout; it validates that the walk reads the right slots and produces a
  nonzero, non-faulting depth, not behaviour on real riscv64/arm32 silicon.
- The rseq half of r11 task 3 is deliberately NOT done here: QEMU-user cannot
  faithfully model rseq/cpu-migration, so writing riscv64/arm32 rseq asm and
  "validating" it under QEMU would be a false green. The classic per-CPU
  fallback stays (coordinator's documented decision).

# RISC-V rseq asm: written, HARDWARE-tested, BUG FOUND -- not shipped

Host "lixr" (ssh gburd), real 8-core Spacemit X100 (rv64gcv), Linux 6.18,
glibc 2.43 (owns rseq, __rseq_offset present). Work dir /scratch/libumem-*.
2026-10-17. Branch r12-riscv-rseq (NOT merged to master).

## What was done
Round 10 deferred RISC-V rseq asm as a false-green risk under QEMU. With real
multi-core RISC-V silicon available, I WROTE it: aal/umem_rseq_riscv64.S, a
direct port of the aarch64 critical sections (4 entry points, the trailing-
store ordering, P5.13b mangling, RSEQ_SIG 0x53053053 which matches the rv64
kernel). umem_rseq.c: TLS-base read via `tp` for riscv, __NR_rseq=293.

## What PASSES on real riscv silicon
- Assembles + links: aal/umem_rseq_riscv64.o in libumem.so.2.0.0 (ELF RISC-V).
- rseq ENGAGES, not fallback: "rseq assembly fast path enabled (glibc=1,
  fs_offset=4416, ncpus=16)".
- Basic functional: umem_test 1/2/3 all PASS; a 100k alloc/free loop through
  the asm fast path: clean (exit 0).
- The RSEQ_SIG sits exactly 4 bytes before each abort_ip (objdump-verified).
- Struct offsets match the C umem_rseq_cache_t exactly (offsetof-checked).

## The BUG (why it is NOT shipped)
Under a SIGALRM storm (10us interval) hitting INSIDE the critical section, the
process SEGVs. Reproduced minimally: a direct-call alloc/free loop + an
itimer SIGALRM storm faults in ~1-2 s. Diagnosis (gdb, SIGALRM passed):
  #0 h () at 0x2aaaaaaac4  (signal handler, CORRUPTED entry pc)
  #1 <signal handler called>
  #2 umem_rseq_free_fastpath at aal/umem_rseq_riscv64.S:245 (lw t0,4(s4), the
     cpu_id load at the top of the critical window)
A signal delivered while pc is inside the rseq window leaves the sigframe /
return pc corrupted -- the kernel's rseq-abort-plus-signal interaction on rv64
is not being satisfied by this asm's rseq_cs setup. The non-rseq fallback
build (--disable-rseq) survives the identical storm (SKIPs the asm path) and
passes umem_test 1/2/3, so the fault is specifically in this asm, not generic.

Leading hypotheses for the fix (for a future session with lixr):
1. rseq_cs descriptor placement/alignment: it is at sp+0 (16-aligned); the
   kernel ABI wants the rseq_cs 32-byte aligned, and the aarch64 version keeps
   it mid-frame (sp+48/sp+64). A misaligned or stack-colliding rseq_cs can make
   the kernel mishandle the in-window signal. TRY FIRST: 32-align the rseq_cs
   (reserve 32 extra bytes, round &rseq_cs up to 32).
2. post_commit_offset / abort_ip must be ABSOLUTE runtime addresses; lla gives
   that, but verify the kernel reads them as this asm lays them out on rv64
   (compare against the kernel selftest rseq-riscv.h critical-section macros).
3. Whether rv64 needs the rseq_cs re-registered after EACH abort (the loop
   re-registers each iteration, which should be fine, but confirm).

## RISC-V shipping status
RISC-V ships on the CORRECT non-rseq fallback path (umem_rseq.c #else branches):
functionally correct (umem_test 1/2/3 PASS, 100k-alloc loop clean), just not
lock-free. The lock-free asm is on branch r12-riscv-rseq pending the fix above.
This is the round-10 discipline held: no false-green -- buggy rseq asm found by
the hardware stress test does not merge.

## 2026-10-17 UPDATE: BUG FOUND AND FIXED -- riscv rseq asm now works

Root cause: **wrong RSEQ signature.** The asm used the x86 filler
`0x53053053`. RISC-V's rseq signature is `0xf1401073` (the encoding of
`csrw mhartid, x0`), per glibc `riscv64-linux-gnu/bits/rseq.h` and the kernel
selftest `rseq-riscv.h`. glibc owns the rseq area on this host (glibc 2.43) and
registered `0xf1401073` with the kernel; the kernel reads `*(abort_ip-4)` at
EVERY abort and SIGSEGVs the process on a signature mismatch. So under the
signal storm, every rseq abort force-killed the process -- the crash localized
to a libc syscall stub (`kill@@GLIBC_2.27 + 0x8`) because that is where the
kernel-forced SIGSEGV happened to land the corrupted pc. This is the SAME class
as the aarch64 signature issue the C-side comment already warned about; the
riscv `#else` branch had wrongly inherited the x86 value.

The fix (commit on branch r12-riscv-rseq, merged to master):
1. `aal/umem_rseq_riscv64.S`: `#define RSEQ_SIG 0xf1401073`.
2. `umem_rseq.c`: add an `#elif defined(__riscv) && __riscv_xlen==64` branch
   setting `UMEM_RSEQ_SIG 0xf1401073` (was falling through to 0x53053053).
3. Hardening (not the fatal bug, but correct): 32-byte-align the on-stack
   `rseq_cs` descriptor -- the kernel's `rseq_get_rseq_cs` requires
   `__alignof__(struct rseq_cs)==32` and `sp` is only 16-aligned. Frames grew
   96->128, saved registers relocated, `&rseq_cs = (sp+31) & ~31`.

### Hardware result after the fix (real Spacemit X100, rv64gcv, glibc 2.43)
- `repro_rseq_trailing_store`: **PASS** -- 6,307,251 pushes, 69,910 signals
  delivered, **0 double-presence events** (trailing-store ordering holds under
  the storm).
- `repro_reload_commit_abort`: **PASS** -- 3,997,434 commits, 2,566 aborts,
  21,392 signals (aborts restart correctly, no SIGSEGV).
- `probe_rseq_armed`: rc=0; `probe_rseq_prodcons`: run-until-killed (rc=124
  timeout, no core).
- `umem_test` 1/2/3: **PASS**; rseq engages (`rseq_enabled=1 asm_safe=1
  ncpus=16`) -- using the lock-free asm, not the fallback.

RISC-V now ships the lock-free rseq fast path. The earlier discipline
(no false-green; keep buggy asm off master) was correct and is why the real
signal-storm stress existed to catch this.

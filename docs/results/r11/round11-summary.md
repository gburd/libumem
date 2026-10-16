# Round 11 -- owed-task completion summary (2026-10-16)

Four owed tasks from the v4.2.0 program. Status, honestly:

## Task 3a -- per-arch getpcstack (riscv64, arm32): DONE, merged
getpcstack.c gained frame-pointer walks for riscv64 (fp=s0/x8) and arm32
(fp=r7, Thumb-2). QEMU-validated: riscv64 depth 0->3, arm32 0->1 (honest limit:
stock libc has no fp chain beyond the first caller), aarch64 unchanged at 4.
x86_64 preprocessing byte-identical (zero risk to existing arches). Commits
d54630f + a3adfde on master.

## Task 3b -- lock-free rseq for riscv64/arm32: DECLINED THIS ROUND (false-green risk)
NOT done, by deliberate decision. Writing aal/umem_rseq_riscv64.S and
umem_rseq_arm32.S blind cannot be TRUSTWORTHILY validated: QEMU-user does not
faithfully model rseq cpu-migration/abort, so a "passing" emulated run would
NOT prove the asm correct -- it would be exactly the false-green the project
forbids. The classic fallback (umem_rseq.c's #else branches) is CORRECT on
these arches today; it is just not lock-free. The honest state: riscv64/arm32
run correctly with the non-rseq per-CPU path; the lock-free fast path on those
arches is owed-on-real-silicon. Recorded, not faked.

## Task 4 -- PAL carve-out part 2: DONE, merged
11 memory-syscall sites (9 in vmem_mmap.c, 2 in umem.c) moved behind the
umem_pal_* contract in pal/pal_mem.h (all static inline -> zero fast-path
indirection). Gates: A (make check identical both arches, per-test diff clean),
B (hotpath_ab +0.00 insn, throughput in null, both arches), C (riscv64/arm32/
aarch64 cross-compile+smoke). Commits 5551ed1 + 4e58644 on master. The PAL now
owns the memory contract; arena policy stayed with the callers. Further
carve-out (envvar/fork/init_lib platform bits) remains incremental follow-up.

## Task 1 -- Pages + CI green: PARTIAL (local-validated; badge owed-on-push)
Validated the WORK on EC2: the Jekyll Pages site builds green (rc 0, 55 files,
index.html contains libumem), the fuzz harness compiles, clang-format present.
The green CI/Pages BADGE requires a push triggering the GitHub Actions / Codeberg
Forgejo runners plus the one-time "Pages: Source = GitHub Actions" repo setting
-- maintainer/push steps not reachable from an EC2 box. No badge claimed green
that was not observed green. docs/results/r11/task1-ci-pages-local-validation.md.

## Task 2 -- illumos/SPARCv9 hardware run: NOT DONE (no silicon reachable)
No SPARC hardware exists to this agent; AWS has none; no illumos AMI on kiwi; a
local qemu-system-sparc64 boot would violate AGENTS.md section 1 (no runs on
floki) and would be emulation, not hardware, anyway. SPARCv9 remains
cross-compile-only (the AAL names it; getpcstack has a SPARC walk). A real
illumos run (even on x86_64) would need an illumos image + VM not available
here. Owed-on-hardware; NOT faked. The honest claim stands: libumem
cross-compiles for SPARCv9/illumos; it has not been run on illumos.

## Net
2 of 4 fully done and merged (getpcstack, PAL carve-out part 2), both
behaviour-preserving with both-arch gates. 1 partial (CI/Pages work validated,
badge owed-on-push). 1 blocked on hardware (illumos/SPARC). 1 sub-item
(riscv/arm32 rseq asm) deliberately deferred to avoid a false-green. No ABI
change; nothing on master is unvalidated.

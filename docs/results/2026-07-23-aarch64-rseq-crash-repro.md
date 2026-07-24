# aarch64 (Graviton) rseq fast-path SIGSEGV — reproduction

**Instance:** arm-lo = `c7g.2xlarge` (aarch64, 8 vCPU), AL2023-arm64, glibc rseq-managed.
**Git SHA at repro:** see `git rev-parse HEAD` (pre-fix, `0284bab` line).
**Headline finding #1** in `docs/results/2026-07-23-baseline.md`.

## Symptom

Every umem allocation on Graviton crashes once the rseq assembly fast path is
active. glibc 2.35+ on AL2023 manages rseq, so `umem_rseq_asm_safe = 1` on
aarch64 (the `umem_rseq_use_glibc` init branch sets it on all arches), and the
call site in `umem.c` (`if (umem_rseq_asm_safe) umem_rseq_alloc_fastpath(...)`
guarded by `__x86_64__ || __aarch64__`) invokes the aarch64 assembly.

Startup prints:
```
umem: rseq assembly fast path enabled (glibc=1, fs_offset=-32, ncpus=8)
```

## Reproduction

`repro_rseq.c`: one single-threaded alloc/free, then 8 threads each doing 2M
alloc/free with periodic `sched_yield()` to force CPU migration (which is what
makes the kernel abort an in-flight rseq critical section → jump to abort_ip).

```
$ ./scripts/ec2/clean-regen.sh && make -j$(nproc)
$ gcc -O2 -I. repro_rseq.c -o /tmp/repro_rseq -L.libs -lumem -lpthread
$ UMEM_DEBUG=1 LD_LIBRARY_PATH=.libs /tmp/repro_rseq
umem: rseq assembly fast path enabled (glibc=1, fs_offset=-32, ncpus=8)
Segmentation fault (core dumped)      # exit 139
```

## GDB backtrace

```
Program received signal SIGSEGV, Segmentation fault.
umem_rseq_alloc_fastpath () at umem_rseq_aarch64.S:168
168		ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
#0  umem_rseq_alloc_fastpath () at umem_rseq_aarch64.S:168
pc  0xfffff7284244  <umem_rseq_alloc_fastpath+228>
=> 0xfffff7284244 <umem_rseq_alloc_fastpath+228>:  ldr  x1, [x19, #40]
```

The PC is stopped at the very first instruction of the abort handler
(`.Lrseq_alloc_abort:` label, `+228`). This is exactly the abort_ip the kernel
was told to jump to on restart.

## Root cause

The Linux rseq ABI requires the 4-byte signature to sit **immediately before**
`abort_ip`. On a restart the kernel reads `*(uint32_t *)(abort_ip - 4)` and, if
it does not equal the registered `RSEQ_SIG`, it SIGSEGVs the process rather than
jumping into arbitrary code.

`umem_rseq_aarch64.S` placed the signature **after** the abort label:

```
	.align	5
.Lrseq_alloc_abort:
	.inst	RSEQ_SIG        ; WRONG: this is at abort_ip, not abort_ip-4
	ldr	x1, [x19, #CACHE_RESTART_COUNT_OFFSET]
```

`abort_ip` is computed as `adrp/add :lo12:.Lrseq_alloc_abort` → the label, so
`abort_ip - 4` points at whatever precedes the label (the tail of the empty/full
path), never `RSEQ_SIG`. First restart → signature mismatch → kernel kill.

The x86_64 file was fixed identically in commit
`4b7934b "Fix RSEQ abort handler: signature must precede label (kernel reads
abort_ip-4)"`; the aarch64 `.S` was never given the same fix. Both handlers in
the aarch64 file (`.Lrseq_alloc_abort`, `.Lrseq_free_abort`) are affected.

## Fix

Move `.inst RSEQ_SIG` to *before* each abort label (after the `.align 5`), so
`abort_ip - 4` lands on the 4-byte signature — matching the x86_64 idiom. The
abort_ip computation already points at the label (post-signature), so it needs
no change.

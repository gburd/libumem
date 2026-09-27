# T9: why x86_64 audit stacks are one frame deep

Diagnosis only; no fix is implemented. It answers finding T9 in
`docs/reviews/2026-09-26-tool-review.md`: under `UMEM_DEBUG=audit` on x86_64,
every recorded stack has `depth=1`, and that one frame is inside libumem.

## Provenance

- Source: `git archive` of `origin/master` via `scripts/ec2/verify-isolated.sh`.
  The probe runs used `1c1eac055993`. The first attempt, at `06f0c415d550`,
  failed on a probe compile error, a bug in my probe, not in libumem. The T9 run
  used `1c1eac055993`. Neither commit touches `getpcstack.c`, `umem.c` or the
  build flags.
- x86_64: `c7i.2xlarge`, Debian AMI (`EC2_OS=debian`), gcc 14.2.0 (`intel-lo@r7t9`).
- aarch64: `c7g.2xlarge`, Debian AMI (`EC2_OS=debian`), gcc 14.2.0 (`arm-lo@r7t9`).
- Library configure: default, which gives `CFLAGS = -g -O2 -std=c17 ...`. The fp
  variant sets `CFLAGS="-g -O2 -fno-omit-frame-pointer"` at configure time.
- Probe: a small C program, `r7_top -> r7_mid -> r7_leaf -> umem_alloc(200)`. It
  calls `umem_whatis()` on the buffer and prints `info.depth` and each PC. The
  PCs are symbolized with `dladdr` + `addr2line`. The probe was built `-O2`, with
  and without `-fno-omit-frame-pointer`.
- T9 target: the in-tree `test/debugger/test_tool_T9_leak_stack.sh`, then
  `tools/umem --pid <tool_target> findleaks -n 3`. Configured with
  `--enable-introspect`. In the fp variant, `CFLAGS` also applies to
  `tool_target`, so that row is "library + program with fp".
- Job logs are in `docs/results/jobs/{intel,arm}-lo-r7t9-{depth2,t9}/`. That
  directory is gitignored, so the key lines are reproduced below.

## Mechanism

1. **The x86_64 walker walks frame pointers only.** It is the
   `EC_UMEM_DUMMY_PCSTACK && __x86_64__` branch of `getpcstack.c`. It follows
   `fp[0]` (the saved `%rbp`) and records `fp[1]` (the return address). It does
   not use DWARF unwind info. Its own comment says it "requires the library to
   be built with -fno-omit-frame-pointer". **No build flag does that.**
   `Makefile.am` `AM_CFLAGS` and `configure.ac` add `-fno-omit-frame-pointer`
   only for ASAN, UBSAN, TSAN and one test (`test_stack_bounds`). gcc 14 at
   `-O2` on x86_64 reports `-fomit-frame-pointer [enabled]`.
2. **The walk begins in `getpcstack()`'s own frame.** `__builtin_frame_address(0)`
   forces that one frame to have a frame pointer. So frame #0 is always the
   return address into whichever function inlined `UMEM_AUDIT`
   (`umem_cache_alloc_debug`, `umem_slab_alloc`, `_umem_cache_free`).
3. **In the default build that caller keeps a data value in `%rbp`.** The
   default `umem_cache_alloc_debug` prologue is:
   `7d10e: push %rbp; 7d10f: mov %rsi,%rbp`. So `%rbp` holds the `buf`
   argument, which is a heap pointer. The `fp[0]` that `getpcstack` reads is
   that heap address.
4. **`umem_frame_readable()` rejects it.** The P5.9 check requires the address
   to be inside `[stk_lo, stk_hi)`. The heap address is not, so the walk stops
   at depth 1. The check is doing its job: it refuses to treat `buf` as a stack
   frame. Without the check, the older `nextfp <= fp` test would most likely
   stop the walk too, because the mmap'd heap sits below the stack. I reasoned
   this and did not measure it.

With `-fno-omit-frame-pointer`, the same function begins
`7dbc0: push %rbp; 7dbc1: mov %rsp,%rbp`. The chain is then intact through the
library and reaches the caller.

On aarch64, the default `-O2` build already emits a frame record
(`7b88c: stp x29, x30, [sp,#-96]!; mov x29, sp`) in `umem_cache_alloc_debug`.
`gcc -Q` still reports `-fomit-frame-pointer` as enabled there, but the
disassembly shows the record. So the same walker works on aarch64.

**Verdict on the hypotheses:**

- **(a) is the root cause.**
- **(b) is a second, independent defect.** The walker skips no internal frames,
  so frames #0–#2 are always libumem. This does not limit depth, but the "top
  frame" that findleaks, leaks and learn show is always the allocator.
- **(c) is only where the walk stops**, and there it is correctly rejecting a
  non-frame. Loosening it would be wrong.

## Evidence

Probe, `umem_alloc(200)` from `r7_top -> r7_mid -> r7_leaf`,
`UMEM_DEBUG=audit`:

| arch | library | program | depth | frames recorded |
|---|---|---|---|---|
| x86_64 | default | -O2 | **1** | `umem_cache_alloc_debug` (umem.c:2215) |
| x86_64 | default | -O2 -fno-omit-fp | **1** | `umem_cache_alloc_debug` |
| x86_64 | -fno-omit-fp | -O2 | **4** | `umem_cache_alloc_debug`, `_umem_cache_alloc`, `_umem_alloc`, **`r7_leaf`** |
| x86_64 | -fno-omit-fp | -O2 -fno-omit-fp | **8** | above, then `r7_mid`, `r7_top`, `main`, `__libc_start_call_main` |
| aarch64 | default | -O2 | **10** | the 8 above, then 2 non-canonical values (`0x2bffffab7e233c`, `0x3aaaab4860b70`) |
| aarch64 | default | -O2 -fno-omit-fp | 10 | same |
| aarch64 | -fno-omit-fp | -O2 | 10 | same |
| aarch64 | -fno-omit-fp | -O2 -fno-omit-fp | 10 | same |

x86_64 `tool_target` (T9 regression script + `umem findleaks`):

| library + program | leak records | depths | first PC in exe | findleaks top frame |
|---|---|---|---|---|
| default | 438 | all `depth=1` | 0 / 438 | `umem_cache_alloc_debug`; also `_umem_cache_free+0x615` (umem.c:2257) |
| both -fno-omit-fp | 433 | 5 (39), 6 (348), 10–12 (46) | **0 / 433** | #0 `umem_cache_alloc_debug`, #1 `_umem_cache_alloc`, #2 `_umem_alloc`, #3 `hold_all` / `churn`, #4 `main` / `start_thread` |

The T9 regression still exits 1 in the fp build, for two reasons. Its first
check wants the first PC in the executable, and (b) makes the first PC always
the allocator. Its second check wants `sig size=200`, and learn still records
`0`. That second part of T9 is the bufsize-vs-request-size bug. It is unrelated
to stacks, and frame pointers do not affect it.

Other things found along the way (not investigated further):

- aarch64 frames #8 and #9 have non-canonical high bits. They are probably
  PAC-signed return addresses from Debian's `libc`: the walker stores `x30`
  as-is and does not strip PAC bits (`xpaclri`). They are harmless for leak
  classification but print as garbage.
- `tools/DEBUGGING.md` says "On Linux this uses `backtrace(3)`, which works with
  frame pointers OR DWARF unwind info". That is false on x86_64 and aarch64,
  which both use the frame-pointer walkers. `backtrace(3)` is only the fallback
  for other architectures, and it returns 0 frames when
  `umem_malloc_is_interposing`. This is an AGENTS.md §7 comment-vs-code bug.
  The same file's "truncated stacks" caveat understates the problem: on x86_64
  the stack is 1 frame, always.

## Recommended fix

1. **Build libumem with `-fno-omit-frame-pointer`.** Put it in `AM_CFLAGS`,
   either unconditionally or at least for x86_64. The walker already assumes
   this. The fix is one line and changes no walker code, including the P5.9
   bounds check.
   - Result, measured above: the immediate caller is always recorded (depth 4
     with a no-fp program). With an fp program, the full chain is recorded. That
     is enough to separate `leak_site` from churn.
   - Cost: on x86_64, `%rbp` is taken out of allocation in every non-leaf
     function, plus a 2-instruction prologue and a 1-instruction epilogue. That
     includes the allocator fast path. Distros that switched to frame pointers
     everywhere (Fedora 38, Ubuntu 24.04) cite about 1–2% typical overhead.
     **Not measured for libumem.** It needs an A/B on the `umem_alloc` /
     `malloc` hot path (intel-hi, isolated ref) before merging.
   - Per-file flags do not help. Every frame between `getpcstack` and the
     caller is on the hot path.
   - aarch64 already behaves this way, so there is nothing to change there.
2. **Skip libumem's own frames when recording (fixes (b)).** At init, compute
   libumem's text range once. When recording, drop leading PCs that fall inside
   it.
   - Do not skip a fixed count. The number of internal frames varies by path:
     3 via `umem_alloc`, a different number via the malloc interposer,
     `umem_slab_alloc` or free.
   - This frees 3 of the 15 `umem_stack_depth` slots. It also makes the T9
     regression's "first PC in exe" check meaningful.
   - Without this step, fix 1 alone leaves the T9 test failing, and every "top
     frame" column still shows the allocator.
3. **Do not switch to `_Unwind_Backtrace` / libdw as the primary walker.** It
   would give full stacks for no-fp programs, which is the Debian x86_64 norm.
   But it costs microseconds per frame, where the frame-pointer walk costs
   nanoseconds. Its lazy `dlopen` of libgcc_s allocates, which is the
   re-entrancy the existing fallback already guards against. The existing
   fallback also returns 0 frames under `libumem_malloc.so`. And libgcc's
   unwinder is not bounded against a corrupted stack the way the P5.9 walk is.
   It is a reasonable opt-in (`UMEM_OPTIONS=unwind=dwarf`) later. It is not the
   fix for T9.
4. Correct the `tools/DEBUGGING.md` claim above in the same change.

Fixes 1 and 2 do not touch the separate learn size bug (224 vs 200). It needs
its own fix.

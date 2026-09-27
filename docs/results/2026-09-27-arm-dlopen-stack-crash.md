# arm-hi `sustained_load.sh` SEGV in `umem_init`: mechanism

Worker `@r6arm`, 2026-09-27. Follows up "Finding: arm-hi sustained_load.sh
SEGVs in umem_init -> dlopen" in `2026-09-26-p85b-aliasing-mechanism-v2.md`.

Status: **fixed** in 59ab75e (libumem half). The findings were committed
before the fix (360a83d, 563e899). Sections 5, 6 and 8-10 come from probe2 and
the experiment runs, and section 12 from the post-fix gate. The harness half
belongs to @r6cmp.

## Summary

There are two causes, and they need to be kept apart.

1. **Primary trigger (harness).** `sustained_load.sh` exports
   `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=8388608`. glibc takes the
   static TLS block out of every thread's stack mapping, so with that tunable
   a default 8 MiB pthread keeps only **2,448 B (arm64) / 8,208 B (x86_64)**
   of stack below `sp`. Nothing non-trivial runs in 2.4 KB. This confirms
   @r6cmp's measurement (2,719 / 8,319 B, same order of magnitude; the exact
   value depends on the binary's own TLS). @r6cmp owns the harness change.
2. **Real libumem defect (smaller, independent of the tunable).** The first
   allocation's `umem_init` uses about **67 KB of stack** on both arches,
   against glibc malloc's first call at about 1-3 KB and jemalloc's at about
   3 KB. **The dlopen is not what uses it.** `dlopen(NULL)` costs about 3 KB.
   The whole cost is `umem_get_max_ncpus()` (`init_lib.c`), which puts a
   `char proc_stat[65536]` on the stack. So:
   - x86_64: a first `umem_alloc()` on a 64 KiB pthread **SEGVs** (every size
     from 16 to 64 KiB, rc=139). glibc malloc and jemalloc survive every one
     of those sizes. This is a real bug: umem needs about 20-60x the stack
     glibc does for the first allocation, and it crashes where glibc and
     jemalloc do not.
   - arm64: `PTHREAD_STACK_MIN` is 128 KiB (64 KiB pages), so no thread can be
     made smaller than 128 KiB, and 128 KiB has ~120 KB usable. That is enough
     for a fresh thread. It still crashes when less than ~67 KB is left at the
     first call: after the tunable, deep in a call chain (the regression gives
     it 8 KiB), or with large TLS.
   - x86_64, worse than a crash: the unprobed 64 KiB frame can jump the guard
     page and `read()` `/proc/stat` into a neighbouring thread's stack or TLS
     (section 5). The x86 bench runs that "passed" under the tunable (8.2 KB
     left) were in that regime.
3. **Fixed** (section 12): the buffer is static, and `misc.c`'s 4 KiB message
   buffers are 1 KiB. The first call now peaks at 5.2-5.4 KB, and the
   regression fails before the fix and passes after, on both arches.

The `sub sp,sp,#0x440` fault in ld.so was the *last* frame to hit the guard.
It is not the frame that used the stack. The backtrace named dlopen because
`umem_setup_envvars()` runs **before** `umem_get_max_ncpus()` in `umem_init`,
and with only 2.4 KB of stack the dlopen already reaches the guard.

## Environment

- arm-lo@r6arm: c7g.2xlarge, Debian 13, kernel 6.12.107+deb13-cloud-arm64,
  page size 4096, gcc 14.2.0, glibc 2.41-12+deb13u4.
- intel-lo@r6arm: c7i.2xlarge, same Debian/gcc/glibc, kernel
  6.12.107+deb13-cloud-amd64.
- Library: `--enable-rseq CFLAGS="-O2 -g"` (the shipped gate config), built
  by `verify-isolated.sh` from committed refs `ead7f36` (probe1) and `e23fb25`
  (probe2). Scripts: `scripts/ec2/r6arm_stack_probe.sh`,
  `scripts/ec2/r6arm_stack_probe2.sh`. Probe:
  `test/integration/probe_first_alloc_stack.c`.
- Raw logs: `docs/results/r6arm/probe1-{arm,intel}-lo.log`.

The probe measures two things on a fresh pthread before the process has made
any allocation:
- `usable` = `sp` minus the low end of the stack from `pthread_getattr_np`.
- `depth` = the peak stack use of ONE operation. It paints the stack below
  `sp` with 0xA5 and then finds the lowest byte that was overwritten.

## 1. Usable stack on a default thread (probe1)

| arch | no tunable | `optional_static_tls=8388608` | `=65536` |
|---|---:|---:|---:|
| arm64 | 8,378,256 B | **2,448 B** | 8,313,232 B |
| x86_64 | 8,379,920 B | **8,208 B** | 8,314,896 B |

The 8 MiB tunable removes 8 MiB from each thread's 8 MiB stack, so every
non-main thread in any process run under `sustained_load.sh` has 2-8 KB of
stack. The 64 KiB value leaves 8.3 MB.

## 2. Peak stack depth of the first call (probe1, default 8 MiB thread)

| operation | arm64 | x86_64 |
|---|---:|---:|
| first `umem_alloc(64)` (umem_init) | **66,880 B** | **69,144 B** |
| same, `UMEM_OPTIONS=norseq` | 66,880 B | 69,144 B |
| `umem_get_max_ncpus()` alone | **66,608 B** | **68,952 B** |
| `dlopen(NULL, RTLD_LAZY)` + `dlclose` | 3,088 B | ≤3,416 B |
| glibc `malloc(64)` first call | ≤1,024 B | ≤3,416 B |
| jemalloc `malloc(64)` first call (LD_PRELOAD) | 3,200 B | ≤3,416 B |
| `libumem_malloc.so` `malloc(64)` first call (LD_PRELOAD) | 1,088 B | ≤3,416 B |

"≤N" means the probe's own paint window floor was N (the paint starts 1 KB
below `sp`, and on x86 the `none` control already reads 3,416), so the call
itself used no more than that.

`umem_get_max_ncpus()` accounts for 66.6 of the 66.9 KB on arm64 (99.6 %),
and for 69.0 of 69.1 KB on x86_64. The cause is `init_lib.c`:

```c
    char proc_stat[65536];   /* on the stack */
    fd = open("/proc/stat", O_RDONLY);
    n = read(fd, proc_stat, sizeof(proc_stat) - 1);
```

The buffer was grown from 8 KB to 64 KB for 192-vCPU `/proc/stat` (see the
comment there), and it lives on whatever thread makes the first allocation.

`libumem_malloc.so` is shallow (1 KB) only because its first `malloc()` is
served during the constructor/bootstrap phase. Under LD_PRELOAD, `umem_init`
runs from the first `malloc()` after the constructor, usually on the main
thread (8 MiB stack, before any other thread exists). An application that
creates threads before its first malloc is rare under the interposer, because
`pthread_create` itself allocates. The direct `umem_alloc()` API has no such
protection: the first caller's thread runs `umem_init`.

## 3. First allocation on a small explicit stack (probe1)

`pthread_attr_setstacksize(k KiB)`, then the first allocation, with no tunable.

| stack | x86 umem | x86 glibc | x86 jemalloc | x86 umem_malloc.so |
|---|---|---|---|---|
| 16 KiB | **SEGV** | ok | ok | ok |
| 20-48 KiB | **SEGV** | ok | ok | ok |
| 64 KiB | **SEGV** | ok | ok | ok |
| 128 KiB | ok | ok | ok | ok |
| 256 KiB | ok | ok | ok | ok |

On arm64, `setstacksize` fails with EINVAL below 128 KiB
(`PTHREAD_STACK_MIN=131072`, glibc on aarch64 builds it for 64 KiB pages),
and 128 and 256 KiB are ok for every allocator. So on arm64 no legal
application stack size triggers the defect without the tunable.

## 4. Why arm and not x86 under the tunable

The measured difference is how much stack the tunable leaves: **2,448 B on
arm64 vs 8,208 B on x86_64** (table 1). I did not establish why the remainder
differs by about 5.8 KB between the arches (the per-arch static TLS/TCB
layout is the likely place). The guard is 4096 B on both, and the page size
is 4096 on both boxes.

- x86_64, 8.2 KB left: `umem_setup_envvars`'s dlopen (≤3.4 KB) fits. The next
  step, `umem_get_max_ncpus` (69 KB), does not fit, and it jumps the guard
  instead of faulting (section 5).
- arm64, 2.4 KB left: the dlopen alone (3.1 KB) runs past the guard, and so
  does the ld.so frame, which is where the backtrace faults.

## 5. Why x86 did not crash under the tunable (probe2, experiment)

A 69 KB frame on an 8.2 KB stack must leave the thread's mapping, so on x86
"did not crash" means the frame landed in some other mapping.

- x86_64, tunable set, default thread, first `umem_alloc()`: rc=0, and
  `PROBE_WHERE` shows that `sp - 66000` lies **inside the next-lower rw
  mapping**, past a single 4 KiB `---p` guard page
  (`docs/results/r6arm/exp-none-intel-lo.log`):
  ```
  thread stack lo=0x7f566b760000 sp=0x7f566b762010; target=0x7f566b751e40
    maps: 7f566af5b000-7f566b75f000 rw-p      <- another thread's stack
    maps: 7f566b75f000-7f566b760000 ---p      <- guard
    maps: 7f566b760000-7f566bf64000 rw-p      <- this thread
  ```
  The frame is not probed. If it were, a probe would have touched the guard
  page and faulted. So the 64 KiB `sub rsp` jumps the guard, and `read()`
  then writes `/proc/stat` into the neighbouring mapping.
- The same neighbour write, measured with canaries (probe2,
  `-DPROBE_NB`, no tunable, 128 KiB probe thread whose TLS pushes usable
  stack to 56 KB): **3,547 bytes of the neighbour thread's TLS changed** on
  x86_64. The glibc control changed 0.
  (`docs/results/r6arm/probe2-intel-lo.log`, "neighbour umem 128K".)
- On aarch64 the same arms fault (rc=139) instead of writing the neighbour.
  The gdb trace faults on the first store after the 64 KiB `sub sp`
  (`stp x29, x30, [sp]` in `umem_get_max_ncpus`, `init_lib.c:142`), so in
  these runs the address 64 KiB below was unmapped. I did not establish
  why the layout differs from x86 (the guard is 4096 B on both). I also did
  not show that arm can never land in a neighbour. The ld.so fault PC in the
  original arm-hi backtrace is explained: with 2.4 KB left, dlopen, which
  runs *before* `umem_get_max_ncpus` in `umem_init`, reached the guard
  first.

So on x86_64 the bug is worse than a crash. A first `umem_alloc()` on a thread
with less than ~69 KB of stack left can **silently write up to 64 KiB of
`/proc/stat` into another thread's stack or TLS**. The arm64 behaviour (a
clean SEGV) is the safer failure mode.

## 6. The bench under the tunable (probe1, 16 threads, prodcons 64:256)

| | arm64 | x86_64 |
|---|---|---|
| umem, tunable 8 MiB | **rc=139** | rc=0 |
| umem, tunable 64 KiB | rc=0 | rc=0 |
| jemalloc, tunable 8 MiB | rc=0 | rc=0 |
| jemalloc, tunable 64 KiB | rc=0 | rc=0 |

My probe1 jemalloc arm used `LD_PRELOAD`, so jemalloc initialized on the main
thread before any bench thread existed. probe2 ran both arms 5 times each on
arm64 with the tunable (`docs/results/r6arm/probe2-arm-lo.log`):

| arm64, tunable 8 MiB | 5 runs |
|---|---|
| umem | 139 ×5 |
| jemalloc via `dlopen` (the bench default, what @r6cmp ran) | **139 ×5** |
| jemalloc via `LD_PRELOAD` | 0 ×5 |

This agrees with @r6cmp: jemalloc also crashes when its first use is on a
2.4 KB bench thread. x86_64 was 0 for all 15 runs. So under the tunable the
crash is not umem-specific. 2.4 KB of stack is not enough for anybody.

## 7. What `dlopen` at `envvar.c:997` does and whether init needs it

`umem_setup_envvars()` calls `dlopen(NULL, RTLD_FIRST | RTLD_LAZY)`, which
returns a handle to the main program (no new object is loaded), then
`dlsym`s `_umem_debug_init`, `_umem_options_init` and `_umem_logging_init`
and calls any that exist. That is the Solaris interface that lets an
application supply default `UMEM_DEBUG`/`UMEM_OPTIONS`/`UMEM_LOGGING`
strings by defining those functions. Nothing in this tree defines them
(grep: only envvar.c's table names them). It is part of the documented
libumem API, so it stays. It costs ~3 KB of stack and is not the defect.

## 8. Sibling sweep: every library frame >= 512 B (gcc `-fstack-usage`)

Built with `-fstack-usage` (`scripts/ec2/r6arm_stack_usage.sh`,
`docs/results/r6arm/su1-*.log`). x86_64 / aarch64, bytes:

| function | x86 | arm | reached on | action |
|---|---:|---:|---|---|
| `umem_get_max_ncpus` (init_lib.c) | 65,568 | 65,584 | **first allocation, always** | **fixed** |
| `umem_findleaks` (umem_inspect.c) | 9,360 | 9,344 | explicit API call only | none: caller asks for it |
| `log_message` (misc.c) | 4,320 | 4,384 | **first allocation, if `UMEM_*` has a bad item** | **fixed** |
| `debug_printf` (misc.c) | 4,320 | 4,384 | no caller in the tree | same buffer as log_message, fixed together |
| `umem_profile_check_phase` | 4,192 | 4,224 | update thread (libumem's own 8 MiB stack) | none |
| `umem_vprintf` (misc.c) | 4,112 | 4,176 | panic / error report paths | **fixed** (same macro) |
| `umem_sm_*` (sm.c) | ≤1,776 | ≤1,712 | not in init; owned by the sparsemap agent | none |
| `print_stacktrace` (umem_fail.c) | 1,040 | 1,040 | panic | below the 4 KiB bar |
| `umem_process_value` (envvar.c:863, `buf[512]`) | 560 | 560 | init | below the bar |
| `umem_cache_init` | 672 | 672 | init | below the bar |

The coordinator's other candidates, checked:
- `umem_inspect.c:1045` / `:1951`: struct members
  (`uintptr_t stack[UMEM_INSPECT_MAX_STACK]` in `leak_class`,
  `uint64_t stack[...]` in the v2 profile record), not stack locals. They
  show up only inside `umem_findleaks` (9.3 KB), an explicit API call.
- `umem_own.c:67`: `ohd_alloc_stack[OWN_STACK_DEPTH=8]`, a 64 B struct member.
- `umem_profile.c:114-143`: members of `profile_state_t`, which is a
  file-scope static (`static profile_state_t profile_state`), not stack.
- `umem_fail.c:130` `cur_stack[128]`: 1,040 B frame, panic path only.

Where the first allocation's remaining depth goes after the fix:
`umem_stacktrace_init` 4.7-5.1 KB (backtrace warm-up into libgcc_s
unwinding, plus libdw's `dwfl_linux_proc_report`), `dlopen(NULL)` ~3 KB,
`umem_numa_init` ≤2-3.4 KB. Those are libgcc/libdw/libnuma frames, not
libumem's own.

## 9. Experiment: candidate fixes applied by sed to an isolated tree

`scripts/ec2/r6arm_fix_experiment.sh`, 0657f3b, `docs/results/r6arm/exp-*.log`.
It measures only and is not the committed fix. First-`umem_alloc()` peak
depth on a default thread, and the smallest stack budget that survives
(`PROBE_LEAVE`, 4 KiB steps up to 16 KiB):

| patch | x86 depth | arm depth | x86 16 KiB thread | survives with N B left (both arches) | x86 16 KiB + bad `UMEM_OPTIONS` |
|---|---:|---:|---|---|---|
| none (master) | 69,208 | 66,880 | SEGV | none of 4-16 KiB | SEGV |
| `proc_stat` static | 5,200 | 5,360 | ok | ≥ 5,120 | **SEGV** (log_message 4.3 KB on top) |
| + `UMEM_MAX_ERROR_SIZE` 1024 | 5,200 (5,528 bad opt) | 5,360 | ok | ≥ 5,120 | ok |

glibc's first `malloc` survived every budget including 4 KiB. umem's
post-fix first allocation needs about 5 KB, which is within about 2 KB of
jemalloc's 3.2 KB. The residual is libgcc_s/libdw/ld.so, not libumem code.

## 10. Regression, pre-fix (0de29ef, test committed alone)

`test/integration/test_first_alloc_small_stack{.c,.sh}`, in `TESTS`.
A first allocation on a 16 KiB thread (x86_64), or on a `PTHREAD_STACK_MIN`
thread (arm64, 128 KiB) with only 8 KiB of stack left. Three arms: the
`umem_alloc` API, a glibc control, and `libumem_malloc.so` under
LD_PRELOAD. `verify-isolated.sh` at 0de29ef, `make check
TESTS=test/integration/test_first_alloc_small_stack.sh`
(`docs/results/r6arm/prefix-0de29ef-*.log`):

```
arm-lo:   FAIL: umem_alloc API exited 139
          PASS: first malloc() on a 131072 B thread ... umem_init never ran (not umem's malloc)
          PASS: first malloc() on a 131072 B thread ... umem_init had already run before this thread
          FAIL test/integration/test_first_alloc_small_stack.sh (exit status: 1)
intel-lo: FAIL: umem_alloc API exited 139
          PASS: first malloc() on a 16384 B thread ... umem_init never ran (not umem's malloc)
          PASS: first malloc() on a 16384 B thread ... umem_init had already run before this thread
          FAIL test/integration/test_first_alloc_small_stack.sh (exit status: 1)
```

The LD_PRELOAD arm passes pre-fix, and the test reports why: libc's own
startup calls `malloc` on the main thread, so under the interposer
`umem_init` has always run on the 8 MiB main stack before any thread exists.
That arm cannot fail from this defect and is kept as a survival check, not
claimed as a regression.

## 11. Metal re-run: skipped (coordinator decision)

The fix is a stack-depth reduction on small threads, and it reproduces and
verifies on the lo boxes (section 10). The original arm-hi bench crash comes
from the harness tunable, which leaves 2.4 KB of stack, and that crashes
jemalloc too (section 6). @r6cmp owns that change. Re-running the metal bench
would test @r6cmp's fix, not this one, so no metal was launched.

## 12. Fix (59ab75e) and post-fix gate (d1e41ab)

- `init_lib.c`: `proc_stat` is `static`. The comment states the invariant:
  the only caller is `umem_init()`, on the single thread that moved
  `umem_ready` STARTUP -> INITING under `umem_init_lock`. Every other thread
  waits on `umem_init_cv`, and re-entry panics, so the buffer is never used
  twice. The `static int ncpus` cache already relied on the same invariant.
  No malloc is involved. The cost is 64 KiB of BSS, which is zero-fill and
  not touched until init. Chunked parsing would also work, but a buffer
  that is never shared does not need it.
- `misc.c`: `UMEM_MAX_ERROR_SIZE` 4096 -> 1024. These buffers stay on the
  stack because the printers run concurrently and from signal handlers
  (`test_errlog_signal`). The longest format literal in the tree is 130 B.
  The widest `%s` argument is an env item bounded by `UMEM_ENV_ITEM_MAX`
  (512 B), so the longest real message is ~540 B, and nothing that fitted
  before is truncated now. The pre-fix demonstration for this half is
  experiment `exp-static` (section 9, sed-applied to an isolated tree):
  with only the `init_lib.c` fix, a first `umem_alloc()` on a 16 KiB x86
  thread with a bad `UMEM_OPTIONS` item still SEGV'd. With both fixes it
  passes. The regression's fourth arm is that case.

`verify-isolated.sh` at d1e41ab (fix + gate script), `--enable-rseq -O2 -g`,
`vm.max_map_count=65530` (`docs/results/r6arm/postfix-d1e41ab-*.log`):

| | arm-lo (c7g.2xlarge) | intel-lo (c7i.2xlarge) |
|---|---|---|
| `make check` | 54 total, 51 PASS, 3 SKIP, 0 FAIL | 54 total, 51 PASS, 3 SKIP, 0 FAIL |
| SKIPs | the 3 introspect tests (no `--enable-introspect`) | same |
| regression, 4 arms | all PASS, `umem_init ran on this thread` | all PASS, `umem_init ran on this thread` |
| first-call depth | 5,360 B (was 66,880) | 5,200 B; 5,528 B with bad option (was 69,144) |
| survives N B of stack left | ≥ 5,120 (4,096: SEGV) | ≥ 5,120 (4,096: SEGV) |
| 16 / 32 / 64 KiB thread | n/a (`PTHREAD_STACK_MIN` 128 KiB) | ok / ok / ok (all SEGV'd before) |

The pre-fix regression at df1af67 failed on both arches (API and
bad-option arms rc=139; `docs/results/r6arm/prefix-df1af67-*.log`).

**Residual.** umem's first allocation still needs about 5.2-5.4 KB, against
glibc's ≤1-3.4 KB. Given 4 KiB of stack, glibc survives and umem does not.
The residual is `umem_stacktrace_init` (4.7-5.1 KB: the `backtrace()`
warm-up into libgcc_s and libdw's `dwfl_linux_proc_report`), `dlopen(NULL)`
(~3 KB) and libnuma probing, and none of those frames is libumem's own. The
next step, if it is ever needed, is to move `umem_stacktrace_init()` out of
`umem_init()` to first use, since it serves only error reports and
`umem_findleaks`. It is not done here because it changes when libgcc_s is
loaded, and the target (a 16 KiB x86 thread) is met.

## Attribution

- Primary: the harness tunable (confirmed on arm-lo and intel-lo, table 1).
  Not in rseq (`norseq` depth is identical).
- Secondary, a real libumem bug independent of the tunable: `umem_init`'s
  first-call stack use is ~67 KB because of `umem_get_max_ncpus()`'s 64 KB
  on-stack buffer. It crashes a first `umem_alloc()` on a 64 KiB thread on
  x86_64, where glibc and jemalloc survive a 16 KiB one.
- On x86_64 the same defect can silently overwrite a neighbouring thread's
  stack/TLS instead of crashing (section 5).
- Fix: section 12.

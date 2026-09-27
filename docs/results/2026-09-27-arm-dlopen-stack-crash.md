# arm-hi `sustained_load.sh` SEGV in `umem_init`: mechanism

Worker `@r6arm`, 2026-09-27. Follows up "Finding: arm-hi sustained_load.sh
SEGVs in umem_init -> dlopen" in `2026-09-26-p85b-aliasing-mechanism-v2.md`.

Status: **findings recorded before any fix.** Sections marked *pending* have no
evidence yet.

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
     made smaller than 128 KiB, and 128 KiB has ~120 KB usable. That is enough,
     so a normal small-stack thread does not crash on arm64. It crashes only
     when something else (the tunable) eats the stack.

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

It is not the ld.so frame size and not the page size. It is how much stack
the tunable leaves: **2,448 B on arm64 vs 8,208 B on x86_64** (table 1). The
static TLS block on each arch is sized differently (TLS alignment and the
aarch64 TCB layout), so the remainder differs by 3.4x.

- x86_64, 8.2 KB left: `umem_setup_envvars`'s dlopen (≤3.4 KB) fits. The next
  step, `umem_get_max_ncpus` (69 KB), does not, but it does not always fault.
  See section 5 (*pending*).
- arm64, 2.4 KB left: the dlopen alone (3.1 KB) runs past the guard, and so
  does the ld.so frame, which is where the backtrace faults.

## 5. Why x86 metal did not crash under the tunable (*pending, probe2*)

A 69 KB frame on an 8.2 KB stack must leave the thread's mapping. glibc puts
a single guard page below each thread stack. A frame that moves `sp` past the
guard in one step, without touching the guard page, writes into whatever is
mapped below it: normally another thread's stack or TLS. So "did not crash"
may mean "silently wrote into a neighbouring thread". probe2 tests this with
canaries in a neighbour thread created directly below the probe thread.

## 6. The bench under the tunable (probe1, 16 threads, prodcons 64:256)

| | arm64 | x86_64 |
|---|---|---|
| umem, tunable 8 MiB | **rc=139** | rc=0 |
| umem, tunable 64 KiB | rc=0 | rc=0 |
| jemalloc, tunable 8 MiB | rc=0 | rc=0 |
| jemalloc, tunable 64 KiB | rc=0 | rc=0 |

My jemalloc arm used `LD_PRELOAD` (so jemalloc initialized on the main thread
before any bench thread existed). @r6cmp reports jemalloc also SEGVs on
arm-lo under the tunable. probe2 runs both jemalloc arms (dlopen and
LD_PRELOAD), five times each, to reconcile the two.

## 7. What `dlopen` at `envvar.c:997` does and whether init needs it

`umem_setup_envvars()` calls `dlopen(NULL, RTLD_FIRST | RTLD_LAZY)`, which
returns a handle to the main program (no new object is loaded), then
`dlsym`s `_umem_debug_init`, `_umem_options_init` and `_umem_logging_init`
and calls any that exist. That is the Solaris interface that lets an
application supply default `UMEM_DEBUG`/`UMEM_OPTIONS`/`UMEM_LOGGING`
strings by defining those functions. Nothing in this tree defines them
(grep: only envvar.c's table names them). It is part of the documented
libumem API, so it stays. It costs ~3 KB of stack and is not the defect.

## Attribution

- Primary: the harness tunable (confirmed on arm-lo and intel-lo, table 1).
  Not in rseq (`norseq` depth is identical).
- Secondary, a real libumem bug independent of the tunable: `umem_init`'s
  first-call stack use is ~67 KB because of `umem_get_max_ncpus()`'s 64 KB
  on-stack buffer. It crashes a first `umem_alloc()` on a 64 KiB thread on
  x86_64, where glibc and jemalloc survive a 16 KiB one.
- Fix direction (not yet applied): move `proc_stat` off the stack. It is read
  exactly once, under `umem_init_lock`'s initializing thread, so a `static`
  buffer is safe and costs 64 KB of BSS. Alternatively read `/proc/stat` in
  chunks. The regression is a first `umem_alloc()` from a 64 KiB pthread.

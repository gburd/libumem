# musl/Alpine SIGSEGV investigation — reproduced and fixed

Follow-up to `docs/results/2026-09-08-allocator-shootout.md` §9, which
reported 14 non-reproducible `CRASH: umem ... rc=139` (SIGSEGV) points from
an early Alpine/musl run, concentrated in `prodcons` and the largest size
class (1024:4096), that two full repeat runs could not reproduce. That
report's own conclusion was that the follow-up needed core-dump capture
configured *before* the triggering run, since the original crashes' core
files were on an already-terminated instance. This is that follow-up.

**Result: reproduced deterministically, root-caused, fixed, verified on
both musl and glibc. Commit `8ee87cd`.**

## 1. Setup: core dumps configured and proven before touching the target

Per the task's own hard requirement (don't repeat the prior investigation's
mistake), core dump capture was set up and end-to-end tested with a
deliberate `int *p=0; *p=1;` crash *before* running anything from libumem:

```
$ doas sh -c 'echo "/var/crash/core.%e.%p.%t" > /proc/sys/kernel/core_pattern'
$ bash -c 'ulimit -c unlimited; ./crashtest'
Segmentation fault (core dumped)
$ gdb -q -batch -ex 'bt full' -ex 'info registers' ./crashtest /var/crash/core.crashtest.*
Program terminated with signal SIGSEGV, Segmentation fault.
#0  0x000055ac3de7e1c7 in main () at /tmp/crashtest.c:6
6           *p = 1;
```

Confirmed readable before proceeding. `gdb`, `build-base`, `musl-dbg`,
`linux-headers` were installed via `apk add` (Alpine's minimal base image
ships none of them).

## 2. Instance

Launched directly (musl/Alpine is not one of the four tagged harness
roles), reusing the harness's key pair and security group, tagged
`Project=libumem`:

| | |
|---|---|
| AMI | `ami-053ecc5b99ded6166` (`alpine-3.23.5-x86_64-uefi-tiny-r0`, us-east-2) |
| Instance type | `c7i.2xlarge` (8 vCPU x86_64) |
| OS | Alpine Linux 3.23.5 |
| Kernel | `6.18.38-0-virt` |
| libc | musl 1.2.5-r23 |
| gcc | 15.2.0 (Alpine) |
| Instance ID | `i-0ae3c7cde76829744` |

Build: `./autogen.sh && CFLAGS='-g -O2 -fno-omit-frame-pointer' ./configure
&& make -j$(nproc)`. Configure output confirmed `RSEQ support: yes` and
`execinfo.h`/`backtrace` **not found** (musl has neither — expected,
matches the `eb3e15e` fix from the shootout).

## 3. Reproduction

Ran the exact documented failing invocation

```
LD_LIBRARY_PATH=.libs test/bench/.libs/bench_main \
    -a umem -w prodcons -t 8 -n 10000000 -s 1024:4096 -r 5 -W 1 -c
```

under `ulimit -c unlimited`. **It crashed on the first attempt**, rc=139,
core file written to `/var/crash/`. Ran again in a loop; the crash rate
was roughly 1-in-a-handful of invocations at this exact point (consistent
with a timing-window bug, not something needing exotic conditions).

## 4. gdb evidence

```
$ gdb -q -batch -ex 'bt full' -ex 'thread apply all bt' -ex 'info registers' \
      -ex 'disassemble' test/bench/.libs/bench_main /var/crash/core.bench_main.*

Program terminated with signal SIGSEGV, Segmentation fault.
#0  umem_rseq_alloc_fastpath () at umem_rseq_x86_64.S:93
93          testl   %edx, %edx
#1  0x00007f661467d208 in _umem_cache_alloc (cp=..., umflag=0) at umem.c:2755
#2  0x00007f661468186b in _umem_alloc (size=3481, umflag=0) at umem.c:3377
#3  0x00005614e1512b87 in umem_alloc_wrapper (size=3465) at allocators.c:98
#4  0x00005614e150f58d in producer_thread (arg=...) at bench_framework.c:487
#5  ... start (pthread_create.c:207) -> __clone
```

The fault instruction (`rip = umem_rseq_alloc_fastpath+113`) is the
`testl %edx, %edx` reading `CACHE_ROUNDS_OFFSET(%rbx)` — an address that
gdb showed was perfectly valid (`rbx = 0x7f66143b7000`, a live mapped
`cache_rseq` mmap region). **This is the tell that the fault is not a bad
pointer at all** — the crash is the kernel's own rseq-abort delivery
mechanism, not a wild memory access from umem's own logic. Confirmed by:

```
$ doas dmesg | grep -i attack
Possible attack attempt. Unexpected rseq signature 0x53053053, expecting 0x0 (pid=11725, addr=00000000cb80ece6).
Possible attack attempt. Unexpected rseq signature 0x53053053, expecting 0x0 (pid=11745, addr=00000000a7741003).
```

That is the Linux kernel's `rseq_get_rseq_cs()` hardening check firing.

## 5. Root cause

`umem_rseq_x86_64.S` / `umem_rseq_aarch64.S` implement lock-free per-CPU
magazine access as rseq critical sections. Per the rseq(2) ABI, every
abort handler must be preceded by a 4-byte signature value, and the
*same* value must be passed as the `sig` argument when the thread
registers its rseq area with the kernel — the kernel checks that the
bytes at `*(abort_ip - 4)` match what the thread registered, precisely so
a corrupted/malicious `rseq_cs.abort_ip` can't be used to jump anywhere
executable. Both assembly files correctly define and embed:

```asm
#define RSEQ_SIG 0x53053053
...
    .align  32
    .long   RSEQ_SIG        /* signature BEFORE label */
.Lrseq_alloc_abort:
```

But `umem_rseq.c`'s **manual** registration path (used when glibc itself
hasn't already registered rseq for the thread) called the raw `rseq(2)`
syscall with a hardcoded `sig=0` at all four call sites:

```c
long ret = sys_rseq(&test_area, sizeof(test_area), 0, 0);        /* umem_rseq_available() */
sys_rseq(&test_area, sizeof(test_area), RSEQ_FLAG_UNREGISTER, 0); /* umem_rseq_available() */
long ret = sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area), 0, 0);        /* umem_rseq_register_thread() */
sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area), RSEQ_FLAG_UNREGISTER, 0); /* umem_rseq_unregister_thread() */
```

**Every thread's rseq area was registered expecting signature `0`, while
every abort point in the asm actually carries `0x53053053`.** As long as
no CPU migration happens while a thread is inside a critical section
(between the `.Lrseq_*_start` and `.Lrseq_*_post_commit` labels), nothing
goes wrong — the abort path is simply never taken. The moment the kernel's
scheduler migrates the thread mid-critical-section (an ordinary,
frequent event under real load, not a rare corner case), the kernel
redirects execution to `abort_ip`, reads the 4 bytes immediately before
it, finds `0x53053053` where it expected `0`, treats the mismatch as
evidence of a corrupted/hijacked `rseq_cs` pointer, logs "Possible attack
attempt", and delivers SIGSEGV to the thread.

### Why this was invisible on every glibc environment in the shootout

glibc >= 2.35 registers rseq for every thread automatically at thread
start (exposing `__rseq_offset`/`__rseq_size`). `umem_rseq_init()`
detects this (`umem_rseq_use_glibc = 1`) and, when true, **never calls
`sys_rseq()` at all** — it just reads glibc's already-correctly-registered
area. The buggy code path in `umem_rseq.c` is only reachable when umem
does its *own* manual registration, which only happens when glibc hasn't
already done it for you: glibc < 2.35, or **no glibc at all**. musl has no
rseq support whatsoever, so every umem process on musl unconditionally
took the manual-registration path and carried this bug on every single
invocation — it just only *manifested* on the (frequent, but not
100%-of-the-time) occasions a migration landed inside a critical section
window. That is exactly the shape of the original finding: concentrated in
`prodcons` (highest thread/cross-CPU churn of the four workloads) and the
largest size class (longest critical-section body, more allocator work
per invocation → larger the exposed window), intermittent, and never
reproduced on any glibc box because the code path was never even entered
there.

## 6. Fix

`umem_rseq.c`: define `UMEM_RSEQ_SIG 0x53053053` (matching the assembly's
`RSEQ_SIG`) and pass it as the 4th argument to all four `sys_rseq()`
call sites instead of `0`. Four-line diff, no behavior change on any
platform where the manual path isn't reached (i.e. every glibc >= 2.35
environment in the shootout — confirmed unaffected below).

```diff
- long ret = sys_rseq(&test_area, sizeof(test_area), 0, 0);
+ long ret = sys_rseq(&test_area, sizeof(test_area), 0, UMEM_RSEQ_SIG);
...
- sys_rseq(&test_area, sizeof(test_area), RSEQ_FLAG_UNREGISTER, 0);
+ sys_rseq(&test_area, sizeof(test_area), RSEQ_FLAG_UNREGISTER, UMEM_RSEQ_SIG);
...
- long ret = sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area), 0, 0);
+ long ret = sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area), 0, UMEM_RSEQ_SIG);
...
- sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area), RSEQ_FLAG_UNREGISTER, 0);
+ sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area), RSEQ_FLAG_UNREGISTER, UMEM_RSEQ_SIG);
```

## 7. A second, unrelated pre-existing bug found along the way

Running the full `test_main --no-fork` suite on this musl build (needed
to prove the rseq fix didn't regress anything) turned up a **separate**
crash, unrelated to rseq: `/options_backend_sbrk` SIGSEGV'd instead of
failing cleanly.

Root cause: `test/unit/umem_env_helper.c`'s warm-up (`void *warm =
umem_alloc(64, UMEM_DEFAULT); umem_free(warm, 64);`) assumed
`umem_alloc()` always succeeds. Under `UMEM_OPTIONS=backend=sbrk` on this
Alpine AMI's PIE/ASLR memory layout, `sbrk(2)` itself fails (`ENOMEM` —
confirmed directly: a standalone `sbrk(4096)` test program on the same
instance returns `-1`/`ENOMEM` past the initial brk segment, because the
heap segment here sits with no room to grow between other mmap'd
regions), so `vmem_sbrk_arena()` never gets a working heap and
`umem_alloc()` legitimately returns `NULL`. The helper then called
`umem_free(NULL, 64)` unconditionally. **`umem_free()` is not `free()`
— by design it does not tolerate a `NULL` pointer** (this matches
illumos umem's own `umem_free()` contract; `_umem_free()`'s first
statement dereferences the buftag unconditionally), so this reliably
SIGSEGVs.

This is **not** the bug this investigation was chartered to find (it's
in the test helper, not libumem itself, and it's about sbrk-backend
heap growth under a specific AMI's memory layout, nothing to do with
rseq or CPU migration) but it was silently taking down the entire
`test_main` run before a single other assertion could even print, so it
had to be fixed to get a real pass/fail count. Fix: guard the warm-up
free with a NULL check (`test/unit/umem_env_helper.c`). One line,
scoped to the test tool only — `_umem_free()`'s no-NULL-tolerance
contract in the library itself is correct/intentional and was not
touched.

## 8. Verification

All on the same Alpine instance, fix applied, full rebuild:

- **Exact repro point, 50 iterations:** `bench_main -a umem -w prodcons
  -t 8 -n 10000000 -s 1024:4096 -r 1 -W 0 -c` in a loop — **0/50
  failures** (previously crashed on the very first attempt pre-fix).
- **Heavier repro (5x ops, 20 iterations):** `-n 50000000` — **0/20
  failures**.
- **All 14 originally-crashing matrix points** (every `w`/`t`/`size`
  combination logged in the original `matrix.log`), 3 iterations each —
  **0/42 failures**.
- **`dmesg` after all of the above: no new "Possible attack attempt"
  lines** (only the two pre-fix ones from before the fix was applied
  remain in the log).
- **`test/bench/matrix.sh -s 1024:4096 -t 1,2,4,8 umem`** (the exact
  size class + full musl thread ladder) — completed, `matrix.log` has
  zero `CRASH:` lines.
- **`test/bench/matrix.sh --quick`** (libc + umem, quick sweep) — zero
  crashes.
- **`test/stress/stress_concurrency_oracle`** — `PASS (no cross-thread
  aliasing or corruption)`.
- **`test_main --no-fork` on musl, post-fix: 417 OK / 0 FAIL / 10
  SKIP** — identical to the documented glibc baseline (CHANGELOG.md).
  Pre-fix (with only the rseq fix, before the helper NULL-guard) this
  run crashed the whole harness on `/options_backend_sbrk`; both fixes
  together restore the full, correct count.
- **glibc non-regression (`intel-lo`, `c7i.2xlarge`, glibc 2.34,
  launched/bootstrapped/torn-down via the harness's own
  `scripts/ec2/{launch,bootstrap,run-remote,terminate}.sh`):**
  `test_main --no-fork` = **417 OK / 0 FAIL / 10 SKIP**, byte-for-byte
  the documented baseline, unchanged. Confirmed via the `UMEM_DEBUG=1`
  startup banner that this environment's rseq path is glibc's own
  (`glibc=1` — i.e. the buggy manual-registration code path is not even
  reached here, exactly as the root-cause analysis predicts): the fix
  is a no-op on every glibc environment in the whole shootout and could
  not have changed any of that report's glibc numbers.
- Confirmed the pre-existing (unfixed) source reproduces the sbrk-backend
  crash too, by temporarily restoring the original `umem_rseq.c` and
  rebuilding — i.e. neither of these two bugs was introduced by this
  investigation's own tooling changes.

## 9. Files changed

- `umem_rseq.c` — the actual fix (4 call sites, `sig=0` → `UMEM_RSEQ_SIG`).
- `test/unit/umem_env_helper.c` — NULL-guard on the warm-up free (test
  tool only, unrelated bug found while verifying test_main parity).

Commit: `8ee87cd` (`fix(musl): register rseq(2) with the signature the
asm abort points use`).

## 10. Instance cleanup

`i-0ae3c7cde76829744` (musl investigation instance, tag
`Role=musl-sigsegv-investigation`) and `i-0fb99c097682ce329` (`intel-lo`,
glibc non-regression check, tag `Role=intel-lo`) are both terminated;
confirmed via `aws ec2 describe-instances` (see the summary at the end of
this workstream — no untracked running instances left behind).

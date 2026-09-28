# O7: first allocation stack -- drop eager umem_stacktrace_init()

Worker `@r8stack`, 2026-09-28. Debian 13 (trixie) EC2, `intel-lo`/`arm-lo`
(c7i.2xlarge / c7g.2xlarge), gcc 14, `--enable-rseq CFLAGS="-O2 -g"` unless
noted, `verify-isolated.sh` from committed refs.

## Finding recap

The 2026-09-27 arm-dlopen-stack-crash doc ("Residual", section 12) left umem's
first allocation needing 5.2-5.4 KB of stack vs glibc's <=3.4 KB, and named
the eager `umem_stacktrace_init()` in `umem_init()` (~4.7-5.1 KB: backtrace
warm-up into libgcc_s + libdw dwfl setup) as the cost to remove, since it
serves only error reports and `umem_findleaks`, not the alloc path.

## Safety argument (confirmed by reading, not asserted)

The lazy warm was already built for; removing the eager call is a correct
degradation until first tool use:

- `resolver_tier` (umem_stacktrace.c:78) defaults to `RESOLVE_DLADDR` (0). Its
  only readers are in umem_stacktrace.c: `umem_stacktrace_init()` sets it,
  `umem_stacktrace_format()` reads it in a switch whose `default:` uses plain
  `dladdr`. Un-warmed => dladdr-only symbolization, correct.
- `umem_backtrace_warmed` (umem_stacktrace.c:143) starts 0. `getpcstack.c:472`
  *refuses* `backtrace()` until warmed, so not warming eagerly only makes the
  fast path keep using its cheaper walker.
- `umem_stacktrace_init()` is idempotent (resets `resolver_tier`, guards
  `backtrace_warm()` on `!umem_backtrace_warmed`).
- Every tool entry calls it on the request path: `umem_findleaks`
  (umem_inspect.c:1261), `umem_log_dump` (:1495), `umem_whatis` (:1870),
  `umem_walk` (:1957). So the resolver is warm by first tool use.

## Fix

`umem.c` `umem_init()`: delete `umem_stacktrace_init();`, replace with a
comment stating the invariant (commit 7be54be). Test 490a22c (was f649c03).

## Measurements (probe_first_alloc_stack.c, paint-and-scan, default thread)

| depth (B) | x86_64 pre | x86_64 post | arm64 pre | arm64 post |
|---|---:|---:|---:|---:|
| first `umem_alloc()` | 5200 | **5200 / 5136** | 5360 | **3568 / 3552** |
| `umem_stacktrace_init` alone | 4696 | 4696 | 5088 | 5088 |
| `dlopen(NULL)` | 3416 | 3416 | 3088 | 3088 |
| ncpus / numa | 3480 | 3480 | 1984 | 1984 |

(pre = a8c3e1a probe; post = 7be54be probe / 490a22c test cap-mode, two
methods, same result.)

**arm64: 5.36 KB -> 3.57 KB.** Now under 4 KiB.
**x86_64: unchanged (~5.2 KB).** O7 removed a frame that was not the x86 peak.
The x86 first-alloc floor is `dlopen(NULL)` + ncpus/numa + `umem_init`'s own
frames, not the stacktrace warm-up. Chasing it (moving `dlopen(NULL)`/numa off
the first-alloc path) is a separate follow-up, not done here.

## Regression (test_first_alloc_small_stack, in TESTS)

Enforced per-arch cap + arm64 4 KiB survival:

| arm | pre-fix | post-fix |
|---|---|---|
| tight 4 KiB budget survival (umem_alloc API) | SEGV rc 139 (both arches, f649c03) | x86 & arm PASS |
| arm64 depth cap 3800 B | 5360 > 3800 => would FAIL | 3552 <= 3800 PASS |
| x86_64 depth cap 5400 B | 5200 (standing floor) | 5136 <= 5400 PASS |
| glibc 4 KiB control | PASS | PASS |

Pre-fix demonstration (verify-isolated at f649c03, pre-fix library):
`make check TESTS=test_first_alloc_small_stack.sh` => **1 FAIL** both arches,
tight umem arm SEGV rc 139, glibc control PASS.

Post-fix (verify-isolated at 490a22c): `make check TESTS=...` => **1 PASS**
both arches, every arm PASS. Teeth-check `api cap:3000` prints FAIL rc 1 on
both arches (5136 / 3552 > 3000), so the cap assertion is enforced, not
cosmetic.

## Symbolization regression (the real risk)

`test_tool_T9_leak_stack.sh`, `--enable-introspect --enable-frame-pointers`
(x86) / `--enable-introspect` (arm), verify-isolated at 7be54be:
- x86: `leak records 442: first pc in exe 442, in libumem 0`, `T9 FIXED`.
- arm: `leak records 437: first pc in exe 437`, `T9 FIXED`.

The lazy warm produces symbolized (caller-in-target) stacks on the tool path;
dropping the eager warm did NOT break symbolization. Safe to ship.

## make check

`--enable-rseq -O2 -g`, `vm.max_map_count=65530`, verify-isolated at 490a22c:
arm64 and x86_64 both **0 FAIL** (post-fix; the pre-fix f649c03 x86 FAIL was
the new tight arm, now green).

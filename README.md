# libumem — slab + magazine allocator with built-in debugging

A portable port of the Solaris userspace slab allocator, modernized
and revived in 2024–2025.  Provides high-throughput, low-contention
memory allocation with first-class runtime debugging on Linux,
FreeBSD, and macOS.

> **Status (v3.2.0): the Linux heap ceiling is gone, the maintenance thread
> runs, the per-CPU caches are used, and the drop-in path scales; still not a
> hardened allocator in the sense a security-critical deployment would want.**
> Ten reachable correctness and lifetime defects found by the 2026-09-21
> design review, ten security findings from the 2026-09-22 adversarial audit,
> and the Phase 6 hard-limit findings are fixed — each with a regression that
> fails before the fix and passes after, on x86_64 and aarch64. The theme of
> this release is *things that were never running*: the update thread was
> never started in any process that had not failed an allocation, every thread
> used per-CPU cache slot 0, and the documented `UMEM_OPTIONS=abort` did not
> exist. Where a first diagnosis was wrong — the heap ceiling (fixed in two
> halves), the 1k:4k collapse (the CPU hint, not the per-thread cache) — the
> record says so. Several diagnostic features have contracts documented
> honestly rather than optimistically — including that `UMEM_DEBUG=audit`
> captures only ~2 frames in a default build — and performance conclusions
> previously published in this file were **withdrawn** and re-measured with a
> null control. Work,
> evidence, and exit criteria:
> [`docs/plans/2026-09-21-production-readiness.md`](docs/plans/2026-09-21-production-readiness.md).
> Claims below are qualified by what has actually been measured; where
> something is unknown, it says so.

```bash
./autogen.sh && ./configure && make -j"$(nproc)" && make check
sudo make install     # installs libumem.so, libumem_malloc.so, the umem(1)
                      # and umemctl tools, and gdb/lldb integrations
```

Drop-in malloc replacement:

```bash
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./myapp
```

> **Privileged use:** v3.0.0 and earlier must not be preloaded into setuid,
> setgid, or root processes. **v3.1.0 fixes the four issues behind that** — a
> `PATH`-resolved exec on the startup path, ungated `UMEM_OPTIONS`,
> symlink-following file writers, and a control socket that trusted the real uid.
> See "Security status" below for what is now hardened and what is still open.

Or link directly for full performance and access to the C API:

```bash
gcc myapp.c -lumem -o myapp
```

---

## Why libumem

Three things that no mainstream allocator does as well:

1. **Object caches** — type-stable allocation with constructor /
   destructor callbacks.  Reduce per-allocation work to ~one
   un-contended atomic on the fast path; reuse expensive
   initialization across allocation cycles.
2. **vmem arenas** — first-class virtual address management with
   quantum caching and hierarchical sub-arenas.  Useful for memory
   regions, NUMA placement, mmap pools, etc.
3. **Production-grade introspection** — alloc-site capture,
   allocation history ring buffer, per-cache statistics, leak
   detection, all queryable at runtime by `umem(1)`, `gdb`, or
   `lldb` without restarting the target. (Stack capture is shallow in a
   default build — see the audit-mode note under Debug modes.)

If you only ever call `malloc` / `free`, you do not need libumem.  If
you maintain a long-running daemon (database, MTA, application
server) that needs typed object pools, address-space management, or
post-mortem heap forensics, libumem is built for you.

---

## History

| Year | Milestone |
|------|-----------|
| **1994** | Jeff Bonwick publishes [*The Slab Allocator*](https://www.usenix.org/legacy/publications/library/proceedings/bos94/bonwick.html) at USENIX.  SunOS 5.4 ships the first slab allocator in production. |
| **2001** | Bonwick and Adams publish [*Magazines and vmem*](https://www.usenix.org/legacy/event/usenix01/full_papers/bonwick/bonwick.pdf) at USENIX.  The per-CPU magazine layer is added; vmem becomes a separate first-class subsystem. |
| **~2002** | Solaris 9 ships **libumem** as a userspace port of the kernel slab allocator.  Same algorithms, just running in process. |
| **2005** | Solaris 10 / OpenSolaris release libumem broadly; `mdb`'s `::findleaks` and `::umem_log` dcmds become the gold standard for userspace heap debugging. |
| **~2007** | Wez Furlong (Message Systems) ports libumem to Linux and Windows for the Ecelerity MTA.  Released as `portableumem`. |
| **~2010** | OmniTI takes over the portable fork.  Steady but minimal maintenance follows. |
| **2014–2023** | Long fallow period.  The original Solaris source is preserved in illumos-gate; the portable fork accumulates rot. |
| **2024–2025** | This revival.  Substantial rework of the allocator hot paths, modernization to C11 / C17, fixes for Linux, FreeBSD, and Windows portability, addition of new features (per-thread cache, scoped arenas, ownership tracking, profiling), and — the headline change — the runtime introspection tools that bring `mdb`-class debugging back. |

---

## What was done in the 2024–2025 revival

This fork is **not** a cosmetic refresh.  The substantive changes:

### Allocator core

- **Atomics modernized.**  All `__sync_*` builtins replaced with C11
  `<stdatomic.h>` operations.  Spin loops gained `_mm_pause` /
  `yield` hints.
- **Depot simplified.**  The lock-free striped array depot replaced
  with a straightforward mutex-protected list per the original
  Bonwick & Adams design (\~280 lines of complexity removed).
- **Per-CPU depot arrays** to eliminate cross-CPU contention on the
  cold path.
- **RSEQ fast path** wired into the x86_64/aarch64 allocation hot path
  (glibc 2.35+ or manual registration). Read the "Known limitation"
  below before counting on this: the assembly runs, but it serves **zero**
  magazine hits today, because nothing populates the per-CPU magazines it
  reads.
- **Per-Thread Cache (PTC)** — lock-free fast path for allocations
  up to 2 KB, generated as inline assembly per architecture.  Falls
  through to the magazine layer cleanly when sizes don't qualify.
- **Slab page reclaim** via `madvise(MADV_DONTNEED)` for idle slabs.
- **Cache-line auditing** of hot fields with `_Static_assert`
  verification.
- **NUMA-aware depot stealing** with statistics for local /
  same-node / cross-node hits. This is the `HAVE_LIBNUMA` topology code in
  `umem.c` (a CPU→node table consulted when the depot scans other CPUs'
  stripes) and it is live in a build configured with libnuma. It is not the
  same thing as the `umem_numa.[ch]` policy layer, most of which was
  removed on 2026-09-21 because it never ran — see `umem_numa.h`.

### Platform fixes

- **Linux:** RSEQ wiring, fixed `getpcstack()` (was a no-op on
  x86_64; stack capture under `UMEM_DEBUG=audit` now actually
  works).  Configure-time cache-line size detection.
- **FreeBSD:** W^X crash fixed (removed `PROT_EXEC` from heap),
  `MAP_ANON` portability, removed broken `_pthread_mutex_init_calloc_cb`
  constructor.
- **Illumos / SPARC:** GAS syntax for assembly, `__EXTENSIONS__` for
  headers, alloca / pcstack fixes, 48-bit VA check skipped on SPARC.
- **RISC-V:** TLS static block exhaustion fix, fallback dlopen paths.
- **Windows / MinGW:** Guarded mmap/munmap symbols, `gettimeofday`
  compat wrapper.
- **macOS:** `dladdr` and `backtrace` paths verified; debugger
  integrations validated under lldb.
- **Cross-compilation:** `configure.ac` fixed for aarch64 / riscv64
  cross builds via Nix.

### New features (some experimental — see below)

- **`umem(1)` runtime introspection CLI** — `findleaks`, `log`,
  `status`, `walk`, `whatis`, `bufctl`, `snapshot`, `break`.
  Restores the entire `mdb` workflow on non-Solaris platforms.
- **GDB and LLDB integrations** — same command set, exposed under
  the `umem` prefix.  Includes conditional breakpoints on
  alloc / free / corruption events.
- **Binary snapshot format (`.ums`)** — capture allocator state in
  production, analyze offline.
- **Stack-Based Objects (SBO)** — bump allocator and scoped arena
  for short-lived allocations.
- **Ownership tracking (`umem_own.h`)** — Rust-inspired
  ownership / borrowing with runtime checks.
- **Allocation profiling (`umem_profile.h`)** — record / replay to
  pre-warm caches.
- **Budget contexts** — PostgreSQL-style per-context memory budgets.

### Quality

- Test coverage: 71.2% line coverage (core sources, excluding the
  vendored third-party `sm.c`/`sm.h` sparsemap dependency) as of
  2026-09-06 at commit `ecd5fa0`; 50.3% across the whole repo
  including `sm.c`. Re-measured against current code — see
  `docs/results/2026-09-06-coverage-verification.md` for the full
  breakdown and how to reproduce (`scripts/generate-coverage.sh` or
  `scripts/ec2/run-remote.sh <role> '... lcov ...'`). This replaces
  the historical "~33% to >80%" v2.0.0 claim, which described a
  2025 point-in-time delta and is no longer accurate for the current
  codebase (multiple releases of new code — sparsemap vendoring,
  `umem_introspect.c`, `tools/umem.c`, `umem_inspect.c` — have
  shifted the aggregate since then).
- Property-based tests, integration tests, stress tests. Note that
  `make check` runs only an 8-entry smoke suite; the broader suites are
  separate targets (see "Testing" below).
- Cross-platform benchmark suite (TOML output with OS / arch /
  compiler metadata). Two harness defects found on 2026-09-21 affect
  previously published numbers — see P2.1/P2.2 in
  [the readiness plan](docs/plans/2026-09-21-production-readiness.md).
- Forgejo Actions CI (build × asan / ubsan / coverage; lint;
  tagged-release pipeline), Linux x86_64 only.
- Removed \~2,500 lines of stale code and several documentation
  artifacts that were lying about features that didn't actually
  work. That housekeeping is not finished: the 2026-09-21 review found
  more of the same, and this README was part of it.

---

## How libumem compares to other allocators

| Property | **libumem** | jemalloc | tcmalloc | mimalloc | glibc ptmalloc |
|---|---|---|---|---|---|
| Slab / object cache API | ✅ first-class | ❌ | ❌ | ❌ | ❌ |
| Constructor / destructor caches | ✅ | ❌ | ❌ | ❌ | ❌ |
| vmem virtual address arenas | ✅ | partial (extents) | ❌ | partial | ❌ |
| Per-CPU magazines | ✅ | ✅ (tcache) | ✅ (thread cache) | ✅ (heap) | partial |
| RSEQ fast path | ⚠️ wired, zero hits (see below) | ❌ | ✅ | ❌ | ❌ |
| Lock-free per-thread cache | ✅ (PTC) | ✅ (tcache) | ✅ | ✅ | ❌ |
| Built-in leak detection | ⚠️ outstanding-allocation report (`::findleaks`) | profile-based | ❌ | profile-based | ❌ |
| Allocation history ring buffer | ✅ | ❌ | ❌ | ❌ | ❌ |
| Live attach for inspection | ✅ (`umem --pid`) | runtime stats only | runtime stats only | runtime stats only | ❌ |
| Per-buffer stack capture | ✅ (`UMEM_DEBUG=audit`) | profile mode | sampling profile | ❌ | mtrace |
| Buffer overrun / UAF detect | ✅ (`UMEM_DEBUG=guards`) | partial | ❌ | ✅ (secure) | ❌ |
| Snapshot / offline analysis | ✅ (`.ums` format) | ❌ | profile heap dump | ❌ | ❌ |
| Cross-platform | Linux/BSD/Solaris/macOS | wide | Linux primary | wide | Linux only |
| Drop-in `LD_PRELOAD` | ✅ | ✅ | ✅ | ✅ | (default) |

Where libumem **does not win**:

- **Heap size on Linux: a ~5 GB ceiling — now fixed, in two halves.** Until
  this release libumem hit `vm.max_map_count` (default **65530**) at roughly
  5 GB and `umem_alloc()` returned NULL; on the same box glibc reached 96 GB.
  The cause was slab density, not the mmap backend (three attempts there failed
  and are documented): under Linux's 4 KiB heap quantum libumem built far
  smaller slabs than under Solaris's 64 KiB one, and each slab span is its own
  `mmap(MAP_FIXED)` the kernel does not merge. Two floors restore Solaris
  density — `UMEM_MIN_SLAB_OBJECTS` for the hashed best-fit path (4 KiB objects:
  16,283 → 75 VMAs at 2 GB) and `UMEM_MIN_QCACHE_SLAB` for the quantum-cache
  path that objects ≤ 512 B take (15,702 → 274 VMAs at 2 GB, zero measured RSS
  cost). The first was shipped as a complete fix and was not; the second came
  from a hard-limit hunt that tested the class the first fix's own regression
  did not. Both arms are now gated (`test_heap_ceiling`, `..._512.sh`), and a
  64 KiB-quantum simulation (`test_slab_floor`) checks both floors are no-ops on
  illumos. Details:
  [`docs/results/2026-09-22-umem-heap-ceiling-vma.md`](docs/results/2026-09-22-umem-heap-ceiling-vma.md).

  If you run an older release, raise the limit:

  ```sh
  sysctl -w vm.max_map_count=1048576     # or a value suited to your heap
  ```
- **Raw malloc / free throughput.** Through the `umem_alloc` API, libumem
  is at or above glibc single-threaded and within 3-13 % of the fastest
  allocator (mimalloc, usually); at 128-192 threads on x86_64 it is 8-24 %
  behind the best. **Through `LD_PRELOAD` before `a74065e` it was 2x slower
  at one thread and 500x slower at 192** -- a global mutex on every
  `free()`, now fixed (P8.1). Objects above 2 KB collapsed to 0.06x glibc
  under threads (P8.2) -- the cause turned out to be that **every thread was
  using the same per-CPU cache** (the hint was `pthread_self() & mask`, always
  0), now fixed; 2560 B at 8 threads went 1.4 -> 16.4 Mops (glibc 22.8). The
  comparison below predates both fixes. Measured with a null control in
  [`docs/results/2026-09-23-allocator-comparison.md`](docs/results/2026-09-23-allocator-comparison.md).
- **Memory overhead.** Measured 2026-09-23 with the repaired pair
  (RSS at the live-set peak / live bytes at that instant, plus `VmHWM`):
  libumem holds **1.55-1.6x** its live set at 64-1024 B objects and **2.6x**
  at 16-63 B, where glibc holds 1.07-1.25x / 1.85x and jemalloc/mimalloc
  1.1-1.2x / 1.65x. scudo lands where umem does. This is the size-class +
  warm-cache trade and is accepted; the earlier "worst-in-field 2.3x" and
  "fixed to 2.7" claims both remain withdrawn (they measured a different,
  wrong quantity). Under `frag` churn umem is also 20-33 % slower than the
  size-class allocators and 2-3x slower sustained (P8.5, open).
- **Tail latency.** Cross-thread handoff (`prodcons`) p999 is umem's
  strongest number at 8 threads -- 0.9 us sustained, second only to
  rpmalloc, 5-13x better than glibc. At 192 threads sustained it is
  240-270 us (x86_64 metal) against 5-25 us for jemalloc/mimalloc/rpmalloc
  and 83 us for glibc: not the tens-of-microseconds tier. Under `frag`
  churn the p999 is 22 us at 8 threads and **6-10 ms at 192**, against
  0.3 us-0.7 ms for every other allocator (P8.5). The old attribution of a
  residual tail gap to the inert rseq reload path was a hypothesis and is
  dropped: the rseq layer serves zero hits either way.
- **Sandboxed / security-hardened allocations.**  mimalloc-secure
  and `scudo` add explicit hardening (segregated metadata, randomized
  freelists, double-free detection by design).  libumem's defenses
  are opt-in via `UMEM_DEBUG=guards`.

**Known limitation — the rseq per-CPU layer serves no allocations:**
the rseq fast-path assembly is registered and executes on every qualifying
alloc/free on x86_64 and aarch64. It nonetheless satisfies **zero**
allocations, because `cache_rseq[cpu].rounds` is permanently 0: the only
functions that would populate a per-CPU magazine are the reload paths, and
nothing calls them. Entering the code is not the same as the code doing its
job — so "rseq's benefit is limited to fastpath hits" would be too generous.
There are no hits. The benefit today is zero, and the cost is the
fast-path check.

The reload is unimplemented pending migration-safe per-CPU-commit assembly
on both architectures; a plain-C reload races the lock-free fastpath across
a CPU migration (measured ~42-47% double-issue rate under contention). See
[`docs/results/2026-09-09-rseq-reload-analysis-v2.md`](docs/results/2026-09-09-rseq-reload-analysis-v2.md)
for the re-evaluation and
[`docs/results/2026-09-09-rseq-reload-asm-design.md`](docs/results/2026-09-09-rseq-reload-asm-design.md)
for the implementation spec — which the 2026-09-21 review judged unsafe as
written, so the spec is a starting point, not an approved design. This does
not affect correctness.

Where libumem **wins decisively**:

- **Object pools with non-trivial init / teardown.**  No other
  mainstream allocator gives you `umem_cache_create` with ctor /
  dtor.  Manual reimplementations are easy to get subtly wrong.
- **Long-running services with episodic leaks.**  `umem --pid
  $(pgrep mydaemon) findleaks` against an unmodified production
  process is a workflow no other allocator supports.
- **Forensics without re-running the workload.** Snapshot a live process
  (`umem_inspect_snapshot()`, or `umem --pid ... snapshot`) and analyze the
  `.ums` file offline, anywhere, with no target process required. Note that
  `umem --core` does **not** work and is refused — see the Debugging section.
- **Embedded address-space management.**  vmem hierarchies handle
  use cases (DMA pools, NUMA-bound allocations, custom
  page-replacement) where you'd otherwise hand-roll.

If your workload is closer to "billions of small mallocs in a tight
loop" than to "long-running server with object lifecycles", pick
jemalloc or mimalloc.  If it's the other way around, pick libumem.

---

## Security status

**Audited 2026-09-22 against v3.0.0; hardened in v3.1.0.**

The audit found one critical and three high-severity issues, all now fixed, each
with a regression that demonstrates the pre-fix exposure:

| Was | Now |
|---|---|
| `execlp("addr2line")` ran on the unconditional `umem_init()` path, resolving through `PATH`, gated only by an ungated `getenv` — arbitrary code execution as the elevated user in a setuid binary *linked* against libumem, before `main()` | **Deleted.** It never worked anyway: `-e /proc/self/exe` names *addr2line itself* after exec, verified to resolve nothing. `dladdr` and `libdw` remain |
| No `issetugid`/`AT_SECURE` gating on option parsing at all, so a hostile environment could make a privileged process create and truncate files | `umem_secure_mode()` consulted before parsing; options with file, socket, or exec side effects ignored in secure mode, tuning options unaffected |
| No `O_EXCL` or `O_NOFOLLOW` in any library writer — snapshot and profile paths followed symlinks (a victim file went 54 → 5472 bytes) | One `umem_open_write()`: `O_NOFOLLOW`, `S_ISREG`, single-link, euid-owned, 0600, truncate only after the checks |
| Freelist links stored **inside freed user buffers**, unmangled — a one-buffer overflow yielded an arbitrary-address allocation | Links mangled with an `AT_RANDOM` cookie and the slot address, plus alignment and slab-containment validation that reports rather than dereferences |
| Control socket at `/tmp/umem.<pid>.sock`, reclaimed via `stat` — which follows symlinks, so it could unlink another process's socket | euid-private directory, `lstat`, and a bind-then-`rename()` reclaim that removes nothing it did not create |
| `SO_PEERCRED` accepted the **real** uid, handing a setuid target's unprivileged invoker full control including a thread-parking DoS | `geteuid()` or root only |
| Interposed `free()` wrote to a foreign pointer's header *before* validating it, and the magic is forgeable | Ownership range checked before the header read and after decode; all mutation after acceptance |
| `getpcstack()` walked frames with no stack bounds — arbitrary reads under `UMEM_DEBUG=audit`, SIGSEGV demonstrated | Real stack bounds consulted; the 16 MiB heuristic is a documented fallback only |

### What is still open

- **One evidence gap in the freelist fix — now closed.** Isolating the two
  controls showed the *containment check* blocks the original test's attack by
  itself, because that test's target is outside the victim slab. A new `inslab`
  case targets the *live neighbour* — inside the slab, aligned, so only the
  mangling stands in the way — and fails with `-DUMEM_NO_LINK_MANGLE` (the
  allocator hands back a still-allocated buffer) while passing by default. Both
  controls are now independently demonstrated:
  [`docs/results/2026-09-23-p54-which-control-blocks.md`](docs/results/2026-09-23-p54-which-control-blocks.md).
- **`umem_may_own()` is a convex hull**, so a forged header landing *between*
  heap spans passes the range check and, for sizes under 128 KiB, the pointer
  goes onto a per-thread free list unvalidated and is later returned by
  `malloc()`. More than glibc does (nothing), less than jemalloc's exact rtree
  or scudo's checksummed header. Closing it means either an unforgeable header
  or an exact ownership structure cheap enough for every `free()`; both are
  projects, not lines. Characterised in the plan (P7.4).
- **`umem_abort = 0`** remains the interpose-mode default, which logs and
  continues where glibc aborts. Defensible now that a rejected pointer leaves
  state untouched. `UMEM_OPTIONS=abort` restores aborting — **this option did
  not exist until now**; it was documented as `abort=1` and nothing implemented
  it, so the escape hatch users were told about was a no-op. Regression:
  `test/security/test_abort_option.sh`.
- **Leading-component symlinks** in output paths are not defended: the final
  component is opened `O_NOFOLLOW`, the directory path is not walked. Same
  boundary as `O_NOFOLLOW` itself; in secure mode these options are ignored
  entirely, which is where it would matter.

### Honest framing

This was a hardening pass, not a security proof. What it establishes is that the
specific exposures found by one adversarial audit are closed and stay closed
under test. It does not establish that none remain — and libumem is a
`malloc` replacement, so the surface is broad. If you are deploying it somewhere
hostile, read
[the plan's Phase 5](docs/plans/2026-09-21-production-readiness.md) for the
threat positions actually considered.

## Platform support

"Status" here means *what has been demonstrated*, not what is expected to
work. It was previously a column of "Production" labels with no recorded
evidence behind most of them; the 2026-09-21 review also found reachable
correctness defects in default paths on the best-covered platform, so no
row can currently claim production readiness.

| Platform | Architecture | Evidence | Status |
|---|---|---|---|
| Linux | x86_64 | CI on every push (normal, ASan, UBSan, gcov); benchmarks on EC2 `c7i` and `c7i.metal-48xl`; all Phase 1 regressions run here; 2026-09-21 build-option matrix ([log](docs/results/2026-09-21-phase4-verification.log)) | Best covered. Not production-ready — see below |
| Linux | aarch64 | Manual EC2 runs on `c7g`/`c8g.metal-48xl` (build, `make check`, `test_main`, benchmarks, and the 2026-09-21 build-option matrix: [log](docs/results/2026-09-21-phase4-verification-aarch64.log)). The nightly CI job exists and is validated but is **not armed** (repo secrets never added) | Builds and tests pass when run by hand; unattended coverage absent |
| Linux | riscv64 | Cross-build via Nix + QEMU only | Cross-compiles; no hardware validation |
| FreeBSD | amd64 | Ported, W^X and `MAP_ANON` fixes verified at the time | No CI, no recent run recorded |
| illumos | x86_64 | Manual `m4.xlarge` run (build, `LD_PRELOAD` smoke, benchmark matrix) | Manually validated at one point in time |
| illumos | SPARCv9 | Source-level support (GAS syntax, `__EXTENSIONS__`, alloca/pcstack) | Compiles; no recorded run on hardware |
| macOS | x86_64, arm64 | `dladdr`/`backtrace` paths and lldb integration exercised by hand | Tested, not continuously |
| Windows | x64 (MSVC, MinGW) | Guarded symbols, compat wrappers | Experimental |

**Not production-ready at this commit, on any platform.** The 2026-09-21
design review found reachable correctness and lifetime defects in *default*
code paths — not only in experimental features — plus measurement defects
that invalidate several previously published performance conclusions. Work
is tracked in
[`docs/plans/2026-09-21-production-readiness.md`](docs/plans/2026-09-21-production-readiness.md);
that plan's exit criteria are what "production" will mean here.

CI (Forgejo Actions, see `.forgejo/workflows/`) covers Linux x86_64 in
normal, AddressSanitizer, UndefinedBehaviorSanitizer, and gcov modes. That
is the only architecture with unattended coverage.

### illumos / Solaris x86 notes

The build wires the x86 `getfp`/`_breakpoint` helpers (via a portable C
compat unit) into the SOLARIS x86 branch, links the introspection control
channel against `libsocket`/`libnsl`, and emits the historic `.so.1`
SONAME — so a stock `./configure && make && make install` produces a
working allocator and `LD_PRELOAD=<prefix>/lib/libumem_malloc.so.1` resolves
as expected, with no out-of-tree patches.

**Dual-ABI (32-bit + 64-bit) install for `LD_PRELOAD` into 32-bit base-OS
binaries.** The build produces a single ABI (whatever the compiler
defaults to). To interpose on both 32- and 64-bit processes (illumos base
tools like `/usr/bin/true` are often 32-bit), build both ABIs in separate
trees and merge them so the runtime linker's `$ISALIST`/`/64` token
expansion picks the matching ELF class per process:

```sh
# 64-bit -> lib/64 (amd64)
./configure --libdir=/usr/lib/64 CC="gcc -m64"
make && make install
make distclean

# 32-bit -> lib
./configure --libdir=/usr/lib CC="gcc -m32"
make && make install
```

A single `LD_PRELOAD=/usr/lib/libumem_malloc.so.1` then resolves to the
32-bit lib for 32-bit targets and, via the `/64` path token, to
`/usr/lib/64/libumem_malloc.so.1` for 64-bit targets.

---

## Build options

The default build is deliberately portable and conservative. Every option
below either works or does not exist — there is no flag that turns on broken
behaviour (Phase 4 of the readiness plan).

| Option | Default | Effect |
|---|---|---|
| `--enable-rseq[=auto]` | auto (on where `linux/rseq.h` exists) | Builds the rseq fast-path assembly. Currently serves **zero** magazine hits — see "Known limitation" above. |
| `--enable-numa[=auto]` | auto (on where libnuma exists) | NUMA **topology queries** (node count, CPU→node map, distances) plus the depot's cross-CPU locality accounting. Not a NUMA allocation policy; see `umem_numa.h`. |
| `--enable-introspect` | off | The in-process `umemctl` control channel. Zero hot-path cost when off. |
| `--enable-avx2` | **off** | `-mavx2` for the whole library. **Not generic**: the result requires an AVX2-capable CPU (Haswell / Excavator or newer) and will `SIGILL` on older x86-64. x86_64 only; refused elsewhere. |
| `--enable-asan` / `--enable-ubsan` / `--enable-tsan` | off | Sanitizer builds. |
| `--enable-coverage` | off | gcov/lcov instrumentation; adds the `coverage` target. |
| `--enable-pgo=generate\|use` | off | Profile-guided optimization. |

The default x86-64 build targets the SSE2 baseline, which every x86-64 CPU
has, so it runs anywhere. Configure used to promote "the compiler accepts
`-mavx2`" into a global `-mavx2`, which silently made a nominally generic
build illegal on pre-Haswell hardware; that is now opt-in and loud. There is
no runtime ISA dispatch.

Removed options: `--enable-percpu-caching` and `--enable-htm` (neither ever
worked — see `attic/README.md`), and `UMEM_OPTIONS=numa` (never registered).

---

## Stable features

### Core allocation

```c
#include <umem.h>

void *p = umem_alloc(1024, UMEM_DEFAULT);
umem_free(p, 1024);

void *z = umem_zalloc(1024, UMEM_DEFAULT);
umem_free(z, 1024);
```

### Object caches

```c
umem_cache_t *c = umem_cache_create("objects",
    sizeof(obj_t), 0, ctor, dtor, NULL, NULL, NULL, 0);
obj_t *o = umem_cache_alloc(c, UMEM_DEFAULT);
umem_cache_free(c, o);
umem_cache_destroy(c);
```

### vmem arenas

Quantum-cached, hierarchical virtual address management.  See
`umem_cache_create(3)` and `examples/`.

### Debug modes

Controlled via environment variable; no recompile.

| Mode | Variable | Measured overhead | Detects |
|------|----------|-------------------|---------|
| Lite | `UMEM_DEBUG=lite` | 28% (1.4× p99) | Cheaper subset of guards |
| Guards | `UMEM_DEBUG=guards` | 32% (1.5× p99) | Buffer overruns, use-after-free |
| Audit | `UMEM_DEBUG=audit` | 58% (2.4× p99) | Per-buffer alloc / free stack capture — **~2 frames deep in a default build** (see below) |
| Contents | `UMEM_DEBUG=contents` | not measured | Buffer contents logging (needs `audit`) |
| Default | `UMEM_DEBUG=default` | 60% (2.4× p99) | All of the above |
| Firewall | `UMEM_DEBUG=firewall` | not measured | Guard page per allocation (≥ `minfirewall`) |
| Logging | `UMEM_LOGGING=transaction=1m` | not measured | Chronological transaction log |

Measured 2026-09-21 on `c7i.2xlarge` (x86_64) with
`test/bench/bench_debug_overhead`: 1M single-threaded 64-byte alloc/free
cycles, one run per mode — order-of-magnitude guidance, not a median-of-N
result, and not measured on aarch64
([log](docs/results/2026-09-21-debug-mode-overhead-x86_64.log)). The two
"not measured" rows are honest: that benchmark's 64-byte allocations are
below the default firewall threshold and do not engage `contents` either, so
it reports ~0% for both, which is a property of the benchmark and not of the
modes. Earlier revisions of this table published ~10%/~30%/~50%/~5% figures
with no recorded measurement behind them.

**Audit-mode stack depth is shallow by default.** Recorded stacks are only about
**two frames** deep in a normal build, and that is a property of how *libumem*
is compiled, not of your application: the library is built `-O2` without
`-fno-omit-frame-pointer`, so `getpcstack()` begins its walk inside
`umem_alloc()` and stops at the first frame-pointer-less allocator frame.
Measured depth is 2 through `umem_alloc()` versus 7 when `getpcstack()` is
called directly from a caller that keeps frame pointers — so rebuilding *your*
application with `-fno-omit-frame-pointer` does not fix it. Build libumem with
frame pointers (`--enable-asan` does this as a side effect, or add the flag to
`CFLAGS`) for deeper capture; `libdw`-based capture, where available, is not
affected. Details: P5.9 in
[the readiness plan](docs/plans/2026-09-21-production-readiness.md).

### Per-Thread Cache (PTC)

Lock-free fast path for allocations up to 2 KB.  Default on.  Tune via
`UMEM_OPTIONS=perthread_cache=2m` or disable with
`perthread_cache=0`.

### Stack-Based allocation (SBO)

Bump allocator and scoped arenas for temporary allocations that
auto-clean on scope exit.

---

## Experimental features

Headers require `#define UMEM_ENABLE_EXPERIMENTAL`.  Active development;
APIs may change.

**These are diagnostic aids, not guarantees.** None of them is a
memory-safety mechanism, a budget-enforcement mechanism, or a monitoring
guarantee, and none should be relied on as one. The 2026-09-21 review found
that ownership tracking can itself corrupt memory and that budgets do not
actually enforce. Each header repeats the specific limitation.

- **Ownership tracking (`umem_own.h`)** — Rust-inspired ownership /
  borrowing with runtime checks.  Two modes: lightweight (~2%) and
  full (~15%).  *Not* a use-after-free defense: violations are reported
  best-effort, and the tracking itself can corrupt memory.
- **Allocation profiling (`umem_profile.h`)** — record / replay,
  phase detection.  Sampling-based and lossy; not an audit trail.
- **Budget contexts (`examples/umem_palloc.h`)** — PostgreSQL-style
  per-context memory management.  The budget is *accounting*, not a limit:
  allocations are not reliably refused when it is exhausted.

---

## Performance

Measured on EC2 through `scripts/ec2/verify-isolated.sh` (committed content
only), never locally, with a fixed total work budget per point, allocators
alternating at the innermost loop, and **a null control at every grid point**:
libumem is run against a relabelled copy of itself so the rig's own resolution
is known before any cross-allocator delta is read. A delta inside that band is
reported as noise. Two full runs against nine allocators (glibc 2.34,
jemalloc 5.2.1, tcmalloc 2.9.1, mimalloc 3.x, snmalloc, scudo, rpmalloc) on
four boxes (`c7i.2xlarge`, `c7g.2xlarge`, `c7i.metal-48xl`, `c8g.metal-48xl`):
[`2026-09-23`](docs/results/2026-09-23-allocator-comparison.md) at `v3.1.0`'s
allocator, with the method and the null analysis, and
[`2026-09-24`](docs/results/2026-09-24-allocator-comparison.md) at `d6f04ab`
after the fixes below. Numbers here are from the second run unless marked.

### Two libumem paths

Every earlier comparison in this repository measured libumem through the
`umem_alloc()` API. Since 2026-09-23 the `LD_PRELOAD=libumem_malloc.so`
drop-in path is measured alongside it, and they were not the same thing:

| `c7i.metal-48xl`, 192 vCPU, `multi` 16:64 | Mops at 1 thread | at 192 threads |
|---|---|---|
| glibc 2.34 malloc | 6.2 | 487 |
| best competitor (mimalloc 3.x) | 7.4 | 583 |
| **libumem, `umem_alloc` API** | 7.7 | **529** |
| libumem, `LD_PRELOAD` at `v3.1.0` (previous run, 20M ops) | 3.0 | **0.82** |
| **libumem, `LD_PRELOAD` now** | 6.7 | **393** |

The `v3.1.0` interposer took a **process-global mutex on every `free()`**
(`is_libc_pointer()`: lock + 512-slot scan of a table that is empty for the
steady-state life of every process), so the drop-in path scaled *negatively*
with threads. Fixed in `a74065e`. The residual -- preload at 0.74-0.91x of the
API, flat across thread counts on every box -- is per-call validation
(`process_free()` header decode and two ownership checks) and is tracked as
P8.3. If you use libumem as a drop-in malloc, you need `a74065e` or later.

### What the API path does, against the field (null-controlled)

- **Single-thread: at or above glibc everywhere** (0.97-1.24x, all boxes, all
  size ranges), within 3-13 % of the best allocator at each point. Not the
  fastest anywhere; never the slowest.
- **`multi` scaling to 192 threads at 16..1024 B is within the null of the
  best allocator on both metals.** x86: umem 529 vs null 528 vs glibc 487 vs
  mimalloc 583 at t=192; arm: 602 vs 561 vs 597 vs 612. The 2026-09-23 run
  had umem 8-24 % behind on x86 metal at 128-192 threads (P8.4); that was the
  CPU-hint bug below acting on per-thread-cache *misses*, and it closed with
  it.
- **`multi` at 1k:4k object sizes: the collapse is fixed up to 64 threads;
  a second cliff remains above.** Before: 0.06-0.10x glibc at 64+ threads on
  both metals. The cause was not what the first diagnosis said (objects above
  2048 B bypassing the per-thread cache -- raising `tcache_max` did not move
  it): the per-thread CPU hint was `pthread_self()` cast to `int`, a
  page-aligned address, so `hint & cache_cpu_mask` was **0 for every thread**
  and the whole process shared one `cc_lock` for every operation that reached
  the magazine layer. Solaris uses `thr_self()`, a small integer; this port
  never had a working hint. Fixed (`ae86536`): 4-18x on both metals up to
  t=64 (x86 t=32: 7.5 -> 98 Mops, glibc 117; arm t=64: 15 -> 277, glibc 255).
  **From t=128 throughput falls again** (x86 105 -> 91, arm 277 -> 77, while
  glibc goes 165 -> 253 / 255 -> 200) with p999 11 us against glibc's 0.4:
  these sizes still bypass the per-thread cache, so 128 CPUs each take a
  blocking depot round trip every 31 operations, and the depot convoys. That
  is the half of the original diagnosis the hint bug was hiding, and its fix
  (per-thread classes through 8 KB, 63-round magazines for 2-8 KB) is P8.2b.
- **`frag` (grow a live set, free half at random, repeat): level with glibc,
  20-40 % behind the size-class allocators at 16..1024 B** at every thread
  count including one. **Sustained at 192 threads it is still the slowest in
  the field, by 2-3x -- down from 8-19x.** x86: 2.0 -> 4.8 Mops (glibc 14.8,
  jemalloc 16.6), p999 6.0 -> 1.0 ms; arm: 1.2 -> 10.8 (glibc 22.1), p999
  9.9 -> 0.34 ms. `perf` at HEAD: **59 % of all cycles in
  `pthread_mutex_trylock`**, and the depot counters say why -- 95 % of
  magazine reloads steal from another CPU's stripe, ~5 failed trylocks per
  success. `frag` frees on a different thread than it allocates, so the local
  stripe is always empty and the neighbour scan runs on every reload (P8.5).
- **`prodcons` is unchanged and within null on every box** (it exercises none
  of the fixed paths). Its p999 at 8 threads, 0.9 us sustained, remains umem's
  best number, second only to rpmalloc. `prodcons` *throughput* on 8-vCPU
  boxes is bimodal (the null control reached +244 %) and is not reported.
- **Memory: umem holds ~1.5x its live set at 64..256 B and 2.6x at 16..63 B;
  glibc 1.25x / 1.85x**, jemalloc/mimalloc 1.2x / 1.65x, scudo 1.5x / 2.6x.
  At 192 threads the ratio came down from 3.3-3.8 to 3.0 with the per-thread
  cache packing; at 8 threads it is unchanged. About a third of the 16-63 B
  overhead is the size header pushing requests one class up; the rest is warm
  slab/magazine retention -- the slab-allocator trade, accepted as such.
- **The heap-ceiling probe runs to completion.** An 8-11 GB live set of
  1-4 KiB objects -- the point that crashed (rc=143) or failed 39 % of
  allocations in every previous run -- completes with **zero failures on
  every box**, at the default `vm.max_map_count`. On `c7i.2xlarge` umem does
  it at 2x glibc's throughput (10.4 vs 4.9 Mops); on x86 metal at 192 threads
  at 0.67x, which is the P8.2b cliff at these sizes.

Every one of these has a table with the null control beside it in the results
documents, and every open gap has a task with the mechanism and a fix
approach in Phase 8 of
[the readiness plan](docs/plans/2026-09-21-production-readiness.md).

### Provenance of older numbers on this page

- The **2026-09-08 8-allocator shootout**
  ([`docs/results/2026-09-08-allocator-shootout.md`](docs/results/2026-09-08-allocator-shootout.md))
  measured only the API path. Its 8-vCPU and single-thread API findings are
  consistent with the 2026-09-23 run; its "holds its own against modern
  allocators on 8-vCPU boxes" was **not true of the drop-in path**, which it
  never measured. Its 192-thread scaling and fragmentation findings remain
  withdrawn (P2.1/P2.2). The illumos-lineage result (4x its own libc under
  concurrency) is unaffected and stands.
- The **2026-07-23 / 2026-09-08 umem-vs-glibc baselines**
  ([x86_64](docs/results/2026-07-23-baseline.md),
  [aarch64](docs/results/2026-09-08-aarch64-baseline.md)) are superseded by
  the 2026-09-23 run for every point they cover; their 192-thread rows carry
  the double-divided budget and were withdrawn.
- The **sustained p999 157 -> 84-93 us** figure for the depot trylock fix
  (x86_64 metal, budget defect present) is superseded by the per-window,
  matched-work sustained tables in the 2026-09-23 document.

Numbers vary with workload and hardware; reproduce with
`scripts/ec2/allocator_comparison.sh` on your own target rather than trusting
a table.

---

## Debugging

libumem ships runtime introspection equivalent to Solaris `mdb`'s
`::findleaks`, `::umem_log`, and friends, via **two complementary tools**:

- **`umem(1)`** — gdb/ptrace-driven, point-in-time. Works against a live
  pid or an offline `.ums` snapshot; emits text or JSON.
  Non-invasive (no in-process thread). Best for CI, post-mortem, and
  scripted leak-finding.

  **`umem --core` does not work, and now says so.** It is refused with exit
  status 2 and an explanation. It previously exited 0 while printing nothing,
  which reads as "no leaks". Verified 2026-09-21 against a control:
  the same command on a live process reported 200 outstanding buffers; on
  that process's own core it produced zero bytes
  ([log](docs/results/2026-09-21-core-mode-produces-no-report.log)). The
  cause is structural — every command is executed by calling
  `umem_inspect(3)` entry points *inside the target process*, and a core has
  no process to call into. A passive core reader is not implemented. Use the
  snapshot workflow instead.

  A zero exit from `umem` now means a report was produced: debugger failures
  (cannot attach, nonzero exit, killed, no output) exit 2 and pass gdb's
  stderr through, instead of being reported as a clean empty run.
- **`umemctl`** — an opt-in in-process channel (`--enable-introspect` +
  `UMEM_OPTIONS=introspect=1`) for the live/interactive things a ptrace
  snapshot cannot do: **streaming** event logs (`logtail`), a live TUI
  (`monitor`), and `record` + **break-before-a-leaked-allocation**
  (stop the allocating thread so a debugger catches the exact stack).
  Zero hot-path cost when off. See [`docs/UMEMCTL.md`](docs/UMEMCTL.md).

```bash
# umem(1): live pid / offline snapshot / CI leak-finding
umem --pid $(pgrep myapp) findleaks
umem --pid $(pgrep myapp) findleaks -f json | jq .
umem --pid $(pgrep myapp) status
umem --pid $(pgrep myapp) snapshot /tmp/state.ums   # capture while alive
umem --dump /tmp/state.ums findleaks               # analyze offline
#   NOT: umem --core ...  (refused with exit 2 -- see above)

# umemctl: live streaming + interactive break-on-leak
#   (built with --enable-introspect; target run with UMEM_OPTIONS=introspect=1)
umemctl $(pgrep myapp) logtail                 # stream alloc/slab/reap events
umemctl $(pgrep myapp) monitor                 # live TUI
umemctl $(pgrep myapp) record --learn-leaks leaks.set   # phase 1
umemctl $(pgrep myapp) break leaked --set leaks.set     # phase 2, then attach gdb

# GDB / LLDB (same umem(1) commands, in-debugger)
(gdb) source /usr/share/umem/debugger/gdb/umem_gdb.py
(gdb) umem findleaks
(gdb) umem break alloc -s 1048576           # break on >=1 MB allocs
```

Detailed walkthrough: [tools/DEBUGGING.md](tools/DEBUGGING.md) and
[docs/UMEMCTL.md](docs/UMEMCTL.md).
Man pages: `umem(1)`, `umem_inspect(3)`, `umem_debugging(7)`.

---

## Building with Nix

```bash
nix develop                 # dev shell with toolchain
nix build                   # native build (does NOT run tests -- see below)
nix build .#libumem-aarch64 # cross-compile for aarch64
nix run .#test              # make check (the 8-entry smoke suite)
nix run .#unit              # test/test_main, the comprehensive unit suite
nix run .#prop              # property tests
nix run .#integ             # integration tests
```

The flake builds the library but **does not run the test suite**
(`doCheck = false`), so `nix build` succeeding is a compile result, not a
correctness result. Use the apps above, or the autotools targets, to actually
test. (Earlier revisions of this file advertised `nix run .#test-native`,
which is not one of the flake's apps.)

---

## Documentation

- [AGENTS.md](AGENTS.md) — working rules for this repository (all
  building/testing happens on EC2; see `scripts/ec2/`).
- [docs/plans/2026-09-21-production-readiness.md](docs/plans/2026-09-21-production-readiness.md)
  — current workstream and what "production-ready" will require.
- [tools/DEBUGGING.md](tools/DEBUGGING.md) — debugging workflows.
- [docs/UMEMCTL.md](docs/UMEMCTL.md) — the live introspection channel.
- [docs/results/](docs/results/) — benchmark and diagnosis reports, with
  the provenance of every number quoted in this file.
- [examples/](examples/) — usage examples, including PostgreSQL
  palloc integration.
- [CHANGELOG.md](CHANGELOG.md) — version history.
- Man pages:
  - `umem(1)` — runtime introspection CLI.
  - `umem_alloc(3)`, `umem_cache_create(3)` — core API.
  - `umem_inspect(3)` — introspection C API.
  - `umem_hooks(3)` — allocation/free hook API.
  - `umem_debug(3)` — debug environment variables.
  - `umem_debugging(7)` — debugging guide.

---

## Testing

`make check` is a deliberately small smoke suite — **8 entries**, listed in
`TESTS` in `Makefile.am`. It is not the comprehensive suite, and passing it
does not mean the allocator is exercised broadly: `test/test_main` (the
several-hundred-assertion unit suite), the property tests, the concurrency
oracle, and the lifecycle stress tests are all separate targets that
`make check` does not run.

```bash
make check                                            # 8-entry smoke suite
LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork  # comprehensive unit suite
test/property/prop_alloc_free2                        # property tests (one of several)
test/stress/stress_concurrency_oracle --help          # concurrency oracle
./test/debugger/test_inspect_e2e.sh                   # gdb integration
./test/debugger/test_lldb_e2e.sh                      # lldb integration
make install-check                                    # installed prefix is usable
```

All building and testing for this project happens on EC2, never on the
development host — see [AGENTS.md](AGENTS.md) and `scripts/ec2/`. More detail
in [test/README.md](test/README.md).

---

## License

CDDL 1.0 (Common Development and Distribution License).  Same license
as OpenSolaris / illumos.  See [COPYING](COPYING) and
[OPENSOLARIS.LICENSE](OPENSOLARIS.LICENSE).

---

## References

- Bonwick, J. (1994).  [*The Slab Allocator: An Object-Caching Kernel
  Memory Allocator*](https://www.usenix.org/legacy/publications/library/proceedings/bos94/bonwick.html).
  USENIX.
- Bonwick, J. and Adams, J. (2001).  [*Magazines and Vmem: Extending
  the Slab Allocator to Many CPUs and Arbitrary Resources*](https://www.usenix.org/legacy/event/usenix01/full_papers/bonwick/bonwick.pdf).
  USENIX.
- Solaris `libumem(3LIB)` and `mdb(1)` `::findleaks` documentation
  (illumos-gate).

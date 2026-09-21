# libumem — slab + magazine allocator with built-in debugging

A portable port of the Solaris userspace slab allocator, modernized
and revived in 2024–2025.  Provides high-throughput, low-contention
memory allocation with first-class runtime debugging on Linux,
FreeBSD, and macOS.

> **Status: not production-ready at this commit.** The 2026-09-21 design
> review found reachable correctness and lifetime defects in *default* code
> paths, plus measurement defects that invalidate several performance
> conclusions previously published in this file. Work and exit criteria:
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
3. **Production-grade introspection** — alloc-site tracebacks,
   allocation history ring buffer, per-cache statistics, leak
   detection, all queryable at runtime by `umem(1)`, `gdb`, or
   `lldb` without restarting the target.

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
| Built-in leak detection | ✅ (`::findleaks`) | profile-based | ❌ | profile-based | ❌ |
| Allocation history ring buffer | ✅ | ❌ | ❌ | ❌ | ❌ |
| Live attach for inspection | ✅ (`umem --pid`) | runtime stats only | runtime stats only | runtime stats only | ❌ |
| Per-buffer stack capture | ✅ (`UMEM_DEBUG=audit`) | profile mode | sampling profile | ❌ | mtrace |
| Buffer overrun / UAF detect | ✅ (`UMEM_DEBUG=guards`) | partial | ❌ | ✅ (secure) | ❌ |
| Snapshot / offline analysis | ✅ (`.ums` format) | ❌ | profile heap dump | ❌ | ❌ |
| Cross-platform | Linux/BSD/Solaris/macOS | wide | Linux primary | wide | Linux only |
| Drop-in `LD_PRELOAD` | ✅ | ✅ | ✅ | ✅ | (default) |

Where libumem **does not win**:

- **Raw malloc / free throughput on tiny allocations.** jemalloc and
  mimalloc are faster on `malloc(8)` / `free` micro-benchmarks,
  primarily because their fast paths are smaller and they don't pay
  for object-cache machinery you may not be using.
- **Fragmentation / memory overhead under sustained load — no supportable
  number either way.** The
  [8-allocator shootout](docs/results/2026-09-08-allocator-shootout.md)
  reported libumem worst-in-field on RSS/allocated ratio, and v2.7.0
  reported that fixed. **Both conclusions are withdrawn as of 2026-09-21**:
  a review of the harness found the fragmentation measurement itself
  invalid, so neither the original finding nor the claimed fix is
  supported by it. Three independent defects, any one of which breaks the
  ratio: the live-bytes denominator accumulated bytes that had already been
  freed; `peak_rss_bytes` was sampled after cleanup, so it is not the peak;
  and the workload labelled "192-thread fragmentation" runs on exactly one
  thread (`test/bench/bench_main.c` sets `thread_count = 1` for it). The
  underlying `umem_max_ncpus` doubling bug *was* real and is fixed — that
  part stands on the code, not on the benchmark. What the fix does to
  fragmentation is simply not measured yet. See
  [`docs/results/2026-09-09-fragmentation-diagnosis.md`](docs/results/2026-09-09-fragmentation-diagnosis.md)
  for the original analysis, read with that caveat, and P2.2 in
  [the readiness plan](docs/plans/2026-09-21-production-readiness.md).
- **Tail latency at very high core counts under sustained load — improved,
  measured narrowly.** The shootout found libumem worst-or-tied-worst p999
  at 192 threads sustained for 3 minutes (157us vs jemalloc's 24.6us).
  Root cause in the code: the depot refill scanned other CPUs' stripes with
  a **blocking** mutex on failure while holding the caller's own per-CPU
  lock — a lock convoy that compounds only under sustained pressure.
  Switched to the already-existing non-blocking trylock primitive over the
  same scan breadth. Measured p999 156.7-163.2us → 83.8-92.6us, and
  independently re-verified at 86,974ns on a from-scratch build — **on
  x86_64 (`c7i.metal-48xl`) only**, with the operation-budget defect
  described in P2.1 of the readiness plan present in the harness at the
  time. The improvement is large and reproducible; treat the exact
  percentage as provisional until re-measured on the corrected harness, and
  note it was not re-measured on aarch64. It does not reach
  jemalloc/mimalloc/rpmalloc's tens-of-microseconds tier. The residual gap
  has been *attributed* to the inert rseq reload path; that is an untested
  hypothesis, not a finding — nothing has isolated it. See
  [`docs/results/2026-09-09-sustained-depot-contention-diagnosis.md`](docs/results/2026-09-09-sustained-depot-contention-diagnosis.md).
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
- **Forensics on a core dump.**  `umem --core core.* --exe ./bin
  findleaks` works without re-running the workload.
- **Embedded address-space management.**  vmem hierarchies handle
  use cases (DMA pools, NUMA-bound allocations, custom
  page-replacement) where you'd otherwise hand-roll.

If your workload is closer to "billions of small mallocs in a tight
loop" than to "long-running server with object lifecycles", pick
jemalloc or mimalloc.  If it's the other way around, pick libumem.

---

## Platform support

"Status" here means *what has been demonstrated*, not what is expected to
work. It was previously a column of "Production" labels with no recorded
evidence behind most of them; the 2026-09-21 review also found reachable
correctness defects in default paths on the best-covered platform, so no
row can currently claim production readiness.

| Platform | Architecture | Evidence | Status |
|---|---|---|---|
| Linux | x86_64 | CI on every push (normal, ASan, UBSan, gcov); benchmarks on EC2 `c7i` and `c7i.metal-48xl`; all Phase 1 regressions run here | Best covered. Not production-ready — see below |
| Linux | aarch64 | Manual EC2 runs on `c7g`/`c8g.metal-48xl` (build, `make check`, `test_main`, benchmarks). The nightly CI job exists and is validated but is **not armed** (repo secrets never added) | Builds and tests pass when run by hand; unattended coverage absent |
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

| Mode | Variable | Overhead | Detects |
|------|----------|----------|---------|
| Guards | `UMEM_DEBUG=guards` | ~10% | Buffer overruns, use-after-free |
| Audit | `UMEM_DEBUG=audit` | ~30% | Per-buffer alloc / free stack traces |
| Contents | `UMEM_DEBUG=default` | ~50% | Uninitialized reads, corruption |
| Firewall | `UMEM_DEBUG=firewall` | high | Guard page per allocation |
| Logging | `UMEM_LOGGING=transaction=1m` | ~5% | Chronological transaction log |

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

Measured with the `test/bench/` harness (CPU-pinned, warm-up discarded,
median of 5, coefficient-of-variation reported) on real AWS EC2 hardware —
never local, never a single quick run.

> **Read this first (2026-09-21).** A review of the harness found defects
> that invalidate part of what is reported below, so the numbers are not all
> equally trustworthy:
>
> - **The operation budget was divided by thread count twice**
>   (`test/bench/matrix.sh` and `test/bench/bench_main.c` each did it). The
>   192-thread points therefore measured ~52k total operations in ~3.8 ms
>   with >27% coefficient of variation — far too little work, far too much
>   noise, to support a scaling conclusion. **All 192-thread
>   throughput/scaling conclusions are withdrawn** pending re-measurement.
> - **The fragmentation metric is wrong in three independent ways**: freed
>   bytes stayed in the live denominator, "peak" RSS was sampled after
>   cleanup, and the workload labelled 192-thread runs on one thread.
>   **All fragmentation conclusions are withdrawn**, in both directions —
>   the original worst-in-field finding and the claimed fix.
> - 8-vCPU points, single-thread latency, and the sustained-load *tail
>   latency* comparison do not depend on the double division and are
>   reported below with their provenance.
>
> Tracked as P2.1/P2.2 in
> [the readiness plan](docs/plans/2026-09-21-production-readiness.md).
> Reproduce on your own target rather than trusting any table here.

### The 8-allocator shootout

[`docs/results/2026-09-08-allocator-shootout.md`](docs/results/2026-09-08-allocator-shootout.md)
compares umem against **libc, jemalloc, tcmalloc, mimalloc, snmalloc,
scudo, and rpmalloc** on x86_64 and aarch64 at 8 and 192 vCPU, plus musl
(Alpine) and illumos (umem's own lineage) — ~9,600 benchmark runs including
3-minute *sustained* 192-thread loads. Read it with the caveat box above:
its 8-vCPU and latency findings stand, its 192-thread scaling and
fragmentation findings do not.

**What still stands:**

| Finding | Detail |
|---|---|
| illumos (its own lineage) | Up to **4×** faster than illumos's own libc malloc under concurrency (16.4M vs 4.1M ops/s at 4 threads); dramatically tighter tail latency. The clearest win in the report — and the most meaningful comparison, since illumos ships the allocator umem re-implements. Low thread count, so unaffected by the budget defect. |
| 8-vCPU multi-thread scaling | Beats glibc by 25–30% on x86_64 through 8 threads; roughly ties glibc on aarch64. |
| Single-thread latency | Competitive but not a winner anywhere against x86_64/aarch64 glibc; mimalloc is fastest almost everywhere. |

**What is withdrawn pending re-measurement:**

| Withdrawn | Why |
|---|---|
| 192-thread `multi` scaling ("10–25% below the top allocators", falloff percentages) | Measured under the double-divided budget: ~52k ops in ~3.8 ms, CoV >27%. Not enough work to conclude anything. |
| Fragmentation, original finding ("worst-in-field, ~2.3× the next-worst") | The ratio's denominator counted freed bytes; "peak" RSS was post-cleanup; the workload is single-threaded. |
| Fragmentation, claimed v2.7.0 fix ("4.19→2.70 / 4.12→2.63, in the competitive 2.2-2.5 range") | Same broken metric — and 2.63-2.70 is not inside 2.2-2.5 in any case. The `umem_max_ncpus` doubling bug behind it was real and is fixed; its effect on fragmentation is unmeasured. |
| Short-burst `prodcons` falloff win (x86_64) | Falloff is computed across the same high-thread-count points as the scaling numbers. |

**Sustained tail latency — improved, narrowly measured.** The shootout found
umem worst-or-tied-worst p999 at 192 threads sustained for 3 minutes
(157us; jemalloc 24.6us). The mechanism was identified in the code, not just
correlated: the depot's cross-CPU steal scan blocked on a mutex while
holding the caller's own per-CPU lock. Replacing that with the existing
non-blocking trylock took p999 from 156.7-163.2us to 83.8-92.6us,
independently re-verified at 86,974ns on a from-scratch build — on x86_64
`c7i.metal-48xl`, with the budget defect present in the harness. Sustained
p999 is a tail-latency distribution rather than a throughput count, so it is
less sensitive to the total-operations error than the scaling numbers are,
but the exact percentage should be treated as provisional and it was not
re-measured on aarch64. It does not reach
jemalloc/mimalloc/rpmalloc's tens-of-microseconds tier; the residual gap has
been *attributed* to the inert rseq reload path, which is a hypothesis
nobody has isolated, not a finding. See
[`docs/results/2026-09-09-sustained-depot-contention-diagnosis.md`](docs/results/2026-09-09-sustained-depot-contention-diagnosis.md).

**Verdict, as narrowly as the evidence allows:** umem clearly outperforms
the traditional coarse-locked malloc it descends from under concurrency
(strongest on illumos, its own lineage), and holds its own against modern
allocators on 8-vCPU boxes. Its sustained 192-thread tail latency improved
substantially in v2.7.0 and still trails the purpose-built
high-concurrency allocators. How it scales at 192 threads, and what its
memory overhead is, are currently **unknown** — the measurements that
claimed to answer both were invalid.

### Prior umem-vs-glibc-only baselines (superseded, kept for provenance)

**x86_64** (`c7i.metal-48xl`, 192 vCPU, performance governor). Full data:
[`docs/results/2026-07-23-baseline.md`](docs/results/2026-07-23-baseline.md),
[`docs/results/2026-07-23-d2-fix-validation.md`](docs/results/2026-07-23-d2-fix-validation.md).

| Workload | umem vs glibc | Notes |
|---|---|---|
| Single-thread (16–64 B) | ~1.2× throughput | p50 ~21 ns vs ~16 ns (glibc) |
| `multi`, 8 threads | 33.8 Mops/s | scales past 4 threads after the 2.1.0 PTC fix; p999 sub-µs |
| `multi`, 192 threads | 320 Mops/s | p999 ~299 ns (was 1.83 ms pre-fix) |
| `prodcons`, 4 threads | ~245% of glibc | ~10× lower p99 (cross-thread handoff) |

**aarch64** (`c8g.metal-48xl` Graviton4, 192 vCPU, same harness). Full data:
[`docs/results/2026-09-08-aarch64-baseline.md`](docs/results/2026-09-08-aarch64-baseline.md).

| Workload | umem vs glibc | Notes |
|---|---|---|
| Single-thread (64–256 B) | ~1.03–1.06× throughput | p50 ~36 ns vs ~35 ns (glibc) — smaller latency gap than x86_64's ~1.3× |
| `multi` (same-size-class 160 B), 8 threads | 49.5 Mops/s | ~99% of glibc; p999 39 ns |
| `multi` (same-size-class 160 B), 192 threads | 457.5 Mops/s | ~99% of glibc; p999 43 ns (flat — no cliff at any thread count measured) |
| `prodcons`, 4 threads | ~120% of glibc | mixed across thread counts (49–120%); does **not** reproduce x86_64's decisive ~245%/10×-lower-p99 win |

These isolated umem-vs-glibc baselines were taken with the same harness as
the shootout, so their 192-thread rows carry the same double-divided-budget
defect and are withdrawn on the same grounds. The single-thread and
8-thread rows are unaffected. Both umem and glibc were measured under the
identical (wrong) budget, so the *relative* 192-thread ratios may well
survive re-measurement — but "may well" is not evidence, and the absolute
Mops/s figures at that thread count are not meaningful.

Numbers vary substantially with workload and hardware; reproduce with the
harness on your own target rather than trusting a single table.

---

## Debugging

libumem ships runtime introspection equivalent to Solaris `mdb`'s
`::findleaks`, `::umem_log`, and friends, via **two complementary tools**:

- **`umem(1)`** — gdb/ptrace-driven, point-in-time. Works against a live
  pid, a **core dump**, or an offline `.ums` snapshot; emits text or JSON.
  Non-invasive (no in-process thread). Best for CI, post-mortem, and
  scripted leak-finding.
- **`umemctl`** — an opt-in in-process channel (`--enable-introspect` +
  `UMEM_OPTIONS=introspect=1`) for the live/interactive things a ptrace
  snapshot cannot do: **streaming** event logs (`logtail`), a live TUI
  (`monitor`), and `record` + **break-before-a-leaked-allocation**
  (stop the allocating thread so a debugger catches the exact stack).
  Zero hot-path cost when off. See [`docs/UMEMCTL.md`](docs/UMEMCTL.md).

```bash
# umem(1): snapshot / core / CI leak-finding
umem --pid $(pgrep myapp) findleaks
umem --pid $(pgrep myapp) findleaks -f json | jq .
umem --pid $(pgrep myapp) status
umem --core core.12345 --exe ./myapp findleaks
umem --dump /tmp/state.ums findleaks    # offline; no live process

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

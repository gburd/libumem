# libumem — slab + magazine allocator with built-in debugging

A portable port of the Solaris userspace slab allocator, modernized
and revived in 2024–2025.  Provides high-throughput, low-contention
memory allocation with first-class runtime debugging on Linux,
FreeBSD, and macOS.

```bash
./autogen.sh && ./configure && make -j"$(nproc)" && make check
sudo make install     # installs libumem.so, libumem_malloc.so,
                      # the umem(1) tool, and gdb/lldb integrations
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
| **2024–2025** | This revival.  Substantial rework of the allocator hot paths, modernization to C11 / C17, fixes for Linux, FreeBSD, and Windows portability, addition of new features (per-thread cache, scoped arenas, ownership tracking, GC, profiling), and — the headline change — the runtime introspection tools that bring `mdb`-class debugging back. |

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
- **RSEQ fast path** wired into the x86_64 allocation hot path
  (glibc 2.35+).
- **Per-Thread Cache (PTC)** — lock-free fast path for allocations
  up to 2 KB, generated as inline assembly per architecture.  Falls
  through to the magazine layer cleanly when sizes don't qualify.
- **Slab page reclaim** via `madvise(MADV_DONTNEED)` for idle slabs.
- **Cache-line auditing** of hot fields with `_Static_assert`
  verification.
- **NUMA-aware depot stealing** with statistics for local /
  same-node / cross-node hits.

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
- **Garbage collector (`umem_gc.h`)** — conservative mark-sweep, GC
  with Boehm-compatible API, using umem slabs as backing store.
- **Allocation profiling (`umem_profile.h`)** — record / replay to
  pre-warm caches.
- **Budget contexts** — PostgreSQL-style per-context memory budgets.

### Quality

- Test coverage from \~33% to >80% line coverage.
- Property-based tests, integration tests, stress tests.
- Cross-platform benchmark suite (TOML output with OS / arch /
  compiler metadata).
- Forgejo Actions CI (build × asan / ubsan / coverage; lint;
  tagged-release pipeline).
- Removed \~2,500 lines of stale code and several documentation
  artifacts that were lying about features that didn't actually
  work.

---

## How libumem compares to other allocators

| Property | **libumem** | jemalloc | tcmalloc | mimalloc | glibc ptmalloc |
|---|---|---|---|---|---|
| Slab / object cache API | ✅ first-class | ❌ | ❌ | ❌ | ❌ |
| Constructor / destructor caches | ✅ | ❌ | ❌ | ❌ | ❌ |
| vmem virtual address arenas | ✅ | partial (extents) | ❌ | partial | ❌ |
| Per-CPU magazines | ✅ | ✅ (tcache) | ✅ (thread cache) | ✅ (heap) | partial |
| RSEQ fast path | ✅ (x86_64) | ❌ | ✅ | ❌ | ❌ |
| Lock-free per-thread cache | ✅ (PTC) | ✅ (tcache) | ✅ | ✅ | ❌ |
| Built-in leak detection | ✅ (`::findleaks`) | profile-based | ❌ | profile-based | ❌ |
| Allocation history ring buffer | ✅ | ❌ | ❌ | ❌ | ❌ |
| Live attach for inspection | ✅ (`umem --pid`) | runtime stats only | runtime stats only | runtime stats only | ❌ |
| Per-buffer stack capture | ✅ (`UMEM_DEBUG=audit`) | profile mode | sampling profile | ❌ | mtrace |
| Buffer overrun / UAF detect | ✅ (`UMEM_DEBUG=guards`) | partial | ❌ | ✅ (secure) | ❌ |
| Conservative GC | ✅ (`umem_gc.h`) | ❌ | ❌ | ❌ | ❌ |
| Snapshot / offline analysis | ✅ (`.ums` format) | ❌ | profile heap dump | ❌ | ❌ |
| Cross-platform | Linux/BSD/Solaris/macOS | wide | Linux primary | wide | Linux only |
| Drop-in `LD_PRELOAD` | ✅ | ✅ | ✅ | ✅ | (default) |

Where libumem **does not win**:

- **Raw malloc / free throughput on tiny allocations.** jemalloc and
  mimalloc are faster on `malloc(8)` / `free` micro-benchmarks,
  primarily because their fast paths are smaller and they don't pay
  for object-cache machinery you may not be using.
- **Memory footprint at idle.** glibc ptmalloc holds less metadata
  per process for small workloads.  libumem's slab + magazine
  metadata is amortized across allocations, so it's competitive once
  the working set is non-trivial.
- **Sandboxed / security-hardened allocations.**  mimalloc-secure
  and `scudo` add explicit hardening (segregated metadata, randomized
  freelists, double-free detection by design).  libumem's defenses
  are opt-in via `UMEM_DEBUG=guards`.

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

| Platform | Architecture | Status |
|---|---|---|
| Linux | x86_64 | Production |
| Linux | aarch64 | Production |
| Linux | riscv64 | Production |
| FreeBSD | amd64 | Production |
| illumos | SPARCv9 | Production |
| macOS | x86_64, arm64 | Tested |
| Windows | x64 (MSVC, MinGW) | Experimental |

CI (Forgejo Actions, see `.forgejo/workflows/`) covers Linux x86_64
in normal, AddressSanitizer, UndefinedBehaviorSanitizer, and gcov
modes.  RISC-V and aarch64 are validated via Nix + QEMU.

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

Headers under `#define UMEM_ENABLE_EXPERIMENTAL`.  Active development;
APIs may change.

- **Ownership tracking (`umem_own.h`)** — Rust-inspired ownership /
  borrowing with runtime checks.  Two modes: lightweight (~2%) and
  full (~15%).
- **Garbage collection (`umem_gc.h`, `gc.h`)** — conservative
  mark-sweep with Boehm-compatible API.  Concurrent marking,
  finalizers, sparsemap for O(1) pointer lookup.
- **Allocation profiling (`umem_profile.h`)** — record / replay,
  phase detection.
- **Budget contexts (`examples/umem_palloc.h`)** — PostgreSQL-style
  per-context memory management.

---

## Performance (relative to glibc on Linux)

Measured with the stabilized `test/bench/` harness (CPU-pinned, warm-up
discarded, median of 5, coefficient-of-variation reported) on tuned EC2
metal instances. Full data + provenance:
[`docs/results/2026-07-23-baseline.md`](docs/results/2026-07-23-baseline.md)
and [`docs/results/2026-07-23-d2-fix-validation.md`](docs/results/2026-07-23-d2-fix-validation.md).

**x86_64** (`c7i.metal-48xl`, 192 vCPU, performance governor):

| Workload | umem vs glibc | Notes |
|---|---|---|
| Single-thread (16–64 B) | ~1.2× throughput | p50 ~21 ns vs ~16 ns (glibc) |
| `multi`, 8 threads | 33.8 Mops/s | scales past 4 threads after the 2.1.0 PTC fix; p999 sub-µs |
| `multi`, 192 threads | 320 Mops/s | p999 ~299 ns (was 1.83 ms pre-fix) |
| `prodcons`, 4 threads | ~245% of glibc | ~10× lower p99 (cross-thread handoff) |

**aarch64** (`c8g.metal-48xl` Graviton4, 192 vCPU): runs correctly as of
2.1.0 (2.0.0 SIGSEGV'd). A post-fix authoritative scaling table is not yet
published — the baseline sweep captured aarch64 before the rseq fix landed.

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
nix build                   # native build
nix build .#libumem-aarch64 # cross-compile for aarch64
nix run .#test-native       # run tests
```

Detail: [NIX_USAGE.md](NIX_USAGE.md).

---

## Documentation

- [tools/DEBUGGING.md](tools/DEBUGGING.md) — debugging workflows.
- [examples/](examples/) — usage examples, including PostgreSQL
  palloc integration.
- [CHANGELOG.md](CHANGELOG.md) — version history.
- Man pages:
  - `umem(1)` — runtime introspection CLI.
  - `umem_alloc(3)`, `umem_cache_create(3)` — core API.
  - `umem_inspect(3)` — introspection C API.
  - `umem_debug(3)` — debug environment variables.
  - `umem_debugging(7)` — debugging guide.

---

## Testing

```bash
make check                                            # autotools suite
LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork  # comprehensive
./test/debugger/test_inspect_e2e.sh                   # gdb integration
./test/debugger/test_lldb_e2e.sh                      # lldb integration
```

---

## License

CDDL 1.0 (Common Development and Distribution License).  Same license
as OpenSolaris / illumos.  See [LICENSE](LICENSE).

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

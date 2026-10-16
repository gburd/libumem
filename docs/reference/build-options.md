---
title: Build options
parent: Reference
nav_order: 2
permalink: /reference/build-options/
lede: >-
  Every configure flag and every runtime knob -- and the ones that were
  removed because they never worked.
---

The default build is deliberately portable and conservative. **Every option
below either works or does not exist** -- there is no flag that turns on
broken behaviour (Phase 4 of the readiness plan).

## configure flags

| Option | Default | Effect |
|---|---|---|
| `--enable-rseq[=auto]` | auto (on where `linux/rseq.h` exists) | Builds the rseq per-CPU fast path, on at runtime by default. `UMEM_OPTIONS=norseq` turns it off. See [rseq per-CPU]({{ '/design/rseq-percpu/' | relative_url }}). |
| `--enable-numa[=auto]` | auto (on where libnuma exists) | NUMA **topology queries** (node count, CPU->node map, distances) plus the depot's cross-CPU locality accounting. **Not** a NUMA allocation policy. |
| `--enable-introspect` | off | The in-process `umemctl` control channel. Zero hot-path cost when off. |
| `--enable-avx2` | **off** | `-mavx2` for the whole library. **Not generic**: the result requires an AVX2-capable CPU (Haswell/Excavator or newer) and will `SIGILL` on older x86-64. x86_64 only. |
| `--enable-asan` / `--enable-ubsan` / `--enable-tsan` | off | Sanitizer builds. (`--enable-asan` also adds frame pointers, which deepens audit stacks.) |
| `--enable-coverage` | off | gcov/lcov instrumentation; adds the `coverage` target. |
| `--enable-pgo=generate\|use` | off | Profile-guided optimization. |

The default x86-64 build targets the SSE2 baseline (every x86-64 CPU has
it), so it runs anywhere. There is no runtime ISA dispatch.

{: .note }
> **Removed options:** `--enable-percpu-caching` and `--enable-htm` -- neither
> ever worked (`--enable-percpu-caching` produced a build failure; the HTM
> depot fast path was comments, and the lock elision was unsound). Both are
> quarantined in `attic/` with the evidence. `UMEM_OPTIONS=numa` was removed
> too (it was never registered).

## Runtime: UMEM_OPTIONS

Comma-separated, read from the environment (ignored for the file/socket/exec
knobs in [secure mode]({{ '/design/hardening/' | relative_url }})):

| Option | Effect |
|---|---|
| `tcache=0` | Disable the per-thread cache. |
| `tcache_max=N` | Lower the PTC size ceiling (default 8192). |
| `norseq` | Turn off the rseq per-CPU layer with no rebuild. |
| `introspect=1` | Start the `umemctl` control socket (needs `--enable-introspect`). |
| `abort` | Abort on a rejected pointer instead of logging and continuing. |

{: .warning }
> `perthread_cache=0` does **not** disable the per-thread cache -- it is a
> Solaris byte-sized knob read by nothing in this port. Use `tcache=0`.

## Runtime: UMEM_DEBUG modes

No recompile needed. Measured overhead is order-of-magnitude guidance from a
single run of a 64-byte alloc/free loop on `c7i.2xlarge`, not a median-of-N
result, and not measured on aarch64:

| Mode | Variable | Measured overhead | Detects |
|---|---|---|---|
| Lite | `UMEM_DEBUG=lite` | 28% | A cheaper subset of guards |
| Guards | `UMEM_DEBUG=guards` | 32% | Buffer overruns, use-after-free |
| Audit | `UMEM_DEBUG=audit` | 58% | Per-buffer alloc/free stack capture (~2 frames in a default build) |
| Contents | `UMEM_DEBUG=contents` | not measured | Buffer-contents logging (needs `audit`) |
| Default | `UMEM_DEBUG=default` | 60% | All of the above |
| Firewall | `UMEM_DEBUG=firewall` | not measured | A guard page per allocation (>= `minfirewall`) |
| Logging | `UMEM_LOGGING=transaction=1m` | not measured | A chronological transaction log |

The two "not measured" rows are honest: the overhead benchmark's 64-byte
allocations fall below the firewall threshold and do not engage `contents`,
so it reports ~0% for both -- a property of the benchmark, not the modes.
Earlier revisions of this table published figures with no measurement behind
them; those are withdrawn.

---
title: What this port changed
parent: Philosophy
nav_order: 2
permalink: /philosophy/what-changed/
lede: >-
  The 2024-2025 revival was not a cosmetic refresh. What it added, what it
  fixed, and -- just as important -- what it removed because it never
  worked.
---

The portable fork libumem descends from had accumulated a decade of rot:
features advertised in headers and comments that the code did not implement,
and performance claims with no measurement behind them. This port's work
divides into three: real additions, real fixes, and honest removals.

## Added

- **The per-thread cache (PTC).** A lock-free fast path for allocations up
  to 8 KB, generated as inline assembly per architecture, in front of the
  magazine layer. See
  [magazine/depot/PTC]({{ '/design/magazine-depot-ptc/' | relative_url }}).
- **The rseq per-CPU layer.** Restartable-sequence lock-free magazine
  reloads, finally *armed and serving* in v3.3.2 after a long inert period.
  See [rseq per-CPU]({{ '/design/rseq-percpu/' | relative_url }}).
- **Runtime introspection.** `umem(1)`, `umemctl`, and the gdb/lldb
  integrations -- `::findleaks`-class debugging on Linux/FreeBSD/macOS. See
  [Introspection]({{ '/design/introspection/' | relative_url }}).
- **Binary snapshots (`.ums`)** for offline analysis.
- Modernization: C11 `<stdatomic.h>` throughout (all `__sync_*` builtins
  replaced), cache-line auditing with `_Static_assert`, slab page reclaim
  via `madvise(MADV_DONTNEED)`.

## Fixed

The fixes fall into two campaigns, each with regressions that fail before
and pass after, on x86_64 and aarch64:

- **Correctness (2026-09-21 design review).** Ten reachable defects in
  *default* code paths -- fork lock order, PTC draining and resize-safe
  magazine ownership, cache destruction with retained slabs,
  `umem_free(NULL, size)` putting NULL on a free list, and more. The
  [heap ceiling]({{ '/design/heap-ceiling/' | relative_url }}) was fixed in two
  halves.
- **Security (three adversarial audits).** The 2026-09-22, 2026-09-24, and
  v3.3.1 audits; see [Hardening]({{ '/design/hardening/' | relative_url }}).

The port also repaired the **measurement machinery** that had hidden these
defects: benchmark work-sizing, fragmentation live-byte accounting, oracle
progress accounting, and result-status honesty (a passing test that
"returned 0 for an unavailable prerequisite" is a bug, not a skip).

## Removed -- the honest part

A revival that only adds is dishonest about what it inherited. This port
removed, with evidence, a lot of code that did not work:

- **`--enable-percpu-caching`** -- turning it on produced a *build failure*
  (fields it indexed did not exist). Quarantined in `attic/`.
- **`--enable-htm`** (Intel TSX) -- the depot fast path was comments; the
  lock elision was unsound (it never added the fallback lock to the
  transaction read set). Quarantined.
- **`UMEM_OPTIONS=numa`** -- never registered; read by nothing.
- **The lock-free striped-array depot** -- replaced with the straightforward
  mutex-protected list of the original Bonwick & Adams design, ~280 lines
  of complexity gone.
- **Most of the `umem_numa.[ch]` policy layer** -- it never ran (removed
  2026-09-21). The NUMA *topology* code that does run is a separate thing.
- **The vendored sparsemap** -- removed in v4.0.0; the soname became
  `libumem.so.2`. See [ADR: sparsemap
  removed]({{ '/adr/0005-sparsemap-removed/' | relative_url }}).
- **~2,500 lines of stale code** and documentation artifacts that lied
  about features. That housekeeping is not finished -- the 2026-09-21 review
  found more, and the README was part of what it corrected.

{: .rationale }
> Removing a non-working feature with a one-line "it never worked, here is
> the evidence" is worth more than carrying it as aspiration. The
> `attic/README.md` records *why* each removed thing is unfinished "in a way
> you could not fix by finishing it."

## Still not production-ready

None of this makes libumem production-ready, and the pages here say so. What
it establishes is a measured, honest baseline: the specific defects found by
one design review and three audits are fixed and stay fixed under test, and
the numbers on these pages were taken with a null control. That is the state
to build the next phase on.

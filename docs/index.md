---
title: Home
layout: home
hero: false
nav_order: 1
---

# libumem
{: .fs-9 }

A portable port of the Solaris userspace slab + magazine allocator:
object caches with constructor/destructor callbacks, vmem address
arenas, a lock-free per-thread cache, and first-class runtime heap
debugging that no mainstream allocator matches.
{: .fs-6 .fw-300 }

[Get started]({{ '/guide/getting-started/' | relative_url }}){: .btn .btn-primary .fs-5 .mb-4 .mb-md-0 .mr-2 }
[API reference]({{ '/reference/api/' | relative_url }}){: .btn .fs-5 .mb-4 .mb-md-0 }

---

libumem is Jeff Bonwick's slab allocator and the Bonwick & Adams magazine
and vmem layers -- the allocator that has run inside the Solaris kernel and
userspace since the 1990s -- revived, modernized to C11/C17, and ported to
Linux, FreeBSD, and macOS. It gives a long-running service three things no
mainstream `malloc` offers as well:

- **Object caches** -- type-stable allocation with constructor/destructor
  callbacks, reducing the fast path to roughly one uncontended atomic.
- **vmem arenas** -- first-class virtual address management with quantum
  caching and hierarchical sub-arenas.
- **Live heap debugging** -- `::findleaks`-class leak detection, an
  allocation-history ring buffer, per-cache statistics, and a *break before
  the allocation that eventually leaks*, all queryable against a running or
  snapshotted process by `umem(1)`, `umemctl`, `gdb`, or `lldb` without
  restarting the target.

{: .note }
> **Status: v4.1.0.** The correctness work from the 2026-09-21 design review
> is done (ten reachable default-path defects fixed, each with a demonstrated
> pre-fix failure on x86_64 and aarch64). The Linux ~5 GB heap ceiling is
> fixed. Phase 5 security hardening is complete for the specific exposures
> three adversarial audits found. This was a hardening pass, not a security
> proof: libumem is a `malloc` replacement, so the surface is broad. It is
> not yet labelled production-ready on any platform. The pages here say what
> has been measured and what has not; see
> [Hardening stance]({{ '/philosophy/hardening-stance/' | relative_url }}).

## Two libraries, one source tree

libumem ships two shared objects:

- **`libumem.so`** -- link with `-lumem` for the full C API: `umem_alloc`,
  object caches, vmem arenas, the introspection API, the experimental
  ownership/profiling/budget headers.
- **`libumem_malloc.so`** -- an `LD_PRELOAD` drop-in that interposes
  `malloc`/`free`/`calloc`/`realloc`. No code change, no API surface; just
  the allocator underneath an existing binary.

```bash
./autogen.sh && ./configure && make -j"$(nproc)" && make check
sudo make install

# drop-in, no rebuild:
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./myapp

# or link directly for the C API:
gcc myapp.c -lumem -o myapp
```

## The manual

### Guide (read in order)

1. [Getting started]({{ '/guide/getting-started/' | relative_url }}) -- build,
   install, and the first `umem_alloc` program.
2. [The LD_PRELOAD drop-in]({{ '/guide/ld-preload/' | relative_url }}) -- replacing
   the system `malloc` without touching the application.
3. [The C API]({{ '/guide/c-api/' | relative_url }}) -- object caches, vmem arenas,
   and the stable surface.
4. [Debugging a live heap]({{ '/guide/debugging/' | relative_url }}) -- `umem`,
   `umemctl`, and the gdb/lldb integrations.

### Design -- one page per mechanism

- [Slab and vmem]({{ '/design/slab-and-vmem/' | relative_url }})
- [Magazines, depot, and the per-thread cache]({{ '/design/magazine-depot-ptc/' | relative_url }})
- [The rseq per-CPU layer]({{ '/design/rseq-percpu/' | relative_url }})
- [Freelist mangling]({{ '/design/freelist-mangling/' | relative_url }})
- [The span table]({{ '/design/span-table/' | relative_url }})
- [Introspection]({{ '/design/introspection/' | relative_url }})
- [The Linux heap ceiling]({{ '/design/heap-ceiling/' | relative_url }})
- [Security hardening]({{ '/design/hardening/' | relative_url }})
- [Platform & architecture abstraction (PAL/AAL)]({{ '/design/pal/' | relative_url }}) -- design

### Reference

- [Build options]({{ '/reference/build-options/' | relative_url }})
- [Limits]({{ '/reference/limits/' | relative_url }})
- [Manual pages]({{ '/reference/man-pages/' | relative_url }}) and the generated
  [API reference]({{ '/reference/api/' | relative_url }}).

### Philosophy

- [Lineage]({{ '/philosophy/lineage/' | relative_url }}) -- Bonwick's slab to this
  port.
- [What this port changed]({{ '/philosophy/what-changed/' | relative_url }})
- [The hardening stance]({{ '/philosophy/hardening-stance/' | relative_url }})

### Decisions

- [Architecture decision records]({{ '/adr/' | relative_url }}) -- GC removed,
  rseq default-on, remote-free declined, involution encoding declined,
  sparsemap removed.

## Source and license

- Git: <https://codeberg.org/gregburd/libumem> (mirror:
  <https://github.com/gburd/libumem>)
- License: CDDL (the OpenSolaris lineage); see
  [`OPENSOLARIS.LICENSE`](https://codeberg.org/gregburd/libumem/src/branch/master/OPENSOLARIS.LICENSE).
- Security policy:
  [`SECURITY.md`](https://codeberg.org/gregburd/libumem/src/branch/master/SECURITY.md).

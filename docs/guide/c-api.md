---
title: The C API
parent: Guide
nav_order: 3
permalink: /guide/c-api/
lede: >-
  The stable surface -- object caches, vmem arenas, allocation flags -- and
  the experimental headers, with their caveats.
---

Link with `-lumem` and include `<umem.h>`. The full per-function reference
is the [generated API]({{ '/reference/api/' | relative_url }}); this page is the
map.

## Core allocation

```c
void *umem_alloc(size_t size, int flags);   /* flags: UMEM_DEFAULT | UMEM_NOFAIL */
void *umem_zalloc(size_t size, int flags);  /* zero-filled */
void  umem_free(void *buf, size_t size);    /* size must match */
```

`UMEM_DEFAULT` returns `NULL` on exhaustion. `UMEM_NOFAIL` instead invokes a
registered nofail callback that can retry or abort -- see
[`umem_alloc(3)`]({{ '/reference/api/umem_alloc/' | relative_url }}).

## Object caches

The reason to use libumem. See
[`umem_cache_create(3)`]({{ '/reference/api/umem_cache_create/' | relative_url }}).

```c
umem_cache_t *umem_cache_create(
    const char *name, size_t bufsize, size_t align,
    umem_constructor_t *ctor, umem_destructor_t *dtor,
    umem_reclaim_t *reclaim, void *callback_data,
    umem_cache_t *source, int cflags);

void *umem_cache_alloc(umem_cache_t *, int flags);
void  umem_cache_free(umem_cache_t *, void *buf);
void  umem_cache_destroy(umem_cache_t *);
```

- `ctor`/`dtor` run **once per buffer** across its reuse lifetime, not on
  every alloc/free.
- `reclaim` is called under memory pressure to let the cache voluntarily
  give back idle buffers.
- `align` requests a stricter alignment than the default.

## vmem arenas

First-class virtual address management: quantum-cached, hierarchical. Create
a child arena that imports from a parent in large chunks and sub-allocates
without touching the parent's lock. Used for DMA pools, NUMA-bound regions,
id spaces, and custom page-replacement schemes. See
[Slab and vmem]({{ '/design/slab-and-vmem/' | relative_url }}) for the model and
the vmem sources for the API.

## Allocation hooks

Register callbacks around allocation events -- see
[`umem_hooks(3)`]({{ '/reference/api/umem_hooks/' | relative_url }}).

## Introspection

The C side of the [debugging tools]({{ '/guide/debugging/' | relative_url }}):
[`umem_inspect(3)`]({{ '/reference/api/umem_inspect/' | relative_url }}) exposes
`umem_inspect_snapshot()` (write a `.ums` file for offline analysis) and
`umem_inspect_set_event_cb()` (a live callback on alloc/free/corruption
events). Debug modes are documented in
[`umem_debug(3)`]({{ '/reference/api/umem_debug/' | relative_url }}) and
[`umem_debugging(7)`]({{ '/reference/api/umem_debugging/' | relative_url }}).

## Experimental headers

{: .warning }
> These require `#define UMEM_ENABLE_EXPERIMENTAL`, their APIs may change,
> and -- critically -- **none is a safety or enforcement guarantee.** The
> 2026-09-21 review found that ownership tracking can itself corrupt memory
> and that budgets do not actually enforce. Treat them as diagnostic aids.

- **Ownership tracking (`umem_own.h`)** -- Rust-inspired ownership/borrowing
  with best-effort runtime checks. *Not* a use-after-free defense.
- **Allocation profiling (`umem_profile.h`)** -- sampling-based record/replay
  to pre-warm caches. Lossy; not an audit trail.
- **Budget contexts (`examples/umem_palloc.h`)** -- PostgreSQL-style
  per-context memory. The budget is *accounting*, not a limit; allocations
  are not reliably refused when it is exhausted.

## Stack-based objects (SBO)

A bump allocator and scoped arenas for short-lived allocations that
auto-clean on scope exit -- when the lifetime is a scope, not an object.

---
title: Manual pages
parent: Reference
nav_order: 1
permalink: /reference/man-pages/
lede: >-
  The man pages libumem installs, and how to read them in-site.
---

libumem installs these manual pages (man(7)-macro format, in the repository
root):

| Page | What it covers |
|---|---|
| [`umem(1)`]({{ '/reference/api/umem/' | relative_url }}) | The runtime introspection CLI -- `findleaks`, `log`, `status`, `walk`, `whatis`, `bufctl`, `snapshot`, `break`. |
| [`umem_alloc(3)`]({{ '/reference/api/umem_alloc/' | relative_url }}) | `umem_alloc`, `umem_zalloc`, `umem_free`, and the nofail callback. |
| [`umem_cache_create(3)`]({{ '/reference/api/umem_cache_create/' | relative_url }}) | Object caches: ctor/dtor, align, reclaim, and vmem sources. |
| [`libumem(3)`]({{ '/reference/api/libumem/' | relative_url }}) | The library overview. |
| [`umem_hooks(3)`]({{ '/reference/api/umem_hooks/' | relative_url }}) | Allocation hook callbacks. |
| [`umem_inspect(3)`]({{ '/reference/api/umem_inspect/' | relative_url }}) | The introspection C API -- snapshots and event callbacks. |
| [`umem_debug(3)`]({{ '/reference/api/umem_debug/' | relative_url }}) | The debug-mode C surface. |
| [`umem_debugging(7)`]({{ '/reference/api/umem_debugging/' | relative_url }}) | The runtime debugging guide for Linux/FreeBSD/macOS. |

The [API reference]({{ '/reference/api/' | relative_url }}) renders each of these
into the site, grouped by category. They are generated from the man-page
sources by `docs/gen_api_reference.sh` (run by the Pages workflow before each
build), so the site is always current with the installed pages.

The Doxygen-extracted C API -- the public headers (`umem.h`,
`umem_inspect.h`, `umem_own.h`, `umem_profile.h`, `umem_hooks.h`) -- is at
[`/api/`]({{ '/api/' | relative_url }}), built into the site at deploy time.

{: .note }
> The in-site API pages do not exist in the repository checkout: they are
> generated at build time from the man-page sources. If you are reading this
> from a local `jekyll build`, run `sh docs/gen_api_reference.sh` first (it
> needs `mandoc`).

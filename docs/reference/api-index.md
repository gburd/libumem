---
title: API reference
parent: Reference
nav_order: 4
has_children: true
permalink: /reference/api/
lede: >-
  The manual pages rendered into the site, by category, plus the Doxygen C
  API.
---

The pages under this section are generated from libumem's man-page sources
by `docs/gen_api_reference.sh` (run by the Pages workflow before each
Jekyll build), grouped into categories:

- **Command-line tools** -- `umem(1)`.
- **Core allocation API** -- `umem_alloc(3)`, `umem_cache_create(3)`,
  `libumem(3)`.
- **Allocation hooks** -- `umem_hooks(3)`.
- **Debugging and introspection** -- `umem_debug(3)`, `umem_inspect(3)`,
  `umem_debugging(7)`.

The Doxygen-extracted C API (the public headers) is built separately into
[`/api/`]({{ '/api/' | relative_url }}) at deploy time.

{: .note }
> These category and function pages are **generated at build time** and are
> not in the repository checkout. In a local preview, run
> `sh docs/gen_api_reference.sh` (needs `mandoc`) before `jekyll build`.

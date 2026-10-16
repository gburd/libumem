---
title: Guide
nav_order: 3
has_children: true
permalink: /guide/
lede: >-
  From an empty directory to a drop-in malloc replacement, the C API, and
  debugging a live heap -- read in order.
---

- [Getting started]({{ '/guide/getting-started/' | relative_url }}) -- build,
  install, and the first `umem_alloc` program.
- [The LD_PRELOAD drop-in]({{ '/guide/ld-preload/' | relative_url }}) -- replace
  the system `malloc` with no code change.
- [The C API]({{ '/guide/c-api/' | relative_url }}) -- object caches, vmem
  arenas, and the stable surface.
- [Debugging a live heap]({{ '/guide/debugging/' | relative_url }}) -- `umem`,
  `umemctl`, and the gdb/lldb integrations.

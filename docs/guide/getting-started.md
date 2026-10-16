---
title: Getting started
parent: Guide
nav_order: 1
permalink: /guide/getting-started/
lede: >-
  Build libumem, install it, and run your first object-cache program.
---

## Build and install

libumem is an autotools project. From a clone:

```bash
./autogen.sh && ./configure && make -j"$(nproc)" && make check
sudo make install
```

`make install` places two shared objects and the tools:

- `libumem.so` -- link with `-lumem` for the C API.
- `libumem_malloc.so` -- the `LD_PRELOAD` drop-in (see
  [LD_PRELOAD]({{ '/guide/ld-preload/' | relative_url }})).
- `umem(1)` and `umemctl` -- the introspection tools.
- gdb/lldb integrations under the install's `tools/` path.

{: .warning }
> `make check` runs an **8-entry smoke suite**, not the comprehensive test
> suite. The property, integration, and stress suites are separate targets.
> A green `make check` is a sanity gate, not a proof of correctness.

## Your first program -- the plain API

```c
#include <umem.h>

int main(void) {
    void *p = umem_alloc(1024, UMEM_DEFAULT);
    if (p == NULL)
        return 1;                 /* UMEM_DEFAULT may still return NULL */
    /* ... use p ... */
    umem_free(p, 1024);           /* size must match the allocation */

    void *z = umem_zalloc(1024, UMEM_DEFAULT);  /* zero-filled */
    umem_free(z, 1024);
    return 0;
}
```

```bash
gcc first.c -lumem -o first && ./first
```

Two things differ from `malloc`:

1. **`umem_free` takes the size.** The size you pass must match the
   allocation. This is how the slab layer finds the object's cache without a
   per-object header.
2. **The allocation flag.** `UMEM_DEFAULT` may return `NULL` on exhaustion;
   `UMEM_NOFAIL` instead invokes a nofail callback (see
   [`umem_alloc(3)`]({{ '/reference/api/umem_alloc/' | relative_url }})). Always
   check the result under `UMEM_DEFAULT`.

## Your first object cache

An object cache is the reason to reach for libumem: type-stable buffers with
a constructor that runs once per buffer and a destructor that runs once when
the slab is released.

```c
#include <umem.h>

typedef struct conn { int fd; char buf[512]; } conn_t;

static int conn_ctor(void *obj, void *priv, int flags) {
    (void)priv; (void)flags;
    ((conn_t *)obj)->fd = -1;      /* runs once, not on every alloc */
    return 0;
}

int main(void) {
    umem_cache_t *c = umem_cache_create(
        "conn", sizeof(conn_t), 0,
        conn_ctor, NULL, NULL, NULL, NULL, 0);

    conn_t *a = umem_cache_alloc(c, UMEM_DEFAULT);   /* fd already -1 */
    umem_cache_free(c, a);                           /* back to the cache */

    umem_cache_destroy(c);
    return 0;
}
```

Between `alloc` and `free` the buffer cycles *already constructed*, so a
workload that reuses the same object shape amortizes initialization across
every reuse. See [Slab and vmem]({{ '/design/slab-and-vmem/' | relative_url }})
for why.

## Next

- Replace an existing binary's allocator with no code change:
  [LD_PRELOAD]({{ '/guide/ld-preload/' | relative_url }}).
- The full C surface -- vmem arenas, the introspection API:
  [The C API]({{ '/guide/c-api/' | relative_url }}).
- Find a leak in a running process:
  [Debugging]({{ '/guide/debugging/' | relative_url }}).

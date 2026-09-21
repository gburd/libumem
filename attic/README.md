# attic/ — quarantined, not built, not supported

Nothing in this directory is compiled, installed, or distributed. There is
no configure flag that turns any of it on. It is kept only so the work is
recoverable; treat it as a design sketch, not as code.

Each entry below records *why* it is here, because "unfinished" is not the
same as "unfinished in a way you could fix by finishing it".

## `umem_percpu.c` / `umem_percpu.h` — per-CPU magazine caching

Removed from the build (and `--enable-percpu-caching` removed from
`configure.ac`) on 2026-09-21. The flag existed and appeared usable; turning
it on did not produce a slower allocator, it produced a build failure.
Captured evidence:
`docs/results/prefix-evidence/2026-09-21-percpu-build-failure.log`.

Compile errors, not warnings:

- `UMEM_PERCPU_CACHE()` indexes `cp->cache_percpu`, a field that does not
  exist in `umem_cache_t` (`umem_impl.h`).
- reads `umem_magazine_t.mag_size`, a field that does not exist.
- `umem_max_ncpus` is not declared in the headers this file includes.
- calls `umem_depot_alloc`, `umem_depot_free`, `umem_slab_alloc`,
  `umem_slab_free`, `umem_cache_free_debug` — all `static` inside `umem.c`,
  so they are not linkable from another translation unit.

Beyond the build, the design was not finished either:

- nothing ever called `umem_init_percpu()`, so even a building version
  would have left `umem_percpu_enabled == 0` and allocated nothing.
- the magazine reload overwrote `pc_loaded` and returned `pc_previous`
  without clearing the reference, so two paths could hold the same
  magazine.
- the free fast path dereferenced `pc_loaded` with no NULL check.

Reviving this means re-deriving the per-CPU layer against the current
`umem_cache_t`, not repairing these files.

## `umem_htm.c` / `umem_htm.h` — Intel TSX / hardware transactional memory

Removed from the build (and `--enable-htm` removed from `configure.ac`) on
2026-09-21. These files were never in any source list; `Makefile.am` claimed
they "have not been written" while they sat in the tree, and the header
advertised a "5-15% improvement". Neither statement was true.

- the depot fast paths the feature exists for are comments, not code;
  `umem_htm_depot_alloc()` unconditionally returns `NULL`.
- lock elision is unsound as written: `UMEM_HTM_TRY` never adds the
  fallback lock to the transaction's read set, so a thread holding the
  fallback lock does not abort a concurrent transaction. Two threads can
  believe they have exclusive access to the same depot.
- `_xtest()` is used as if a nonzero result proved the fallback lock was
  free. It proves only that *this* thread is inside a transaction.
- `_xabort()` requires an 8-bit immediate operand; it is called with a
  runtime variable, which does not compile on the paths that use it.

A correct implementation needs the standard subscribe-to-the-lock protocol
and a real depot fast path; there is nothing here to salvage incrementally.

## `test_percpu_cache.c`, `bench_percpu.c`, `bench_experimental.c`

Tests and benchmarks for the two features above. They were not in
`Makefile.am` either, so they have never been compiled by this project's
build. Kept with the code they exercise.

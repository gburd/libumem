# Finding: `examples/umem_palloc` non-PREALLOC (dynamic) contexts cannot allocate

**Severity:** Medium (example/experimental API). Any budget context created
without `UMEM_BUDGET_PREALLOC` silently fails every allocation.

**Discovered by:** Workstream H4, `test/property/prop_palloc.c`, on
`intel-lo` under ASan.

## Symptom

```
UmemBudgetContext *top  = umem_budget_create("top", 256*1024, UMEM_BUDGET_PREALLOC);
UmemBudgetContext *mid  = umem_budget_create_child(top, "mid",  64*1024, 0);   /* dynamic */
void *a = umem_budget_alloc(top, 1024);   /* a  != NULL  (prealloc parent) */
void *b = umem_budget_alloc(mid,  512);   /* b  == NULL  (dynamic child!)  */
```

`umem_budget_alloc` on a context created **without** `UMEM_BUDGET_PREALLOC`
always returns NULL, even when the budget and the parent budget both have
ample room.

## Root cause

`umem_palloc.c:create_dynamic_arena()` builds the arena as:

```c
ctx->arena = vmem_create(ctx->name,
    NULL, 0,          /* no initial span            */
    8,                /* quantum                    */
    NULL, NULL, NULL, /* no import / release / source */
    0, VM_NOSLEEP);
```

A vmem arena with **no initial span and no source/import callback** has no
backing address space to hand out, so every `vmem_alloc()` fails. The
comment in `create_dynamic_arena` claims the arena "grows on demand via the
default vmem heap", but no source arena is wired up, so it never grows.

The `UMEM_BUDGET_PREALLOC` path (`create_prealloc_arena`) works because it
`mmap`s a fixed region and passes it as the arena's initial span.

## Impact

- The dynamic-budget path advertised by the API is non-functional.
- The shipped demo hides this: `demo_stats_summary()` creates non-PREALLOC
  contexts and allocates from them, but never checks the return value, so
  the NULLs go unnoticed.

## Suggested fix (example owner)

Give the dynamic arena a real source, e.g.:

```c
ctx->arena = vmem_create(ctx->name, NULL, 0, 8,
    vmem_alloc, vmem_free, umem_default_arena /* or heap_arena */,
    0, VM_NOSLEEP);
```

so it imports spans on demand, or document that budget contexts require
`UMEM_BUDGET_PREALLOC`.

## Resolution (FIXED)

`create_dynamic_arena()` now wires the arena to libumem's heap arena as its
source, with `heap_alloc`/`heap_free` as the import/release callbacks (the
same idiom `umem.c` uses for `umem_internal_arena`, `umem_cache_arena`, etc.):

```c
vmem_t *heap = vmem_heap_arena(&heap_alloc, &heap_free);
ctx->arena = vmem_create(ctx->name, NULL, 0, 8,
    heap_alloc, heap_free, heap, 0, VM_NOSLEEP);
```

The arena now imports spans on demand from the heap. Budget enforcement is
unchanged: `umem_budget_alloc()` still checks `vmem_size(VMEM_ALLOC)` against
the budget (and the parent hierarchy) before every `vmem_alloc`, so NOWAIT
still returns NULL over budget, NOFAIL still `abort()`s, and backpressure
still blocks.  `umem_budget_reset()` was routed through the same helper so a
reset dynamic context keeps a working source.

Reproduction: the old span-less/source-less arena did not merely return
NULL -- `vmem_alloc()` on it **segfaults** on this build (VM_NOSLEEP with no
source has no backing to hand out).  `test/property/prop_palloc.c`'s
`prop_dynamic_arena_broken` was flipped to `prop_dynamic_arena_alloc`, which
now asserts a dynamic allocation succeeds *and* the budget is still enforced.
All palloc property tests pass and the full `test_main --no-fork` suite still
reports 459 OK / 0 failures on `intel-lo`.

## Effect on Workstream H4

`test/property/prop_palloc.c` proves budget enforcement, parent/child
hierarchy free, and shared-memory-survives-fork on the working
`UMEM_BUDGET_PREALLOC` path. It also pins this bug with
`prop_dynamic_arena_broken`, which asserts the *current* (broken) behavior
so that if the dynamic path is ever fixed the test flips and must be updated.

# Fix: magazine-cache slab-freelist corruption at high concurrency

**Bug:** the D3 concurrency oracle
(`docs/results/2026-07-24-concurrency-oracle-findings.md`) at 192 threads / 60 s
/ `--pattern=all` reproducibly drove the allocator into slab-freelist
corruption on the `umem_magazine_*` caches, surfacing as either a
`sp->slab_cache == cp` SIGABRT (`umem.c:1588`) via the PTC teardown flush, or a
SIGSEGV in the background reap thread (`umem_cache_reclaim_pages`, `umem.c:3851`).

**Status: FIXED.** Root cause was a `MADV_DONTNEED` range in
`umem_slab_reclaim()` that discarded the page holding a slab's embedded
`umem_slab_t` metadata. It is **not** a locking race; the slab-list locking was
already correct. Gate is GREEN on both arches (see bottom).

Built/run on EC2 per Global Constraints.

| role    | instance       | arch    | vCPU | kernel        | gcc    | glibc |
|---------|----------------|---------|------|---------------|--------|-------|
| intel-hi| c7i.metal-48xl | x86_64  | 192  | 6.18 amzn2023 | 11.5.0 | 2.34  |
| arm-hi  | c8g.metal-48xl | aarch64 | 192  | amzn2023      | 11.5.0 | 2.34  |
| intel-lo| c7i.2xlarge    | x86_64  | 8    | 6.18 amzn2023 | 11.5.0 | 2.34  |

Fix commit: `8640e17` on top of shipped `50830de` (v2.1.0).

---

## 1. Root cause (with gdb evidence, not assumed)

### 1.1 The aborting thread (authoritative backtrace)

Default `-O2` build (asserts live: no `NDEBUG`), core from the 60 s `all` run:

```
#4  __umem_assert_failed (assertion="sp->slab_cache == cp", file="umem.c", line=1588)
#5  umem_slab_alloc  (cp=0x7f4ed0635040 = "umem_magazine_255")  at umem.c:1588
#6  _umem_cache_alloc (cp=0x7f4ed0635040)                       at umem.c:2813
#7  _umem_cache_free  (cp=0x7f4ed007e040, buf=...)              at umem.c:3038
#8  umem_ptc_bin_flush (bin=..., size=...)                      at umem_ptc.c:427
#9  umem_ptc_destroy  (ptc=...)                                 at umem_ptc.c:264/473
#11 umem_ptc_cleanup  (arg=...)   <- pthread TSD destructor     at umem_ptc.c:114
#12 __nptl_deallocate_tsd
#13 start_thread
```

The other 191 threads are in `churn_worker -> umem_free -> vmem_mmap_free ->
mmap64` (freeing large buffers). The background `umem_update_thread` is parked in
`pthread_cond_timedwait` — i.e. it had recently run `umem_cache_update ->
umem_cache_reclaim_pages` over the magazine caches.

### 1.2 The corrupted slab

Inspecting the offending slab in the core, at `frame 5` (`umem_slab_alloc`,
`cp = umem_magazine_255`):

```
(gdb) print cp->cache_name        $1 = "umem_magazine_255"
(gdb) print sp                    $3 = (umem_slab_t *) 0x7f4e0da6bfb8
(gdb) print *sp
  $4 = {slab_cache = 0x0, slab_base = 0x0, slab_next = 0x0, slab_prev = 0x0,
        slab_head = 0x0, slab_refcnt = 0, slab_chunks = 0,
        slab_state = 2 /* SLAB_CLEAN */, slab_idle_time = 0,
        slab_reclaim_next = 0x0}
(gdb) print cp->cache_freelist    $6 = (umem_slab_t *) 0x7f4e0da6bfb8   # == sp
```

The freelist head is a slab whose **entire `umem_slab_t` is zeroed** — not a
foreign cache's slab, as first hypothesized, but this cache's own slab with its
metadata wiped. `slab_state = 2` is `SLAB_CLEAN` (the state
`umem_slab_reclaim()` sets), which is the smoking gun: this slab had just been
run through the page-reclaim path. The zeroed `slab_cache` (0x0 != cp) is what
trips the assertion; the zeroed `slab_next`/`slab_prev` are what SEGV the reap
walk in the other observed crash — same corruption, two observers.

### 1.3 Why the metadata got zeroed

Geometry from the same core:

```
(gdb) print sizeof(umem_slab_t)   $ = 72
(gdb) print cp->cache_slabsize    $ = 4096       # single page
(gdb) print cp->cache_maxcolor    $ = 1976
(gdb) print cp->cache_align       $ = 64
(gdb) print/x (unsigned long)sp   $ = 0x7f4e0da6bfb8   # sp + 72 = ...c000 (page end)
```

For a **non-hash** cache the `umem_slab_t` is embedded at the END of the slab:
`sp == UMEM_SLAB(cp, slab) == P2END(slab, slabsize) - 1`. Buffers occupy
`[slab + color, sp)`; the metadata occupies `[sp, slab + slabsize)`, in the
**same final page** as the buffer tail.

`umem_slab_reclaim()` did:

```c
size_t reclaim_size = cp->cache_slabsize - sizeof(umem_slab_t);   /* 4024 */
madvise(sp->slab_base, reclaim_size, MADV_DONTNEED);
```

`MADV_DONTNEED` operates on whole pages. Confirmed empirically on the instance:

```
madvise(page_aligned, 4024 /* < 4096 */, MADV_DONTNEED)  -> rc=0, whole page discarded
madvise(page+64      , 4024,             MADV_DONTNEED)  -> rc=-1 EINVAL (start not page aligned)
```

So for a slab with **color 0** (`slab_base` page-aligned), the sub-page-length
advise still made the kernel discard the *entire* final page — including the
embedded `umem_slab_t`. On next touch the kernel supplied a zero page, wiping
`slab_cache`, `slab_next`, `slab_prev`, `slab_head`. (For color != 0 the advise
start was unaligned and `madvise` returned EINVAL, so nothing was ever
reclaimed — a latent no-op, not corruption.)

This is a single-page-slab hazard: the magazine caches use `slabsize == 4096`,
so their slab metadata always shares the only page with the buffer region and
can never be safely `MADV_DONTNEED`'d.

### 1.4 Why it needs sustained 192-thread churn

Reclaim only fires on empty slabs idle past `umem_reclaim_delay`, and only on a
color-0 slab does the corruption trigger. The 60 s `multi` stage first has to
build a large magazine/depot population; the subsequent stages' thread-exit PTC
flushes then re-allocate from a magazine cache whose freelist head was just
zeroed by the reap thread. Shorter runs and low core counts never accumulate
the idle color-0 magazine slabs, which is why 8-vCPU and 25 s runs pass.

---

## 2. The fix (minimal, correctness-first)

`umem_slab_reclaim()` now reclaims only **whole pages strictly below the page
that holds the metadata**: round the start up and the end down to page
boundaries, and for a non-hash cache stop at `(uintptr_t)sp` (metadata start).
If no full page remains (the single-page magazine slabs), it reclaims nothing
and just marks the slab `SLAB_CLEAN`.

```c
uintptr_t start = P2ROUNDUP((uintptr_t)sp->slab_base, PAGESIZE);
uintptr_t limit = (cp->cache_flags & UMF_HASH)
    ? P2END((uintptr_t)sp->slab_base, cp->cache_slabsize)   /* metadata not embedded */
    : (uintptr_t)sp;                                        /* embedded at end */
uintptr_t end = P2ALIGN(limit, PAGESIZE);
if (end <= start) { sp->slab_state = SLAB_CLEAN; return; }  /* nothing safe to reclaim */
madvise((void *)start, end - start, MADV_DONTNEED);
```

Why this is the smallest sound change:

- It touches only the reclaim helper — the alloc/free fast paths and all
  slab-list locking are unchanged, so v2.1.0's PTC/magazine scaling is preserved
  (no wider lock scope, no serialization added).
- It is correct for both cache geometries: non-hash (metadata embedded at slab
  end) and hash (metadata separate — reclaim to slab-page end).
- Single-page slabs correctly reclaim nothing; multi-page slabs still reclaim
  their full interior pages. The prior code reclaimed nothing on those anyway
  whenever color != 0 (EINVAL), so no meaningful RSS regression.
- The `sp->slab_cache == cp` invariant is untouched — the symptom is fixed at
  its source, not masked.

---

## 3. A second, latent deadlock the fix exposed (also fixed)

With the corruption gone, the 192 t `all` run survived past the point that
used to crash and then WEDGED: every thread stuck in `pthread_mutex_lock` at
`umem_depot_pop` (`umem.c:1993`). Live-attach `thread apply all bt` (5 threads
left after the churn workers joined):

```
#2  umem_depot_pop     (cp=..., mlp=0x...86f600)   at umem.c:1993   <- blocked
#3  umem_depot_alloc   (cp=..., mlp=&cp->cache_empty)
#4  _umem_cache_alloc  (cp=...)
#5  _umem_cache_free   (cp=...)
#6  umem_magazine_destroy
#7  umem_maglist_ws_reap (cp=..., mlp=0x...86f600, full_rounds=127)  <- HOLDS 0x...86f600
#8  umem_depot_ws_reap
#9  umem_cache_reap
#10 umem_process_updates / umem_ptc_cleanup (other threads)
```

`umem_maglist_ws_reap` holds `mlp->ml_lock` across `umem_magazine_destroy`,
which frees the magazine struct back to `cp` via `_umem_cache_free` ->
`umem_depot_alloc` -> `umem_depot_pop`.  The striped `depot_alloc` scan lands
on the same stripe (`0x...86f600`) the reap already holds -> self-deadlock on
the non-recursive mutex.  This was pre-existing (the corruption just crashed
first); it lives in the same depot/magazine reap path.

**Fix (commit `6b30eb1`):** pop the reap candidates under `ml_lock` into a
private chain, release the lock, then destroy them unlocked -- the same
collect-then-do-slow-work-unlocked pattern `umem_cache_reclaim_pages` uses.

---

## 4. Gate results (GREEN)

All on EC2, fixed binary (`6b30eb1`).  No cores produced on any run.

### intel-hi (x86_64, 192 vCPU) -- 4/4 clean, default build

```
run 1  multi 964.0 Mops/s ok  prodcons ok  churn ok  -> Result: PASS  rc=0
run 2  multi 877.3 Mops/s ok  prodcons ok  churn ok  -> Result: PASS  rc=0
run 3  multi 803.6 Mops/s ok  prodcons ok  churn ok  -> Result: PASS  rc=0
run 4  multi 811.4 Mops/s ok  prodcons ok  churn ok  -> Result: PASS  rc=0
GATE_COMPLETE
```
(`--threads=192 --duration=60 --size-class=mixed --pattern=all`)

### intel-hi -- clean under --enable-asan (1x 60s)

```
multi 625.1 Mops/s ok  prodcons ok  churn ok
Result: PASS (no cross-thread aliasing or corruption)  rc=0  ASAN_GATE_COMPLETE
```
ASan reported no heap error; libumem's own invariant no longer trips.

### intel-hi -- ./scripts/ec2/oracle_matrix.sh 192 60 default

```
BUILD_OK mode=default ncpu=192 threads=192 dur=60s
small/multi  1272.9 Mops/s ok   -> PASS  exit=0
mag/multi     205.4 Mops/s ok   -> PASS  exit=0
mixed/all     805.3 Mops/s ok (multi+prodcons+churn) -> PASS  exit=0
large/churn     0.1 Mops/s ok   -> PASS  exit=0
MATRIX_COMPLETE
```

### arm-hi (aarch64, 192 vCPU) -- 1/1 clean, default build

```
multi 465.8 Mops/s ok  prodcons ok  churn ok
Result: PASS (no cross-thread aliasing or corruption)  rc=0  ARM_GATE_COMPLETE
```

### Non-regression (intel-lo, 8 vCPU)

- `test_main --no-fork`: **459 OK / 0 FAIL / 10 SKIP** (matches v2.1.0 baseline).
- `oracle_fast.sh`: PASS 3/3.

### Throughput non-regression

`small/multi` at 192 t on intel-hi = **1272.9 Mops/s**, vs the v2.1.0
reference of ~1290 Mops/s -- within ~1.4%.  The fixes touch only the cold
reap/reclaim paths; the alloc/free fast path and its locking are unchanged, so
v2.1.0's PTC/magazine scaling is preserved.

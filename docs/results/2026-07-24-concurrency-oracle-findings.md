# Concurrency oracle (D3) — design, cross-arch results, and a REAL allocator
# bug it found

**Status:** Oracle delivered and wired into `make check`. Passes at low/moderate
scale on all three arches. **At 192 threads over a sustained multi-stage run it
reproducibly drove the CURRENT allocator (PTC + magazine + depot) into an
internal slab-freelist corruption** that surfaced as either a
`sp->slab_cache == cp` assertion abort or a SIGSEGV in the background
reap/update thread. This was a genuine find in shipped v2.1.0 code, independent
of the (inert) rseq lock-free path.

> **GATE NOW GREEN (2026-07-24).** Root cause was a `MADV_DONTNEED` range in
> `umem_slab_reclaim()` that discarded the page holding a slab's embedded
> `umem_slab_t` metadata (single-page magazine slabs, color 0), zero-filling
> `slab_cache`/`slab_next`/`slab_prev`. Fixed in commit `8640e17`; a second
> latent depot-reap self-deadlock the fix exposed was fixed in `6b30eb1`. Full
> diagnosis, gdb evidence, and the GREEN gate results are in
> `docs/results/2026-07-24-slab-freelist-corruption-fix.md`. The failing rows
> in §3 below are now PASS with the fix (see that doc).

Built/run on EC2 per the repo Global Constraints:

| role     | instance          | arch    | vCPU | kernel                          | gcc     | glibc |
|----------|-------------------|---------|------|---------------------------------|---------|-------|
| intel-lo | c7i.2xlarge       | x86_64  | 8    | 6.18 amzn2023                   | 11.5.0  | 2.34  |
| intel-hi | c7i.metal-48xl    | x86_64  | 192  | 6.18 amzn2023 (Xeon 8488C, 2S)  | 11.5.0  | 2.34  |
| arm-hi   | c8g.metal-48xl    | aarch64 | 192  | amzn2023                        | 11.5.0  | 2.34  |

Code under test: git `992beb6` (oracle) on top of shipped `50830de` (v2.1.0).

---

## 1. Oracle design

`test/stress/stress_concurrency_oracle.c`. The premise: a correct allocator
hands each live buffer to exactly one owner at a time. Stamp every allocation
with a globally-unique owner token and re-check it; any mismatch means the
same live buffer was handed to two owners (aliasing / double-alloc) or
corrupted under contention.

- **Owner token:** `canary = (tid << 40) | (seq & ((1<<40)-1))` — unique per
  `(thread, allocation)`.
- **Stamp:** the token is written as a repeating 8-byte pattern across the
  *entire* usable buffer (plus a low-byte tail), so a short overrun by a
  different owner is also visible.
- **Verify** at three points:
  1. `alloc` — read-back immediately after stamping (catches a buffer that a
     second owner is concurrently stamping);
  2. `hold` — periodic re-scan of the whole live pool (catches a buffer that
     gets aliased to another thread *after* we stamped it);
  3. `free` / `consume` / `drain` — just before releasing (the primary
     aliasing catch).
- **No false positives on valid use:** each live buffer has one owner, so the
  token read back is always the one this thread wrote. The oracle passed
  cleanly across billions of ops at low/moderate scale (see §3).

**Patterns (why it is an oracle, not a smoke test):**

| pattern    | what it exercises |
|------------|-------------------|
| `multi`    | same-size-class hammering — all threads pound one PTC/magazine size class, maximizing depot refill / magazine swap contention + per-CPU migration (rseq reload path). |
| `prodcons` | cross-thread handoff — producers stamp+enqueue, consumers dequeue+verify+free. A buffer freed by a thread that did not allocate it exercises the depot return path and catches corruption in transit. |
| `churn`    | varied sizes across PTC/magazine/slab, with random pool eviction to force depot refills, magazine swaps, and slab reclaim. |
| `all`      | (default) `multi` → `prodcons` → `churn` back-to-back in one process. Each stage spawns and joins N threads, so `all` at N=192 does 576 thread create/exit cycles — every exit runs the PTC teardown flush. |

**Size classes** (`--size-class`): `small` (8–256 B, PTC-eligible ≤2048),
`mag` (512–8192 B, magazine), `large` (64–256 KB, slab-direct), `mixed`
(weighted blend, default).

**Modes:** `--threads=N`, `--iters=M` (per-thread op budget), `--duration=SECS`
(wall-clock per stage), `--pattern=multi|prodcons|churn|all`.

---

## 2. THE FINDING — magazine-cache slab-freelist corruption at high thread count

### Reproduction (reliable, cross-arch)

```
# default build (no ASan needed to trigger)
./scripts/ec2/clean-regen.sh && make -j$(nproc) test/stress/stress_concurrency_oracle
LD_LIBRARY_PATH=.libs ./test/stress/.libs/stress_concurrency_oracle \
    --threads=192 --duration=60 --size-class=mixed --pattern=all
```

- **intel-hi (x86_64, 192 vCPU):** aborts/segfaults 4/4 times. The `multi` and
  `prodcons` stages print `ok`; the process dies entering/at the `churn` stage.
- **arm-hi (Graviton, 192 vCPU):** same — dies at the same stage (rc=134).
- **intel-lo (8 vCPU):** does NOT reproduce (16 threads, 60s, all patterns pass).
  The bug needs high real parallelism + sustained magazine population, not mere
  oversubscription.
- **Shorter runs (25s × 3 stages) at 192 threads:** pass 3/3. The bug needs the
  ~60s `multi` stage to build a large magazine/depot population before the
  subsequent stages' thread-exit flushes hit it.

It is **not** an oracle token mismatch — the oracle's own canary check never
fired. It is libumem's *own* internal consistency check tripping, i.e. the
allocator detects that its slab freelist is corrupted.

### Symptom A — assertion abort (SIGABRT)

Authoritative backtrace of the aborting thread (default build, core 55265):

```
#4  __umem_assert_failed (assertion="sp->slab_cache == cp",
                          file="umem.c", line=1588)
#5  umem_slab_alloc (cp=0x...086e8040 = umem_magazine_NN)   at umem.c:1588
#6  _umem_cache_alloc (cp=0x...086e8040)                    at umem.c:2813
#7  _umem_cache_free (cp=0x...08130040, buf=0x...)          at umem.c:3038
#8  umem_ptc_bin_flush (bin=..., size=...)                  at umem_ptc.c:427
#9  umem_ptc_destroy (ptc=...)                              at umem_ptc.c:264/473
#11 umem_ptc_cleanup (arg=...)   <-- pthread TSD destructor at umem_ptc.c:114
#12 __nptl_deallocate_tsd
#13 start_thread
```

Meanwhile the other 191 threads are live in `churn_worker` → `umem_free` →
`vmem_mmap_free` → `mmap64` (freeing large buffers back to vmem).

**What this shows:** on thread exit, `umem_ptc_cleanup` flushes the per-thread
cache bins back to their size-class cache (`08130040`) via `_umem_cache_free`
(umem.c:3038). The depot has no empty magazine, so `_umem_cache_free` allocates
a fresh magazine from the shared **magazine-type cache** (`086e8040`,
`umem_magazine_NN`) — and `umem_slab_alloc` finds a slab on that magazine
cache's freelist whose `slab_cache` field points at a *different* cache. The
magazine-type cache's slab freelist has been corrupted (a slab belonging to
another cache got linked onto it) under concurrent depot/magazine traffic.

### Symptom B — SIGSEGV in the background update thread

A second run (default build, core 75722) crashed differently but in the SAME
subsystem:

```
#0  umem_cache_reclaim_pages (cp=0x...  cache_name="umem_magazine_1")
        at umem.c:3851   ->   next = sp->slab_prev;   (sp is a bad pointer)
#1  umem_cache_update      at umem.c:3991
#2  umem_cache_applyall    at umem.c:891
#3  umem_update_thread     at umem_update_thread.c:115
```

Here the background reap/update thread walks the `umem_magazine_1` cache's slab
list and dereferences a corrupted slab link. Same root subsystem (magazine-type
cache slab-list management), different observer — proving this is real memory
corruption of the slab list, not a benign assertion.

### ASan result

Built `--enable-asan`, same 192-thread `all` run: the process still aborts at
the same stage (rc=134), but **ASan reports no heap error** before the abort.
That is consistent with the diagnosis: the corruption is a *logical* slab
linked-list mismatch (wrong slab on a cache's freelist), not a byte-level
overflow/UAF that ASan's redzones would flag. The guard that fires is
libumem's own `sp->slab_cache == cp` invariant. **ASan clean; libumem's own
invariant not.**

### Assessment

- Real, reproducible, architecture-independent (x86_64 + aarch64), and
  ASan-clean-but-self-inconsistent.
- Lives in the **PThread-cache teardown ↔ magazine-type-cache slab management**
  interaction under high real parallelism, i.e. squarely in the depot/magazine
  path this oracle was built to guard — *before* the inert rseq lock-free path
  is even in play.
- FIXED (2026-07-24): the root cause was NOT in the rseq/scaling path but in
  `umem_slab_reclaim()` — a `MADV_DONTNEED` range that discarded the page
  holding a single-page slab's embedded `umem_slab_t` metadata (commit
  `8640e17`), plus a latent depot-reap self-deadlock the fix then exposed
  (commit `6b30eb1`). The oracle now passes at 192 threads / 60s / `all` on
  both arches, default and ASan. See
  `docs/results/2026-07-24-slab-freelist-corruption-fix.md` for the full gdb
  evidence and GREEN gate results. This doc's §3 rows are updated accordingly.

---

## 3. Results matrix (pass = oracle canary clean AND no allocator crash)

`iters` runs and low/moderate scale — all clean:

| arch     | threads | run                                   | result |
|----------|---------|----------------------------------------|--------|
| intel-lo | 8       | fast CI (`--iters=50000 --pattern=all`)| PASS ×5 |
| intel-lo | 16      | 60s multi small / mag; 60s all mixed; 60s churn large | PASS |
| intel-hi | 192     | 20s churn × {small,mag,large,mixed}    | PASS |
| intel-hi | 192     | 30s all mixed                          | PASS |
| intel-hi | 192     | 25s × 3 all mixed (×3 runs)            | PASS |
| intel-hi | 192     | 60s prodcons-only mixed                | PASS |
| arm-hi   | 192     | (short/individual as above)            | PASS |

Sustained high-scale multi-stage — **WAS FAIL, NOW PASS after fix
`8640e17`+`6b30eb1`** (see `2026-07-24-slab-freelist-corruption-fix.md`):

| arch     | threads | run                          | result (fixed) |
|----------|---------|-------------------------------|----------------|
| intel-hi | 192     | 60s `all` mixed (default)     | **PASS** 4/4 |
| intel-hi | 192     | 60s `all` mixed (`--enable-asan`) | **PASS**, ASan clean |
| intel-hi | 192     | `oracle_matrix.sh 192 60 default` | **PASS** (4 workloads) |
| arm-hi   | 192     | 60s `all` mixed (default)     | **PASS** 1/1 |

Throughput observed (context, not a perf claim): `multi/small` ~1290 Mops/s at
192t on intel-hi, ~315 Mops/s on arm-hi; magazine ~220 Mops/s; large-churn
~0.1 Mops/s (dominated by mmap/munmap). The oracle sustains billions of ops
per stage, which is why it reaches the corruption low-scale tests miss.

---

## 4. Fast CI invocation

Added `test/stress/oracle_fast.sh` (in `make check` `TESTS`):

```
./test/stress/.libs/stress_concurrency_oracle \
    --threads=8 --iters=50000 --size-class=mixed --pattern=all
```

~1–2 s, 8 threads, 400k ops/stage. Deterministically PASS on the current code
at this scale (5/5 on intel-lo), so it guards the common PTC/magazine/depot
paths in CI without dominating runtime. The heavy 192-thread / 60s / `all`
invocation (which finds the bug in §2) is documented here and in
`scripts/ec2/oracle_matrix.sh` for EC2 only.

### Heavy EC2 invocations

```
# full per-arch sweep (build default or asan, 4 patterns × size classes)
./scripts/ec2/run-remote.sh intel-hi './scripts/ec2/oracle_matrix.sh 192 60 default'
./scripts/ec2/run-remote.sh intel-hi './scripts/ec2/oracle_matrix.sh 192 60 asan'
./scripts/ec2/run-remote.sh arm-hi  './scripts/ec2/oracle_matrix.sh 192 60 default'

# the specific repro of the §2 bug
LD_LIBRARY_PATH=.libs ./test/stress/.libs/stress_concurrency_oracle \
    --threads=192 --duration=60 --size-class=mixed --pattern=all
```

---

## 5. TSan note

Not run: TSan cannot see the inline-asm rseq critical sections (documented in
the plan), and the found bug is a logical slab-list corruption that TSan would
at best flag as a benign-looking data race on `slab_cache`/`slab_prev` without
the causal chain the assertion + core backtraces already give. The oracle +
ASan + libumem's own invariant checks are the real guard here, and they
already localized the defect.

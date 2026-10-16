# snmalloc headline features, evaluated for libumem

Plan: `docs/plans/2026-10-16-adopt-from-snmalloc.md`, item 6. Source study:
`/tmp/snmalloc-lessons-for-libumem.md`. snmalloc's own headline list:
`~/src/snmalloc/README.md` ("key design features").

This is an **analysis-only** document (no EC2 available; see the plan's HARD
CONSTRAINT). Where a verdict would need a measurement to settle, it is marked
**measurement owed** rather than asserted. The point of the exercise is to
drive the roadmap, so a wrong ADOPT is expensive: every verdict below is tied
to libumem code (`file:line`) or a results doc, and anything inferred is
labelled inferred.

The two allocators are different animals. snmalloc is a header-only C++17/20
allocator built around a message-passing remote-free design and a per-thread
*allocator object* with its own identity. libumem is a C port of the Solaris
slab allocator: a size-class / slab / magazine / depot hierarchy with a
per-thread cache (PTC) and an optional Linux rseq per-CPU layer on top. Some of
snmalloc's mechanisms depend on structure libumem does not have, and the
honest verdict for those is DECLINE, not "aspire to."

## Verdict table

| # | snmalloc headline feature | verdict | one-line reason |
|---|---|---|---|
| 1 | Same-thread free needs no synchronising operation | **ALREADY-HAVE** | libumem's PTC bin push/pop is a thread-local array index, no atomics, no fences (umem.c:4755, 4460) |
| 2 | Remote free via message passing, 1000s of remote deallocs per atomic | **DECLINE** | needs per-allocator identity + MPSC return queue libumem lacks; a fundamental re-architecture (user guardrail). Gap is REAL and stays characterized-and-deferred |
| 3 | Large page ranges to reduce meta-data | **PARTIAL (already partly done)** | libumem already raised span density (UMEM_MIN_SLAB_OBJECTS / UMEM_MIN_QCACHE_SLAB) and the one-reservation mmap heap; the remaining snmalloc win (a separate pagemap) is a deeper change, not needed for the ceiling that was fixed |
| 4 | Two-branch malloc fast path | **ADOPT-as-goal (bounded)** | libumem's PTC-hit path is already close (~3-5 branches); matching exactly two is feasible in C but costs readability, so adopt the *intent* (keep the hit path near-minimal, measure branch count), not a literal rewrite |
| 5 | Platform dependencies abstracted (PAL/AAL) | **ADOPT** | = plan item 1; libumem's platform code is scattered and a `pal/`+`aal/` carve-out is a behaviour-preserving refactor that directly supports the cross-platform claims |

Plus the two "hard scenarios" snmalloc calls out (cross-thread free, batch
dealloc): covered at the end. Both map onto feature 2's decline; libumem's
standing on each is recorded from round-8/9 data.

---

## 1. "Same-thread free is lock-free" — ALREADY-HAVE

**snmalloc:** memory freed by the thread that allocated it needs no
synchronising operation.

**libumem:** the PTC (per-thread cache) is exactly this, and it is the default
on every build. The inlined fast paths in `_umem_alloc` / `_umem_free` do the
same-thread hit with no atomic, no lock, and no fence:

- Free hit (`umem.c:4755-4780`): `b = &ptc->bins[bin]; ... void **sp =
  &b->slots[b->count++]; *sp = UMEM_SLOT_MANGLE(sp, buf);` — one thread-local
  load of `thread_ptc` (a `__thread` pointer), a bounds compare against
  `ptc_bin_capacity(bin)`, a store, and a counter increment. No synchronising
  operation of any kind.
- Alloc hit (`umem.c:4460-4467`): `void **sp = &b->slots[--b->count];
  return (UMEM_SLOT_DEMANGLE(sp, *sp));` — symmetric pop.

The push is **deliberately not even ordered for `fork()`** (comment at
`umem.c:4760-4776`, rule in `umem_ptc.h`): every release-store / signal-fence /
plain-store ordering was measured at −4..−5 % at t=8 on c7i.2xlarge and none is
paid; the fork child instead drops the top slot of each bin. So libumem's
same-thread free is not merely lock-free, it is fence-free, which is as strong
as snmalloc's claim.

Two libumem-specific caveats, stated so the "already-have" is honest:

1. **The slot is mangled (P5.13).** `UMEM_SLOT_MANGLE` / `UMEM_SLOT_DEMANGLE`
   XOR the stored pointer with a per-process cookie keyed on the slot address.
   That is a few ALU ops on the hit path — a cost snmalloc's *unhardened* fast
   path does not pay, and snmalloc's *hardened* path pays a different (stronger,
   doubly-linked) encoding. libumem's is a security feature, not overhead to
   remove; it is the always-on hardening the README's honest framing describes.
2. **libumem has no per-thread *allocator*, only a per-thread *cache*.** The
   object a thread frees goes into *that thread's* PTC regardless of which
   thread allocated it. This is what makes same-thread free free — and it is
   also precisely why feature 2 (remote free) does not apply the way it does to
   snmalloc: there is no owning allocator to message. See feature 2.

**Verdict: ALREADY-HAVE.** No work. The PTC fast path is at least as
synchronisation-free as snmalloc's same-thread free, with the deliberate
addition of cookie mangling that libumem keeps for hardening.

---

## 2. "Remote free via message passing, 1000s of remote deallocs per atomic" — DECLINE

**snmalloc:** a free by a thread *other* than the allocating thread does not
lock. The object is batched into a multi-producer-single-consumer (MPSC)
message queue addressed to the *owning allocator*, which recycles it warm. A
whole batch costs a single atomic, so the scheme scales with core count and is
good at the two hard scenarios below.

**Why libumem cannot adopt this without re-architecting (the user guardrail).**
snmalloc's scheme rests on two things libumem does not have:

- **Per-allocator identity.** snmalloc objects know which allocator owns them,
  so a remote free can be *addressed*. libumem has no per-allocator object: it
  has a global size-class → cache table (`umem_alloc_table`), per-CPU depot
  stripes, and per-thread caches that are *caches*, not owners. An object
  carries no "who owns me" tag; its home is "whatever cache serves this size
  class," which is global.
- **An MPSC return queue per owner.** libumem's recycle path is *anonymous*: a
  freeing thread pushes a filled magazine onto **its own** CPU's depot stripe
  (`umem.c:3002-3005` spells this out — "`umem_depot_free()` pushes a freed
  magazine only onto the *freeing* thread's own local stripe"), and a later
  allocator on a different CPU *steals* it via the NUMA-aware scan
  (`umem.c:2944-3062`, local → same-node → remote-node → global). There is no
  queue back to an origin; there is no origin.

Grafting message-passing remote-free onto libumem means inventing per-allocator
identity and an owner-addressed MPSC queue, and rewiring the whole cross-thread
path around them. That is a fundamental change to what libumem *is* (a Solaris
slab port), not an adoption of a feature. The plan's SCOPE REVISION dropped it
for exactly this reason (item 4, dropped per user guardrail 1).

**The gap is real — this is not a decline-because-it-does-not-matter.** Round 9
measured it directly on x86 metal (`docs/results/r9f3/step0/README.md`), not
derived from perf share:

| `perf c2c` counter (c7i.metal-48xl, prodcons 1024:4096, t=192) | umem | jemalloc | umem/je |
|---|---|---|---|
| Load LLC Misses | 8,923,771 | 569,805 | **15.7x** |
| Load Remote HITM | 2,557,422 | 217,070 | 11.8x |
| Load Remote DRAM | 6,036,128 | 345,509 | 17.5x |

Throughput on the same runs: jemalloc ~4.5x umem on Graviton4, consistent on
Sapphire Rapids. The physical mechanism is confirmed on x86: jemalloc returns
a freed object to its allocating thread, so the producer re-mallocs warm lines;
libumem recycles anonymously, so the producer's `memset` repeatedly writes
cold or remotely-owned lines. snmalloc's design is the reference for *not*
doing this. The counters are x86-only; on arm the throughput gap reproduces
(4.5x) but the counter mechanism is inferred, not proven (SPE unavailable on
that image — a clean arm `remote_access` pass is **measurement owed** if the
decline is ever revisited).

**What libumem gives up by declining, and when it bites.** The honest cost of
staying with anonymous recycling, from round-8/9 data:

- **Cross-thread (producer/consumer) free at high core count.** This is the
  worst case. On 192 vCPU metal at 1-4 KB, umem is 5.8-8.8x behind jemalloc
  (r8rss F3). The gap *grows with core count* because the two per-CPU layers
  (classic cc + rseq) both fill with parked stock as cores multiply (54 % of
  metal RSS, r8rss F1). On 8 vCPU boxes umem is at or ahead of jemalloc, so
  this is specifically a many-core NUMA problem — today's hardware, which is
  the point of the evaluation.
- **RSS under cross-thread free.** The circulating pool is ~850 MB on metal
  (r8rss F1), far larger than LLC, which is both an RSS cost and the reason the
  producer's lines are cold. Return-to-owner would shrink the pool a producer
  cycles through; anonymous recycling does not.

**Round-9 already tried the cheap version and it did not help.** The
stripe-rotation prototype (route a freed magazine to a rotated stripe rather
than always the freeing CPU's) left remote traffic unchanged — the problem is
not *which* stripe, it is that no stripe is the allocating thread's. Only true
return-to-owner addresses it, and that needs the identity libumem lacks. So the
cheap fix is spent, and the real fix is the re-architecture the guardrail
forbids.

**Verdict: DECLINE.** The cross-thread locality gap is **characterized and
deferred**, not fixed and not phantom. Recorded here so the roadmap does not
re-litigate it: it is real (15.7x LLC misses), it hurts specifically at high
core count under cross-thread free and batch dealloc, the cheap mitigation was
tried and failed (round 9), and the only known cure is a fundamental change the
user has ruled out. If that constraint ever lifts, snmalloc's `freelist_mpscq`
and remote-dealloc path (`~/src/snmalloc`, `docs/AddressSpace.md`,
`src/test/func/freelist_mpscq/`) are the design to study, and the arm
counter-confirmation is the first owed measurement.

---

## 3. "Large page ranges to reduce meta-data" — PARTIAL (and libumem has already done the half that mattered)

**snmalloc:** allocate address space in large ranges and track it in a separate
pagemap, so per-object and per-slab meta-data shrinks and allocation memory
never doubles as meta-data.

**What libumem already does in this direction.** libumem's "too much
meta-data" problem showed up concretely as the Linux heap ceiling: a page-sized
quantum let a 4 KiB object sit one-per-4-KiB-slab, each slab costing a kernel
VMA, and the process hit `vm.max_map_count` (65,530) at ~5 GB. That is the
meta-data-per-span cost snmalloc's large ranges are designed to avoid, and
libumem fixed it *within the slab model*, not by re-architecting:

- **Span-density floor** (`umem.c:5979-5996`, `UMEM_MIN_SLAB_OBJECTS` /
  `UMEM_MIN_SLAB_CEILING` in `umem_impl.h:809-810`): a hashed-cache slab must
  hold ≥16 objects (capped at 64 KiB), restoring Solaris span density. Measured:
  2 GB of 4 KiB objects went from 16,283 VMAs to 75; 96 GB reached without a
  failure (`test/integration/test_heap_ceiling.c:19-24`).
- **Qcache slab floor** (`umem.c:5962-5976`, `UMEM_MIN_QCACHE_SLAB` =
  `4 * 1024 * 1024`, `umem_impl.h:857`): small (≤512 B) objects' one-page slabs
  come from the `umem_va` qcache; 4 MiB qcache slabs cost 57x fewer VMAs
  (15,702 → 274) at zero measured RSS cost.
- **One PROT_NONE reservation for the whole mmap heap** (`vmem_mmap.c:156-183`),
  committed RW with `MAP_FIXED` as spans are handed out, and freed spans
  returned by `MADV_DONTNEED` below 16 MiB (not `PROT_NONE`, which would split
  the VMA). This is already "large ranges with cheap per-span bookkeeping,"
  arrived at from the P6.2/P6.4 VMA-leak work.

So the *goal* snmalloc states — fewer, larger spans, less per-span meta-data —
is substantially met already, and the measurements prove it on the exact
ceiling that motivated it.

**The remaining snmalloc-specific win, and why it is not urgent.** snmalloc goes
further: a *separate pagemap* that maps any address to its size class and owner
without an in-band header, which (a) removes the per-object header entirely and
(b) enables sizeless free. libumem still keeps size-class routing through the
caller-supplied size (native API) or an in-band tag (the `LD_PRELOAD` drop-in
reads an 8-byte `malloc_stat` in front of the buffer, `malloc.c`; r8rss O5
showed that header read is a cross-core coherence miss and 37.6 % of the
wrapper's cost under cross-thread free). A pagemap would remove that read and
give a true sizeless free — but it is a deep change (a global radix/trie keyed
on address), it interacts with the span layout, and it is *not* needed for the
heap ceiling, which is already fixed. It is the same class of change as feature
2: a real win, not a drop-in.

**Verdict: PARTIAL.** The density/large-range half that fixed libumem's actual
meta-data pain is **done** (ADOPTED, in effect, before this evaluation). The
further snmalloc idea — a separate pagemap replacing in-band headers, enabling
sizeless free and removing the O5 coherence miss — is a worthwhile but separate
design, correctly a candidate not a commitment. **Measurement owed** if pursued:
the pagemap-vs-header trade-off needs its own A/B (lookup cost on the fast path
vs the saved coherence miss), which cannot be settled by reasoning.

---

## 4. "Two-branch malloc fast path" — ADOPT-as-goal (bounded)

**snmalloc:** on Linux/Clang the malloc fast path is two branches. This leans
on C++ metaprogramming (size-class tables resolved at compile time, the
allocator object in a register) to collapse the path.

**libumem's current PTC-hit alloc path, branch by branch** (`umem.c:4438-4467`,
the common case: size in range, PTC enabled, cache created, bin non-empty):

1. `index < UMEM_MAXBUF >> UMEM_ALIGN_SHIFT` — in-range size check (1 branch).
2. `likely(bin >= 0)` — PTC-eligible / not-disabled / not-hooked, one signed
   compare of the precomputed `umem_ptc_bin_table[index]` (1 branch).
3. `unlikely(ptc == NULL)` — thread-local cache exists (1 branch, predicted
   not-taken after the first allocation).
4. `likely(ptc != NULL)` — redundant with 3 on the hit path but present (1
   branch).
5. `likely(b->count > 0)` — bin non-empty (1 branch).

So the hit path is roughly **5 branches**, of which 3 and 4 are the lazy-init
guard for the very first allocation on a thread and are perfectly predicted
thereafter. snmalloc's "two" counts the essential ones (in-range, bin
non-empty); libumem's essential ones are the same two (1 and 5), with the PTC
eligibility check (2) and the init guard (3/4) as libumem-specific extras.

**Can C match exactly two without snmalloc's metaprogramming?** Mostly, with
cost:

- Branches 3/4 (the init guard) could be removed by making `thread_ptc` always
  point at a valid (possibly empty) PTC after first use, so the hit path never
  tests for NULL — but something must still create it lazily, and moving that
  off the hit path for *every* thread's first touch is the awkward part in C
  (no constructor-on-TLS-access). It is doable (a never-NULL sentinel PTC whose
  empty bins force the slow path) but adds a sentinel and complexity.
- Branch 2 (PTC eligibility) is load-bearing: it folds "PTC disabled," "debug
  cache," "ineligible size," and "T1/T8 hooks on" into one signed compare
  (comment at `umem.c:4448-4451`). Removing it means those four conditions each
  need their own test somewhere, which is worse, not better. This branch stays.

So libumem can plausibly reach **~3 essential branches** (in-range, eligible,
non-empty) with a never-NULL PTC sentinel, and arguably fold in-range into the
table lookup, but reaching a literal two would trade real readability and the
one-compare eligibility fold for one branch. That is not obviously worth it:
the init guard is already predicted-away, so the dynamic cost is already near
snmalloc's, and the hot-path A/B rig (`hotpath_ab.sh`) is the arbiter, not the
static count.

**Verdict: ADOPT-as-goal (bounded).** Adopt the *intent* — treat the PTC-hit
branch count as a tracked quantity, keep it near-minimal, and justify any
addition with the A/B. Do **not** adopt a literal "rewrite to exactly two
branches" target: libumem is C, the eligibility fold is worth its branch, and
the init guard is free dynamically. Any concrete reduction (e.g. the never-NULL
sentinel removing branches 3/4) is a normal perf change under the AGENTS.md §10
bar: fast path in the A/B null, before/after recorded. **Measurement owed**
before claiming any reduction helps: the branch count is static, the win is
not, so a sentinel-PTC A/B is required to say it is faster rather than just
shorter.

---

## 5. "Platform dependencies abstracted" — ADOPT

**snmalloc:** a formal Platform Abstraction Layer (`src/snmalloc/pal/`, ~21
OSes, defined by a C++20 Concept so a missing method is a compile error) plus an
Arch Abstraction Layer (`src/snmalloc/aal/`), documented by `docs/PORTING.md`.

**libumem today:** platform code is scattered — `getpcstack.c`,
`x86_subr_compat.c`, `i386_subr_sol.s`, `sparc_subr_sol.S`, `vmem_mmap.c`,
`init_lib.c`, the rseq assembly (`umem_rseq_{x86_64,aarch64}.S`), and
`#ifdef __linux__ / __FreeBSD__` sprinkled through `umem.c`. There is no single
place that answers "what must a port provide" and no compile-time contract.

This is plan item 1, and it is the one snmalloc mechanism that transfers
cleanly because it is **behaviour-preserving**: a `pal/` + `aal/` carve-out
*moves* the mmap / getentropy / page-size / yield / cpu-count / abort / atomics
shims behind one interface; it does not change the allocator. That fits the
"don't fundamentally change libumem" guardrail exactly. The C analogue of
snmalloc's Concept is one header enumerating the required symbols plus a
compile-time `#error` cascade (or a PAL smoke test that instantiates the set),
and a `docs/porting/` contract modelled on libdb's per-OS override dirs and
snmalloc's `PORTING.md`.

**Verdict: ADOPT.** This is plan item 1 (design-now, cross-compile/test on EC2
later — BLOCKED until a fresh account, see the plan). Cross-reference the PAL
design once `docs/design/PAL.md` and `docs/porting/` land (owned by the item-1
work and `@r10docs`); this evaluation does not duplicate that design, it only
records the verdict and the rationale that it is a refactor, not a
re-architecture. **Measurement owed:** none for the verdict (it is a structural
refactor); validation owed is the cross-compile+test matrix (QEMU-user
riscv64/armhf/aarch64, a real illumos/SPARC run if hardware exists), per the
plan's item-1 validation-owed list.

---

## The two "hard scenarios"

snmalloc's README names two workloads that "cause massive reductions in
performance of other allocators, but do not for snmalloc." Both are the
cross-thread story of feature 2; here is where libumem stands on each, from
round-8/9 data.

### A. Allocations on one thread freed by a different thread (cross-thread free)

**libumem is weak here at high core count, strong at low.** This is the
prodcons workload. On 8 vCPU boxes libumem is at or ahead of jemalloc (r8rss
F3). On 192 vCPU metal at 1-4 KB it is 5.8-8.8x behind, with 15.7x the LLC
misses (r9f3). The mechanism is anonymous recycling vs snmalloc/jemalloc's
return-to-owner, as detailed in feature 2. **Verdict inherited: DECLINE the
cure (re-architecture), characterize-and-defer the gap.** The one cheap thing
that *does* help RSS without re-architecting — F1-a PTC trim for free-only
threads (r8rss: `tcache=0` cuts RSS 35 % at zero throughput cost) — is a
footprint fix, not a locality fix, and is tracked separately in the round
plans; it does not touch the 15.7x-LLC-miss throughput gap.

### B. Deallocations in large batches (batch dealloc)

**libumem's magazine layer is designed for batching, but the batch still
recycles anonymously.** A batch of frees fills the thread's PTC bin, then its
loaded/previous magazines, then a full magazine is pushed to the freeing CPU's
depot stripe (the overflow cascade in `_umem_free`, `umem.c:4789` onward; the
freeing-CPU-stripe rule is `umem.c:3002`). Within a
thread that is efficient — a full magazine moves as a unit under one depot lock,
which is libumem's analogue of "many deallocs per atomic." Where it still loses
to snmalloc is the *cross-thread* batch: the batched magazine lands on the
freeing CPU's stripe, not back at the allocating thread, so the same cold-line
cost as scenario A applies when another thread re-allocates. For a
*same-thread* batch (alloc N, free N on one thread) libumem is fine and the
magazine priming fix (P8.6, `umem_ptc_mag_prime`, `umem.c:3254`) specifically
removed the cliff that used to appear at the bin boundary (137 Mpairs/s at
N=128 after the fix, per `umem_ptc.h`). **Verdict: same-thread batch is
ALREADY-HAVE (magazines batch under one lock); cross-thread batch inherits the
DECLINE** — it is the same anonymous-recycling gap, not a separate problem.

---

## Summary for the roadmap

- **1 Same-thread lock-free:** ALREADY-HAVE. The PTC fast path is fence-free;
  nothing to do but keep it that way (and keep the P5.13 mangling, which is
  deliberate hardening, not overhead to cut).
- **2 Remote free message passing:** DECLINE. Real 15.7x-LLC-miss gap at high
  core count under cross-thread free; cure requires per-allocator identity +
  MPSC queue libumem does not have; cheap mitigation already failed (round 9).
  Characterized-and-deferred, with the arm counter pass as the first owed
  measurement if ever revisited.
- **3 Large page ranges:** PARTIAL — the density/large-range half that fixed
  the heap ceiling is done; a separate pagemap (sizeless free, no in-band
  header, removes the O5 coherence miss) is a candidate design, A/B owed.
- **4 Two-branch fast path:** ADOPT-as-goal — track and minimise the PTC-hit
  branch count (~5 now, ~3 essential), but do not chase a literal two; any
  reduction is a normal A/B'd perf change.
- **5 Platform deps abstracted:** ADOPT — plan item 1, a behaviour-preserving
  PAL/AAL refactor; cross-compile/test validation owed on EC2.

No library code is changed by this document. It records verdicts and the
evidence behind them so the roadmap work (plan items 1-6) proceeds from a
written baseline rather than re-deriving each judgement.

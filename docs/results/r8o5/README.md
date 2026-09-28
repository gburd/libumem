# r8o5: prodcons frees the umem API arm with the ring's size (O5)

Agent @r8o5, 2026-09-28. **Harness-only change** (`test/bench/*`); no library
code touched. Plan: `docs/plans/2026-09-28-round8.md`, O5. Diagnosis it acts
on: `docs/results/r8rss/README.md`, section O5.

## The finding (recap)

The benchmark's umem **API** arm (`allocator_umem` in `test/bench/allocators.c`)
wraps every allocation with a 16-byte `umem_size_header_t` so its sizeless
`free()` can recover the size for `umem_free(ptr, size)`. In the prodcons
workload the consumer reads `header->size`/`header->magic` on a line the
**producer** wrote on another core -- a cross-core coherence miss that r8rss
measured as `umem_free_wrapper` at ~37.6 % of samples (x86 annotate: 99 % of
the function's samples on the instruction after the `header` load). That
distorts the umem-vs-others comparison for the API arm.

The prodcons ring **already carries each buffer's size** (`ring_slot_t.size`,
written by the producer, returned by `ring_pop`), so the consumer can free
with that size and never touch the header.

## The change

- `allocator_ops_t` (bench_framework.h) gains an optional
  `free_sized(void *ptr, size_t alloc_size)`. When set, a caller that already
  knows the size it passed to `alloc()` frees through it instead of the
  sizeless `free()`. NULL => use `free()` (the default for every allocator via
  designated initializers).
- `allocator_umem` sets `free_sized = umem_free_sized_wrapper`, which
  reconstructs the raw allocation address (`ptr - header`) and size
  (`alloc_size + sizeof(header)`) **arithmetically** and calls
  `umem_free()` -- no `header` read. The header is still written by
  `umem_alloc_wrapper` (so `umem_free_wrapper`, `realloc`, and the
  single/multi/frag arms that free without a caller size keep working); the
  prodcons consumer just no longer reads it.
- `consumer_thread` (bench_framework.c) calls `free_sized(ptr, sz)` with the
  ring's size when the allocator offers it, in all three of its free sites
  (last-interval drain, timed free, final drain). Every other workload
  (single/multi/frag) frees without a known size and is unchanged. libc /
  jemalloc / others leave `free_sized` NULL and are unaffected.
- The **LD_PRELOAD drop-in** arm (`umem_preload_free`) is deliberately left
  sizeless: a real `malloc()/free()` caller has no size at free time, so that
  path must recover the size from the in-band `malloc_stat` tag -- the same
  class of miss, but a **class-(ii) library** question (page-map sizeless
  free), not a harness one. A `ponytail:` note marks it; not implemented here.

## Test: fails-before, passes-after (in one run)

`test/bench/test_bench_accounting.c::test_prodcons_free_sized` uses a mock
allocator whose `free()` reads a size header (increments a counter) and whose
`free_sized()` does not. It runs prodcons twice:

- **fix path** (`free_sized` set): `header_reads == 0`, every op through
  `free_sized`.
- **control** (`free_sized == NULL`, i.e. pre-fix behaviour and every sizeless
  allocator): the consumer falls back to `free()`, so `header_reads == ops`.

The control is the pre-fix state and it fails the "0 header reads" assertion;
the fix path passes it. Both are asserted in the same executable, so the
regression demonstrates the exposure it closes.

### Result (intel-lo@r8o5, c7i.2xlarge, Debian 13, isolated `79ae76f`)

```
=== bench harness accounting regressions (P2.1, P2.2) ===
...
prodcons: size-carrying free skips the in-band header (O5)
   free_sized set : header_reads=0 sized_frees=400000 ops=800000
   free_sized NULL: header_reads=400000 plain_frees=400000 ops=800000
...
Result: PASS (0 failures)
```

`header_reads=0` with the fix; `header_reads=400000` (= ops/2, one free per
op-pair) in the control. All 12 accounting cases PASS.

The accounting test is pure harness arithmetic and C control flow with **no
architecture-dependent code** in the changed paths, so the arm result is not
in question; the arm box was re-run for the perf work (below).

## Perf before/after (INCOMPLETE -- methodology bug, see below)

Intent: `perf record` umem prodcons `-D 5:4 -W 1 -T 16 -t 8` at `8c70e66`
(before) and `79ae76f` (after), report `umem_free_wrapper` (or its
`umem_free_sized_wrapper` replacement) self %, and confirm umem throughput
rises-or-unchanged while libc/jemalloc stay within run-to-run spread.

**Not completed in this task's budget.** The first perf pass was run as
`perf record -g -- bench_main ...` and reported as `perf report -g none | head`.
That captured the whole process tree (the profile's top frames were
`execve`/`_Fork`/`clone`/bash, not the workload) and the throughput lines were
not parsed (the grep pattern did not match bench_main's output). The data is
not trustworthy and is NOT reported as a result.

**What a correct redo needs** (for the next agent / a follow-up call):
- `perf record -g` on the bench_main PID only (it self-execs? no -- bench_main
  is a single process; the tree frames came from the `verify-isolated` shell
  wrapper's `$(nproc)` subshells being sampled too). Restrict with
  `perf record -p <pid>` after launch, or `perf stat`/`perf record` the binary
  directly without the surrounding `sh -c` doing command substitution during
  the sampled window.
- Sort by self time: `perf report --stdio --no-children -g none` (the run used
  the default children view, which shows call-graph roots at the top).
- Parse throughput from bench_main's actual CSV/human output (it prints
  `Throughput: N ops/sec` per interval; capture and take the median of the 3
  kept intervals).
- Compare **same arch** before vs after (intel-before vs intel-after,
  arm-before vs arm-after), not cross-arch.

The mechanism is sound and matches r8rss's annotate (the header load is a
cross-core miss; removing the read removes the miss), but per this project's
evidence rules a perf delta is only a result once it is measured cleanly. It
is not claimed here.

## Provenance

| box | instance | arch | ref(s) |
|---|---|---|---|
| intel-lo@r8o5 | c7i.2xlarge, Debian 13 | x86_64 | acct + perf @ `79ae76f`, perf @ `8c70e66` |
| arm-lo@r8o5 | c7g.2xlarge, Debian 13 | aarch64 | perf @ `79ae76f`, `8c70e66` (first arm box hit a post-bootstrap SSH key failure and was replaced) |

Isolated via `scripts/ec2/verify-isolated.sh` (committed content only).

## Commits

- `79ae76f` test(bench): O5 -- prodcons frees umem API buffers with the ring's
  size, skipping the header.

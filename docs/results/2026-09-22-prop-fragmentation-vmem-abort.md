# `prop_fragmentation` aborts: `VM_SLEEP` reaches `vmem_populate()`

**Date:** 2026-09-22
**STATUS: FIXED** (2026-09-22, same day). The record below is the original
diagnosis, unchanged. What fixed it, three separate defects:

| Defect | Fix | Commit |
|---|---|---|
| `vmem_populate()` ASSERTed on `VM_SLEEP` (aborted, no diagnostic; vanished under `NDEBUG`) | reports ENOMEM via `log_message`, `vmem.c:604-628` | `dd658b1` |
| the test passed `VM_SLEEP` (11 sites, two files) to an API whose header says it is unsupported | tests use `VM_NOSLEEP` | `52c24b3` |
| the test freed with `umem_free(ptr, 0)`; reachable only once `QCC_getValue()` was fixed and the property actually ran | free with the original size | `188e3c0`, `216f3ac` |

Gate exemption removed in `7ba9914`, which also deleted this file. Restored
2026-09-24 (comment review #5, class 5): `vmem.c:617` cites it, and `docs/`
is durable (AGENTS.md §8). The "Not fixed here, deliberately" and
"Relationship to the heap ceiling" sections below were written before the
fix; the ceiling's suggested quantum fix later failed (`553d42e`) and the
ceiling was closed by slab density instead (`3f2e67c`, `umem_impl.h`
`UMEM_MIN_SLAB_OBJECTS`; `docs/results/2026-09-22-umem-heap-ceiling-vma.md`).
Plan entry: `docs/plans/2026-09-21-production-readiness.md`, "Resolved since
the first gate run".

---

**Found by:** the coordinator's exit-criteria gate, which ran the property tests
that `make check` does not.

## Symptom

```
$ LD_LIBRARY_PATH=.libs ./test/property/.libs/prop_fragmentation
Aborted (core dumped)          # rc=134
```

No output on stdout or stderr. The abort is libumem's own assertion:

```
__umem_assert_failed(assertion="vmflag & VM_NOSLEEP", file="vmem.c", line=603)
  vmem_populate       (vmem.c:603)   vmflag=256
  vmem_xalloc         (vmem.c:875)   size=4096 align=4096
  vmem_alloc          (vmem.c:1134)  vmflag=0
  vmem_mmap_alloc     (vmem_mmap.c:107)
```

## Mechanism

`vmem_populate()` refills an arena's segment-structure free list. When it must
go to its source for more, it asserts the caller is non-blocking:

```c
ASSERT(vmflag & VM_NOSLEEP);    /* we do not allow sleep allocations */
```

The backtrace shows `vmem_alloc(..., vmflag=0)` from `vmem_mmap_alloc()` —
`VM_SLEEP`, i.e. flag 0 — reaching that path. `vmflag=256` at the assertion is
`VM_PUSHPAGE`, with `VM_NOSLEEP` clear.

So a blocking allocation can reach a populate path that requires a non-blocking
one. In a debug build that aborts; with `NDEBUG` the assertion vanishes and the
code proceeds into a path its own comment says is not allowed.

## Pre-existing

Confirmed at the pre-wave baseline:

| Commit | `prop_fragmentation` |
|---|---|
| `ebcb467` (before this workstream) | **rc=134, aborts** |
| `e5cd027` (current) | rc=134, aborts |

Identical failure. Nothing in the 2026-09-21 work caused or worsened it.

## Why it went unnoticed

`prop_fragmentation` is **not in `TESTS`**, so `make check` never ran it — on
any commit, including every release. It is built by default, so it looked like
covered code. The property-test binaries only get exercised if someone runs them
by hand, and the last person to do so evidently did not.

This is the same shape as the other evidence defects found this session: a test
that exists, builds, and is never consulted.

## Relationship to the heap ceiling

Both this and `docs/results/2026-09-22-umem-heap-ceiling-vma.md` are
address-space/segment-supply problems under a large fragmented live set, and
both surface through `vmem_mmap_alloc()`. They are probably worth investigating
together: the ceiling is about running out of VMAs, this is about the
segment-structure refill path being entered with the wrong blocking discipline.
Fixing the quantum (the ceiling's suggested fix) changes how often this path is
reached, so neither should be fixed without re-running the other's check.

## Not fixed here, deliberately

The correct fix is either to give `vmem_mmap_alloc()` a `VM_NOSLEEP` discipline
on this path, or to make `vmem_populate()` tolerate a sleeping caller. Both are
core address-space changes in `vmem.c`/`vmem_mmap.c` with consequences for the
populate reserve and the no-sleep lock, and both need a regression that drives
an arena to segment exhaustion. Landing that inside an exit-criteria pass,
without its own test, is exactly the pattern the readiness plan forbids.

## Reproducing

```sh
export AWS_PROFILE=hotdog
./scripts/ec2/launch.sh intel-lo@vmem && ./scripts/ec2/bootstrap.sh intel-lo@vmem
./scripts/ec2/verify-isolated.sh intel-lo@vmem HEAD vmem 1800 \
  './scripts/ec2/clean-regen.sh && make -j$(nproc) && \
   LD_LIBRARY_PATH=.libs gdb -batch -ex run -ex "bt 10" \
     ./test/property/.libs/prop_fragmentation'
./scripts/ec2/terminate.sh intel-lo@vmem
```

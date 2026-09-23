# P5.4: which control actually blocks the freelist attack

**Date:** 2026-09-23
**Commit tested:** `b6ecd3a` (HEAD of the P5.4 work)
**Hardware:** `c7i.2xlarge` x86_64, isolated builds via `verify-isolated.sh`
**Author:** coordinator, verifying the P5.4 agent's report rather than accepting it

## Why this exists

The P5.4 report states that the fix "mangles" the freelist link so an
attacker-chosen address is no longer returned, and cites the attack regression
passing post-fix and aborting pre-fix. Both halves are true. But the report
attributes the block to the **mangling**, and that attribution is wrong for this
attack shape.

Verified by isolating the two controls independently:

| Mangling | Link validation | `test_freelist_mangle` attack case |
|---|---|---|
| off (`-DUMEM_NO_LINK_MANGLE`) | off (validator neutered) | **rc=134, abort — primitive reached** |
| off (`-DUMEM_NO_LINK_MANGLE`) | on | blocked, reported via `umem_error` |
| on (default) | on | blocked, reported via `umem_error` |

Confirmed the "off" build really was off rather than trusting the flag:
`Makefile` shows `CC = gcc -DUMEM_NO_LINK_MANGLE`, and `objdump -d umem.o`
contains **zero** references to `umem_link_cookie`.

## Conclusion

**`umem_slab_link_valid()` is the load-bearing control for this attack shape,
not the mangling.** Its containment check —

```c
if (!(cp->cache_flags & UMF_HASH) &&
    !UMEM_SLAB_MEMBER(sp, UMEM_BUF(cp, bcp)))
        return (0);
```

— rejects any link pointing outside the slab, and the test's attacker target is a
static in the test binary, which is far outside. So the attack is refused before
mangling is ever consulted.

That is not a criticism of the fix. Both controls are worth having, and the
layering is correct:

- **Validation** stops any link that leaves the slab. That covers the classic
  case: aim a freed buffer's link at a chosen address elsewhere in the process.
- **Mangling** is what remains when an attacker aims *within* the same slab, and
  what stops them predicting a usable stored value at all. It is the control that
  degrades gracefully when validation's structural checks are satisfiable — which
  the code's own `ponytail:` comment is candid about: "structural checks only,
  not a MAC."

## Consequence for the evidence

The existing regression **cannot distinguish the two controls**, so it does not
demonstrate that mangling works. It demonstrates that the *combination* blocks
the attack, and that removing both restores the primitive.

A regression that isolates mangling needs an attacker target **inside the victim
slab**, so containment passes and only the mangling stands between the overwrite
and an arbitrary-address return. That test does not exist yet.

**Recorded as a gap rather than fixed here**, because writing it is P5.4 work and
this document is a verification of P5.4, not a continuation of it. Until it
exists, the honest claim is:

> The freelist attack shape is blocked, and structural validation is what blocks
> the tested case. Mangling is implemented and reviewed but is not independently
> covered by a failing test.

That is the same distinction Phase 3 had to make about the rseq subtraction:
compiled and reviewed is not the same as covered.

## What was verified good

- The cookie derivation (`umem.c:1462`) uses `AT_RANDOM` via `getauxval` — no
  syscall, no allocation, which matters because `umem_init()` cannot allocate —
  and is genuinely idempotent from fixed inputs, so it needs no lock, CAS or
  `pthread_once`. The ASLR+pid fallback is documented as weaker rather than
  presented as equivalent.
- `umem_slab_link_valid()` does not dereference a rejected link.
- Every `bc_next` reader in the tree was enumerated and dispositioned, including
  the `umem_inspect.c` sites the agent correctly overrode my instruction to edit,
  and the HASH allocated-address chains correctly left plain.
- The throughput methodology is the strongest in this repo so far: the agent
  discarded its own first A/B as between-process noise, then built a **null
  control from two bit-identical builds** to establish the rig's resolution
  (±4%) before quoting a −0.27% median, and stated plainly that a ~3% cost at 96
  threads can be neither confirmed nor excluded because the null was only run at
  1 and 8 threads. It also recorded that aarch64 metal was never measured
  (capacity errors), rather than quietly omitting it.

## 2026-09-23 follow-up: the gap is closed

A new `inslab` case in `test/unit/test_freelist_mangle.c` aims the overwritten
link at the **live neighbour** -- inside the victim slab and `UMEM_ALIGN`-aligned,
so both structural checks pass and only the mangling stands in the way. The
target being a *live* buffer also makes the primitive precise: success means a
double allocation.

Isolated on `c7i.2xlarge` at `c8d83bd`, with the unmangled build confirmed via
`objdump` (0 references to `umem_link_cookie` in `umem.o`):

| Build | `inslab` |
|---|---|
| default (mangled) | **PASS** -- `alloc1=(nil)`, the demangled garbage was rejected; live buffer never returned |
| `-DUMEM_NO_LINK_MANGLE` | **FAIL** -- `alloc2 == live`: the allocator handed back a buffer still allocated |

So the verified claim is now the full one: **containment blocks out-of-slab
targets, and mangling blocks in-slab targets**, each demonstrated by a test that
fails when that specific control is removed. "Compiled and reviewed but not
covered" no longer applies to the mangling.

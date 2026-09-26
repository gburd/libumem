# P8.5b-arch: feed the armed rseq layer through the PTC magazine refill (2026-09-26)

**Branch:** `p85b-arch` (off master `5744436`). **Worker:** `@r5rseq`, Debian.
**Prior state:** `p85b-r4rseq2` armed the rseq lock-free per-CPU reload and
proved it SAFE but STARVED (`docs/results/2026-09-25-p85b-rseq-starvation.md`):
`rseq_alloc = 0` under every config because PTC drains the depot's `cache_full`
list before the rseq slowpath ever looks.

## The starvation, precisely located

The live hot path (inlined in `_umem_alloc`/`_umem_free`, umem.c) has three
thread/CPU-local tiers before the shared depot:

1. PTC bin (`ptc->bins[bin].slots[]`, thread-local L1).
2. PTC per-thread magazine (`ptc->mags[bin]`, loaded+previous, thread-local L2).
3. On a PTC-magazine miss, **PTC refills a whole magazine directly from the
   depot** via `umem_depot_alloc_trylock(cp, &cp->cache_full)`
   (umem.c ~4105 alloc; ~4368 free).

`_umem_cache_alloc` — which holds the armed rseq fast path + slowpath — is only
reached as the LAST resort, after the PTC depot trylock returns NULL. So the
rseq slowpath's own `umem_depot_alloc(cp, &cp->cache_full)` finds the list
already drained by tier 3: `no_full = 100%`, `armed = 0`, `rseq_alloc = 0`.

Confirmed on this branch, intel-lo Debian, before any change (probe_rseq_armed
8t/8s): `rseq_alloc = rseq_free = 0` across every cache.

## The architectural choice: revival option 2 (PTC refills THROUGH rseq)

The prior writeup named two revival options. Option 1 (rseq ahead of PTC)
inverts the whole hot path and pays the rseq fast path even when the
thread-local PTC bin would have served lock-free — a t=1 regression risk and it
abandons PTC's thread-locality. Option 2 keeps PTC in front (no change to the
common PTC-bin hit, so t=1 is protected) and inserts the rseq per-CPU magazine
as the tier the PTC per-thread magazine refills *through*, replacing the direct
`umem_depot_alloc_trylock`.

**Chosen: option 2.** When the PTC per-thread magazine is empty and needs a
refill, and rseq is armed-capable for this cache, fill the fresh PTC magazine by
popping buffers through the existing rseq alloc fast path (arming the rseq layer
via its already-safe slowpath when `cache_rseq[cpu]` is empty), instead of
pulling a whole magazine straight from the depot. Symmetric on the free side:
flush the full PTC magazine by pushing buffers through the rseq free fast path.

Why this shape:

- **Reuses the proven-safe machinery verbatim.** The migration-safe commit asm,
  the `old_rounds` ABI, and the `umem_ptc_mag_return` depot classification are
  all already on the branch and gated. Nothing new touches `cache_rseq[cpu]`
  except the existing fast path (single writer, kernel-atomic) and the existing
  slowpath commit (rseq-atomic). The invariant "exactly ONE per-CPU magazine
  layer owns a given CPU's current magazine" holds: the rseq layer owns it; the
  PTC magazine is a per-*thread* copy filled from it, never a second per-CPU
  owner.
- **Depot contracts unchanged.** The only depot interaction is still the rseq
  slowpath's single `umem_depot_alloc(&cache_full)` per rseq-magazine, and its
  `umem_ptc_mag_return` of the outgoing magazine — both already audited against
  invariants 1-2. The PTC magazine refill no longer touches the depot directly
  when rseq serves it.
- **Reversible and gated.** If rseq can't serve (not armed, not asm-safe,
  migration storm exhausts the bounded retry), the code falls straight through
  to the original `umem_depot_alloc_trylock` path, which is unchanged and always
  correct. Turning rseq off (`umem_rseq_enabled=0`) restores exact prior
  behaviour.

### The granularity note (why per-buffer, not per-magazine)

The rseq layer pops/pushes single buffers; the PTC magazine wants a magazine's
worth. We fill the PTC magazine buffer-by-buffer through the rseq fast path. The
depot is still hit only once per rseq-magazine (the slowpath arm), so depot
amortization is unchanged; the added cost is N rseq-fastpath calls per PTC-mag
refill (N = magsize), amortized over the N allocations that PTC magazine then
serves. Whether that cost is a net win over the direct depot magazine pull is an
empirical question — measured below, and the change is kept only if it beats the
85 us sustained p999 without regressing t=1.

## Fork / exit-drain

The PTC magazine refill already sets `ptc->fork_busy` around the swap (P1.3d).
Routing the fill through the rseq fast/slow path adds no new *unordered* per-CPU
owner: the rseq layer was already a live path in `_umem_cache_alloc`; this only
changes *when* it is reached. The outgoing-magazine handoff still goes through
`umem_ptc_mag_return`, which the P1.3a/P1.3d oracles already cover. Gated behind
the same `fork_busy` window as today.

## STATUS

(filled in after the gates run — see below)

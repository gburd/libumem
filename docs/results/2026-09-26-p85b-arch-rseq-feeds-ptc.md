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

**Resolved: the routed rseq layer FIRES, all gates pass, and it beats the
85 us sustained p999 target.** Worker `@r5rseq`, Debian 13 (trixie), GCC 14.2,
glibc 2.41 (glibc-managed rseq -> `umem_rseq_asm_safe=1` on BOTH x86_64 and
aarch64). Branch tip at gate time: this commit's parent line on `p85b-arch`.

### Does the layer fire? (counter evidence)

The predecessor's two probes could not show firing:
- `probe_rseq_armed` self-recycles each thread's own buffers, so it stays in
  the PTC L1 bin / L2 magazine and never MISSES the PTC magazine -- the new
  `umem_rseq_ptc_alloc/free` routing is only reached on a PTC-magazine miss,
  so it was never exercised.
- `probe_rseq_prodcons` had a racy MPMC ring (producers read `head`, write
  `ring[h]`, then store `head` with no exclusive claim -> two producers share
  a slot -> one buffer double-consumed -> double free, aborting in
  `umem_cache_reap` at `slab_refcnt >= 1`). Reproduced identically with
  `UMEM_RSEQ_OFF=1` (rseq bypassed), so it was a PROBE bug, not an allocator
  bug. Fixed the ring (CAS-claim `head`, per-slot `ready` handshake).

Added `test/stress/probe_rseq_batch.c`: each thread allocs a batch > magsize,
holds it, frees it -> drains and fills the PTC per-thread magazine so BOTH
PTC-miss routing paths fire, with no cross-thread buffer sharing (no
double-free hazard). Same binary, `UMEM_RSEQ_OFF` A/B, `-DUMEM_RSEQ_ARM_DEBUG`,
`UMEM_DBG_RSEQ_PROBE=1`:

| metric (umem_alloc_64, 8t/8s intel-lo) | rseq ON | rseq OFF |
|---|---|---|
| rseq_alloc | 257,482,015 | 0 |
| rseq_free  | 257,528,383 | 0 |
| armed      | 760,651 | 0 |
| cc_alloc (mutex path) | 7,505 | 37,444 |
| dep_local (depot pulls) | 1,507,788 | 10,791,773 |

The layer that was starved (`armed=0`, `rseq_alloc=0`, `no_full=100%`) now
serves 257 M allocations lock-free and cuts depot pulls ~7x. Confirmed at
192t on metal too: intel-hi `rseq_alloc=57.8M armed=198k`, arm-hi
`rseq_alloc=241M armed=908k` (both with the expected handful of migration
aborts, correctly retried: intel abort=17, arm abort=56).

### Mandatory soundness gate: stress_concurrency_oracle 192t/60s mixed all

Debian metal, BOTH arches, DEFAULT and `--enable-asan` (vm.max_map_count
raised to 1966080 for asan). Zero aliasing, zero corruption, zero alloc
failures in every configuration:

| box | config | allocs_ok | result |
|---|---|---|---|
| intel-hi c7i.metal-48xl | default | 391,379,323 | PASS |
| intel-hi c7i.metal-48xl | asan    | 936,218,219 | PASS |
| arm-hi  c8g.metal-48xl  | default | 673,676,258 | PASS |
| arm-hi  c8g.metal-48xl  | asan    | 1,465,835,954 | PASS |

ASan under 192t churn (where CPU migration AND the once-per-cache magazine
resize both happen) reported no heap overflow, no use-after-free, no aliasing.

### Sustained p999 A/B (the ship criterion): intel-hi c7i.metal-48xl, 192t

`sustained_load.sh umem 60 192`, prodcons 64:256, same binary, `UMEM_RSEQ_OFF`
toggled, 3 measured windows each (1 warmup discarded):

| prodcons p999 (ns) | BEFORE (rseq off) | AFTER (rseq on) |
|---|---|---|
| window 0 | 87,490 | 44,504 |
| window 1 | 93,698 | 48,667 |
| window 2 | 102,642 | 46,542 |
| **median** | **~93.7 us** | **~46.5 us** |

p99 also improved (7.2 us -> 6.1 us) and prodcons RSS at peak dropped
(~300 MB -> ~133 MB, less depot magazine retention). The tail is roughly
halved and lands well under the 85 us target. The `frag` workload p999 is
unchanged (~1.7 ms, dominated by large allocations that never touch the depot
magazine tail -- expected).

### t=1 fast path A/B (not regressed): intel-lo, multi 16:256, 5 runs each

| t=1 | rseq ON (median) | rseq OFF (median) |
|---|---|---|
| ops/sec | 5,734,864 | 5,704,456 |
| p50/p90/p99 (ns) | 34/37/39 | 34/37/39 |

Statistically identical (rseq on marginally faster, within noise). Expected:
at t=1 the PTC L1 bin serves the common case and the rseq routing is only
reached on a PTC-magazine miss.

### Other gates (all PASS)

- `make check` (intel-lo): 50 PASS, 3 SKIP, 0 FAIL (with
  `vm.max_map_count=65530`, the AL2023-equivalent default). This includes the
  fork oracles `test_fork_ptc_drain_probe` (P1.3d) and
  `test_ptc_thread_exit_drain_probe` (P1.3a), and the new
  `test_norseq.sh` (the runtime off switch). The 3 SKIPs are the
  `test_introspect_*` trio (no `--enable-introspect` channel to test),
  identical on AL2023 and Debian. On Debian's default kernel
  (`vm.max_map_count=1048576`) two MORE skip -- `test_heap_ceiling` and
  `test_heap_ceiling_512.sh` -- because that limit is above the test's 200000
  meaningfulness threshold (a permissive kernel cannot distinguish the density
  fix from a permissive kernel); they PASS once the limit is at 65530, so this
  is a distro kernel-default artifact, not a hidden failure or a missing
  Debian prerequisite.
- 6-bug rseq suite `test_rseq_fastpath`: PASS.
- `repro_rseq_trailing_store`: PASS (0 leaked, 0 double-presence,
  966k+1.9M signals delivered).
- `repro_reload_commit_safe`: PASS (migration-safety gate holds).
- `repro_naive_reload_race`: RACE CONFIRMED (rc=1 is this repro's intended
  outcome -- it demonstrates why the asm reload is needed).
- `test_ptc_slot_mangle`, `test_mag_round_mangle`, `test_freelist_mangle`
  (P5.13/P5.13b): PASS.
- `test_ptc_thread_exit_drain`, `test_ptc_resize_no_loss` (P1.3a/P1.3c): PASS.
- `test_fork_mt_load` (P1.2): 300/300 forks, 2,672,193 allocs = frees.

### Known bounded ceiling (not corruption)

`cache_rseq[cpu].magsize` is set once at cache creation to the cache's INITIAL
magtype `mt_magsize` and is never updated on a magazine resize. Magtypes only
grow, so the free fastpath (bounded by `rc->magsize`) can only UNDER-fill a
physically-larger depot magazine -- never overflow (ASan confirms: 0 overflow
reports across 2.4 B allocations at 192t). The alloc slowpath commits
`rc->magsize` rounds from a pulled full magazine; if that magazine is
physically larger (only possible for a size class that STARTED at a small
magtype and grew -- the default alloc classes 8..256 start at magsize 255, the
max, so they are immune), the rounds above `rc->magsize` ride along untouched
and are reclaimed intact on the next swap-out via `umem_ptc_mag_return`, which
classifies by the returned round count. No buffer is lost or double-issued
(oracle: 0 aliasing over 3.4 B allocations). The residual cost is at most a
modest under-utilization of an oversized magazine for the few large size
classes that resize -- an efficiency ceiling, not a correctness or leak
hazard. `ponytail:` if a large-class p999 ever matters, track the current
magtype into `rc->magsize` at resize time (single writer, the update thread).

### Runtime off switch (added per merge condition)

`UMEM_OPTIONS=norseq` disables the routed layer with no rebuild:
`umem_rseq_init` is skipped, `umem_rseq_enabled` stays 0, and every allocation
falls back to the always-correct `cc_lock`/depot path. Honoured under
`AT_SECURE` (disabling has no file/socket/exec side effect).
`test/stress/test_norseq.sh` proves both directions on intel-lo Debian:

| | rseq_enabled | rseq_alloc (total) |
|---|---|---|
| default (no option) | 1 | 234,031 |
| `UMEM_OPTIONS=norseq` | 0 | 0 |

### Ship decision

**Ship to master.** Every mandatory gate passes on both arches and both build
configurations, the layer demonstrably fires (0 -> 257 M served), and the
sustained p999 is roughly halved to ~46 us, beating the 85 us criterion, with
no t=1 regression and no fork/exit regression.

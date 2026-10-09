# Round 9 F3 -- owner-routed recycling prototype (branch `r9f3`)

**VERDICT (@r9f3c, metal measured): DO NOT MERGE -- characterized-but-deferred.**
Gates 1, 2, 4 GREEN on both arches (fast path byte-identical OFF, A/B inside
the null, soundness ON+OFF). Gate 5 (RSS) PASS: no regression. But gate 3 FAILS
the merge bar: owner routing produced NO non-overlapping throughput win on any
arch/thread-count (x86 t=192 ON actually REGRESSES), and the x86 perf c2c
mechanism check shows the remote-HITM / LLC-miss / remote-DRAM VOLUME it was
designed to attack did NOT drop (within +-2% ON vs OFF). The lever is measured
and characterized here; it stays on branch `r9f3` and does not touch master.
Measured on c7i.metal-48xl (x86) + c8g.metal-48xl (arm), Debian 13, account
170848442262, post library 3c5a247 for both OFF and ON.

Base: branch `r9f3` from `origin/master` **4bbfe44**. Prototype commit
**3c5a247**. Account 170848442262, profile hotdog, Debian EC2.

## The mechanism (step 0, already CONFIRMED, see step0/README.md)

x86 metal perf c2c, umem vs jemalloc, cross-thread-free 1-4 KB t=192:
15.7x LLC misses, 11.8x remote-HITM, 17.5x remote-DRAM. umem recycles freed
objects anonymously: a consumer's full magazine is pushed onto the FREEING
CPU's own depot stripe, so under prodcons (producers alloc on node A, consumers
free on node B, disjoint CPU sets) every full magazine piles on a node-B stripe,
a producer's node-A stripe is empty, and `umem_depot_alloc()` falls through its
local scan (step 2) to the remote cross-node steal (step 3). The producer then
memsets objects whose lines a remote consumer last wrote.

## What was built

`UMEM_OPTIONS=owner_recycle=1` (default OFF). When on, a FULL magazine on free
is pushed to a NODE-ROTATED depot stripe (round-robin via `umem_owner_rotor`)
instead of the freeing CPU's own stripe, spreading full magazines across every
node's stripes so a cross-node producer finds one on its OWN node (step 2)
rather than always stealing cross-node (step 3). Empty magazines are unchanged.

Diff: 3 files, +66/-4. The gate lives ONLY inside
`umem_depot_free()` / `umem_depot_free_trylock()` -- the magazine full/empty
slowpath reached under `cc_lock`, never per-op.

- `umem.c`: `umem_owner_recycle` global + `umem_owner_rotor`; the two depot-free
  functions branch on it for the full list only.
- `envvar.c`: `owner_recycle` option (ITEM_UINT, AT_SECURE-safe: pure perf knob,
  no file/socket/exec/disclosure side effect).
- `umem_base.h`: extern.

**Soundness invariant (stated in the code):** this only changes which depot
stripe INDEX a magazine lands on. The magazine still moves atomically from a
thread-owned loaded slot (under `cc_lock`) onto exactly one depot `ml_list`
(under that stripe's `ml_lock`): no second owner, no leak, no object handed to
two callers. Fork drains the PTC and magazines (P1.3d) regardless of stripe.
The rotor is a plain relaxed atomic; a torn increment only picks another valid
stripe.

## Gate 1 -- fast path byte-identical with the feature OFF: PASS (both arches)

objdump of `_umem_alloc` / `_umem_free` / `_umem_cache_alloc` /
`_umem_cache_free`, pre=4bbfe44 vs post=3c5a247. Instruction COUNTS identical
per symbol on both arches:

| symbol            | x86 pre/post | arm pre/post |
|-------------------|--------------|--------------|
| _umem_alloc       | 373 / 373    | 373 / 373    |
| _umem_free        | 512 / 512    | 452 / 452    |
| _umem_cache_alloc | 309 / 309    | 278 / 278    |
| _umem_cache_free  | 501 / 501    | 446 / 446    |

Every residual objdump diff is relocation drift from adding unrelated globals:
RIP-relative displacements, `@@Base±offset` symbol annotations, PLT/call rel32
targets (identical target SYMBOL), and `__LINE__` immediates passed to
assert/log helpers (debug-only constants shifted because umem.c grew lines).
No instruction inserted or removed; the owner-recycle branch is confined to the
depot slow path. Reproduce: `scripts/ec2/r9f3_gate1.sh` style normalization.

## Gate 2 -- fast-path A/B inside the null, insn/pair +0.00: PASS (both arches)

`hotpath_ab.sh`, pre=4bbfe44 post=3c5a247, feature OFF (default), t=1 and t=8,
9 replicates/point. EVERY A/B `insn/pair` median = **+0.00%** (noise band
+-0.06..0.15%) on x86 and arm. Every A/B throughput median is inside its null's
min..max. Full tables: `fastpath/hotpath-ab-intel.txt`,
`fastpath/hotpath-ab-arm.txt` (to be fetched).

## Gate 4 -- soundness: PASS (both arches, feature ON and OFF)

`p85b_fix_lo_gate.sh 30` rc=0 (OFF baseline): `make check` 0 FAIL (72 total,
61 pass / 11 skip), rseq 6-bug suite, trailing-store, commit-safe,
resize-leak 16/32, alias-e2e, commit-abort 0/30, oracle small/mag/mixed/large.

Feature ON (`UMEM_OPTIONS=owner_recycle=1`): `make check` 0 FAIL both arches;
`test_fork_ptc_drain` PASS (P1.3d fork-drain holds); `test_ptc_thread_exit_drain`
PASS; `repro_rseq_resize_leak` PASS; 8-thread oracle small/mag/mixed/large PASS.

## Gate 3 (metal throughput) + perf mechanism -- MEASURED (@r9f3c): NO WIN

`scripts/ec2/r9f3_metal.sh` on fresh c7i.metal-48xl (x86) and c8g.metal-48xl
(arm). prodcons 1024:4096 at t=192 AND t=384, `-D 60:10 -W 5` (10-min run, first
5 min dropped), 3 runs per (config,threads), umem OFF vs umem ON, same post
library 3c5a247, `numactl --interleave=all`,
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536`. The non-overlap rule:
a win counts ONLY if the ON ops/s range across all 3 runs' steady intervals
(n=15) does NOT overlap the OFF range.

### Throughput (ops/s, min..max across 15 steady intervals of 3 runs)

| arch | t   | OFF min..max (mean)      | ON min..max (mean)       | verdict |
|------|-----|--------------------------|--------------------------|---------|
| x86  | 192 | 138.6..147.4M (141.6M)   | 132.7..136.7M (134.7M)   | **ON WORSE** (ON max 136.7 < OFF min 138.6; ranges disjoint, wrong way) |
| x86  | 384 | 141.5..146.6M (143.8M)   | 143.2..147.5M (144.9M)   | OVERLAP (143.2..146.6 shared) -> no win |
| arm  | 192 | 147.0..163.0M (154.6M)   | 142.5..160.1M (154.1M)   | OVERLAP -> no win |
| arm  | 384 | 127.6..138.9M (132.2M)   | 131.5..133.4M (132.2M)   | OVERLAP -> no win |

### Tail latency (ns, median / max across the same intervals)

| arch | t   | OFF p99 med/max | ON p99 med/max | OFF p999 med/max | ON p999 med/max |
|------|-----|-----------------|----------------|------------------|-----------------|
| x86  | 192 | 1184 / 1233     | 1270 / 1384    | 4128 / 4143      | 4122 / 4454     |
| x86  | 384 | 1028 / 1065     | 1156 / 1227    | 3026 / 3216      | 3646 / 3861     |
| arm  | 192 | 1261 / 1337     | 1295 / 1418    | 4720 / 4788      | 4740 / 5389     |
| arm  | 384 | 1664 / 1775     | 1775 / 1833    | 6134 / 7017      | 6199 / 6557     |

ON tail latency is equal-or-slightly-worse everywhere; never better.

### Gate 5 -- RSS (peak live-heap RSS, MiB): PASS, no regression

| arch | t   | OFF peak | ON peak | delta  |
|------|-----|----------|---------|--------|
| x86  | 192 | 1002.6   | 1019.3  | +1.7%  |
| x86  | 384 | 1653.4   | 1629.8  | -1.4%  |
| arm  | 192 | 1000.5   | 1032.2  | +3.2%  |
| arm  | 384 | 1600.9   | 1588.9  | -0.7%  |

ON is within OFF's run-to-run spread (both directions; LOWER at t=384 on both
arches). Owner routing did NOT re-create F1's parked stock -- gate 5 is met.
The problem is gate 3, not gate 5.

### Mechanism -- the point of owner routing, and why it failed

x86 `perf c2c` (t=192, `-D 60:7 -W 2`, data -> tmpfs /tmp), OFF vs ON, SAME
library -- the counters the feature was supposed to move:

| x86 c2c counter (volume)       | OFF         | ON          | delta  |
|--------------------------------|-------------|-------------|--------|
| Load Remote HITM               | 2,244,133   | 2,198,124   | -2.0%  |
| Load LLC Misses                | 7,647,247   | 7,684,261   | +0.5%  |
| Load Remote DRAM               | 5,001,180   | 5,045,530   | +0.9%  |
| LLC Misses to Remote DRAM      | 65.4%       | 65.7%       | ~flat  |
| LLC Misses to Remote cache HITM| 29.3%       | 28.6%       | ~flat  |

**The remote traffic did not drop.** Owner routing spread full magazines across
node-rotated depot stripes, but the remote-HITM / LLC-miss / remote-DRAM volume
-- the exact cost step 0 identified -- is unchanged within +-2%. The producer
still memsets lines a remote consumer last wrote. Node-rotation evidently does
not land a producer's refill on a magazine whose objects it (or a node-local
peer) last touched; under disjoint producer/consumer CPU sets the freed objects
were written on the consumer node no matter which stripe index receives the
magazine, so rotating the stripe index does not change line ownership.

arm `perf stat`: the targeted PMU events (`remote_access`, `ll_cache_miss_rd`)
were unavailable on this Graviton4 (same limitation @r9f3a hit); perf fell back
to the generic event set, so arm has no remote-access volume counter. The
fallback shows ON executed +8.6% instructions (150.5e12 vs 138.5e12) and more
sys time (18635s vs 16680s) for identical work -- extra rotor/spread overhead
with no locality payoff, consistent with the x86 c2c result and the flat arm
throughput.

### Verdict against the merge bar

Merge requires (2) a non-overlapping throughput win AND the mechanism dropped
AND (5) no RSS regression. Gate 5 passes; gates 2's win and the mechanism both
FAIL. Two of three required conditions are absent, and x86 t=192 regresses. Per
the round-9 bar ("If the ranges overlap or RSS regresses, this stays a
characterized-but-deferred lever ... and does NOT merge"), **this does not
merge.** The hypothesis -- that routing full magazines to a producer's own node
would relieve the remote-cache cost -- is measured false at the depot-stripe
granularity: stripe index does not control which node physically wrote the
object's lines. A finer design (tag objects/rounds with the WRITING cpu and
refill from same-writer magazines) would be a different, larger change on the
magazine round layout, not this stripe-index rotor; it is out of scope for a
fast-path-free prototype and is left as the upgrade path.

Raw data: `docs/results/r9f3/metal/{x86,arm}/` -- `csv-{off,on}-t{192,384}-r{1,2,3}.csv`,
`c2c-{off,on}.report.txt` (x86), `stat-{off,on}.txt` (arm), `prov.txt`.
Full job log (SUMMARY block): `docs/results/jobs/intel-hi-r9f3c-metal/`.
Box-hours: ~2.5 h each on c7i.metal-48xl and c8g.metal-48xl; both terminated.

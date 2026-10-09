# Round 9 F3 -- owner-routed recycling prototype (branch `r9f3`)

**Status as of this writeup: gates 1, 2, 4 GREEN on both arches; metal A/B
(gate 3) + RSS (gate 5) + perf mechanism were IN FLIGHT on c7i.metal-48xl /
c8g.metal-48xl when the worker's tool budget ran out. NOT yet merged.** The
decision (merge vs characterized-deferred) depends on the metal throughput/RSS
result, which a follow-up must fetch from `~/libumem-metal-r9f3b/out/` on the
metal boxes (or re-run `scripts` below) before touching master.

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

## Gate 3 (metal throughput + RSS) + perf mechanism -- IN FLIGHT, verdict pending

`scripts` on the metal boxes: `~/libumem-metal-r9f3b/repo` built r9f3 tip;
prodcons 1024:4096 at t=192 AND t=384, `-D 60:10 -W 5` (10-min run, first 5 min
dropped), 3 runs per (config,threads), umem OFF vs umem ON (same post library).
Per-interval CSV captures ops/s, p99, p999, peak RSS. Then a perf pass per
config: perf c2c (x86) / perf stat remote_access (arm), perf data to /tmp
(tmpfs 189 GB), to show the remote-HITM / LLC-miss volume ON vs OFF.

**Merge bar for gate 3/5:** a CLEAR win = umem-ON ops/s beats umem-OFF with
NON-OVERLAPPING ranges across the steady intervals of all 3 runs, AND no RSS
regression (ON peak RSS within OFF's spread or lower), AND c2c/remote_access
shows the remote traffic dropped ON vs OFF. If the ranges overlap or RSS
regresses, this stays a characterized-but-deferred lever (like F1-a) and does
NOT merge -- write the measured result here and leave it on `r9f3`.

Output to fetch: `~/libumem-metal-r9f3b/out/` on intel-hi@r9f3b and
arm-hi@r9f3b -- `csv-{off,on}-t{192,384}-r{1,2,3}.csv`, the SUMMARY block in
`~/libumem-jobs/metal/out.log`, `c2c-{off,on}.report.txt` (x86),
`stat-{off,on}.txt` (arm).

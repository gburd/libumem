# @r7h4: T1/T8 x86 512/N=128/t=1 -7 %: change cost or layout?

Boxes: intel-lo@r7h4 c7i.2xlarge Debian 13 gcc 14; arm-lo@r7h4 c7g.2xlarge
**AL2023 gcc 11** (it launched with the AL2023 AMI despite EC2_OS=debian; login
ec2-user). Hooks OFF, default build. hotpath_ab.sh, 9 alternating pairs, null
first, one bench_pairs binary per run. Scripts: `*.sh` here. Raw logs:
`intel/r7h4/`, `arm/r7h4/`.

## Step 1: minimise (r7hook3 ae9af7e; 20a4f8e tried and reverted by the tip commit)
- ae9af7e drops `umem_cache_alloc_tail`. Both pop paths in `_umem_cache_alloc`
  keep f40a2da's inline `umem_cache_alloc_debug` under one mask test that now
  includes UMF_HOOKED; the `umem_hook_alloc` tail call sits inside that
  unlikely arm.
- 20a4f8e brought back f40a2da's inline buftag block in `_umem_free`, on the
  guess that dc4f29c's `umem_free_redzone_ok` factoring was what grew it. That
  guess was **wrong**: at 20a4f8e the function is still 468 (+22 .cold), the same as dc4f29c.
  The +34 comes from the hooked bin<0/oversize arms, i.e. the change itself.
  REVERTED, so r7hook3's tip has the same content as ae9af7e.

objdump insn counts (x86 | arm; "@n" = start address mod 64):

| fn | f40a2da | dc4f29c | ae9af7e | 20a4f8e |
|---|---|---|---|---|
| _umem_cache_alloc | 262@32 \| 260 | 283@0 \| 280 | 293@0 \| 282 | 293 |
| _umem_cache_free | 447@16 \| 436 | 457@0 \| 448 | 461@16 \| 447 | 461 |
| _umem_alloc | 349 \| 351 | 353 (+10 .cold) \| 379 | 353 (+10 .cold) \| 379 | 353 (+10 .cold) |
| _umem_free | 434 \| 394 | 468 (+22 .cold) \| 416 | 468 (+22 .cold) \| 416 | 468 (+22 .cold) |
| umem_cache_alloc_tail | - | 31 \| 33 | - | - |

The ae9af7e `_umem_cache_alloc` diff against f40a2da (intel/r7h4/disasm-diff.txt)
is the `testb $0x6` -> `mov; test $0x2006` mask plus the cold arm: the inline
debug call, the UMF_HOOKED test and the `jmp umem_hook_alloc` tail. The hot
straight line is otherwise the same instructions with the same registers. Inlining the
debug call brought back gcc's inline copy of the arm, so the function
is +31 insns, not the +21 of the call. `_umem_cache_free`: +14, i.e. the same
mask change and a register renaming (r12<->rbp) across the whole function.

## Step 2: perf stat, x86, 512 N=128 t=1, 5 runs per arm, pre=f40a2da post=ae9af7e
`frontend_retired.dsb_miss` and `.l1i_miss` are `<not supported>` on this
VM, so `idq.*`, `icache_data.stalls` and `baclears.any` stand in for them.

| per pair (median) | f40a2da | ae9af7e | delta |
|---|---|---|---|
| instructions | 141.005 | 140.995 | -0.01 % |
| cycles | 25.74 | 27.70 | **+7.63 %** |
| idq_uops_not_delivered.core | 16.62 | 28.94 | **+74.1 %** |
| idq.dsb_uops | 135.23 | 134.41 | -0.60 % |
| idq.mite_uops | 0.152 | 0.102 | -32.9 % |
| branch-misses | 0.0314 | 0.0314 | +0.01 % |
| L1-icache-load-misses | 0.0014 | 0.0016 | +12 % (abs 0.0002/pair) |
| icache_data.stalls | 0.0053 | 0.0064 | (abs 0.001/pair) |
| baclears.any | 0.0005 | 0.0005 | noise |
| pairs/s | 109.0 M | 101.2 M | -7.22 % |

At the same instruction count and branch misses, 12.3 more uops/pair go
undelivered by the front end, and those account for the +2 cycles/pair. The
DSB still delivers almost every uop, and I-cache misses and BACLEARs are
negligible. So this is a uop-delivery/alignment effect (DSB line or
LSD/branch-placement), not extra work and not I-cache pressure.

## Step 3: A/B pre=f40a2da post=ae9af7e (median %, null min..max)
| point | x86 null | x86 A/B | arm null | arm A/B |
|---|---|---|---|---|
| 512 N=1 t=1 | -0.10 (-0.92..+0.77) | +0.81 | +0.01 (-0.27..+0.33) | +0.70 |
| 512 N=64 t=1 | -0.01 (-0.04..+0.07) | -0.23 | -0.02 (-0.14..+0.03) | +0.36 |
| 512 N=128 t=1 | -0.03 (-1.56..+1.59) | **-7.31** (-8.21..-7.21) | +0.02 (-0.20..+0.21) | -0.36 |
| 16:64 N=1 t=1 | -0.20 (-0.76..+0.67) | +0.93 | -0.03 (-0.08..+0.19) | +0.43 |
| 16:64 N=64 t=1 | -0.83 (-2.01..+4.86) | +0.71 | -0.03 (-0.13..+0.09) | +0.53 |
| 16:1024 N=1 t=1 | -0.09 (-0.40..+0.19) | +1.19 | -0.11 (-0.47..+0.14) | -0.44 |
| 16:1024 N=64 t=1 | +0.00 (-1.70..+0.96) | +1.64 | +0.10 (-1.57..+0.79) | +0.25 |
| 512 N=1 t=8 | -0.01 (-1.33..+1.52) | +0.12 | -0.12 (-0.95..+0.24) | +0.31 |
| 512 N=128 t=8 | +0.12 (-0.50..+0.68) | -0.50 | +0.07 (-0.18..+1.00) | -0.64 |
| 16:64 N=1 t=8 | -0.04 (-0.38..+0.32) | +0.21 | +0.04 (-0.69..+0.64) | +0.47 |
| 16:64 N=64 t=8 | +0.07 (-0.36..+0.60) | +0.14 | -0.06 (-0.84..+0.44) | +0.41 |
| 16:1024 N=1 t=8 | +0.00 (-0.88..+1.24) | +0.66 | -0.03 (-0.33..+0.70) | -0.44 |
| 16:1024 N=64 t=8 | -0.01 (-2.31..+0.27) | +0.36 | -0.04 (-0.68..+0.59) | +0.28 |
| insn/pair, all 7 t=1 points | 0.00 | 0.00..+0.08 | 0.00 | 0.00 |

ae9af7e did not move the x86 point (-7.31 % here vs -7.33 % at dc4f29c). Arm
shows nothing comparable.

## Step 4: layout control and forced alignment (x86, 512 N=128 t=1 only)
| post arm | what | null | A/B |
|---|---|---|---|
| r7h4-layout (987b980) | unused 30-insn fn before `_umem_alloc` | +0.03 | -0.67 (-1.84..-0.53) |
| r7h4-layoutb (3030cc3) | same, before `_umem_cache_alloc` | +0.01 | -0.35 (-5.98..-0.28); per pair -1.6 % on 4 of 9, -6 % on 1 |
| ae9af7e, both built `-O2 -g -falign-functions=64 -falign-loops=32` | the change, forced alignment | +0.00 (-0.08..+1.24) | **+1.43** (-0.26..+3.57) |

(The first layout commit used `cold`; gcc puts that in `.text.unlikely`, so it
would have moved nothing, and 987b980 removed it.) The pads move the point by
0.4-0.7 % at the median, but layoutb shows a bimodal -1.6 % and one -6 %
replicate. The decisive result is the aligned build: with function and loop
alignment pinned, the -7.31 % becomes +1.43 %, at identical insn/pair
(+0.07 %). The regression follows code placement, not the change.

## Re-check at 20a4f8e (x86, 512 N=128, `ab-fin/`)
t=1: null +0.03 (-1.52..+0.09), A/B **-7.34 %**, insn/pair +0.00 %. t=8: A/B
-0.04 % (null -2.02..+1.80). perf stat (5 runs): cycles +8.06 %,
idq_uops_not_delivered.core +76 %, instructions +0.01 %. This reproduces step 2.

## Verdict (for the coordinator)
- The -7.3 % comes with +0 instructions, +0 branch misses and negligible I-cache
  misses, and all of it is undelivered front-end uops (+12.4/pair).
- Force alignment on both arms (`-falign-functions=64 -falign-loops=32`) and
  post is **+1.43 %** at that point. So the hooks-off static diff (a few
  cold-arm insns, a wider mask test) has no measurable execution cost, and
  the default-build number is a placement artifact.
- The null "unused function" pads moved that point by only -0.35..-0.67 % at
  the median (layoutb had one -6 % replicate). So one arbitrary pad does not
  land on the bad alignment reliably, but placement alone can reach it.
- Arm: no comparable point; every A/B is within about ±0.7 %.
- Not done: bisecting which function's placement matters (e.g. aligning only
  `_umem_cache_alloc`'s pop loop), and a default-build run on metal.


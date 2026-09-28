# @r7h3: T1/T8 hook redesign, A/B + gates (not merged)

Boxes: c7i.2xlarge (x86, gcc 14) and c7g.2xlarge (arm), Debian 13, default build,
hooks OFF.  hotpath_ab.sh, 9 alternating pairs, bench_pairs from post. pre=f40a2da.

## Pass 1: post = e263698 (tail checks), in ab-{intel,arm}-e263698.log
insn/pair is outside the +/-0.1 % null at every point where it was measured:
x86 512/N=1 +1.36 %, N=64 +1.59 %, N=128 +1.05 %; arm +0.68 / +0.74 / +0.34 %.
x86 512/N=128/t=1 is -7.24 % (null -0.03..+0.10).  Disasm: a `umem_hooks` load
and branch after _umem_cache_alloc in _umem_alloc (with rax spilled to r8
around it), and before _umem_cache_free / vmem_free in _umem_free.

## Pass 2: post = dc4f29c (hooks tested only on the bin<0 / oversize arms), in g2-*-dc4f29c.log
insn/pair: every t=1 point is inside the null on both arches (x86 147.2->147.1,
142.2->142.2; arm 148.2->148.1, 145.9->145.9).  x86 umem_hooks refs: 2 in
_umem_alloc, 2 in _umem_free (the bin<0 and oversize arms), 0 in _umem_cache_*.

Throughput points OUTSIDE the null (median; null min..max):
| arch | point | A/B | null |
|---|---|---|---|
| x86 | 512 N=128 t=1 | **-7.33 %** | -2.38..+0.11 |
| x86 | 512 N=64 t=1 | -0.20 % | -0.04..+0.03 |
| x86 | 16:1024 N=64 t=1 | +1.79 % | -1.87..+0.27 |
| x86 | 512 N=128 t=8 | -0.53 % | -0.39..+1.08 |
| arm | 512 N=1 t=1 | +0.18 % | -0.04..+0.09 |
| arm | 512 N=64 t=1 | +0.27 % | -0.07..+0.06 |
| arm | 16:1024 N=1 t=1 | -0.81 % | -0.42..+0.04 |
| arm | 512 N=128 t=8 | -0.49 % | -0.42..+0.07 |
| arm | 16:64 N=64 t=8 | +0.78 % | -0.35..+0.38 |
| arm | 16:1024 N=1 t=8 | -0.76 % | -0.08..+0.12 |
All other points are inside.  x86 512/N=128/t=1 drops 7.3 % with insn/pair
unchanged, and by the same amount in pass 1 and pass 2, so the instruction
count is not the cause.  Hypothesis, not yet tested: layout/alignment of the
magazine path (_umem_free +34 insns; _umem_cache_alloc +21 from the
noinline umem_cache_alloc_tail call replacing the inline debug call).
Next: perf stat cycles and frontend/DSB counters at that point, and restore
the inline umem_cache_alloc_debug with only the hooked part out of line.

## Gates at dc4f29c (isolated), both arches: PASS
introspect make check: x86 67 total / 64 PASS / 3 SKIP, arm 67 / 65 / 2 SKIP;
0 XFAIL/FAIL/XPASS/ERROR on both.  T1_event_cb, T1_gdb_break, T8_break_ptc
PASS on both.  Default build: 67 / 57 PASS / 10 SKIP, 0 FAIL/XFAIL.  rseq
6-bug, trailing_store, commit_safe, resize_leak 16/32, alias_e2e,
commit_abort 0/30, and the 8t oracle (small/mag/mixed/large) PASS on both.

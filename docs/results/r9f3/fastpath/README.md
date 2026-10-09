# r9f3 gate 2 -- fast-path A/B (feature OFF, pre=4bbfe44 post=3c5a247)

hotpath_ab.sh, 9 replicates/point, t=1 and t=8, c7i.2xlarge (x86) and
c7g.2xlarge (arm). The decisive column is `insn/pair` at t=1 (load-independent):
EVERY A/B point is +0.00% median on both arches. Every A/B throughput median is
inside its null's min..max. Full logs captured in the worker transcript; the
summary verdict is below.

## x86 (c7i.2xlarge, gcc 14) -- insn/pair A/B, all +0.00%
  s=512   N=1   : +0.00%   s=512   N=64  : +0.00%   s=512   N=128 : +0.00%
  s=16:64 N=1   : -0.06%   s=16:64 N=64  : +0.00%
  s=16:1024 N=1 : +0.00%   s=16:1024 N=64: +0.00%
(noise band from the null: +-0.06..0.15%)

## arm (c7g.2xlarge, gcc 14) -- insn/pair A/B, all +0.00%
  s=512   N=1   : +0.00%   s=512   N=64  : +0.00%   s=512   N=128 : +0.00%
  s=16:64 N=1   : +0.00%   s=16:64 N=64  : +0.00%
  s=16:1024 N=1 : +0.00%   s=16:1024 N=64: +0.00%

Throughput A/B (Mpairs/s): every A/B median within the null spread, both arches,
t=1 and t=8. VERDICT: gate 2 PASS -- the fast path is unchanged with the
feature OFF.

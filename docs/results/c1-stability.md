# C1 stability validation — intel-lo (c7i.2xlarge, 8 vCPU)

Task C1: pin CPUs, discard warm-up, report median + CoV.

Workload: `single-thread`, size `64:64`, 2M ops.

## Before (5 separate single-run invocations, unpinned)

ops/sec: 5487750, 5476242, 5488126, 5474222, 5010573
- mean ≈ 5.39M, stddev ≈ 189K → **CoV ≈ 3.5%**, one run 8% low (the swing the plan describes).

## After (one invocation `-r 5 -W 1` pinned via `numactl --physcpubind=0 --localalloc`)

CSV row:
```
umem,single-thread,1,2000000,0.363137,5507563.93,30,33,34,35,38,17727,33,5136384,128000000,0.04,363.2,0.0,0.0052,5,0
```
- `ops_cov = 0.0052` → **CoV = 0.52%**, `runs = 5`, `unstable = 0`.

**Result: CoV 3.5% → 0.52%, well under the 5% target.** Warm-up discard + pinning
removes the migration/cold-cache outliers.

Note: c7i.2xlarge is virtualized and exposes no cpufreq governor files
(`scaling_governor` absent); `--pin`'s governor check emits a warning rather
than aborting on such hosts. Governor gating is enforced on the bare-metal
roles (intel-hi/arm-hi) where the files exist.

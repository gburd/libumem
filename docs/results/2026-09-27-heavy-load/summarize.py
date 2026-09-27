#!/usr/bin/env python3
"""Summarise heavy_load_compare.sh TOML (sustained_load.sh continuous mode).

usage: summarize.py FILE.toml [FILE.toml ...]

An arm is one 15-minute process; "x" and "x@2" are two independent runs of
allocator x.  Per arm: over its measured intervals, median Mops/s (with
min/max), median p50/p99/p999 (with min-max for the tails), and peak RSS
(max of the per-interval max VmRSS).  Per allocator: the mean of its two
runs, and its run-to-run GAP = |run1 - run2| / mean, per metric.

A difference between allocators A and B on a metric counts only if
|A - B| / min(A, B) exceeds max(gap_A, gap_B) on that metric; otherwise it
is reported as "inside noise".

Invariants (exit 1 if any fails): each arm has exactly intervals
WARMUPS..WARMUPS+WINDOWS-1 (the dropped warm-up is absent), every interval is
within 5 % of its length and has an RSS sample, rc 0, zero allocation
failures, one thread count.
"""
import sys, tomllib, statistics as st
from collections import defaultdict

METRICS = [('mops', 'Mops/s', True), ('p50', 'p50 ns', False), ('p99', 'p99 ns', False),
           ('p999', 'p999 ns', False), ('rss', 'peak RSS MB', False)]
bad = 0


def arm_stats(ws):
    m = [w['ops_per_sec'] / 1e6 for w in ws]
    p99 = [w['lat_p99'] for w in ws]
    p999 = [w['lat_p999'] for w in ws]
    return dict(mops=st.median(m), lo=min(m), hi=max(m),
                p50=st.median([w['lat_p50'] for w in ws]),
                p99=st.median(p99), p99r=(min(p99), max(p99)),
                p999=st.median(p999), p999r=(min(p999), max(p999)),
                rss=max(w['vmhwm_bytes'] for w in ws) / 2**20, n=len(ws))


for path in sys.argv[1:]:
    d = tomllib.load(open(path, 'rb'))
    W, N, T = d['warmup_windows_discarded'], d['windows_per_allocator'], d['duration_target_sec_per_window']
    print(f"## {path}\n{d['instance_type']} {d['arch']} threads={d['threads']} sha={d['git_sha'][:12]} "
          f"mode={d.get('mode')} numa={d.get('numa_policy')} tunables={d.get('glibc_tunables')} "
          f"stack={d.get('thread_stack_usable_bytes')}")
    runs = {(r['allocator'], r['workload']): r for r in d.get('run', [])}
    g = defaultdict(list)
    for w in d.get('window', []):
        g[(w['workload'], w['allocator'])].append(w)
    for (wl, a), ws in sorted(g.items()):
        iv = sorted(w['interval'] for w in ws)
        if iv != list(range(W, W + N)):
            print(f"  FAIL {a} {wl}: intervals {iv}, want {W}..{W+N-1}"); bad += 1
        for w in ws:
            if abs(w['elapsed_sec'] - T) > 0.05 * T:
                print(f"  FAIL {a} {wl} interval {w['interval']}: {w['elapsed_sec']}s vs {T}s"); bad += 1
            if w['vmhwm_bytes'] <= 0:
                print(f"  FAIL {a} {wl} interval {w['interval']}: no RSS sample"); bad += 1
            if w['alloc_failures'] or w['threads'] != d['threads']:
                print(f"  FAIL {a} {wl} interval {w['interval']}: failures={w['alloc_failures']} threads={w['threads']}"); bad += 1
        r = runs.get((a, wl))
        if r is None or r['rc'] != 0:
            print(f"  FAIL {a} {wl}: run rc={None if r is None else r['rc']}"); bad += 1
    for (a, wl), r in runs.items():
        if (wl, a) not in g:
            print(f"  FAIL {a} {wl}: run recorded no intervals (rc={r['rc']})"); bad += 1

    arms = {k: arm_stats(v) for k, v in g.items()}
    print("\nPer run (each row = one 15-min process, stats over its 10 kept intervals):\n")
    print("| workload | run | Mops/s med (min-max) | p50 | p99 med (min-max) | p999 med (min-max) | peak RSS MB |")
    print("|---|---|---|---:|---|---|---:|")
    for (wl, a), s in sorted(arms.items()):
        print(f"| {wl} | {a} | {s['mops']:.3f} ({s['lo']:.3f}-{s['hi']:.3f}) | {s['p50']:.0f} | "
              f"{s['p99']:.0f} ({s['p99r'][0]:.0f}-{s['p99r'][1]:.0f}) | "
              f"{s['p999']:.0f} ({s['p999r'][0]:.0f}-{s['p999r'][1]:.0f}) | {s['rss']:.0f} |")

    # Per allocator: two runs -> mean and gap.
    alloc = defaultdict(dict)
    for (wl, a), s in arms.items():
        alloc[(wl, a.split('@')[0])][a] = s
    summ = {}
    print("\nPer allocator (mean of its two runs; gap = |run1 - run2| / mean):\n")
    print("| workload | allocator | " + " | ".join(f"{h} (gap)" for _, h, _ in METRICS) + " |")
    print("|---|---|" + "---:|" * len(METRICS))
    for (wl, b), rs in sorted(alloc.items()):
        if len(rs) != 2:
            print(f"  FAIL {b} {wl}: {len(rs)} runs, want 2"); bad += 1
            continue
        x, y = rs.values()
        mean = {k: (x[k] + y[k]) / 2 for k, _, _ in METRICS}
        gap = {k: (abs(x[k] - y[k]) / mean[k] if mean[k] else 0.0) for k, _, _ in METRICS}
        summ[(wl, b)] = (mean, gap)
        print(f"| {wl} | {b} | " + " | ".join(
            (f"{mean[k]:.3f}" if k == 'mops' else f"{mean[k]:.0f}") + f" ({gap[k]*100:.1f} %)"
            for k, _, _ in METRICS) + " |")

    print("\nPairwise (ratio A/B of the means; 'noise' = the larger of A's and B's own gap):\n")
    print("| workload | A vs B | " + " | ".join(h for _, h, _ in METRICS) + " |")
    print("|---|---|" + "---|" * len(METRICS))
    for wl in sorted({k[0] for k in summ}):
        for a, b in (('umem', 'libc'), ('umem', 'jemalloc'), ('jemalloc', 'libc')):
            if (wl, a) not in summ or (wl, b) not in summ:
                continue
            (ma, ga), (mb, gb) = summ[(wl, a)], summ[(wl, b)]
            cells = []
            for k, _, higher_better in METRICS:
                if not ma[k] or not mb[k]:
                    cells.append("n/a"); continue
                ratio = ma[k] / mb[k]
                diff = abs(ma[k] - mb[k]) / min(ma[k], mb[k])
                noise = max(ga[k], gb[k])
                if diff <= noise:
                    verdict = "inside noise"
                else:
                    better = (ratio > 1) == higher_better
                    verdict = f"{a if better else b} better"
                cells.append(f"{ratio:.2f}x, {verdict} (noise {noise*100:.1f} %)")
            print(f"| {wl} | {a} vs {b} | " + " | ".join(cells) + " |")
    print()
print("RESULT:", "FAIL" if bad else "PASS", f"({bad} invariant failures)")
sys.exit(1 if bad else 0)

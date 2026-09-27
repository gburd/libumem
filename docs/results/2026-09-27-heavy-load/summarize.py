#!/usr/bin/env python3
"""Summarise heavy_load_compare.sh TOML (sustained_load.sh continuous mode).

usage: summarize.py FILE.toml [FILE.toml ...]

Per (workload, allocator): median over the measured intervals of Mops/s,
p50/p99/p999 (ns), peak RSS (max over intervals of the per-interval max
VmRSS), and min/median/max Mops/s across intervals.  Checks the invariants the
comparison rests on and exits 1 if any fails: every measured interval present
(no SHORT/CRASH), none of the discarded warm-up intervals present, interval
lengths near their target, zero allocation failures, and the same thread count
everywhere.  The null line reports |umem - umem@null| / umem for each metric.
"""
import sys, tomllib, statistics as st
from collections import defaultdict

def med(v): return st.median(v) if v else float('nan')

bad = 0
for path in sys.argv[1:]:
    d = tomllib.load(open(path, 'rb'))
    W, N, T = d['warmup_windows_discarded'], d['windows_per_allocator'], d['duration_target_sec_per_window']
    print(f"## {path}\n{d['instance_type']} {d['arch']} threads={d['threads']} sha={d['git_sha'][:12]} "
          f"mode={d.get('mode')} tunables={d.get('glibc_tunables')} stack={d.get('thread_stack_usable_bytes')}")
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
            if w['alloc_failures'] or w['threads'] != d['threads']:
                print(f"  FAIL {a} {wl} interval {w['interval']}: failures={w['alloc_failures']} threads={w['threads']}"); bad += 1
        r = runs.get((a, wl))
        if r is None or r['rc'] != 0:
            print(f"  FAIL {a} {wl}: run rc={None if r is None else r['rc']}"); bad += 1
    print(f"| workload | allocator | Mops/s med | min / max | p50 | p99 | p999 (ns, med) | peak RSS MB |")
    print(f"|---|---|---:|---|---:|---:|---:|---:|")
    summ = {}
    for (wl, a), ws in sorted(g.items()):
        m = [w['ops_per_sec'] / 1e6 for w in ws]
        s = dict(mops=med(m), lo=min(m), hi=max(m), p50=med([w['lat_p50'] for w in ws]),
                 p99=med([w['lat_p99'] for w in ws]), p999=med([w['lat_p999'] for w in ws]),
                 rss=max(w['vmhwm_bytes'] for w in ws) / 2**20)
        summ[(wl, a)] = s
        print(f"| {wl} | {a} | {s['mops']:.3f} | {s['lo']:.3f} / {s['hi']:.3f} | {s['p50']:.0f} | "
              f"{s['p99']:.0f} | {s['p999']:.0f} | {s['rss']:.0f} |")
    for wl in sorted({k[0] for k in summ}):
        u, n = summ.get((wl, 'umem')), summ.get((wl, 'umem@null'))
        if u and n:
            print(f"null {wl}: " + ", ".join(
                f"{k} {abs(u[k] - n[k]) / u[k] * 100:.1f}%" for k in ('mops', 'p50', 'p99', 'p999', 'rss')))
print("RESULT:", "FAIL" if bad else "PASS", f"({bad} invariant failures)")
sys.exit(1 if bad else 0)

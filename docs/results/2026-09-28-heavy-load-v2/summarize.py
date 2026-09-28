#!/usr/bin/env python3
"""Summarise a @r7cmp2 cell (heavy_load_compare.sh, 3 runs per allocator).

usage: summarize.py FILE.toml [FILE.toml ...]   (markdown on stdout)

A run is one 15-minute bench_main -D process ("x", "x@2", "x@3").  Per run,
over its 10 measured intervals: median Mops/s, median p99 and p999 (sampled
1 op in lat_sample_every per thread), peak RSS = max per-interval max VmRSS.
Per allocator: min / median / max of those per-run values across its 3 runs.

Rule (pre-registered, docs/plans/2026-09-27-round7.md): a difference between
two allocators on a metric counts ONLY if their 3-run [min, max] ranges do not
overlap; otherwise "no resolvable difference".

Invariants (exit 1 if any fails): every run has rc 0 and exactly intervals
WARMUPS..WARMUPS+WINDOWS-1, each within 5 % of its length, zero allocation
failures, and each allocator has exactly 3 runs.
"""
import sys, tomllib, statistics as st
from collections import defaultdict
from itertools import combinations

# (key, label, higher_is_better)
METRICS = [('mops', 'Mops/s', True), ('p99', 'p99 ns', False),
           ('p999', 'p999 ns', False), ('rss', 'peak RSS MiB', False)]
bad = 0


def fail(msg):
    global bad
    bad += 1
    print(f'- **INVARIANT FAIL**: {msg}')


def run_stats(ws):
    return dict(mops=st.median(w['ops_per_sec'] / 1e6 for w in ws),
                p99=st.median(w['lat_p99'] for w in ws),
                p999=st.median(w['lat_p999'] for w in ws),
                rss=max(w['vmhwm_bytes'] for w in ws) / 2**20)


def fmt(v):
    return f'{v:,.1f}' if v < 1000 else f'{v:,.0f}'


for path in sys.argv[1:]:
    d = tomllib.load(open(path, 'rb'))
    warm, win, sec = d['warmup_windows_discarded'], d['windows_per_allocator'], d['interval_sec']
    print(f"\n## {path.split('/')[-1]}\n")
    print(f"{d['instance_type']} {d['arch']}, threads {d['threads']}, "
          f"prodcons {d['prodcons_sizes']}, frag {d['frag_sizes']}, "
          f"sha {d['git_sha'][:7]}\n")
    runs = {(r['allocator'], r['workload']): r for r in d.get('run', [])}
    rows = defaultdict(list)
    for w in d.get('window', []):
        rows[(w['allocator'], w['workload'])].append(w)
    for (a, wl), r in runs.items():
        ws = rows.get((a, wl), [])
        if r['rc'] != 0:
            fail(f'{a} {wl} rc={r["rc"]}')
        ks = sorted(w['interval'] for w in ws)
        if ks != list(range(warm, warm + win)):
            fail(f'{a} {wl} intervals {ks}')
        for w in ws:
            if abs(w['elapsed_sec'] - sec) > 0.05 * sec:
                fail(f'{a} {wl} interval {w["interval"]} ran {w["elapsed_sec"]} s')
            if w['alloc_failures']:
                fail(f'{a} {wl} interval {w["interval"]} alloc_failures={w["alloc_failures"]}')
    for wl in sorted({wl for _, wl in runs}):
        per = defaultdict(list)            # base allocator -> [run_stats]
        for (a, w2), ws in sorted(rows.items()):
            if w2 == wl and ws:
                per[a.split('@')[0]].append(run_stats(ws))
        for b, rs in per.items():
            if len(rs) != 3:
                fail(f'{b} {wl}: {len(rs)} runs, want 3')
        every = {w.get('lat_sample_every', 1) for (a, w2), ws in rows.items() if w2 == wl for w in ws}
        print(f'### {wl}  (latency sampled 1/{",".join(map(str, sorted(every)))})\n')
        print('| allocator | ' + ' | '.join(f'{l} min / med / max' for _, l, _ in METRICS) + ' |')
        print('|---|' + '---|' * len(METRICS))
        rng = {}
        for b in sorted(per):
            cells = []
            for k, _, _ in METRICS:
                v = sorted(r[k] for r in per[b])
                rng[(b, k)] = (v[0], v[-1])
                cells.append(f'{fmt(v[0])} / {fmt(st.median(v))} / {fmt(v[-1])}')
            print(f'| {b} | ' + ' | '.join(cells) + ' |')
        print('\nVerdicts (non-overlap of 3-run ranges):\n')
        for k, l, hib in METRICS:
            out = []
            for x, y in combinations(sorted(per), 2):
                (xl, xh), (yl, yh) = rng[(x, k)], rng[(y, k)]
                if xl > yh or yl > xh:
                    hi, lo = (x, y) if xl > yh else (y, x)
                    better, worse = (hi, lo) if hib else (lo, hi)
                    ratio = st.median(r[k] for r in per[hi]) / st.median(r[k] for r in per[lo])
                    out.append(f'{better} better than {worse} ({ratio:.2f}x)')
                else:
                    out.append(f'{x} vs {y}: no resolvable difference')
            print(f'- {l}: ' + '; '.join(out))
        print()

print(f'\nRESULT {"FAIL" if bad else "PASS"} ({bad} invariant failures)')
sys.exit(1 if bad else 0)

#!/usr/bin/env python3
"""
compare_runs.py -- before/after tables from two allocator_comparison.sh result
trees (docs/results/<date>-<instance>-<arch>/).

    compare_runs.py OLD_DIR NEW_DIR [workload] [size ...]

For each (workload, size, threads) prints the median-over-replicates Mops/s
for umem, umem@null, umem-preload, libc and the best competitor, in both
trees, and the umem OLD->NEW ratio.  The null column is the rig's resolution
at that point: a umem delta inside |umem - umem@null| is noise.

Reads matrix.toml as written by test/bench/matrix.sh (one [[point]] per
allocator x threads x size x replicate).  No dependencies.
"""
import sys, os, collections

ALLOCS = ['libc', 'umem', 'umem@null', 'umem-preload', 'jemalloc', 'tcmalloc',
          'mimalloc', 'snmalloc', 'scudo', 'rpmalloc']
COMPETITORS = ['jemalloc', 'tcmalloc', 'mimalloc', 'snmalloc', 'scudo', 'rpmalloc']


def load(path):
    pts, cur = [], None
    for line in open(path):
        line = line.strip()
        if line == '[[point]]':
            if cur:
                pts.append(cur)
            cur = {}
        elif '=' in line and cur is not None:
            k, v = line.split('=', 1)
            cur[k.strip()] = v.strip().strip('"')
    if cur:
        pts.append(cur)
    by = collections.defaultdict(list)
    for p in pts:
        if 'allocator' not in p:
            continue
        try:
            by[(p['allocator'], int(p['threads']), p['size'])].append(
                float(p['ops_per_sec']) / 1e6)
        except (KeyError, ValueError):
            pass
    return by


def med(v):
    v = sorted(v)
    return v[len(v) // 2] if v else None


def fmt(x):
    return f'{x:7.1f}' if x is not None else '      -'


def table(old, new, sz):
    ths = sorted({t for (a, t, s) in list(old) + list(new) if s == sz})
    print(f'\n### {sz}')
    print(f"{'t':>4} | {'umem old':>8} {'umem new':>8} {'ratio':>6} | {'null new':>8} | "
          f"{'libc new':>8} | {'best new':>8} {'who':>9} | {'umem/best':>9}")
    for t in ths:
        uo, un = med(old.get(('umem', t, sz), [])), med(new.get(('umem', t, sz), []))
        nn = med(new.get(('umem@null', t, sz), []))
        ln = med(new.get(('libc', t, sz), []))
        best, who = None, ''
        for a in COMPETITORS + ['libc']:
            v = med(new.get((a, t, sz), []))
            if v is not None and (best is None or v > best):
                best, who = v, a
        ratio = f'{un/uo:6.2f}' if (uo and un) else '     -'
        ub = f'{un/best:9.2f}' if (un and best) else '        -'
        print(f'{t:>4} | {fmt(uo)} {fmt(un)} {ratio} | {fmt(nn)} | {fmt(ln)} | {fmt(best)} {who:>9} | {ub}')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    old_dir, new_dir = sys.argv[1], sys.argv[2]
    wl = sys.argv[3] if len(sys.argv) > 3 else 'multi'
    op, np_ = os.path.join(old_dir, wl, 'matrix.toml'), os.path.join(new_dir, wl, 'matrix.toml')
    old = load(op) if os.path.exists(op) else {}
    new = load(np_) if os.path.exists(np_) else {}
    if not new:
        print(f'no {wl}/matrix.toml under {new_dir}')
        sys.exit(1)
    sizes = sys.argv[4:] or sorted({s for (a, t, s) in new})
    print(f'## {wl}: {os.path.basename(old_dir)} -> {os.path.basename(new_dir)}  (Mops/s, median over replicates)')
    for sz in sizes:
        table(old, new, sz)


if __name__ == '__main__':
    main()

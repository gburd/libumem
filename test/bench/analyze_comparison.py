#!/usr/bin/env python3
"""
analyze_comparison.py - turn matrix.toml files into the comparison tables.

Reads every [[point]] from the matrix.toml files under a results directory
(single/, multi/, multi-hi/, prodcons/, frag/, frag-mid/, frag-big/,
ceiling/), and for every grid point (workload, size, threads):

  * null delta   = (umem@null - umem) / umem   per replicate, and the same
                   for umem-preload; pooled across the grid this is the rig's
                   resolution.  It is printed FIRST.
  * arm medians  = median ops/sec over replicates for every arm.
  * gap          = (best_competitor - umem_arm) / best_competitor, and vs libc.
                   A gap is a FINDING only if it exceeds the pooled null
                   spread (|null| max, or 2*sd, whichever is larger) AND the
                   point's own null delta.
  * frag pair    = rss_at_live_peak / live_bytes_at_peak / vmhwm for frag.
  * alloc_failures for every row, always.

Output is Markdown to stdout.  No plotting, no stats library: median, sd,
min, max only.
"""
import glob
import os
import re
import statistics as st
import sys
from collections import defaultdict

ARMS_UMEM = ("umem", "umem-preload")
COMPETITORS = ("libc", "jemalloc", "tcmalloc", "mimalloc", "snmalloc", "scudo", "rpmalloc")


def parse_toml_points(path):
    txt = open(path).read()
    header, *pts = txt.split("[[point]]")
    meta = dict(re.findall(r'^(\w+) = (.*)$', header, re.M))
    rows = []
    for p in pts:
        d = {}
        for k, v in re.findall(r'^(\w+) = (.*)$', p, re.M):
            v = v.strip()
            if v.startswith('"'):
                v = v.strip('"')
            else:
                try:
                    v = float(v) if ("." in v or "e" in v) else int(v)
                except ValueError:
                    pass
            d[k] = v
        d["_file"] = os.path.basename(os.path.dirname(path))
        rows.append(d)
    return meta, rows


def load(resdir):
    allrows = []
    metas = {}
    for f in sorted(glob.glob(os.path.join(resdir, "*", "matrix.toml"))):
        meta, rows = parse_toml_points(f)
        metas[os.path.basename(os.path.dirname(f))] = meta
        allrows.extend(rows)
    return metas, allrows


def key(r):
    return (r["_file"], r["workload"], r["size"], int(r["threads_requested"]))


def fmt_pct(x):
    return "n/a" if x is None else f"{x:+.2f}%"


def mb(x):
    return f"{x/1e6:,.0f}"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    resdir = sys.argv[1]
    metas, rows = load(resdir)
    if not rows:
        print("no points under", resdir)
        sys.exit(1)

    # group rows by grid point, then by arm -> list of replicate rows
    grid = defaultdict(lambda: defaultdict(list))
    for r in rows:
        grid[key(r)][r["allocator"]].append(r)

    # --- null control ---------------------------------------------------
    null_deltas = {a: [] for a in ARMS_UMEM}
    null_by_point = {}
    for k, arms in grid.items():
        for a in ARMS_UMEM:
            base, alias = arms.get(a, []), arms.get(a + "@null", [])
            if not base or not alias:
                continue
            ds = []
            for rb, ra in zip(sorted(base, key=lambda r: r.get("replicate", 1)),
                              sorted(alias, key=lambda r: r.get("replicate", 1))):
                if rb["ops_per_sec"] > 0:
                    ds.append((ra["ops_per_sec"] - rb["ops_per_sec"]) / rb["ops_per_sec"] * 100)
            if ds:
                null_deltas[a].extend(ds)
                null_by_point[(k, a)] = ds

    print(f"# Null control (rig resolution) -- {resdir}\n")
    resolution = {}
    for a in ARMS_UMEM:
        ds = null_deltas[a]
        if not ds:
            print(f"- {a}: no null pairs")
            continue
        sd = st.pstdev(ds) if len(ds) > 1 else 0.0
        lo, hi = min(ds), max(ds)
        # resolution bar: the larger of the observed |max| and 2 sd
        res = max(abs(lo), abs(hi), 2 * sd)
        resolution[a] = res
        print(f"- **{a} vs {a}@null**: n={len(ds)} pairs, median {st.median(ds):+.2f}%, "
              f"sd {sd:.2f}, range {lo:+.2f}% .. {hi:+.2f}%  => **resolution bar +/-{res:.1f}%**")
    print()
    # per-workload null spread (the workloads differ a lot)
    print("Per-workload null spread (umem arm):\n")
    print("| workload | n | median | sd | min | max |")
    print("|---|---|---|---|---|---|")
    perwl = defaultdict(list)
    for (k, a), ds in null_by_point.items():
        if a == "umem":
            perwl[(k[0], k[1])].extend(ds)
    for (f, wl), ds in sorted(perwl.items()):
        sd = st.pstdev(ds) if len(ds) > 1 else 0.0
        print(f"| {f}/{wl} | {len(ds)} | {st.median(ds):+.2f}% | {sd:.2f} | {min(ds):+.2f}% | {max(ds):+.2f}% |")
    print()

    # --- per-point tables --------------------------------------------------
    def med(arm_rows, field="ops_per_sec"):
        vals = [r[field] for r in arm_rows if isinstance(r.get(field), (int, float))]
        return st.median(vals) if vals else None

    all_arms = sorted({a for arms in grid.values() for a in arms if "@null" not in a},
                      key=lambda a: (a not in ("libc", "umem", "umem-preload"), a))
    gaps = []  # (gap_pct, k, umem_arm, best, best_ops, umem_ops, null_here)
    failures = []
    floor_raised = []

    for f in sorted({k[0] for k in grid}):
        print(f"\n## {f}\n")
        m = metas.get(f, {})
        print(f"budget (total ops/point): {m.get('operations_total_per_point','?')}  sha: {m.get('git_sha','?')}\n")
        is_frag = any(k[1] == "frag" for k in grid if k[0] == f)
        cols = ["workload", "size", "t"] + all_arms + ["null(umem)", "null(preload)", "umem gap vs best", "preload gap vs best"]
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        for k in sorted([k for k in grid if k[0] == f], key=lambda k: (k[1], int(k[2].split(':')[0]), k[3])):
            arms = grid[k]
            meds = {a: med(arms.get(a, [])) for a in all_arms}
            for a, rs in arms.items():
                for r in rs:
                    if r.get("alloc_failures", 0):
                        failures.append((k, a, r.get("replicate", 1), r["alloc_failures"], r["total_ops"]))
                    if r.get("ops_floor_raised") is True or r.get("ops_floor_raised") == "true":
                        floor_raised.append((k, a, r.get("replicate", 1)))
            comp = {a: v for a, v in meds.items() if a in COMPETITORS and v}
            best_a, best_v = (max(comp.items(), key=lambda kv: kv[1]) if comp else (None, None))
            cells = [k[1], k[2], str(k[3])]
            for a in all_arms:
                v = meds[a]
                s = "-" if v is None else f"{v/1e6:.2f}"
                if a == best_a:
                    s = f"**{s}**"
                cells.append(s)
            nu = null_by_point.get((k, "umem"))
            npre = null_by_point.get((k, "umem-preload"))
            cells.append(fmt_pct(st.median(nu)) if nu else "-")
            cells.append(fmt_pct(st.median(npre)) if npre else "-")
            for a in ARMS_UMEM:
                v = meds.get(a)
                if v and best_v:
                    g = (best_v - v) / best_v * 100
                    nh = null_by_point.get((k, a))
                    null_here = max(abs(x) for x in nh) if nh else 0.0
                    res = resolution.get(a, 0.0)
                    flag = ""
                    if g > max(res, null_here):
                        flag = " **GAP**"
                        gaps.append((g, k, a, best_a, best_v, v, null_here))
                    elif g < -max(res, null_here):
                        flag = " (umem faster)"
                    cells.append(f"{g:+.1f}%{flag}")
                else:
                    cells.append("-")
            print("| " + " | ".join(cells) + " |")

        if is_frag:
            print(f"\n### {f}: fragmentation PAIR (median over replicates; MB)\n")
            fc = ["workload", "size", "t"]
            for a in all_arms:
                fc += [f"{a} rss@peak", f"{a} live@peak", f"{a} vmhwm", f"{a} frag"]
            print("| " + " | ".join(fc) + " |")
            print("|" + "---|" * len(fc))
            for k in sorted([k for k in grid if k[0] == f and k[1] == "frag"], key=lambda k: (int(k[2].split(':')[0]), k[3])):
                arms = grid[k]
                cells = [k[1], k[2], str(k[3])]
                for a in all_arms:
                    rs = arms.get(a, [])
                    rss, live, hwm, fr = med(rs, "rss_at_live_peak"), med(rs, "live_bytes_at_peak"), med(rs, "vmhwm_bytes"), med(rs, "frag")
                    cells += [mb(rss) if rss else "-", mb(live) if live else "-", mb(hwm) if hwm else "-", f"{fr:.2f}" if fr else "-"]
                print("| " + " | ".join(cells) + " |")

            # latency p999 for frag
        print(f"\n### {f}: p999 latency (ns, median over replicates)\n")
        lc = ["workload", "size", "t"] + all_arms
        print("| " + " | ".join(lc) + " |")
        print("|" + "---|" * len(lc))
        for k in sorted([k for k in grid if k[0] == f], key=lambda k: (k[1], int(k[2].split(':')[0]), k[3])):
            arms = grid[k]
            cells = [k[1], k[2], str(k[3])]
            for a in all_arms:
                v = med(arms.get(a, []), "lat_p999")
                cells.append("-" if v is None else f"{v:,.0f}")
            print("| " + " | ".join(cells) + " |")

    # --- findings ---------------------------------------------------------
    print("\n## alloc_failures (every nonzero row)\n")
    if not failures:
        print("none: every row completed its full budget.")
    else:
        print("| file/workload | size | t | arm | rep | failures | completed ops |")
        print("|---|---|---|---|---|---|---|")
        for k, a, rep, fl, ops in failures:
            print(f"| {k[0]}/{k[1]} | {k[2]} | {k[3]} | {a} | {rep} | {fl:,} | {ops:,} |")
    print("\n## ops_floor_raised rows\n")
    print("none" if not floor_raised else "\n".join(f"- {k} {a} rep {rep}" for k, a, rep in floor_raised))

    print("\n## Gaps beyond the null resolution (ranked)\n")
    if not gaps:
        print("none: no umem arm is behind the best competitor by more than the null control resolution.")
    else:
        print("| gap | file/workload | size | t | umem arm | Mops | best | Mops | null here |")
        print("|---|---|---|---|---|---|---|---|---|")
        for g, k, a, ba, bv, v, nh in sorted(gaps, reverse=True):
            print(f"| {g:+.1f}% | {k[0]}/{k[1]} | {k[2]} | {k[3]} | {a} | {v/1e6:.2f} | {ba} | {bv/1e6:.2f} | +/-{nh:.1f}% |")


if __name__ == "__main__":
    main()

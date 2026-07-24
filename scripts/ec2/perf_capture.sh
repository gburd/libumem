#!/usr/bin/env bash
# D1 perf capture for the 128-thread umem `multi` run.
set +e
export LD_LIBRARY_PATH=".libs:${LD_LIBRARY_PATH}"
NCPU=$(nproc)
T="${T:-128}"
last=$((T-1)); [ "$last" -ge "$NCPU" ] && last=$((NCPU-1))
OUTDIR="docs/results/2026-07-23-perf-128"
mkdir -p "$OUTDIR"

echo "=== perf availability ==="
which perf; perf --version 2>&1 | head -1
sysctl kernel.perf_event_paranoid 2>/dev/null

echo "=== perf record: umem multi t=$T ==="
sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1
perf record -F 997 -g --call-graph dwarf -o "$OUTDIR/perf.data" -- \
    numactl --physcpubind=0-"$last" --localalloc -- \
    test/bench/.libs/bench_contention -t "$T" -n 20000000 -s 64:256 \
    2>"$OUTDIR/bench.stderr" | tee "$OUTDIR/bench.stdout"

echo "=== perf report (top 40) ==="
perf report -i "$OUTDIR/perf.data" --stdio -g none 2>/dev/null | head -60 \
    | tee "$OUTDIR/perf-report-top.txt"

echo "=== folded stacks (for flamegraph) ==="
perf script -i "$OUTDIR/perf.data" 2>/dev/null > "$OUTDIR/perf.script"
# Fold with a minimal awk collapse (stackcollapse-perf.pl not installed).
perf report -i "$OUTDIR/perf.data" --stdio -g folded,0,caller 2>/dev/null \
    > "$OUTDIR/perf-folded.txt"
wc -l "$OUTDIR/perf.script" "$OUTDIR/perf-folded.txt"
echo "perf capture done -> $OUTDIR"

#!/usr/bin/env bash
# scripts/ec2/sustained_perf_capture.sh - perf record over a full ~180s
# sustained 192-thread prodcons run (bench_contention -w prodcons), plus the
# umem_dump_contention counter dump at the end, for the sustained-load depot
# contention diagnosis (docs/results/2026-09-08-allocator-shootout.md sec 7).
set -euo pipefail
export LD_LIBRARY_PATH=".libs:${LD_LIBRARY_PATH:-}"
THREADS="${1:-192}"
OPS="${2:-190000000}"
OUTDIR="${3:-docs/results/2026-09-09-sustained-perf-intel-hi}"
mkdir -p "$OUTDIR"
LAST=$((THREADS - 1))

sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1 || true

perf record -F 997 -g --call-graph dwarf -o "$OUTDIR/perf.data" -- \
    numactl --physcpubind=0-"$LAST" --localalloc -- \
    test/bench/.libs/bench_contention -w prodcons -t "$THREADS" -n "$OPS" -s 64:256 \
    > "$OUTDIR/bench.stdout" 2> "$OUTDIR/bench.stderr"

echo "=== bench.stdout ==="
cat "$OUTDIR/bench.stdout"

echo "=== perf report (top 60) ==="
perf report -i "$OUTDIR/perf.data" --stdio -g none 2>/dev/null | head -80 \
    | tee "$OUTDIR/perf-report-top.txt"

echo "=== folded stacks ==="
perf report -i "$OUTDIR/perf.data" --stdio -g folded,0,caller 2>/dev/null \
    > "$OUTDIR/perf-folded.txt"
wc -l "$OUTDIR/perf-folded.txt"
echo "perf capture done -> $OUTDIR"

#!/usr/bin/env bash
# D1 contention sweep: run umem `multi` at several thread counts, dump the
# already-tracked contention counters after each. Pinned like matrix.sh.
set +e
export LD_LIBRARY_PATH=".libs:${LD_LIBRARY_PATH}"
NCPU=$(nproc)
BIN=test/bench/.libs/bench_contention
SIZE="${SIZE:-64:256}"
OPS="${OPS:-20000000}"
OUT="${OUT:-docs/results/2026-07-23-contention-sweep.txt}"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"
{
  echo "# D1 contention sweep  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# instance vcpu=$NCPU size=$SIZE ops=$OPS"
} | tee "$OUT"
for t in 8 32 128 192; do
    [ "$t" -gt "$NCPU" ] && continue
    last=$((t-1)); [ "$last" -ge "$NCPU" ] && last=$((NCPU-1))
    echo "" | tee -a "$OUT"
    echo "############ threads=$t ############" | tee -a "$OUT"
    numactl --physcpubind=0-"$last" --localalloc -- "$BIN" \
        -t "$t" -n "$OPS" -s "$SIZE" 2>&1 | tee -a "$OUT"
done
echo "" | tee -a "$OUT"
echo "sweep done -> $OUT" | tee -a "$OUT"

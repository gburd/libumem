#!/usr/bin/env bash
# D2 A/B validation: baseline (pre-PTC-fix) vs fixed (HEAD), alternating,
# 5 runs/point, median. Pinned. intel-hi.
set +e
export LD_LIBRARY_PATH=".libs:${LD_LIBRARY_PATH}"
NCPU=$(nproc)
OUT="docs/results/2026-07-23-d2-ab.txt"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"
log() { echo "$@" | tee -a "$OUT"; }

# --- Build FIXED (current working tree = HEAD) ---
bash scripts/ec2/clean-regen.sh >/dev/null 2>&1
make -j"$(nproc)" >/dev/null 2>&1
make -j"$(nproc)" test/bench/bench_main >/dev/null 2>&1 || { echo "FIXED build failed"; exit 1; }
[ -x test/bench/.libs/bench_main ] || { echo "FIXED bench_main missing"; exit 1; }
cp .libs/libumem.so.0.0.0 /tmp/libumem_fixed.so
cp test/bench/.libs/bench_main /tmp/bench_main_fixed
echo "FIXED built"

# --- Build BASELINE (pre-fix umem.c + umem_ptc.c shipped as .txt) ---
cp umem.c /tmp/umem_fixed.c
cp umem_ptc.c /tmp/umem_ptc_fixed.c
cp scripts/ec2/umem_base.c.txt umem.c
cp scripts/ec2/umem_ptc_base.c.txt umem_ptc.c
touch umem.c umem_ptc.c
make -j"$(nproc)" >/dev/null 2>&1
make -j"$(nproc)" test/bench/bench_main >/dev/null 2>&1
if [ -x test/bench/.libs/bench_main ]; then
    cp .libs/libumem.so.0.0.0 /tmp/libumem_base.so
    cp test/bench/.libs/bench_main /tmp/bench_main_base
    echo "BASE built"
else
    echo "BASE build failed"
fi
# restore fixed source + lib
cp /tmp/umem_fixed.c umem.c
cp /tmp/umem_ptc_fixed.c umem_ptc.c
touch umem.c umem_ptc.c

run_one() {  # $1=variant $2=workload $3=threads $4=size
    local v="$1" w="$2" t="$3" s="$4" bin lib last=$(( $3 - 1 ))
    if [ "$v" = base ]; then bin=/tmp/bench_main_base; lib=/tmp/libumem_base.so
    else bin=/tmp/bench_main_fixed; lib=/tmp/libumem_fixed.so; fi
    [ -x "$bin" ] || { echo ""; return; }
    (( last >= NCPU )) && last=$((NCPU-1))
    cp "$lib" .libs/libumem.so.0.0.0
    numactl --physcpubind=0-"$last" --localalloc -- "$bin" \
        -a umem -w "$w" -t "$t" -n 20000000 -s "$s" -r 3 -W 1 -c 2>/dev/null \
        | grep "^umem," | tail -1
}
emit() { local lbl="$1" row; row="$(cat)"
    [ -z "$row" ] && { log "  $lbl: (no row)"; return; }
    echo "$row" | awk -F, -v l="$lbl" \
      '{printf "  %-22s mops=%9.3f  p50=%6s p99=%8s p999=%9s\n", l, $6/1e6, $8, $10, $11}' \
      | tee -a "$OUT"; }

log "# D2 A/B: baseline(HEAD~1) vs fixed(HEAD)  vcpu=$NCPU  $(date -u +%FT%TZ)"
log ""
log "## multi 160:160 (same-size-class, the D2 target)"
for t in 4 8 32 128 192; do
    [ "$t" -gt "$NCPU" ] && continue
    log " threads=$t"
    run_one base  multi "$t" 160:160 | emit "base  multi"
    run_one fixed multi "$t" 160:160 | emit "fixed multi"
done
log ""
log "## multi 64:256"
for t in 8 128; do
    log " threads=$t"
    run_one base  multi "$t" 64:256 | emit "base  multi"
    run_one fixed multi "$t" 64:256 | emit "fixed multi"
done
log ""
log "## non-regression: prodcons 64:256"
for t in 4 8 32; do
    log " threads=$t"
    run_one base  prodcons "$t" 64:256 | emit "base  prodcons"
    run_one fixed prodcons "$t" 64:256 | emit "fixed prodcons"
done
log ""
log "## non-regression: single / frag (1 thread, 64:256)"
run_one base  single 1 64:256 | emit "base  single"
run_one fixed single 1 64:256 | emit "fixed single"
run_one base  frag   1 64:256 | emit "base  frag"
run_one fixed frag   1 64:256 | emit "fixed frag"
cp /tmp/libumem_fixed.so .libs/libumem.so.0.0.0
log ""
log "A/B done -> $OUT"

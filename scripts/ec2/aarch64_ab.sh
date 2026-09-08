#!/usr/bin/env bash
# aarch64 headline A/B: umem vs libc, alternating per point, 5 measured runs
# + 1 warm-up each, pinned. Same shape as scripts/ec2/d2_ab.sh (x86_64 D2
# validation) so the two arches are directly comparable. Covers the two
# workloads the x86_64 story is built on:
#   - multi 160:160 (same-size-class -- the PTC bin-table fix's target)
#   - prodcons 64:256 (cross-thread handoff -- umem's decisive x86_64 win)
set +e
export LD_LIBRARY_PATH=".libs:${LD_LIBRARY_PATH}"
NCPU=$(nproc)
OUT="docs/results/$(date +%Y-%m-%d)-aarch64-ab-$(hostname -s 2>/dev/null || echo host).txt"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"
log() { echo "$@" | tee -a "$OUT"; }

BIN="test/bench/.libs/bench_main"
[ -x "$BIN" ] || { echo "missing $BIN -- build first"; exit 1; }

run_one() {  # $1=allocator $2=workload $3=threads $4=size
    local a="$1" w="$2" t="$3" s="$4" last=$(( $3 - 1 ))
    (( last >= NCPU )) && last=$((NCPU-1))
    numactl --physcpubind=0-"$last" --localalloc -- "$BIN" \
        -a "$a" -w "$w" -t "$t" -n 20000000 -s "$s" -r 5 -W 1 -c 2>/dev/null \
        | grep "^$a," | tail -1
}
emit() { local lbl="$1" row; row="$(cat)"
    [ -z "$row" ] && { log "  $lbl: (no row)"; return; }
    echo "$row" | awk -F, -v l="$lbl" \
      '{printf "  %-14s mops=%9.3f  p50=%6s p99=%8s p999=%9s cov=%s\n", l, $6/1e6, $8, $10, $11, $(NF-2)}' \
      | tee -a "$OUT"; }

log "# aarch64 headline A/B: umem vs libc  vcpu=$NCPU  $(date -u +%FT%TZ)"
log ""
log "## multi 160:160 (same-size-class)"
for t in 1 8 32 128 192; do
    [ "$t" -gt "$NCPU" ] && continue
    log " threads=$t"
    run_one umem multi "$t" 160:160 | emit "umem multi"
    run_one libc multi "$t" 160:160 | emit "libc multi"
done
log ""
log "## prodcons 64:256"
for t in 2 4 8 16 32 128 192; do
    [ "$t" -gt "$NCPU" ] && continue
    log " threads=$t"
    run_one umem prodcons "$t" 64:256 | emit "umem prodcons"
    run_one libc prodcons "$t" 64:256 | emit "libc prodcons"
done
log ""
log "## single-thread 64:256"
run_one umem single 1 64:256 | emit "umem single"
run_one libc single 1 64:256 | emit "libc single"
log ""
log "A/B done -> $OUT"

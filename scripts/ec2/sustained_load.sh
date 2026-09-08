#!/usr/bin/env bash
# scripts/ec2/sustained_load.sh <allocator> [duration_sec] [threads]
#
# Runs a SUSTAINED high-thread-count prodcons + frag workload for several
# minutes to surface tail-latency degradation and fragmentation growth
# under real duress (not just a quick burst) -- the "punishing load" part
# of the allocator shootout, distinct from matrix.sh's scaling sweep.
#
# Emits one row per workload to docs/results/<date>-<instance>-<arch>/
# sustained.toml. Run once per allocator on a *-hi role (full core count).
set -euo pipefail

ALLOC="${1:?usage: sustained_load.sh <allocator> [duration_sec=180] [threads=\$(nproc)]}"
DURATION="${2:-180}"
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)
THREADS="${3:-$NCPU}"
BENCH_BIN="${BENCH_BIN:-test/bench/.libs/bench_main}"
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$REPO_ROOT"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:}.libs"
export GLIBC_TUNABLES="${GLIBC_TUNABLES:-glibc.rtld.optional_static_tls=8388608}"

# Same per-allocator LD_PRELOAD resolution as matrix.sh (see its comment):
# never global, only for the one allocator under test.
IS_MUSL=0
{ ldd --version 2>&1 || true; } | grep -qi musl && IS_MUSL=1
preload_for() {
    case "$1" in
        scudo) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*scudo[_a-z]*[^ ]*\.so[^ ]*' | head -1 ;;
        jemalloc) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libjemalloc\.so[^ ]*' | head -1 ;;
        mimalloc) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libmimalloc\.so[^ ]*' | head -1 ;;
        rpmalloc) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*librpmalloc\.so[^ ]*' | head -1 ;;
        *) : ;;
    esac
}
preload_for_musl() {
    case "$1" in
        scudo) echo /usr/local/lib/libscudo_standalone.so ;;
        jemalloc) echo /usr/lib/libjemalloc.so.2 ;;
        mimalloc) echo /usr/lib/libmimalloc.so ;;
        rpmalloc) echo /usr/local/lib/librpmalloc.so ;;
        *) : ;;
    esac
}
PRELOAD=""
if [[ "$ALLOC" != libc && "$ALLOC" != umem ]]; then
    if [[ $IS_MUSL -eq 1 ]]; then PRELOAD="$(preload_for_musl "$ALLOC")"
    elif [[ "$ALLOC" == scudo ]]; then PRELOAD="$(preload_for scudo)"
    fi
fi

ARCH=$(uname -m)
INSTANCE=$( { TOK=$(curl -s --max-time 2 -X PUT "http://169.254.169.254/latest/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null); \
    curl -s --max-time 2 -H "X-aws-ec2-metadata-token: $TOK" \
        http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null; } || true )
[[ -z "$INSTANCE" ]] && INSTANCE="unknown"
DATE=$(date +%Y-%m-%d)
OUTDIR="docs/results/${DATE}-${INSTANCE}-${ARCH}"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/sustained.toml"

# operation count sized so the run takes roughly DURATION seconds --
# derived empirically per workload below rather than guessed once, since
# prodcons and frag have very different per-op costs. We estimate with a
# short calibration run, then scale to hit the target duration.
calibrate_and_run() {
    local workload="$1" min="$2" max="$3" label="$4"
    local calib_n=2000000 calib_start calib_end calib_sec target_n

    calib_start=$(date +%s.%N)
    LD_PRELOAD="${PRELOAD:-}" "$BENCH_BIN" -a "$ALLOC" -w "$workload" -t "$THREADS" \
        -n "$calib_n" -s "$min:$max" -c > /tmp/sustained_calib.csv 2>>"$OUTDIR/sustained.log" || true
    calib_end=$(date +%s.%N)
    calib_sec=$(awk "BEGIN{print $calib_end-$calib_start}")
    if awk "BEGIN{exit !($calib_sec>0)}"; then
        target_n=$(awk "BEGIN{n=int($calib_n*$DURATION/$calib_sec); if(n<$calib_n) n=$calib_n; print n}")
    else
        target_n=$((calib_n * 20))
    fi

    echo "  $ALLOC $workload sustained: target_n=$target_n (~${DURATION}s, calibrated from ${calib_sec}s @ ${calib_n})"
    local out rc
    set +e
    out=$(LD_PRELOAD="${PRELOAD:-}" "$BENCH_BIN" -a "$ALLOC" -w "$workload" -t "$THREADS" \
        -n "$target_n" -s "$min:$max" -c 2>>"$OUTDIR/sustained.log")
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        echo "  CRASH: $ALLOC $workload sustained rc=$rc" | tee -a "$OUTDIR/sustained.log"
        return
    fi
    local row; row=$(printf '%s\n' "$out" | grep "^$ALLOC," | tail -1)
    [[ -z "$row" ]] && return
    IFS=',' read -ra f <<< "$row"
    {
        echo ""
        echo "[[sustained]]"
        echo "allocator = \"${f[0]}\""
        echo "workload = \"$label\""
        echo "threads = ${f[2]}"
        echo "size = \"$min:$max\""
        echo "ops = ${f[3]}"
        echo "elapsed_sec = ${f[4]}"
        echo "ops_per_sec = ${f[5]}"
        echo "lat_min = ${f[6]}"
        echo "lat_p50 = ${f[7]}"
        echo "lat_p90 = ${f[8]}"
        echo "lat_p99 = ${f[9]}"
        echo "lat_p999 = ${f[10]}"
        echo "lat_max = ${f[11]}"
        echo "lat_mean = ${f[12]}"
        echo "peak_rss_bytes = ${f[13]}"
        echo "frag = ${f[15]}"
    } >> "$OUT"
}

if [[ ! -f "$OUT" ]]; then
    {
        echo "# sustained high-thread-count punishing-load results"
        echo "# threads=$THREADS duration_target_sec=$DURATION"
        echo "instance_type = \"$INSTANCE\""
        echo "arch = \"$ARCH\""
    } > "$OUT"
fi

echo "sustained load: $ALLOC threads=$THREADS duration~${DURATION}s -> $OUT"
calibrate_and_run prodcons 64 256 "prodcons-sustained"
calibrate_and_run frag 16 4096 "frag-sustained"
echo "done: $ALLOC"

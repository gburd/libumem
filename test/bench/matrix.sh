#!/usr/bin/env bash
#
# matrix.sh - authoritative allocator scaling matrix.
#
# Sweeps {single,multi,prodcons,frag} x threads{1..192, capped at vCPU}
# x sizes{16:64,64:256,256:1024,1024:4096} x allocators{libc,umem,+je/tc if
# present}, using the stabilized harness (warm-up discard + median-of-N + CoV,
# pinned via numactl/taskset). Emits one TOML per (instance,arch):
#
#   docs/results/<date>-<instance>-<arch>/matrix.toml   (per-point results)
#   docs/results/<date>-<instance>-<arch>/meta.toml     (provenance)
#
# Run on EC2 via scripts/ec2/run-remote.sh <role> "... test/bench/matrix.sh".
# single-thread and fragmentation are 1-thread workloads (not swept over
# thread counts); multi and prodcons are swept across the thread list.

set -euo pipefail

# --- config / defaults ------------------------------------------------------
OPERATIONS=10000000
RUNS=5
WARMUPS=1
PIN=1
WORKLOADS=(single multi prodcons frag)
THREAD_LADDER=(1 2 4 8 16 32 64 128 192)
SIZE_RANGES=("16:64" "64:256" "256:1024" "1024:4096")
BENCH_BIN="${BENCH_BIN:-.libs/bench_main}"
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTDIR=""                 # default derived from date/instance/arch below

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] [ALLOCATORS...]

Run the authoritative allocator scaling matrix and emit TOML results.

OPTIONS:
    -n COUNT     Operations per single-thread point (default: $OPERATIONS)
    -r RUNS      Measured runs per point; median + CoV (default: $RUNS)
    -W WARMUPS   Warm-up runs discarded per point (default: $WARMUPS)
    -o DIR       Output dir (default: docs/results/<date>-<instance>-<arch>)
    -t LIST      Comma-separated thread ladder override (default: $(IFS=,; echo "${THREAD_LADDER[*]}"))
    -s LIST      Comma-separated size-range override (default: $(IFS=,; echo "${SIZE_RANGES[*]}"))
    --no-pin     Do not pin threads / skip governor check (NOT authoritative)
    --quick      Small smoke sweep (few threads/sizes, 2 runs)
    -h           Help

ALLOCATORS: default = libc umem (+ jemalloc/tcmalloc auto-detected if present)
EOF
    exit "${1:-1}"
}

ALLOCATORS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -n) OPERATIONS="$2"; shift 2 ;;
        -r) RUNS="$2"; shift 2 ;;
        -W) WARMUPS="$2"; shift 2 ;;
        -o) OUTDIR="$2"; shift 2 ;;
        -t) IFS=',' read -ra THREAD_LADDER <<< "$2"; shift 2 ;;
        -s) IFS=',' read -ra SIZE_RANGES <<< "$2"; shift 2 ;;
        --no-pin) PIN=0; shift ;;
        --quick)
            OPERATIONS=1000000; RUNS=2; WARMUPS=1
            THREAD_LADDER=(1 4)
            SIZE_RANGES=("64:256")
            shift ;;
        -h|--help) usage 0 ;;
        *) ALLOCATORS+=("$1"); shift ;;
    esac
done

cd "$(dirname "${BASH_SOURCE[0]}")"   # test/bench
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:}../../.libs"
# Third-party allocators are loaded at runtime by allocators.c (dlopen or
# LD_PRELOAD detection -- see the comment at the top of that file for why:
# statically linking them collides with the process-wide malloc symbol).
# glibc's static-TLS surplus needs bumping for some of them under dlopen;
# this must be set before the process starts.
export GLIBC_TUNABLES="${GLIBC_TUNABLES:-glibc.rtld.optional_static_tls=8388608}"

# Some allocators (scudo always; on musl, every third-party allocator) only
# work via LD_PRELOAD, not dlopen (see allocators.c's file-header comment).
# LD_PRELOAD is a process-wide, no-undo setting: stacking multiple
# allocators' LD_PRELOAD in one invocation lets the LAST one silently win
# the global malloc symbol, contaminating libc's *own* baseline numbers
# (verified empirically on musl: allocator_libc's throughput/latency
# changed completely when 4 unrelated LD_PRELOADs were stacked). So
# LD_PRELOAD is resolved and applied per-allocator, per-invocation --
# never globally -- and is empty for libc/umem.
preload_for() {
    case "$1" in
        scudo)
            ldconfig -p 2>/dev/null | grep -oE '/[^ ]*scudo[_a-z]*[^ ]*\.so[^ ]*' | head -1 ;;
        jemalloc)
            ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libjemalloc\.so[^ ]*' | head -1 ;;
        tcmalloc)
            ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libtcmalloc(_minimal)?\.so[^ ]*' | head -1 ;;
        mimalloc)
            ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libmimalloc\.so[^ ]*' | head -1 ;;
        snmalloc)
            ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libsnmallocshim\.so[^ ]*' | head -1 ;;
        rpmalloc)
            ldconfig -p 2>/dev/null | grep -oE '/[^ ]*librpmalloc\.so[^ ]*' | head -1 ;;
        *) : ;;
    esac
}
# musl (no ldconfig): fall back to a fixed install path per allocator.
preload_for_musl() {
    case "$1" in
        scudo) echo /usr/local/lib/libscudo_standalone.so ;;
        jemalloc) echo /usr/lib/libjemalloc.so.2 ;;
        mimalloc) echo /usr/lib/libmimalloc.so ;;
        rpmalloc) echo /usr/local/lib/librpmalloc.so ;;
        *) : ;;
    esac
}
# On glibc, dlopen already works for everything except scudo (see
# allocators.c); LD_PRELOAD is opt-in per allocator there. On musl,
# nothing dlopens cleanly, so every non-libc/umem allocator needs it.
IS_MUSL=0
{ ldd --version 2>&1 || true; } | grep -qi musl && IS_MUSL=1
allocator_preload() {
    if [[ "$1" == libc || "$1" == umem ]]; then return 0; fi
    if [[ $IS_MUSL -eq 1 ]]; then
        preload_for_musl "$1"
    elif [[ "$1" == scudo ]]; then
        preload_for scudo
    fi
}

if [[ ! -x "$BENCH_BIN" ]]; then
    echo "$BENCH_BIN missing; building test/bench/bench_main ..." >&2
    ( cd "$REPO_ROOT" && make -j"$(nproc)" test/bench/bench_main ) >/dev/null 2>&1 || true
fi
if [[ ! -x "$BENCH_BIN" ]]; then
    echo "ERROR: $BENCH_BIN not found and build failed. Build it first: make -j\$(nproc) test/bench/bench_main" >&2
    exit 1
fi

# --- environment probe ------------------------------------------------------
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)
ARCH=$(uname -m)
# EC2 instance type (metadata service, IMDSv2); "unknown" off-EC2.
INSTANCE=$( { TOK=$(curl -s --max-time 2 -X PUT "http://169.254.169.254/latest/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null); \
    curl -s --max-time 2 -H "X-aws-ec2-metadata-token: $TOK" \
        http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null; } || true )
[[ -z "$INSTANCE" ]] && INSTANCE="unknown"
DATE=$(date +%Y-%m-%d)

if [[ -z "$OUTDIR" ]]; then
    OUTDIR="$REPO_ROOT/docs/results/${DATE}-${INSTANCE}-${ARCH}"
fi
mkdir -p "$OUTDIR"
MATRIX="$OUTDIR/matrix.toml"
META="$OUTDIR/meta.toml"
LOG="$OUTDIR/matrix.log"
: > "$LOG"

# --- governor check (only meaningful when the files exist) ------------------
GOV="unknown"
if [[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
    GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
    if [[ $PIN -eq 1 && "$GOV" != "performance" ]]; then
        echo "ERROR: authoritative matrix requires 'performance' governor (have: $GOV)." >&2
        echo "       Fix: sudo cpupower frequency-set -g performance (or bootstrap.sh)." >&2
        exit 1
    fi
fi

# --- allocator auto-detection -----------------------------------------------
# A row is produced only for available allocators (main skips alloc==NULL).
# Probes with the SAME per-allocator LD_PRELOAD run_point will use, so
# detection matches reality on musl (nothing dlopens) and for scudo.
probe_alloc() {
    local pre; pre="$(allocator_preload "$1")"
    LD_PRELOAD="${pre:-}" "$BENCH_BIN" -a "$1" -w single -n 1000 -s 16:16 -c 2>/dev/null \
        | grep -q "^$1,"
}
if [[ ${#ALLOCATORS[@]} -eq 0 ]]; then
    ALLOCATORS=(libc umem)
    for extra in jemalloc tcmalloc mimalloc snmalloc scudo rpmalloc; do
        if probe_alloc "$extra"; then ALLOCATORS+=("$extra"); fi
    done
fi

# thread list capped at vCPU (always keep 1)
THREADS=()
for t in "${THREAD_LADDER[@]}"; do
    (( t <= NCPU )) && THREADS+=("$t")
done
[[ ${#THREADS[@]} -eq 0 ]] && THREADS=(1)

# pin prefix for a given thread count (contiguous CPU set, local alloc)
pin_prefix() {
    local n="$1" last=$(( $1 - 1 ))
    [[ $PIN -eq 0 ]] && return 0
    (( last >= NCPU )) && last=$(( NCPU - 1 ))
    if command -v numactl >/dev/null 2>&1; then
        printf 'numactl --physcpubind=0-%s --localalloc -- ' "$last"
    elif command -v taskset >/dev/null 2>&1; then
        printf 'taskset -c 0-%s ' "$last"
    fi
}

echo "matrix: instance=$INSTANCE arch=$ARCH vcpu=$NCPU governor=$GOV"
echo "  allocators: ${ALLOCATORS[*]}"
echo "  threads:    ${THREADS[*]}"
echo "  sizes:      ${SIZE_RANGES[*]}"
echo "  runs=$RUNS warmups=$WARMUPS pin=$PIN ops=$OPERATIONS"
echo "  -> $MATRIX"

# --- provenance (meta.toml) -------------------------------------------------
{
    echo "# libumem benchmark matrix provenance"
    echo "captured = \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\""
    echo "instance_type = \"$INSTANCE\""
    echo "arch = \"$ARCH\""
    echo "vcpu = $NCPU"
    echo "governor = \"$GOV\""
    echo "thp = \"$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo unknown)\""
    echo "numa_balancing = \"$(cat /proc/sys/kernel/numa_balancing 2>/dev/null || echo unknown)\""
    echo "pinned = $([[ $PIN -eq 1 ]] && echo true || echo false)"
    echo "runs = $RUNS"
    echo "warmups = $WARMUPS"
    echo "operations = $OPERATIONS"
    echo "uname = \"$(uname -a)\""
    echo "gcc = \"$(gcc --version 2>/dev/null | head -1 || echo n/a)\""
    echo "glibc = \"$(ldd --version 2>/dev/null | head -1 || echo n/a)\""
    echo "git_sha = \"$(cd "$REPO_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)\""
    echo "allocators = [$(printf '"%s",' "${ALLOCATORS[@]}" | sed 's/,$//')]"
    echo ""
    echo "[cpu]"
    lscpu 2>/dev/null | sed 's/^/# /' || true
} > "$META"

# --- TOML header ------------------------------------------------------------
{
    echo "# libumem allocator scaling matrix"
    echo "# instance=$INSTANCE arch=$ARCH vcpu=$NCPU governor=$GOV pinned=$PIN"
    echo "# columns mirror bench CSV: ops_per_sec, latency percentiles (ns),"
    echo "# peak_rss_bytes, ops_cov (stddev/mean over $RUNS runs), unstable flag."
    echo "date = \"$DATE\""
    echo "instance_type = \"$INSTANCE\""
    echo "arch = \"$ARCH\""
    echo "vcpu = $NCPU"
} > "$MATRIX"

# CSV field indices (1-based, from bench_print_csv_header):
# 1 allocator 2 workload 3 threads 4 ops 5 elapsed_sec 6 ops_per_sec
# 7 min 8 p50 9 p90 10 p99 11 p999 12 max 13 mean
# 14 rss_bytes 15 alloc_bytes 16 frag 17 cpu_user 18 cpu_sys
# 19 ops_cov 20 runs 21 unstable
emit_point() {
    # $1=workload label $2=threads-requested  reads one CSV row on stdin
    local wl="$1" treq="$2" row
    row="$(cat)"
    [[ -z "$row" ]] && { echo "  (no row for $wl t=$treq)" >&2; return; }
    IFS=',' read -ra f <<< "$row"
    {
        echo ""
        echo "[[point]]"
        echo "allocator = \"${f[0]}\""
        echo "workload = \"$wl\""
        echo "threads_requested = $treq"
        echo "threads = ${f[2]}"
        echo "size = \"$SIZE\""
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
        echo "ops_cov = ${f[18]}"
        echo "runs = ${f[19]}"
        echo "unstable = ${f[20]}"
    } >> "$MATRIX"
}

run_point() {
    # $1=bench workload (single|multi|prodcons|frag) $2=threads $3=label
    # A crashing/failing allocator at one point must NOT abort the sweep
    # (e.g. umem currently SIGSEGVs on aarch64) -- capture the row, log a
    # crash, and continue so the rest of the matrix still lands.
    local w="$1" t="$2" lbl="$3" ops="$OPERATIONS" out rc pre
    if [[ "$w" == "multi" ]]; then ops=$(( OPERATIONS / t )); fi
    pre="$(allocator_preload "$ALLOC")"
    set +e
    out=$(LD_PRELOAD="${pre:-}" $(pin_prefix "$t") "$BENCH_BIN" -a "$ALLOC" -w "$w" -t "$t" \
        -n "$ops" -s "$SIZE" -r "$RUNS" -W "$WARMUPS" -c 2>>"$LOG")
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        echo "  CRASH: $ALLOC $w t=$t $SIZE rc=$rc (skipped)" | tee -a "$LOG"
    fi
    printf '%s\n' "$out" | { grep "^$ALLOC," || true; } | tail -1 | emit_point "$lbl" "$t"
}

# --- the sweep --------------------------------------------------------------
for ALLOC in "${ALLOCATORS[@]}"; do
    for SIZE in "${SIZE_RANGES[@]}"; do
        for wl in "${WORKLOADS[@]}"; do
            case "$wl" in
                single|frag)
                    echo "  $ALLOC $wl $SIZE t=1"
                    run_point "$wl" 1 "$wl" ;;
                multi|prodcons)
                    for t in "${THREADS[@]}"; do
                        echo "  $ALLOC $wl $SIZE t=$t"
                        run_point "$wl" "$t" "$wl"
                    done ;;
            esac
        done
    done
done

echo "matrix complete -> $MATRIX"

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
# Run on EC2 via scripts/ec2/verify-isolated.sh (preferred -- it supplies the
# commit identity this script records) or run-remote.sh, always under job.sh.
# single-thread is a 1-thread workload; multi, prodcons and frag are all swept
# across the thread list, and allocators alternate at the innermost loop so an
# A/B comparison is between adjacent-in-time points rather than between
# separately-batched runs.
#
# If the commit sha cannot be determined (run-remote.sh excludes .git), pass
# LIBUMEM_SHA=<sha>; see resolve_sha() below.

set -euo pipefail

# --- config / defaults ------------------------------------------------------
# OPERATIONS is the TOTAL operation budget per point, across all threads.
# It is passed to bench_main's -n unchanged: bench_main/bench_framework divide
# it by the point's thread count exactly once.  This script used to divide it
# as well for the multi workload, and bench_main divided again, so aggregate
# work fell as 1/threads^2 -- the 192-thread points measured ~52k total ops in
# ~3.8ms with >27% CoV (P2.1).  Never pre-divide here.
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
    -n TOTAL     TOTAL operations per point, across ALL threads (default: $OPERATIONS).
                 NOT per-thread: bench_main divides by the thread count itself.
                 A per-thread share below bench_framework.h's
                 BENCH_MIN_OPS_PER_THREAD is raised to it, and the point is
                 marked ops_floor_raised = true.
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
# P2.5: a result without its identity is not a result.  git_sha used to come
# out "unknown" in published matrices because run-remote.sh excludes .git from
# the rsync, so `git rev-parse` in the remote tree had no repository to read.
# Resolution order:
#   1. $LIBUMEM_SHA (pass it in explicitly)
#   2. ISOLATED_PROVENANCE, written by scripts/ec2/verify-isolated.sh
#   3. git rev-parse, for a local checkout that does have .git
# and "unknown" only when all three fail -- with the reason recorded.
resolve_sha() {
    if [[ -n "${LIBUMEM_SHA:-}" ]]; then
        echo "$LIBUMEM_SHA git_sha_source=env"
    elif [[ -r "$REPO_ROOT/ISOLATED_PROVENANCE" ]]; then
        local s
        s=$(sed -n 's/^sha=//p' "$REPO_ROOT/ISOLATED_PROVENANCE" | head -1)
        if [[ -n "$s" ]]; then echo "$s git_sha_source=ISOLATED_PROVENANCE"
        else echo "unknown git_sha_source=ISOLATED_PROVENANCE-unparsable"; fi
    elif s=$(cd "$REPO_ROOT" && git rev-parse HEAD 2>/dev/null) && [[ -n "$s" ]]; then
        echo "$s git_sha_source=git"
    else
        echo "unknown git_sha_source=no-git-dir-and-no-LIBUMEM_SHA"
    fi
}
read -r GIT_SHA SHA_SOURCE <<< "$(resolve_sha)"
SHA_SOURCE="${SHA_SOURCE#git_sha_source=}"
if [[ "$GIT_SHA" == "unknown" ]]; then
    echo "WARNING: commit sha unknown ($SHA_SOURCE). Pass LIBUMEM_SHA=<sha> or" >&2
    echo "         run via scripts/ec2/verify-isolated.sh; a matrix without a" >&2
    echo "         commit identity cannot be cited as evidence." >&2
fi

# Identity of the binaries that produced the numbers, not just of the source.
bin_digest() {
    [[ -r "$1" ]] || { echo "missing"; return; }
    { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1" 2>/dev/null; } |
        awk '{print $1}'
}
LIBUMEM_SO=$(ls ../../.libs/libumem.so.*.*.* 2>/dev/null | head -1)

# Versions/paths of the third-party allocator libraries actually loaded, so a
# shootout row can be traced to the library that produced it.
alloc_identity() {
    local a="$1" path=""
    case "$a" in
        libc) echo "path=\"(process libc)\", version=\"$({ ldd --version 2>&1 || true; } | head -1 | tr -d '"')\""; return ;;
        umem) echo "path=\"${LIBUMEM_SO:-unknown}\", digest=\"$(bin_digest "$LIBUMEM_SO")\""; return ;;
    esac
    path="$(preload_for "$a")"
    [[ -z "$path" && $IS_MUSL -eq 1 ]] && path="$(preload_for_musl "$a")"
    if [[ -z "$path" ]]; then
        # dlopen'd rather than preloaded: ask the loader where it lives.
        path=$(ldconfig -p 2>/dev/null | grep -oE "/[^ ]*${a}[^ ]*\.so[^ ]*" | head -1)
    fi
    if [[ -n "$path" && -r "$path" ]]; then
        echo "path=\"$path\", digest=\"$(bin_digest "$path")\", realpath=\"$(readlink -f "$path" 2>/dev/null || echo "$path")\""
    else
        echo "path=\"unresolved\", digest=\"unknown\""
    fi
}

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
    echo "# total operations per point, across ALL threads (never per-thread)"
    echo "operations_total_per_point = $OPERATIONS"
    echo "min_ops_per_thread_floor = $(grep -oE 'BENCH_MIN_OPS_PER_THREAD[[:space:]]+[0-9]+' bench_framework.h | awk '{print $2}' | head -1)"
    echo "uname = \"$(uname -a)\""
    echo "gcc = \"$(gcc --version 2>/dev/null | head -1 || echo n/a)\""
    echo "libc = \"$({ ldd --version 2>&1 || true; } | head -1)\""
    echo "git_sha = \"$GIT_SHA\""
    echo "git_sha_source = \"$SHA_SOURCE\""
    echo "configure_flags = \"$(sed -n 's/.*\$ \.\/configure//p' ../../config.log 2>/dev/null | head -1 | sed 's/"/\\"/g')\""
    echo "bench_bin = \"$BENCH_BIN\""
    echo "bench_bin_digest = \"$(bin_digest "$BENCH_BIN")\""
    echo "libumem_so = \"${LIBUMEM_SO:-missing}\""
    echo "libumem_so_digest = \"$(bin_digest "$LIBUMEM_SO")\""
    echo "allocators = [$(printf '"%s",' "${ALLOCATORS[@]}" | sed 's/,$//')]"
    echo ""
    for a in "${ALLOCATORS[@]}"; do
        echo "[allocator_identity.$a]"
        echo "$(alloc_identity "$a")" | tr ',' '\n' | sed 's/^ *//'
        echo ""
    done
    echo "[cpu]"
    lscpu 2>/dev/null | sed 's/^/# /' || true
} > "$META"

# --- TOML header ------------------------------------------------------------
{
    echo "# libumem allocator scaling matrix"
    echo "# instance=$INSTANCE arch=$ARCH vcpu=$NCPU governor=$GOV pinned=$PIN"
    echo "# columns mirror bench CSV: ops_per_sec, latency percentiles (ns),"
    echo "# peak_rss_bytes, ops_cov (stddev/mean over $RUNS runs), unstable flag."
    echo "#"
    echo "# UNITS (P2.1/P2.2 -- read before comparing to anything published"
    echo "# before 2026-09-22, which used different and partly wrong ones):"
    echo "#   total_ops           operations completed, summed over ALL threads"
    echo "#   ops_per_thread      total_ops / threads, as actually run"
    echo "#   threads             threads that RAN (may differ from requested)"
    echo "#   frag                rss_at_live_peak / live_bytes_at_peak, both"
    echo "#                       sampled at the SAME instant, at the LIVE-SET"
    echo "#                       PEAK (not at the worst ratio: maximising the"
    echo "#                       ratio finds the smallest denominator, which"
    echo "#                       once reported 505x while implied RSS was flat"
    echo "#                       at ~1.1GB).  frag_median summarises the"
    echo "#                       sampled series.  Absent for workloads with no"
    echo "#                       live set, or too few samples.  Read the PAIR"
    echo "#                       and vmhwm, never the quotient alone."
    echo "#   ops_floor_raised    true => the per-thread budget was below the"
    echo "#                       minimum and was raised; the point ran MORE"
    echo "#                       work than -n asked for."
    echo "date = \"$DATE\""
    echo "instance_type = \"$INSTANCE\""
    echo "arch = \"$ARCH\""
    echo "vcpu = $NCPU"
    echo "git_sha = \"$GIT_SHA\""
    echo "operations_total_per_point = $OPERATIONS"
} > "$MATRIX"

# CSV field indices (0-based into bash array f, from bench_print_csv_header):
#  0 allocator  1 workload  2 threads  3 total_ops  4 ops_per_thread
#  5 elapsed_sec  6 ops_per_sec
#  7 lat_min  8 p50  9 p90  10 p99  11 p999  12 max  13 mean
# 14 rss_at_live_peak  15 vmhwm_bytes  16 allocated_bytes
# 17 live_bytes_at_peak  18 live_bytes_median  19 frag  20 frag_median
# 21 frag_samples  22 cpu_user  23 cpu_sys  24 ops_cov  25 runs
# 26 unstable  27 ops_floor_raised
#
# frag/frag_median are EMPTY for workloads that define no fragmentation ratio
# (single/multi/prodcons hold no live set) and for series too thin to summarise.
# Emit them only when present rather than writing 0.0, so an undefined value
# cannot be read as a measured one.
#
# The fragmentation PAIR is emitted, never a lone quotient: RSS is
# near-monotonic, so the ratio's numerator carries history its denominator does
# not, and rss_at_live_peak vs vmhwm is what lets a reader tell "holds 2x the
# live set" from "RSS was already high".  live_bytes_median/frag_median show
# whether the peak sample is representative of a moving series.
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
        # threads = what actually ran.  A 1-thread workload asked for 192
        # must report 1; these two differing is information, not an error.
        echo "threads = ${f[2]}"
        echo "size = \"$SIZE\""
        echo "total_ops = ${f[3]}"
        echo "ops_per_thread = ${f[4]}"
        echo "elapsed_sec = ${f[5]}"
        echo "ops_per_sec = ${f[6]}"
        echo "lat_min = ${f[7]}"
        echo "lat_p50 = ${f[8]}"
        echo "lat_p90 = ${f[9]}"
        echo "lat_p99 = ${f[10]}"
        echo "lat_p999 = ${f[11]}"
        echo "lat_max = ${f[12]}"
        echo "lat_mean = ${f[13]}"
        echo "rss_at_live_peak = ${f[14]}"
        echo "vmhwm_bytes = ${f[15]}"
        echo "allocated_bytes = ${f[16]}"
        echo "live_bytes_at_peak = ${f[17]}"
        echo "live_bytes_median = ${f[18]}"
        echo "frag_samples = ${f[21]}"
        if [[ -n "${f[19]:-}" ]]; then
            echo "frag = ${f[19]}"
            echo "frag_median = ${f[20]}"
            echo "frag_definition = \"rss_at_live_peak / live_bytes_at_peak, sampled together at the live-set peak; frag_median is the median of the sampled rss/live series. Report the PAIR (rss_at_live_peak, live_bytes_at_peak) and vmhwm, not the quotient alone.\""
        else
            echo "# frag: undefined here (workload holds no live set, or too"
            echo "#       few samples to summarise -- see frag_samples)"
        fi
        echo "ops_cov = ${f[24]}"
        echo "runs = ${f[25]}"
        echo "unstable = $([[ "${f[26]}" == "1" ]] && echo true || echo false)"
        echo "ops_floor_raised = $([[ "${f[27]:-0}" == "1" ]] && echo true || echo false)"
    } >> "$MATRIX"
}

run_point() {
    # $1=bench workload (single|multi|prodcons|frag) $2=threads $3=label
    # A crashing/failing allocator at one point must NOT abort the sweep
    # (e.g. umem currently SIGSEGVs on aarch64) -- capture the row, log a
    # crash, and continue so the rest of the matrix still lands.
    #
    # -n is passed UNCHANGED: bench_main divides by the thread count.  This
    # function used to pass OPERATIONS/t for the multi workload, which
    # bench_main then divided again (P2.1).
    local w="$1" t="$2" lbl="$3" out rc pre
    pre="$(allocator_preload "$ALLOC")"
    set +e
    out=$(LD_PRELOAD="${pre:-}" $(pin_prefix "$t") "$BENCH_BIN" -a "$ALLOC" -w "$w" -t "$t" \
        -n "$OPERATIONS" -s "$SIZE" -r "$RUNS" -W "$WARMUPS" -c 2>>"$LOG")
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        echo "  CRASH: $ALLOC $w t=$t $SIZE rc=$rc (skipped)" | tee -a "$LOG"
    fi
    printf '%s\n' "$out" | { grep "^$ALLOC," || true; } | tail -1 | emit_point "$lbl" "$t"
}

# --- the sweep --------------------------------------------------------------
# Alternate allocators at the INNERMOST level, not the outermost: batching all
# of one allocator's points together lets slow drift (thermal, neighbour
# noise, page-cache state) land differently on each allocator and show up as a
# difference between them.  With the allocator loop inside, the A and B points
# for a given (workload, size, threads) are adjacent in time.
#
# 'frag' is swept across the thread ladder like multi/prodcons: it is genuinely
# multithreaded now (it used to hard-code one thread while being reported as a
# 192-thread workload).
for SIZE in "${SIZE_RANGES[@]}"; do
    for wl in "${WORKLOADS[@]}"; do
        case "$wl" in
            single)
                for ALLOC in "${ALLOCATORS[@]}"; do
                    echo "  $ALLOC $wl $SIZE t=1"
                    run_point "$wl" 1 "$wl"
                done ;;
            multi|prodcons|frag)
                for t in "${THREADS[@]}"; do
                    for ALLOC in "${ALLOCATORS[@]}"; do
                        echo "  $ALLOC $wl $SIZE t=$t"
                        run_point "$wl" "$t" "$wl"
                    done
                done ;;
        esac
    done
done

echo "matrix complete -> $MATRIX"

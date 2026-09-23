#!/usr/bin/env bash
# scripts/ec2/sustained_load.sh <allocator>[,<allocator>...] [duration_sec] [threads]
#
# Runs a SUSTAINED high-thread-count prodcons + frag workload for several
# minutes to surface tail-latency degradation and fragmentation growth under
# real duress (not just a quick burst) -- the "punishing load" part of the
# allocator shootout, distinct from matrix.sh's scaling sweep.
#
# WHAT CHANGED 2026-09-22 (P2.1/P2.2/P2.5), and why any older sustained.toml
# is not comparable to a new one:
#
#   * PER-WINDOW rows, not one whole-run aggregate.  A single p999 over a
#     3-minute run cannot show whether the tail degraded over that run; a
#     whole-run RSS cannot show fragmentation growing.  Each allocator now
#     emits one row per window (bench_main -A), each with its own latency
#     distribution and its own RSS/live-bytes pair.
#   * ALTERNATING A/B when several allocators are named.  Batching all of one
#     allocator's windows and then all of the next lets slow drift (thermal,
#     neighbour noise) land differently on each and appear as a difference
#     between allocators.  Windows now interleave: A,B,A,B,...
#   * MATCHED protocols.  Every allocator gets the same warm-up count, window
#     count, thread count, AND THE SAME OPERATION BUDGET.  Equalising
#     wall-clock instead (calibrating each allocator separately) hands them
#     different amounts of work and destroys the comparison -- see the note at
#     calibrate().  Equal work with unequal duration is the honest form.
#   * FULL provenance, including the commit sha (which used to be recorded as
#     "unknown" because run-remote.sh excludes .git) and a digest of every
#     binary involved.
#   * frag honours the thread count, and its ratio is RSS / live bytes sampled
#     together at the LIVE-SET PEAK -- not at the worst observed ratio, which
#     just finds the smallest denominator (it once reported 505x while implied
#     RSS was flat at ~1.1GB across every thread count).  The PAIR
#     (rss_at_live_peak, live_bytes_at_peak) plus vmhwm is emitted, because a
#     lone quotient cannot separate allocator overhead from RSS that was
#     already high.  Older files' frag column is a different, wrong quantity.
#
# Emits docs/results/<date>-<instance>-<arch>/sustained.toml.
# Run under scripts/ec2/job.sh; prefer verify-isolated.sh so the sha is known.
set -euo pipefail

ALLOC_ARG="${1:?usage: sustained_load.sh <alloc>[,<alloc>...] [duration_sec=180] [threads=\$(nproc)]}"
IFS=',' read -ra ALLOCS <<< "$ALLOC_ARG"
DURATION="${2:-180}"
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)
THREADS="${3:-$NCPU}"
# Windows per allocator.  The run is split into WINDOWS measured segments so
# the tail and RSS can be read as a time series.
WINDOWS="${SUSTAINED_WINDOWS:-6}"
# frag size range.  16:4096 (the historical default) at 192 threads holds
# ~10 GB live at the 100k/thread floor, which is above umem's ~5 GB Linux
# heap ceiling -- so every umem window measured the ceiling (39% alloc
# failures, 2026-09-22), not fragmentation.  The comparison run sets 64:256.
IFS=: read -r FRAG_MIN FRAG_MAX <<< "${SUSTAINED_FRAG_SIZES:-16:4096}"
WARMUPS="${SUSTAINED_WARMUPS:-1}"
BENCH_BIN="${BENCH_BIN:-test/bench/.libs/bench_main}"
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$REPO_ROOT"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:}.libs"
export GLIBC_TUNABLES="${GLIBC_TUNABLES:-glibc.rtld.optional_static_tls=8388608}"

if [[ ! -x $BENCH_BIN ]]; then
    echo "SKIP: $BENCH_BIN not built -- nothing measured" >&2
    exit 77
fi

# Same per-allocator LD_PRELOAD resolution as matrix.sh (see its comment):
# never global, only for the one allocator under test.
IS_MUSL=0
{ ldd --version 2>&1 || true; } | grep -qi musl && IS_MUSL=1
preload_for() {
    case "$1" in
        scudo) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*scudo[_a-z]*[^ ]*\.so[^ ]*' | head -1 ;;
        jemalloc) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libjemalloc\.so[^ ]*' | head -1 ;;
        tcmalloc) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libtcmalloc(_minimal)?\.so[^ ]*' | head -1 ;;
        mimalloc) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libmimalloc\.so[^ ]*' | head -1 ;;
        snmalloc) ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libsnmallocshim\.so[^ ]*' | head -1 ;;
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
preload_of() {
    local a="$1"
    [[ $a == libc || $a == umem ]] && return 0
    if [[ $a == umem-preload ]]; then
        ls .libs/libumem_malloc.so.*.*.* 2>/dev/null | head -1; return 0
    fi
    if [[ $IS_MUSL -eq 1 ]]; then preload_for_musl "$a"
    elif [[ $a == scudo ]]; then preload_for scudo
    fi
}
# "name@tag" = null-control alias: same allocator, rows labelled with the tag
# (see matrix.sh).  The binary only ever sees the base name.
alloc_base() { echo "${1%%@*}"; }

digest() {
    [[ -r "$1" ]] || { echo missing; return; }
    { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1" 2>/dev/null; } | awk '{print $1}'
}

# P2.5: commit identity.  run-remote.sh excludes .git, so `git rev-parse` in a
# synced tree has no repository -- which is how published matrices recorded
# git_sha = "unknown".  Prefer an explicit sha, then verify-isolated.sh's
# provenance file, then git.
if [[ -n ${LIBUMEM_SHA:-} ]]; then
    GIT_SHA="$LIBUMEM_SHA"; SHA_SRC=env
elif [[ -r ISOLATED_PROVENANCE ]]; then
    GIT_SHA=$(sed -n 's/^sha=//p' ISOLATED_PROVENANCE | head -1)
    SHA_SRC=ISOLATED_PROVENANCE
    [[ -z $GIT_SHA ]] && { GIT_SHA=unknown; SHA_SRC=ISOLATED_PROVENANCE-unparsable; }
elif GIT_SHA=$(git rev-parse HEAD 2>/dev/null) && [[ -n $GIT_SHA ]]; then
    SHA_SRC=git
else
    GIT_SHA=unknown; SHA_SRC=no-git-dir-and-no-LIBUMEM_SHA
fi
if [[ $GIT_SHA == unknown ]]; then
    echo "WARNING: commit sha unknown ($SHA_SRC).  Pass LIBUMEM_SHA=<sha> or run" >&2
    echo "         via verify-isolated.sh; results without an identity are not" >&2
    echo "         citable evidence." >&2
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
# SUSTAINED_OUT names the file inside OUTDIR (default sustained.toml) so two
# groups of allocators can be run separately without overwriting each other.
OUT="$OUTDIR/${SUSTAINED_OUT:-sustained.toml}"
LOG="$OUTDIR/${SUSTAINED_OUT:-sustained.toml}.log"
LIBUMEM_SO=$(ls .libs/libumem.so.*.*.* 2>/dev/null | head -1)
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)

{
    echo "# sustained high-thread-count punishing-load results"
    echo "# PER-WINDOW rows: one row per measured window per allocator, with"
    echo "# that window's own latency percentiles and its own RSS/live-bytes"
    echo "# pair.  Windows for the named allocators INTERLEAVE (A,B,A,B,...)"
    echo "# so drift over the run does not masquerade as a difference between"
    echo "# allocators.  Not comparable to sustained.toml files written before"
    echo "# 2026-09-22: those aggregate a whole run and their frag column is a"
    echo "# different (and wrong) quantity.  See P2.1/P2.2 in"
    echo "# docs/plans/2026-09-21-production-readiness.md."
    echo "captured = \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\""
    echo "instance_type = \"$INSTANCE\""
    echo "arch = \"$ARCH\""
    echo "vcpu = $NCPU"
    echo "threads = $THREADS"
    echo "governor = \"$GOV\""
    echo "duration_target_sec_per_window = $DURATION"
    echo "windows_per_allocator = $WINDOWS"
    echo "warmup_windows_discarded = $WARMUPS"
    echo "frag_sizes = \"$FRAG_MIN:$FRAG_MAX\""
    echo "prodcons_sizes = \"64:256\""
    echo "allocators = [$(printf '"%s",' "${ALLOCS[@]}" | sed 's/,$//')]"
    echo "order = \"interleaved\""
    echo "git_sha = \"$GIT_SHA\""
    echo "git_sha_source = \"$SHA_SRC\""
    echo "configure_flags = \"$(sed -n 's/.*\$ \.\/configure//p' config.log 2>/dev/null | head -1 | sed 's/"/\\"/g')\""
    echo "uname = \"$(uname -a)\""
    echo "gcc = \"$(gcc --version 2>/dev/null | head -1 || echo n/a)\""
    echo "libc = \"$({ ldd --version 2>&1 || true; } | head -1)\""
    echo "bench_bin = \"$BENCH_BIN\""
    echo "bench_bin_digest = \"$(digest "$BENCH_BIN")\""
    echo "libumem_so = \"${LIBUMEM_SO:-missing}\""
    echo "libumem_so_digest = \"$(digest "$LIBUMEM_SO")\""
    for a in "${ALLOCS[@]}"; do
        b="$(alloc_base "$a")"
        p="$(preload_of "$b" || true)"
        echo ""
        echo "[allocator_identity.\"$a\"]"
        [[ "$a" == *@* ]] && echo "null_control_alias_of = \"$b\""
        if [[ -n ${p:-} ]]; then
            echo "path = \"$p\""
            echo "realpath = \"$(readlink -f "$p" 2>/dev/null || echo "$p")\""
            echo "digest = \"$(digest "$p")\""
            [[ $b == umem-preload ]] && echo "libumem_digest = \"$(digest "$LIBUMEM_SO")\""
        elif [[ $b == umem ]]; then
            echo "path = \"${LIBUMEM_SO:-unknown}\""
            echo "digest = \"$(digest "$LIBUMEM_SO")\""
        elif [[ $b == libc ]]; then
            echo "path = \"(process libc)\""
            echo "version = \"$({ ldd --version 2>&1 || true; } | head -1 | tr -d '"')\""
        else
            echo "path = \"dlopen-resolved (see allocators.c)\""
        fi
    done
} > "$OUT"

# Calibrate the per-window operation budget PER WORKLOAD -- deliberately NOT
# per (allocator, workload).
#
# Calibrating per allocator gives each one a DIFFERENT amount of work, chosen
# so that each takes ~DURATION seconds.  That equalises wall-clock and thereby
# destroys the comparison: the throughput columns then differ because the
# budgets differ, not because the allocators do.  Observed on the first
# 192-thread run: frag calibration handed libc 36,540,723 ops and umem
# 14,909,682, and umem's window ran 113s against libc's 8.5s.  Nothing in
# those two rows could be compared.
#
# So: calibrate on the SLOWEST allocator (so no window is absurdly short) and
# give every allocator that same budget.  Windows then differ in DURATION,
# which is the honest outcome -- equal work, unequal time -- and Mops/s is
# comparable across allocators.  -n is a TOTAL budget across threads
# (bench_main divides by the thread count itself; passing a pre-divided value
# was the P2.1 double division).
declare -A TARGET_N
calibrate() {
    local a; a="$(alloc_base "$1")"; local w="$2" min="$3" max="$4"
    local calib_n=2000000 t0 t1 sec n
    local pre; pre="$(preload_of "$a" || true)"
    t0=$(date +%s.%N)
    LD_PRELOAD="${pre:-}" "$BENCH_BIN" -a "$a" -w "$w" -t "$THREADS" \
        -n "$calib_n" -s "$min:$max" -c >/dev/null 2>>"$LOG" || true
    t1=$(date +%s.%N)
    sec=$(awk "BEGIN{print $t1-$t0}")
    if awk "BEGIN{exit !($sec>0.01)}"; then
        n=$(awk "BEGIN{n=int($calib_n*$DURATION/$sec); if(n<$calib_n) n=$calib_n; print n}")
    else
        n=$((calib_n * 20))
    fi
    # Never below bench_framework.h's per-thread floor, or bench_main raises
    # it internally and flags every window ops_floor_raised.  prodcons splits
    # the budget over t/2 producers, so its floor is half.
    local floor=$(( THREADS * 100000 ))
    (( n < floor )) && n=$floor
    CALIB_N="$n"
    echo "  calibrate $a $w: would need total_n=$n (~${DURATION}s/window, from ${sec}s @ $calib_n; floor $floor)"
}

emit_windows() {
    # $1=allocator $2=workload $3=min $4=max $5=label
    local label_a="$1" a; a="$(alloc_base "$1")"; local w="$2" min="$3" max="$4" label="$5"
    local pre; pre="$(preload_of "$a" || true)"
    local n="${TARGET_N["$w"]}"
    local out rc
    set +e
    # -A: one CSV row per measured window, each with its own percentiles/RSS.
    out=$(LD_PRELOAD="${pre:-}" "$BENCH_BIN" -a "$a" -w "$w" -t "$THREADS" \
        -n "$n" -s "$min:$max" -r 1 -W 0 -A -c 2>>"$LOG")
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        echo "  CRASH: $label_a $w rc=$rc (window not recorded)" | tee -a "$LOG"
        return
    fi
    local row
    row=$(printf '%s\n' "$out" | grep "^$a," | tail -1 | sed "s/^$a,/$label_a,/")
    [[ -z "$row" ]] && { echo "  (no row: $label_a $w)" | tee -a "$LOG"; return; }
    IFS=',' read -ra f <<< "$row"
    # Field order from bench_print_csv_header (0-based):
    #  0 allocator 1 workload 2 threads 3 total_ops 4 ops_per_thread
    #  5 elapsed_sec 6 ops_per_sec 7..13 latency 14 rss_at_live_peak
    # 15 vmhwm_bytes 16 allocated_bytes 17 live_bytes_at_peak
    # 18 live_bytes_median 19 frag 20 frag_median 21 frag_samples
    # 22/23 cpu 24 ops_cov 25 runs 26 unstable 27 ops_floor_raised
    # 28 alloc_failures
    {
        echo ""
        echo "[[window]]"
        echo "allocator = \"${f[0]}\""
        echo "workload = \"$label\""
        echo "window = $WINDOW_INDEX"
        echo "threads = ${f[2]}"
        echo "size = \"$min:$max\""
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
        else
            echo "# frag: undefined here (no live set, or too few samples)"
        fi
        echo "ops_floor_raised = $([[ "${f[27]:-0}" == "1" ]] && echo true || echo false)"
        echo "alloc_failures = ${f[28]:-0}"
    } >> "$OUT"
    printf '  %-12s %-18s w=%-2s ops=%-9s mops=%8.3f p99=%9s p999=%10s rss@peak=%s%s\n' \
        "$label_a" "$label" "$WINDOW_INDEX" "${f[3]}" \
        "$(awk "BEGIN{print ${f[6]}/1e6}")" "${f[10]}" "${f[11]}" "${f[14]}" \
        "$([[ "${f[28]:-0}" != "0" ]] && echo "  ALLOC_FAILURES=${f[28]}" || echo "")"
}

echo "sustained load: allocators=${ALLOCS[*]} threads=$THREADS"
echo "  windows=$WINDOWS x ~${DURATION}s each, interleaved; warmups=$WARMUPS"
echo "  sha=$GIT_SHA ($SHA_SRC) -> $OUT"

# One budget per workload: the largest requirement across allocators, i.e. the
# slowest allocator's.  Every allocator then does identical work.
for w in prodcons frag; do
    case "$w" in
        prodcons) mn=64; mx=256 ;;
        frag)     mn=$FRAG_MIN; mx=$FRAG_MAX ;;
    esac
    best=0
    for a in "${ALLOCS[@]}"; do
        calibrate "$a" "$w" "$mn" "$mx"
        # Smallest n = slowest allocator (it needs fewer ops for DURATION).
        if [[ $best -eq 0 ]] || (( CALIB_N < best )); then best="$CALIB_N"; fi
    done
    TARGET_N["$w"]="$best"
    echo "  -> $w: ALL allocators get total_n=$best (matched work; windows will"
    echo "     differ in duration, which is the honest result)"
done

# Matched warm-up: every allocator gets the same number of discarded windows
# before any measured window is recorded.
for (( wu = 0; wu < WARMUPS; wu++ )); do
    for a0 in "${ALLOCS[@]}"; do
        a="$(alloc_base "$a0")"
        echo "  warmup window $wu: $a0 (discarded)"
        pre="$(preload_of "$a" || true)"
        LD_PRELOAD="${pre:-}" "$BENCH_BIN" -a "$a" -w prodcons -t "$THREADS" \
            -n "${TARGET_N["prodcons"]}" -s 64:256 -c >/dev/null 2>>"$LOG" || true
        LD_PRELOAD="${pre:-}" "$BENCH_BIN" -a "$a" -w frag -t "$THREADS" \
            -n "${TARGET_N["frag"]}" -s "$FRAG_MIN:$FRAG_MAX" -c >/dev/null 2>>"$LOG" || true
    done
done

# Interleaved measured windows.
for (( WINDOW_INDEX = 0; WINDOW_INDEX < WINDOWS; WINDOW_INDEX++ )); do
    for a in "${ALLOCS[@]}"; do
        emit_windows "$a" prodcons 64 256 "prodcons-sustained"
        emit_windows "$a" frag "$FRAG_MIN" "$FRAG_MAX" "frag-sustained"
    done
done

echo "done: ${ALLOCS[*]} -> $OUT"

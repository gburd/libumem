#!/usr/bin/env bash
# scripts/ec2/allocator_comparison.sh - the 2026-09-23 allocator comparison.
#
# One job, one build, one box, so the identity of every number is the same.
#
#   0. availability: which competitor allocators actually load (allocators.c
#      reports the rest as "(not available)"), with library versions/digests.
#      A missing competitor is RECORDED, never silently dropped.
#   1. the matrix, WITH THE NULL CONTROL INSIDE IT: umem@null and
#      umem-preload@null are the same builds re-labelled, alternated at the
#      innermost loop with every competitor, so each grid point carries its
#      own "umem vs itself" delta.  That is the rig's resolution; a
#      cross-allocator delta inside it is noise and is reported as such.
#      Arms: libc, umem (API + 16-byte wrapper header), umem-preload (the real
#      LD_PRELOAD drop-in), and every competitor that loaded.  Fixed TOTAL
#      work per point across all arms (matrix.sh never pre-divides;
#      ops_floor_raised must be false), -R replicate processes per arm.
#      Budgets are per WORKLOAD, identical across arms at a point:
#        single/multi  OPS          (>= 100k/thread at the top thread count)
#        prodcons      PRODCONS_OPS (divided by producers = t/2)
#        frag          FRAG_OPS     (live set ~ FRAG_OPS/4 objects, so sized
#                                    to keep umem's RSS under its ~5 GB
#                                    Linux ceiling -- otherwise every point
#                                    measures the ceiling, not fragmentation)
#        frag 1k:4k    FRAG_BIG_OPS at FRAG_BIG_THREADS only (same reason)
#   2. one explicit CEILING PROBE: frag 1k:4k at the top thread count with
#      the full budget, umem vs libc, one replicate.  Records alloc_failures
#      at HEAD with provenance; it is the known-open item, not a new gap.
#   3. one SUSTAINED run per arm at the box's top thread count (matched
#      work, per-window rows, interleaved).
#   4. diagnostics on umem only: umem_dump_contention() at the top thread
#      count, both API and preload paths; perf record (PERF=1, metal).
#
# Everything lands under docs/results/<date>-<instance>-<arch>/ inside the
# (isolated) tree.  Fetch with rsync afterwards.
#
# Usage (through verify-isolated.sh so LIBUMEM_SHA is real):
#   ./scripts/ec2/verify-isolated.sh intel-lo@perf HEAD cmp 28800 \
#       'THREADS=1,2,4,8 ./scripts/ec2/allocator_comparison.sh'
#   ./scripts/ec2/verify-isolated.sh intel-hi@perf HEAD cmp 36000 \
#       'THREADS=1,8,32,64,128,192 RUNS=3 PERF=1 ./scripts/ec2/allocator_comparison.sh'
set -uo pipefail

THREADS="${THREADS:-1,2,4,8}"
SIZES="${SIZES:-16:64,64:256,256:1024,1024:4096}"
OPS="${OPS:-20000000}"                            # single
MULTI_OPS="${MULTI_OPS:-$OPS}"                    # multi (metal: raise so t=192 is not a 50 ms point)
MULTI_THREADS="${MULTI_THREADS:-$THREADS}"
MULTI_HI_OPS="${MULTI_HI_OPS:-}"                  # optional 2nd multi pass with a bigger budget
MULTI_HI_THREADS="${MULTI_HI_THREADS:-}"          #   at the high thread counts only (metal)
PRODCONS_OPS="${PRODCONS_OPS:-10000000}"          # divided by producers (= t/2); >= 100k/producer at t=192
PRODCONS_THREADS="${PRODCONS_THREADS:-$THREADS}"
PRODCONS_SIZES="${PRODCONS_SIZES:-$SIZES}"
# frag live set ~= budget/4 objects per thread (floor 100k ops/thread => 25k
# objects/thread minimum).  Budgets and thread caps below keep the live set in
# the 0.2-2 GB range at every size so umem's ~5 GB ceiling is probed ONCE, in
# step 2, rather than contaminating every frag point.
FRAG_OPS="${FRAG_OPS:-20000000}"                  # 16:64, 64:256: all threads (>=100k/thread at t=192)
FRAG_MID_OPS="${FRAG_MID_OPS:-8000000}"           # 256:1024: threads <= 64
FRAG_BIG_OPS="${FRAG_BIG_OPS:-3200000}"           # 1k:4k:    threads <= 32 (=100k/thread at t=32)
RUNS="${RUNS:-3}"
WARM="${WARM:-1}"
REPS="${REPS:-2}"
SUSTAINED_SEC="${SUSTAINED_SEC:-20}"
SUSTAINED_WINDOWS="${SUSTAINED_WINDOWS:-4}"
SUSTAINED_FRAG_SIZES="${SUSTAINED_FRAG_SIZES:-16:64}"
PERF="${PERF:-0}"
SKIP_INSTALL="${SKIP_INSTALL:-0}"
NCPU=$(nproc)
HI_T="${THREADS##*,}"
threads_le() { local r; r=$(tr ',' '\n' <<< "$THREADS" | awk -v c="$1" '$1<=c' | paste -sd, -); echo "${r:-1}"; }
FRAG_MID_THREADS=$(threads_le 64)
FRAG_BIG_THREADS=$(threads_le 32)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# ---- commit identity -------------------------------------------------------
if [[ -z "${LIBUMEM_SHA:-}" && -r ISOLATED_PROVENANCE ]]; then
    LIBUMEM_SHA="$(sed -n 's/^sha=//p' ISOLATED_PROVENANCE | head -1)"
fi
export LIBUMEM_SHA="${LIBUMEM_SHA:-unknown}"
echo "== allocator comparison  sha=$LIBUMEM_SHA  ncpu=$NCPU  threads=$THREADS  $(date -u +%FT%TZ)"
echo "   ops=$OPS multi_ops=$MULTI_OPS@t=$MULTI_THREADS prodcons_ops=$PRODCONS_OPS@t=$PRODCONS_THREADS/$PRODCONS_SIZES frag_ops=$FRAG_OPS frag_mid=$FRAG_MID_OPS@t=$FRAG_MID_THREADS frag_big=$FRAG_BIG_OPS@t=$FRAG_BIG_THREADS runs=$RUNS warm=$WARM reps=$REPS"

# ---- 0. competitor allocators ---------------------------------------------
if [[ "$SKIP_INSTALL" != 1 ]]; then
    echo "== installing competitor allocators (install_extra_allocators.sh)"
    ./scripts/ec2/install_extra_allocators.sh > /tmp/install_allocators.log 2>&1
    echo "   rc=$? (log: /tmp/install_allocators.log)"
    grep -E 'BUILD FAILED|NOT FOUND' /tmp/install_allocators.log || true
fi

# ---- build -----------------------------------------------------------------
echo "== build"
./scripts/ec2/clean-regen.sh > /tmp/regen.log 2>&1 || { echo "clean-regen FAILED"; tail -20 /tmp/regen.log; exit 1; }
./configure > /tmp/configure.log 2>&1 || { echo "configure FAILED"; tail -20 /tmp/configure.log; exit 1; }
make -j"$NCPU" > /tmp/make.log 2>&1 || { echo "make FAILED"; tail -40 /tmp/make.log; exit 1; }
make -j"$NCPU" test/bench/bench_main test/bench/bench_contention > /tmp/make2.log 2>&1 || { echo "make bench FAILED"; tail -40 /tmp/make2.log; exit 1; }
ls -la .libs/libumem.so.*.*.* .libs/libumem_malloc.so.*.*.* test/bench/.libs/bench_main

ARCH=$(uname -m)
INSTANCE=$( { TOK=$(curl -s --max-time 2 -X PUT "http://169.254.169.254/latest/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null); \
    curl -s --max-time 2 -H "X-aws-ec2-metadata-token: $TOK" \
        http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null; } || true )
[[ -z "$INSTANCE" ]] && INSTANCE=unknown
DATE=$(date +%Y-%m-%d)
OUT="$REPO_ROOT/docs/results/${DATE}-${INSTANCE}-${ARCH}"   # absolute: matrix.sh cds into test/bench
mkdir -p "$OUT"
cp /tmp/install_allocators.log "$OUT/install_allocators.log" 2>/dev/null || true

# ---- availability table ----------------------------------------------------
echo "== allocator availability"
AVAIL="$OUT/availability.toml"
{
    echo "# which allocators loaded on this host, with identity"
    echo "captured = \"$(date -u +%FT%TZ)\""
    echo "instance_type = \"$INSTANCE\""
    echo "arch = \"$ARCH\""
    echo "vcpu = $NCPU"
    echo "git_sha = \"$LIBUMEM_SHA\""
    echo "glibc = \"$(ldd --version | head -1)\""
    echo "gcc = \"$(gcc --version | head -1)\""
    echo "kernel = \"$(uname -r)\""
    echo "mem_total_kb = $(awk '/MemTotal/{print $2}' /proc/meminfo)"
    echo "vm_max_map_count = $(cat /proc/sys/vm/max_map_count)"
    echo ""
} > "$AVAIL"
export LD_LIBRARY_PATH=".libs:${LD_LIBRARY_PATH:-}"
export GLIBC_TUNABLES="glibc.rtld.optional_static_tls=8388608"
PRELOAD_SO=$(ls .libs/libumem_malloc.so.*.*.* | head -1)
SCUDO_SO=$(ldconfig -p 2>/dev/null | grep -oE '/[^ ]*scudo[_a-z]*[^ ]*\.so[^ ]*' | head -1)
avail_of() {
    local a="$1" pre=""
    case "$a" in
        umem-preload) pre="$PRELOAD_SO" ;;
        scudo) pre="$SCUDO_SO" ;;
    esac
    if LD_PRELOAD="$pre" test/bench/.libs/bench_main -a "$a" -w single -n 1000 -s 16:16 -c 2>/dev/null | grep -q "^$a,"; then
        echo yes; else echo no; fi
}
lib_of() {
    case "$1" in
        libc) echo "(process glibc)" ;;
        umem) ls .libs/libumem.so.*.*.* | head -1 ;;
        umem-preload) echo "$PRELOAD_SO" ;;
        jemalloc) ldconfig -p | grep -oE '/[^ ]*libjemalloc\.so[^ ]*' | head -1 ;;
        tcmalloc) ldconfig -p | grep -oE '/[^ ]*libtcmalloc(_minimal)?\.so[^ ]*' | head -1 ;;
        mimalloc) ldconfig -p | grep -oE '/[^ ]*libmimalloc\.so[^ ]*' | head -1 ;;
        snmalloc) ldconfig -p | grep -oE '/[^ ]*libsnmallocshim\.so[^ ]*' | head -1 ;;
        scudo) echo "$SCUDO_SO" ;;
        rpmalloc) ldconfig -p | grep -oE '/[^ ]*librpmalloc\.so[^ ]*' | head -1 ;;
    esac
}
ver_of() {
    case "$1" in
        libc) ldd --version | head -1 ;;
        umem|umem-preload) echo "libumem $LIBUMEM_SHA" ;;
        jemalloc) rpm -q jemalloc 2>/dev/null || echo "?" ;;
        tcmalloc) rpm -q gperftools-libs 2>/dev/null || echo "?" ;;
        scudo) rpm -q compiler-rt 2>/dev/null || echo "?" ;;
        mimalloc) (cd /tmp/build/mimalloc-src 2>/dev/null && echo "git $(git log -1 --format='%h %cs')") || echo "?" ;;
        snmalloc) (cd /tmp/build/snmalloc-src 2>/dev/null && echo "git $(git log -1 --format='%h %cs')") || echo "?" ;;
        rpmalloc) (cd /tmp/build/rpmalloc-src 2>/dev/null && echo "git $(git log -1 --format='%h %cs')") || echo "?" ;;
    esac
}
LOADED=(); MISSING=()
for a in libc umem umem-preload jemalloc tcmalloc mimalloc snmalloc scudo rpmalloc; do
    ok=$(avail_of "$a"); lib=$(lib_of "$a"); ver=$(ver_of "$a")
    dg="n/a"; [[ -r "$lib" ]] && dg=$(sha256sum "$lib" | cut -c1-16)
    printf '  %-13s %-4s %-60s %s\n' "$a" "$ok" "${lib:-unresolved}" "$ver"
    {
        echo "[allocator.\"$a\"]"
        echo "loaded = $([[ $ok == yes ]] && echo true || echo false)"
        echo "path = \"${lib:-unresolved}\""
        echo "version = \"$ver\""
        echo "digest16 = \"$dg\""
        echo ""
    } >> "$AVAIL"
    if [[ $ok == yes ]]; then LOADED+=("$a"); else MISSING+=("$a"); fi
done
echo "   loaded:  ${LOADED[*]}"
echo "   MISSING: ${MISSING[*]:-none}"

ARMS=()
for a in "${LOADED[@]}"; do
    ARMS+=("$a")
    case "$a" in umem|umem-preload) ARMS+=("$a@null") ;; esac
done

run_matrix() {  # $1=subdir $2=ops $3=threads $4=sizes $5=workloads $6=reps
    local sub="$1" ops="$2" th="$3" sz="$4" wl="$5" reps="$6"
    test/bench/matrix.sh -o "$OUT/$sub" -n "$ops" -r "$RUNS" -W "$WARM" -R "$reps" \
        -t "$th" -s "$sz" -w "$wl" "${ARMS[@]}" > "$OUT/$sub.log" 2>&1
    echo "   rc=$? -> $OUT/$sub/matrix.toml  ($(grep -c '^\[\[point\]\]' "$OUT/$sub/matrix.toml" 2>/dev/null) points, floor_raised=$(grep -c 'ops_floor_raised = true' "$OUT/$sub/matrix.toml" 2>/dev/null), failures>0: $(grep -c 'alloc_failures = [1-9]' "$OUT/$sub/matrix.toml" 2>/dev/null))"
}

# ---- 1. THE MATRIX ---------------------------------------------------------
echo "== 1a. single  ops=$OPS  arms: ${ARMS[*]}  ($(date -u +%T))"
run_matrix single "$OPS" 1 "$SIZES" single "$REPS"
echo "== 1a'. multi  ops=$MULTI_OPS threads=$MULTI_THREADS  ($(date -u +%T))"
run_matrix multi "$MULTI_OPS" "$MULTI_THREADS" "$SIZES" multi "$REPS"
if [[ -n "$MULTI_HI_OPS" && -n "$MULTI_HI_THREADS" ]]; then
    echo "== 1a''. multi-hi  ops=$MULTI_HI_OPS threads=$MULTI_HI_THREADS  ($(date -u +%T))"
    run_matrix multi-hi "$MULTI_HI_OPS" "$MULTI_HI_THREADS" "$SIZES" multi "$REPS"
fi
echo "== 1b. prodcons  ops=$PRODCONS_OPS threads=$PRODCONS_THREADS sizes=$PRODCONS_SIZES  ($(date -u +%T))"
run_matrix prodcons "$PRODCONS_OPS" "$PRODCONS_THREADS" "$PRODCONS_SIZES" prodcons "$REPS"
echo "== 1c. frag 16:64,64:256  ops=$FRAG_OPS  ($(date -u +%T))"
run_matrix frag "$FRAG_OPS" "$THREADS" 16:64,64:256 frag "$REPS"
echo "== 1d. frag 256:1024  ops=$FRAG_MID_OPS threads=$FRAG_MID_THREADS  ($(date -u +%T))"
run_matrix frag-mid "$FRAG_MID_OPS" "$FRAG_MID_THREADS" 256:1024 frag "$REPS"
echo "== 1e. frag 1k:4k  ops=$FRAG_BIG_OPS threads=$FRAG_BIG_THREADS  ($(date -u +%T))"
run_matrix frag-big "$FRAG_BIG_OPS" "$FRAG_BIG_THREADS" 1024:4096 frag "$REPS"

# ---- 2. CEILING PROBE ------------------------------------------------------
# Budget chosen so the live set is well past 5 GB: >= 12M ops => >= 3M live
# objects * ~2.5 KB = 7.7 GB (libc RSS ~9 GB: fits a 16 GiB lo box).  At
# t=192 the 100k/thread floor lifts it to 19.2M / 12 GB; never floor-raised.
CEIL_OPS=$(( HI_T * 100000 )); (( CEIL_OPS < 12000000 )) && CEIL_OPS=12000000
echo "== 2. ceiling probe: frag 1k:4k t=$HI_T ops=$CEIL_OPS (live set >> 5 GB), libc vs umem vs umem-preload, 1 rep  ($(date -u +%T))"
SAVE_ARMS=("${ARMS[@]}"); ARMS=(libc umem umem-preload)
RUNS_SAVE=$RUNS; RUNS=1
run_matrix ceiling "$CEIL_OPS" "$HI_T" 1024:4096 frag 1
RUNS=$RUNS_SAVE; ARMS=("${SAVE_ARMS[@]}")

# ---- 3. SUSTAINED ----------------------------------------------------------
echo "== 3. SUSTAINED at t=$HI_T: ${ARMS[*]}  ($(date -u +%T))"
SUSTAINED_WINDOWS="$SUSTAINED_WINDOWS" SUSTAINED_WARMUPS=1 SUSTAINED_FRAG_SIZES="$SUSTAINED_FRAG_SIZES" \
    ./scripts/ec2/sustained_load.sh "$(IFS=,; echo "${ARMS[*]}")" "$SUSTAINED_SEC" "$HI_T" \
    > "$OUT/sustained.log" 2>&1
echo "   rc=$? -> $OUT/sustained.toml ($(grep -c '^\[\[window\]\]' "$OUT/sustained.toml" 2>/dev/null) windows)"

# ---- 4. DIAGNOSTICS (umem only) --------------------------------------------
echo "== 4. contention dumps at t=$HI_T  ($(date -u +%T))"
last=$((HI_T-1)); (( last >= NCPU )) && last=$((NCPU-1))
PIN="numactl --physcpubind=0-$last --localalloc --"
command -v numactl >/dev/null || PIN=""
for w in multi prodcons frag; do
    for s in 16:64 64:256 256:1024; do
        o="$MULTI_OPS"; [[ $w == prodcons ]] && o="$PRODCONS_OPS"; [[ $w == frag ]] && o="$FRAG_OPS"
        $PIN test/bench/.libs/bench_contention -a umem -w "$w" -t "$HI_T" -n "$o" -s "$s" \
            > "$OUT/contention-umem-$w-t$HI_T-${s/:/_}.txt" 2>&1
        LD_PRELOAD="$PRELOAD_SO" $PIN test/bench/.libs/bench_contention -a umem-preload -w "$w" -t "$HI_T" -n "$o" -s "$s" \
            > "$OUT/contention-umem-preload-$w-t$HI_T-${s/:/_}.txt" 2>&1
    done
done
# and single-thread, where the depot is irrelevant and the per-op cost shows
test/bench/.libs/bench_contention -a umem -w multi -t 1 -n "$OPS" -s 16:64 > "$OUT/contention-umem-multi-t1-16_64.txt" 2>&1
LD_PRELOAD="$PRELOAD_SO" test/bench/.libs/bench_contention -a umem-preload -w multi -t 1 -n "$OPS" -s 16:64 > "$OUT/contention-umem-preload-multi-t1-16_64.txt" 2>&1
echo "   $(ls "$OUT"/contention-*.txt | wc -l) dumps"

if [[ "$PERF" == 1 ]] && command -v perf >/dev/null; then
    echo "== 4b. perf record  ($(date -u +%T))"
    sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1 || true
    sudo sysctl -w kernel.kptr_restrict=0 >/dev/null 2>&1 || true
    for spec in "multi:$HI_T:64:256:$((MULTI_OPS*10))" "prodcons:$HI_T:64:256:$((PRODCONS_OPS*2))" \
                "frag:$HI_T:64:256:$FRAG_OPS" "single:1:16:64:$((OPS*3))" "multi:8:16:64:$((OPS*3))"; do
        IFS=: read -r w t mn mx pops <<< "$spec"
        last=$((t-1)); (( last >= NCPU )) && last=$((NCPU-1))
        for arm in umem umem-preload libc; do
            pre=""; [[ $arm == umem-preload ]] && pre="$PRELOAD_SO"
            tag="perf-$arm-$w-t$t-${mn}_$mx"
            LD_PRELOAD="$pre" perf record -F 499 -g -o "/tmp/$tag.data" -- \
                numactl --physcpubind=0-"$last" --localalloc -- \
                test/bench/.libs/bench_main -a "$arm" -w "$w" -t "$t" -n "$pops" -s "$mn:$mx" -r 1 -W 0 -c \
                > "$OUT/$tag.csv" 2>"$OUT/$tag.stderr"
            perf report -i "/tmp/$tag.data" --stdio --no-children -g none --percent-limit 0.5 2>/dev/null \
                | grep -v '^#' | grep -v '^$' | head -60 > "$OUT/$tag.flat.txt"
            perf report -i "/tmp/$tag.data" --stdio --children -g none --percent-limit 1 2>/dev/null \
                | grep -v '^#' | grep -v '^$' | head -60 > "$OUT/$tag.children.txt"
            perf report -i "/tmp/$tag.data" --stdio -g folded,0.5,caller,count --no-children 2>/dev/null \
                | grep -v '^#' | grep -v '^$' | head -400 > "$OUT/$tag.folded.txt"
            echo "   $tag: $(wc -l < "$OUT/$tag.flat.txt") flat lines"
            rm -f "/tmp/$tag.data"
        done
    done
fi

echo "== done  ($(date -u +%FT%TZ)) -> $OUT"
du -sh "$OUT"; ls "$OUT"

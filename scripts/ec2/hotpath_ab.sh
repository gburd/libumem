#!/usr/bin/env bash
# scripts/ec2/hotpath_ab.sh - A/B of a hot-path change on the SAME box.
#
# Three arms built from `git archive` extractions in $PWD/{pre,post,pre2}:
# pre, post, and an independent rebuild of pre as the null control.  Every
# point is measured as REPLICATES alternating (A, B) pairs and reported as
# the median per-pair delta; the null (pre vs pre2) is measured FIRST at
# every point so the rig's own spread is on the record before any A/B delta
# is read.  A post delta inside the null's min..max is noise.
#
# Instrument: test/bench/bench_pairs (bare alloc-N-then-free-N loop, no
# histogram, no per-op clock) -- bench_main's own overhead is ~70 % of its
# cycles at t=1 (team brief 2026-09-24).  Plus `perf stat` instructions per
# pair at t=1, which is load-independent.
#
#   ./scripts/ec2/job.sh intel-lo@hot start ab 3600 \
#     'cd ~/libumem-ab && PRE=<sha> POST=<sha> ./post/scripts/ec2/hotpath_ab.sh'
set +e
NCPU=$(nproc)
PRE="${PRE:?pre sha}"; POST="${POST:?post sha}"
OUTDIR="${OUTDIR:-$PWD/results}"; mkdir -p "$OUTDIR"
LOG="$OUTDIR/hotpath-ab.txt"; CSV="$OUTDIR/hotpath-ab.csv"
: > "$LOG"; : > "$CSV"
log() { echo "$@" | tee -a "$LOG"; }
REPLICATES="${REPLICATES:-9}"
DUR="${DUR:-1}"
# points: "size N threads"
POINTS="${POINTS:-512 1 1|512 64 1|512 128 1|16:64 1 1|16:64 64 1|16:1024 1 1|16:1024 64 1|512 1 8|512 128 8|16:64 1 8|16:64 64 8|16:1024 1 8|16:1024 64 8}"

build_into() {  # $1 tag $2 src
	local tag="$1" src="$2" so
	( cd "$src" && ./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && \
	  make -j"$NCPU" >/dev/null 2>&1 && \
	  make -j"$NCPU" test/bench/bench_pairs >/dev/null 2>&1 ) || { log "build $tag FAILED"; return 1; }
	so="$(ls "$src"/.libs/libumem.so.*.*.* | head -1)"
	rm -rf "/tmp/hab_$tag"; mkdir -p "/tmp/hab_$tag"
	cp "$so" "/tmp/hab_$tag/$(objdump -p "$so" | awk '/SONAME/{print $2}')"
	cp "$src/test/bench/.libs/bench_pairs" "/tmp/hab_$tag/bench_pairs"
	log "  built $tag from $src: libumem.so sha256=$(sha256sum "$so" | cut -c1-16)"
}

run_one() {  # $1 tag $2 size $3 N $4 threads -> Mpairs/s
	local last=$(( $4 - 1 )); (( last >= NCPU )) && last=$((NCPU-1))
	LD_LIBRARY_PATH="/tmp/hab_$1" numactl --physcpubind=0-"$last" --localalloc -- \
	    "/tmp/hab_$1/bench_pairs" -s "$2" -n "$3" -t "$4" -d "$DUR" | awk '{print $(NF-1)}'
}
insn_one() {  # $1 tag $2 size $3 N -> instructions per pair (t=1)
	local out ins pairs
	out="$(LD_LIBRARY_PATH="/tmp/hab_$1" perf stat -x, -e instructions:u \
	    numactl --physcpubind=0 -- "/tmp/hab_$1/bench_pairs" -s "$2" -n "$3" -t 1 -d "$DUR" 2>&1)"
	ins="$(echo "$out" | awk -F, '/instructions/{print $1}')"
	pairs="$(echo "$out" | awk '/Mpairs/{print $(NF-1)*1e6*'"$DUR"'}')"
	awk -v i="$ins" -v p="$pairs" 'BEGIN{ if (p>0) printf "%.1f", i/p; else print "nan" }'
}

measure() {  # $1 A $2 B $3 size $4 N $5 threads $6 label $7 fn
	local A="$1" B="$2" s="$3" n="$4" t="$5" label="$6" fn="$7" i a b d deltas=()
	for i in $(seq 1 "$REPLICATES"); do
		a="$($fn "$A" "$s" "$n" "$t")"; b="$($fn "$B" "$s" "$n" "$t")"
		echo "$label,$i,$A,$a,$B,$b" >> "$CSV"
		d="$(awk -v a="$a" -v b="$b" 'BEGIN{ if (a>0) printf "%.3f",(b-a)/a*100; else print "nan" }')"
		deltas+=("$d")
	done
	printf '%s\n' "${deltas[@]}" | sort -g | awk -v l="$label" -v a="$a" -v b="$b" '
	    {v[NR]=$1} END{ med=(NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2
	    printf "  %-34s last A=%-9s B=%-9s  median %+7.2f%%  (min %+7.2f%%, max %+7.2f%%, n=%d)\n", l, a, b, med, v[1], v[NR], NR }' | tee -a "$LOG"
}

log "# hotpath A/B  vcpu=$NCPU  $(date -u +%FT%TZ)  pre=$PRE post=$POST  replicates=$REPLICATES dur=${DUR}s"
log "# instance=$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null) gcc=$(gcc -dumpversion)"
log "## builds"
build_into pre  "$PWD/pre"  || exit 1
build_into post "$PWD/post" || exit 1
build_into pre2 "$PWD/pre2" || exit 1
cmp -s /tmp/hab_pre/libumem.so.1 /tmp/hab_post/libumem.so.1 && { log "FATAL: pre and post libumem.so byte-identical"; exit 1; }
log ""
IFS='|' read -ra PTS <<< "$POINTS"
log "## bench_pairs Mpairs/s (delta = B relative to A)"
for p in "${PTS[@]}"; do
	set -- $p; s="$1"; n="$2"; t="$3"
	[ "$t" -gt "$NCPU" ] && continue
	measure pre pre2 "$s" "$n" "$t" "null  s=$s N=$n t=$t" run_one
	measure pre post "$s" "$n" "$t" "A/B   s=$s N=$n t=$t" run_one
done
log ""
log "## perf stat instructions per pair, t=1 (delta = B relative to A; lower is better)"
for p in "${PTS[@]}"; do
	set -- $p; s="$1"; n="$2"; t="$3"
	[ "$t" -ne 1 ] && continue
	measure pre pre2 "$s" "$n" 1 "null  insn s=$s N=$n" insn_one
	measure pre post "$s" "$n" 1 "A/B   insn s=$s N=$n" insn_one
done
log "# done $(date -u +%FT%TZ)"

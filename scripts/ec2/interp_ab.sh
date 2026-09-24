#!/usr/bin/env bash
# scripts/ec2/interp_ab.sh - A/B of an interposer (malloc_interpose.c /
# malloc.c) change on the SAME box, P8.3.
#
# Three arms from `git archive` extractions in $PWD/{pre,post,pre2}
# (ship_ab_arms.sh); pre2 is an independent rebuild of pre for the null
# control.  Each arm is a libumem.so + libumem_malloc.so pair.  The instrument
# is ONE bench_pairs binary (from post, sha256 logged; see hotpath_ab.sh for
# why one binary) run two ways against each arm:
#
#   api      bench_pairs            (umem_alloc/umem_free, no preload)
#   preload  bench_pairs -m         under LD_PRELOAD=<arm>/libumem_malloc.so
#
# Every point: REPLICATES alternating (A, B) pairs, median per-pair delta,
# null (pre vs pre2) first.  Reported per point:
#   - preload arm: pre vs post Mpairs/s (the candidate's effect)
#   - preload/api ratio for pre and for post (the P8.3 number)
#   - t=1 only: perf stat instructions per pair, api and preload, per arm
#
#   ./scripts/ec2/job.sh intel-lo@interp start ab 3600 \
#     'cd ~/libumem-ab && PRE=<sha> POST=<sha> ./post/scripts/ec2/interp_ab.sh'
set +e
NCPU=$(nproc)
PRE="${PRE:?pre sha}"; POST="${POST:?post sha}"
OUTDIR="${OUTDIR:-$PWD/results}"; mkdir -p "$OUTDIR"
LOG="$OUTDIR/interp-ab.txt"; CSV="$OUTDIR/interp-ab.csv"
: > "$LOG"; : > "$CSV"
log() { echo "$@" | tee -a "$LOG"; }
REPLICATES="${REPLICATES:-9}"
DUR="${DUR:-1}"
# points: "size N threads"
POINTS="${POINTS:-16:64 1 1|16:64 64 1|16:64 1 8|16:64 64 8}"

build_into() {  # $1 tag $2 src
	local tag="$1" src="$2" so mso
	( cd "$src" && ./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && \
	  make -j"$NCPU" >/dev/null 2>&1 && \
	  make -j"$NCPU" test/bench/bench_pairs >/dev/null 2>&1 ) || { log "build $tag FAILED"; return 1; }
	so="$(ls "$src"/.libs/libumem.so.*.*.* | head -1)"
	mso="$(ls "$src"/.libs/libumem_malloc.so.*.*.* | head -1)"
	rm -rf "/tmp/iab_$tag"; mkdir -p "/tmp/iab_$tag"
	cp "$so" "/tmp/iab_$tag/$(objdump -p "$so" | awk '/SONAME/{print $2}')"
	cp "$mso" "/tmp/iab_$tag/$(objdump -p "$mso" | awk '/SONAME/{print $2}')"
	cp "$mso" "/tmp/iab_$tag/libumem_malloc.so"
	log "  built $tag from $src: libumem.so sha256=$(sha256sum "$so" | cut -c1-16)  libumem_malloc.so sha256=$(sha256sum "$mso" | cut -c1-16)"
}
BENCH=/tmp/iab_bench_pairs

run_one() {  # $1 tag $2 arm(api|preload) $3 size $4 N $5 threads -> Mpairs/s
	local last=$(( $5 - 1 )) pre="" m=""; (( last >= NCPU )) && last=$((NCPU-1))
	[ "$2" = preload ] && { pre="/tmp/iab_$1/libumem_malloc.so"; m="-m"; }
	LD_LIBRARY_PATH="/tmp/iab_$1" LD_PRELOAD="$pre" numactl --physcpubind=0-"$last" --localalloc -- \
	    "$BENCH" -s "$3" -n "$4" -t "$5" -d "$DUR" $m | awk '{print $(NF-1)}'
}
insn_one() {  # $1 tag $2 arm $3 size $4 N -> instructions per pair (t=1)
	local out ins pairs pre="" m=""
	[ "$2" = preload ] && { pre="/tmp/iab_$1/libumem_malloc.so"; m="-m"; }
	out="$(LD_LIBRARY_PATH="/tmp/iab_$1" LD_PRELOAD="$pre" perf stat -x, -e instructions:u \
	    numactl --physcpubind=0 -- "$BENCH" -s "$3" -n "$4" -t 1 -d "$DUR" $m 2>&1)"
	ins="$(echo "$out" | awk -F, '/instructions/{print $1}')"
	pairs="$(echo "$out" | awk '/Mpairs/{print $(NF-1)*1e6*'"$DUR"'}')"
	awk -v i="$ins" -v p="$pairs" 'BEGIN{ if (p>0) printf "%.1f", i/p; else print "nan" }'
}

median() { printf '%s\n' "$@" | sort -g | awk '{v[NR]=$1} END{ printf "%.3f", (NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2 }'; }

# Alternating pairs of (A,armA) and (B,armB); prints median delta B rel. A
# plus the medians of each side.
measure() {  # $1 A $2 armA $3 B $4 armB $5 size $6 N $7 threads $8 label $9 fn
	local A="$1" aA="$2" B="$3" aB="$4" s="$5" n="$6" t="$7" label="$8" fn="$9" i a b d deltas=() as=() bs=()
	for i in $(seq 1 "$REPLICATES"); do
		a="$($fn "$A" "$aA" "$s" "$n" "$t")"; b="$($fn "$B" "$aB" "$s" "$n" "$t")"
		echo "$label,$i,$A,$aA,$a,$B,$aB,$b" >> "$CSV"
		d="$(awk -v a="$a" -v b="$b" 'BEGIN{ if (a>0) printf "%.3f",(b-a)/a*100; else print "nan" }')"
		deltas+=("$d"); as+=("$a"); bs+=("$b")
	done
	printf '%s\n' "${deltas[@]}" | sort -g | awk -v l="$label" -v a="$(median "${as[@]}")" -v b="$(median "${bs[@]}")" '
	    {v[NR]=$1} END{ med=(NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2
	    printf "  %-40s medA=%-9s medB=%-9s  B/A=%.3f  median delta %+7.2f%%  (min %+7.2f%%, max %+7.2f%%, n=%d)\n", l, a, b, (a>0?b/a:0), med, v[1], v[NR], NR }' | tee -a "$LOG"
}

log "# interposer A/B  vcpu=$NCPU  $(date -u +%FT%TZ)  pre=$PRE post=$POST  replicates=$REPLICATES dur=${DUR}s"
log "# instance=$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null) gcc=$(gcc -dumpversion)"
log "## builds"
build_into pre  "$PWD/pre"  || exit 1
build_into post "$PWD/post" || exit 1
build_into pre2 "$PWD/pre2" || exit 1
cmp -s /tmp/iab_pre/libumem_malloc.so /tmp/iab_post/libumem_malloc.so && cmp -s /tmp/iab_pre/libumem.so.1 /tmp/iab_post/libumem.so.1 && { log "FATAL: pre and post byte-identical"; exit 1; }
log "  pre vs post libumem.so differ: $(cmp -s /tmp/iab_pre/libumem.so.1 /tmp/iab_post/libumem.so.1 && echo no || echo yes)"
log "  pre vs post libumem_malloc.so differ: $(cmp -s /tmp/iab_pre/libumem_malloc.so /tmp/iab_post/libumem_malloc.so && echo no || echo yes)"
cp "$PWD/post/test/bench/.libs/bench_pairs" "$BENCH"
log "  bench_pairs (one binary, from post): sha256=$(sha256sum "$BENCH" | cut -c1-16)"
log "  preload resolves: $(LD_LIBRARY_PATH=/tmp/iab_post LD_PRELOAD=/tmp/iab_post/libumem_malloc.so ldd "$BENCH" | grep umem | tr -s ' ' | tr '\n' ';')"
log ""
IFS='|' read -ra PTS <<< "$POINTS"
log "## bench_pairs Mpairs/s"
for p in "${PTS[@]}"; do
	set -- $p; s="$1"; n="$2"; t="$3"
	[ "$t" -gt "$NCPU" ] && continue
	measure pre  preload pre2 preload "$s" "$n" "$t" "null  preload s=$s N=$n t=$t" run_one
	measure pre  preload post preload "$s" "$n" "$t" "A/B   preload s=$s N=$n t=$t" run_one
	measure pre  api     pre  preload "$s" "$n" "$t" "ratio pre  preload/api s=$s N=$n t=$t" run_one
	measure post api     post preload "$s" "$n" "$t" "ratio post preload/api s=$s N=$n t=$t" run_one
done
log ""
log "## perf stat instructions per pair, t=1 (lower is better)"
for p in "${PTS[@]}"; do
	set -- $p; s="$1"; n="$2"; t="$3"
	[ "$t" -ne 1 ] && continue
	measure pre  preload pre2 preload "$s" "$n" 1 "null  insn preload s=$s N=$n" insn_one
	measure pre  preload post preload "$s" "$n" 1 "A/B   insn preload s=$s N=$n" insn_one
	measure pre  api     pre  preload "$s" "$n" 1 "insn  pre  preload/api s=$s N=$n" insn_one
	measure post api     post preload "$s" "$n" 1 "insn  post preload/api s=$s N=$n" insn_one
done
log "# done $(date -u +%FT%TZ)"

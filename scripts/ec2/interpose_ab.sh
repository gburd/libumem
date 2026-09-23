#!/usr/bin/env bash
# scripts/ec2/interpose_ab.sh - A/B of the LD_PRELOAD interposer fix
# (a74065e, "stop taking a global mutex on every free()") against the commit
# just before it (5513c81), on the SAME box, with the same protocol as
# p54_mangle_ab.sh: two arms built from git archives into their own
# directories, REPLICATE alternating pairs per point, and a NULL CONTROL
# (pre vs an independent rebuild of pre) run FIRST so the rig's own
# resolution is on the record before any A/B delta is read.
#
# The two shas differ ONLY in malloc_interpose.c (verified with
# `git diff --stat 5513c81 a74065e`), so libumem.so itself is the same source
# in both arms; the arm-specific library is libumem_malloc.so, and the API
# arm (bench_main -a umem, no LD_PRELOAD) is measured at post only, as the
# reference the preload arm is being compared against.
#
# Run via job.sh in a directory that holds both archives:
#   ./scripts/ec2/job.sh intel-hi@perf start ab 7200 \
#     'cd ~/libumem-ab && PRE=5513c81 POST=a74065e ./post/scripts/ec2/interpose_ab.sh'
# where ~/libumem-ab/pre and ~/libumem-ab/post are `git archive` extractions.
set +e
NCPU=$(nproc)
PRE="${PRE:?pre sha}"; POST="${POST:?post sha}"
OUTDIR="${OUTDIR:-$PWD/results}"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/interpose-ab.csv"; LOG="$OUTDIR/interpose-ab.txt"
: > "$OUT"; : > "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

RUNS="${RUNS:-3}"; WARM="${WARM:-1}"
OPS="${OPS:-20000000}"
REPLICATES="${REPLICATES:-3}"
THREADS="${THREADS:-1 8 32 64 128 192}"

build_into() {  # $1 = tag, $2 = source dir
	local tag="$1" src="$2" so mso
	( cd "$src" && ./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && ./configure >/dev/null 2>&1 && \
	  make -j"$NCPU" >/dev/null 2>&1 && make -j"$NCPU" test/bench/bench_main >/dev/null 2>&1 ) || { log "build $tag FAILED"; return 1; }
	so="$(ls "$src"/.libs/libumem.so.*.*.* | head -1)"; mso="$(ls "$src"/.libs/libumem_malloc.so.*.*.* | head -1)"
	rm -rf "/tmp/ab_$tag"; mkdir -p "/tmp/ab_$tag"
	cp "$so" "/tmp/ab_$tag/$(objdump -p "$so" | awk '/SONAME/{print $2}')"
	cp "$mso" "/tmp/ab_$tag/$(objdump -p "$mso" | awk '/SONAME/{print $2}')"
	cp "$mso" "/tmp/ab_$tag/libumem_malloc.so"
	cp "$src/test/bench/.libs/bench_main" "/tmp/ab_$tag/bench_main"
	log "  built $tag from $src: libumem.so sha256=$(sha256sum "$so" | cut -c1-16)  libumem_malloc.so sha256=$(sha256sum "$mso" | cut -c1-16)"
}

run_one() {  # $1=tag $2=arm(umem|umem-preload) $3=workload $4=threads $5=size
	local tag="$1" arm="$2" w="$3" t="$4" s="$5" last=$(( $4 - 1 )) pre=""
	(( last >= NCPU )) && last=$((NCPU-1))
	[ "$arm" = umem-preload ] && pre="/tmp/ab_$tag/libumem_malloc.so"
	LD_LIBRARY_PATH="/tmp/ab_$tag" LD_PRELOAD="$pre" \
	    numactl --physcpubind=0-"$last" --localalloc -- \
	    "/tmp/ab_$tag/bench_main" -a "$arm" -w "$w" -t "$t" -n "$OPS" -s "$s" -r "$RUNS" -W "$WARM" -c 2>/dev/null \
	    | grep "^$arm," | tail -1
}
ops_of()   { echo "$1" | awk -F, '{print $7}'; }
field_of() { echo "$2" | awk -F, -v n="$1" '{print $n}'; }

measure_point() {  # $1=tagA $2=tagB $3=arm $4=workload $5=threads $6=size $7=label
	local A="$1" B="$2" arm="$3" w="$4" t="$5" s="$6" label="$7"
	local i rowA rowB oa ob d deltas=() flags=""
	log "  $label  arm=$arm workload=$w threads=$t sizes=$s  (${REPLICATES} pairs)"
	for i in $(seq 1 "$REPLICATES"); do
		rowA="$(run_one "$A" "$arm" "$w" "$t" "$s")"
		rowB="$(run_one "$B" "$arm" "$w" "$t" "$s")"
		{ [ -z "$rowA" ] || [ -z "$rowB" ]; } && { log "    pair $i: missing row"; continue; }
		echo "$A,$label,$i,$rowA" >> "$OUT"
		echo "$B,$label,$i,$rowB" >> "$OUT"
		oa="$(ops_of "$rowA")"; ob="$(ops_of "$rowB")"
		d="$(awk -v a="$oa" -v b="$ob" 'BEGIN{printf "%.3f",(b-a)/a*100}')"
		deltas+=("$d")
		[ "$(field_of 28 "$rowA")" != 0 ] && flags="$flags floorA:$i"
		[ "$(field_of 28 "$rowB")" != 0 ] && flags="$flags floorB:$i"
		printf '    pair %d: %-6s %10.3f Mops p999=%7s   %-6s %10.3f Mops p999=%7s   delta %+9.2f%%\n' \
		    "$i" "$A" "$(awk -v v="$oa" 'BEGIN{print v/1e6}')" "$(field_of 12 "$rowA")" \
		    "$B" "$(awk -v v="$ob" 'BEGIN{print v/1e6}')" "$(field_of 12 "$rowB")" "$d" | tee -a "$LOG"
	done
	printf '%s\n' "${deltas[@]}" | sort -g | awk -v l="$label" -v f="$flags" '
	    {v[NR]=$1}
	    END{ if(NR==0){printf "    %s: no data\n",l; exit}
	         med=(NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2
	         printf "    => %s: median delta %+.2f%%  (min %+.2f%%, max %+.2f%%, n=%d)%s\n",
	                l, med, v[1], v[NR], NR, (f==""?"":"  FLAGS:"f) }' | tee -a "$LOG"
}

log "# interpose A/B  vcpu=$NCPU  $(date -u +%FT%TZ)  pre=$PRE post=$POST"
log "# runs=$RUNS warmups=$WARM total_ops=$OPS replicates=$REPLICATES gcc=$(gcc -dumpversion) instance=$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)"
log ""
log "## builds"
build_into pre   "$PWD/pre"   || exit 1
build_into post  "$PWD/post"  || exit 1
build_into nullB "$PWD/pre2"  || exit 1
cmp -s /tmp/ab_pre/libumem_malloc.so /tmp/ab_post/libumem_malloc.so && { log "FATAL: pre and post libumem_malloc.so are byte-identical"; exit 1; }
log "  pre vs post libumem_malloc.so differ: yes (required)"
log "  pre vs nullB libumem_malloc.so differ: $(cmp -s /tmp/ab_pre/libumem_malloc.so /tmp/ab_nullB/libumem_malloc.so && echo 'no (bit-identical rebuild)' || echo 'yes (layout-only)')"
log "  pre vs post libumem.so differ: $(cmp -s /tmp/ab_pre/libumem.so.1 /tmp/ab_post/libumem.so.1 && echo 'no (allocator identical, as expected)' || echo 'yes')"
for a in pre post nullB; do
	log "  $a preload resolves: $(LD_LIBRARY_PATH=/tmp/ab_$a LD_PRELOAD=/tmp/ab_$a/libumem_malloc.so ldd /tmp/ab_$a/bench_main | grep umem | tr -s ' ' | tr '\n' ';')"
done
log ""
printf 'arm,label,replicate,' >> "$OUT"; /tmp/ab_post/bench_main -H 2>/dev/null | tr -d '\n' >> "$OUT"; printf '\n' >> "$OUT"

log "## NULL CONTROL (preload arm): pre vs an independent build of the same source"
for t in 1 8 192; do
	[ "$t" -gt "$NCPU" ] && continue
	measure_point pre nullB umem-preload multi "$t" 16:64 "null-preload-multi-$t-16:64"
done
log ""
log "## PRELOAD ARM: pre ($PRE) vs post ($POST)"
for t in $THREADS; do
	[ "$t" -gt "$NCPU" ] && continue
	measure_point pre post umem-preload multi "$t" 16:64 "preload-multi-$t-16:64"
done
measure_point pre post umem-preload multi 8 64:256 "preload-multi-8-64:256"
measure_point pre post umem-preload multi 192 64:256 "preload-multi-192-64:256"
measure_point pre post umem-preload prodcons 8 64:256 "preload-prodcons-8-64:256"
measure_point pre post umem-preload prodcons 192 64:256 "preload-prodcons-192-64:256"
log ""
log "## REFERENCE at post: API arm vs PRELOAD arm, same build, alternating (delta = preload relative to API)"
for t in 1 8 32 64 128 192; do
	[ "$t" -gt "$NCPU" ] && continue
	log "  api-vs-preload multi t=$t 16:64 (${REPLICATES} pairs)"
	for i in $(seq 1 "$REPLICATES"); do
		rowA="$(run_one post umem multi "$t" 16:64)"; rowB="$(run_one post umem-preload multi "$t" 16:64)"
		echo "post-api,api-vs-preload-multi-$t,$i,$rowA" >> "$OUT"; echo "post-preload,api-vs-preload-multi-$t,$i,$rowB" >> "$OUT"
		oa="$(ops_of "$rowA")"; ob="$(ops_of "$rowB")"
		printf '    pair %d: api %10.3f Mops p999=%7s   preload %10.3f Mops p999=%7s   preload/api = %.3f\n' "$i" \
		    "$(awk -v v="$oa" 'BEGIN{print v/1e6}')" "$(field_of 12 "$rowA")" \
		    "$(awk -v v="$ob" 'BEGIN{print v/1e6}')" "$(field_of 12 "$rowB")" "$(awk -v a="$oa" -v b="$ob" 'BEGIN{print b/a}')" | tee -a "$LOG"
	done
done
log ""
log "A/B done -> $OUT / $LOG"

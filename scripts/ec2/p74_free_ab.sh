#!/usr/bin/env bash
# scripts/ec2/p74_free_ab.sh - A/B of the P7.4 exact-span-table change on the
# free() path, on the SAME box, with a NULL CONTROL measured first (pre vs an
# independent rebuild of pre) so the rig's own spread is on the record before
# any A/B delta is read.  Same protocol as interpose_ab.sh / hotpath_ab.sh.
#
# The change is in libumem.so (vmem.c span table, malloc.c umem_may_own) and
# the interposer's free() reaches it, so both arms differ in libumem.so AND
# libumem_malloc.so.  Three arms built from git archives in $PWD/{pre,post,pre2}.
# ONE bench binary per bench, from the POST tree, so the loop cost is identical
# across arms (hotpath_ab.sh's lesson).
#
#   HEAP FREE (must be inside the null): bench_pairs -m under LD_PRELOAD -- a
#     malloc/free loop through the interposer.  A heap free pays one span
#     search per accepted free; this measures whether that is inside the null.
#   FOREIGN FREE (where the search runs): bench_foreign_free -- repeatedly
#     free() a non-owned pointer.  mode=far is a hull miss (no search);
#     mode=inhull is the P7.4 in-hull no-span case that pays the search.
#
#   ./scripts/ec2/job.sh intel-lo@r4span start ab 3600 \
#     'cd ~/libumem-ab-r4span && PRE=<sha> POST=<sha> ./post/scripts/ec2/p74_free_ab.sh'
set +e
NCPU=$(nproc)
PRE="${PRE:?pre sha}"; POST="${POST:?post sha}"
OUTDIR="${OUTDIR:-$PWD/results}"; mkdir -p "$OUTDIR"
LOG="$OUTDIR/p74-free-ab.txt"; CSV="$OUTDIR/p74-free-ab.csv"
: > "$LOG"; : > "$CSV"
log() { echo "$@" | tee -a "$LOG"; }
REPLICATES="${REPLICATES:-9}"
DUR="${DUR:-1}"

build_into() {  # $1 tag $2 src
	local tag="$1" src="$2" so mso
	( cd "$src" && ./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && \
	  make -j"$NCPU" >/dev/null 2>&1 && \
	  make -j"$NCPU" test/bench/bench_pairs test/bench/bench_foreign_free >/dev/null 2>&1 ) \
	  || { log "build $tag FAILED"; return 1; }
	so="$(ls "$src"/.libs/libumem.so.*.*.* | head -1)"
	mso="$(ls "$src"/.libs/libumem_malloc.so.*.*.* | head -1)"
	rm -rf "/tmp/p74_$tag"; mkdir -p "/tmp/p74_$tag"
	cp "$so"  "/tmp/p74_$tag/$(objdump -p "$so"  | awk '/SONAME/{print $2}')"
	cp "$mso" "/tmp/p74_$tag/$(objdump -p "$mso" | awk '/SONAME/{print $2}')"
	cp "$mso" "/tmp/p74_$tag/libumem_malloc.so"
	log "  built $tag: libumem.so sha256=$(sha256sum "$so" | cut -c1-16) libumem_malloc.so sha256=$(sha256sum "$mso" | cut -c1-16)"
}
BP=/tmp/p74_bench_pairs
BF=/tmp/p74_bench_foreign_free

run_heap() {  # $1 tag $2 threads -> Mpairs/s (malloc/free via interposer)
	local last=$(( $2 - 1 )); (( last >= NCPU )) && last=$((NCPU-1))
	LD_LIBRARY_PATH="/tmp/p74_$1" LD_PRELOAD="/tmp/p74_$1/libumem_malloc.so" \
	    numactl --physcpubind=0-"$last" --localalloc -- \
	    "$BP" -s 512 -n 64 -t "$2" -d "$DUR" -m 2>/dev/null | awk '{print $(NF-1)}'
}
run_foreign() {  # $1 tag $2 mode $3 threads -> Mfrees/s
	local last=$(( $3 - 1 )); (( last >= NCPU )) && last=$((NCPU-1))
	LD_LIBRARY_PATH="/tmp/p74_$1" LD_PRELOAD="/tmp/p74_$1/libumem_malloc.so" \
	    numactl --physcpubind=0-"$last" --localalloc -- \
	    "$BF" -m "$2" -t "$3" -d "$DUR" 2>/dev/null | awk '{print $(NF-1)}'
}

measure() {  # $1 A $2 B $3 label $4 fn (rest: fn args)
	local A="$1" B="$2" label="$3" fn="$4"; shift 4
	local i a b d deltas=()
	for i in $(seq 1 "$REPLICATES"); do
		a="$($fn "$A" "$@")"; b="$($fn "$B" "$@")"
		echo "$label,$i,$A,$a,$B,$b" >> "$CSV"
		d="$(awk -v a="$a" -v b="$b" 'BEGIN{ if (a>0) printf "%.3f",(b-a)/a*100; else print "nan" }')"
		deltas+=("$d")
	done
	printf '%s\n' "${deltas[@]}" | sort -g | awk -v l="$label" -v a="$a" -v b="$b" '
	    {v[NR]=$1} END{ med=(NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2
	    printf "  %-30s last A=%-9s B=%-9s  median %+7.2f%%  (min %+7.2f%%, max %+7.2f%%, n=%d)\n", l, a, b, med, v[1], v[NR], NR }' | tee -a "$LOG"
}

log "# P7.4 free A/B  vcpu=$NCPU  $(date -u +%FT%TZ)  pre=$PRE post=$POST replicates=$REPLICATES dur=${DUR}s"
log "# instance=$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null) gcc=$(gcc -dumpversion)"
log "## builds"
build_into pre  "$PWD/pre"  || exit 1
build_into post "$PWD/post" || exit 1
build_into pre2 "$PWD/pre2" || exit 1
cmp -s /tmp/p74_pre/libumem.so.1 /tmp/p74_post/libumem.so.1 && { log "FATAL: pre/post libumem.so byte-identical"; exit 1; }
cp "$PWD/post/test/bench/.libs/bench_pairs" "$BP"
cp "$PWD/post/test/bench/.libs/bench_foreign_free" "$BF"
log "  bench_pairs sha256=$(sha256sum "$BP" | cut -c1-16)  bench_foreign_free sha256=$(sha256sum "$BF" | cut -c1-16)"
log ""
log "## HEAP FREE (interposer malloc/free): a heap free must be inside the null"
for t in 1 8; do
	[ "$t" -gt "$NCPU" ] && continue
	measure pre pre2 "null  heap-free t=$t" run_heap "$t"
	measure pre post "A/B   heap-free t=$t" run_heap "$t"
done
log ""
log "## FOREIGN FREE (non-owned free): far = hull miss (no search), inhull = span search"
for m in far inhull; do
	for t in 1 8; do
		[ "$t" -gt "$NCPU" ] && continue
		measure pre pre2 "null  foreign-$m t=$t" run_foreign "$m" "$t"
		measure pre post "A/B   foreign-$m t=$t" run_foreign "$m" "$t"
	done
done
log "# done $(date -u +%FT%TZ)"

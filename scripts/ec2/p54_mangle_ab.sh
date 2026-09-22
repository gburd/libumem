#!/usr/bin/env bash
# P5.4 A/B: freelist-link mangling on vs off, same commit, same compiler.
#
# The control arm is the SAME source built with -DUMEM_NO_LINK_MANGLE, which
# reduces UMEM_LINK_MANGLE() to a cast.  That isolates the mangling from every
# other difference: one tree, one configure, one gcc.
#
# WHY THIS SCRIPT IS SHAPED THE WAY IT IS
#
# The first version measured each point once per arm (median-of-5 inside one
# process) and produced paired deltas scattered from -4% to +16% while the
# WITHIN-process CoV stayed under 5%.  Two XOR-and-shift instructions on a path
# that already takes cp->cache_lock cannot cost or save 16%.  What that spread
# actually measured is BETWEEN-PROCESS variance -- code layout, page placement,
# which cores the pinning picked -- and bench_main's own CoV is blind to it,
# because it only varies runs inside one process image.
#
# So:
#   - Each point is measured as REPLICATE PAIRS: nomangle, mangle, nomangle,
#     mangle, ... REPLICATES times, alternating at the innermost level.  The
#     reported figure is the median of the per-replicate paired deltas, which
#     cancels drift that affects both arms equally.
#   - A NULL CONTROL runs the identical protocol with two independent builds of
#     IDENTICAL source (nomangle vs nomangle).  Its spread is the noise floor.
#     A mangle delta inside the null spread is not a measurement of mangling,
#     and this script says so rather than quoting it as an effect.
#
# Other methodology rules (test/bench/README.md, the P2.1/P2.2 protocol):
#   - -n is a TOTAL budget, divided by thread count exactly once, 100k/thread
#     floor; any point with ops_floor_raised is flagged.
#   - median-of-N with a warm-up discarded inside each process; CoV recorded.
#   - pinned to a contiguous CPU set sized to the thread count.
#
# Usage: scripts/ec2/p54_mangle_ab.sh   (run through job.sh / verify-isolated)
set +e
NCPU=$(nproc)
OUT="${OUT:-docs/results/p54-mangle-ab.csv}"
LOG="${LOG:-docs/results/p54-mangle-ab.txt}"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"
: > "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

RUNS="${RUNS:-5}"		# measured runs inside one process
WARM="${WARM:-1}"		# warm-ups discarded inside one process
OPS="${OPS:-40000000}"		# TOTAL ops, divided by thread count once
REPLICATES="${REPLICATES:-5}"	# independent A/B pairs per point
SONAME=""

# Build one arm into its own directory.  Each arm keeps its own copy of the
# library and its own binary, and the run points LD_LIBRARY_PATH at that
# directory: overwriting one shared .libs path in place (what d2_ab.sh does)
# makes the file under test depend on which build ran last.
build_into() {  # $1 = tag, $2 = extra CPPFLAGS
	local tag="$1" extra="$2" solib
	./scripts/ec2/clean-regen.sh >/dev/null 2>&1
	./configure CPPFLAGS="$extra" >/dev/null 2>&1 || return 1
	make -j"$NCPU" >/dev/null 2>&1
	make -j"$NCPU" test/bench/bench_main >/dev/null 2>&1
	[ -x test/bench/.libs/bench_main ] || return 1
	solib="$(ls .libs/libumem.so.*.*.* 2>/dev/null | head -1)"
	[ -n "$solib" ] || { log "  no .libs/libumem.so.*.*.* after building $tag"; return 1; }
	SONAME="$(objdump -p "$solib" | awk '/SONAME/{print $2}')"
	[ -n "$SONAME" ] || { log "  no SONAME in $solib"; return 1; }
	rm -rf "/tmp/lib_$tag" && mkdir -p "/tmp/lib_$tag" || return 1
	cp "$solib" "/tmp/lib_$tag/$SONAME" || return 1
	cp test/bench/.libs/bench_main "/tmp/bench_main_$tag" || return 1
	log "  built $tag: cppflags='${extra:-<none>}' soname=$SONAME sha256=$(sha256sum "/tmp/lib_$tag/$SONAME" | cut -c1-16)"
	return 0
}

run_one() {  # $1=tag $2=workload $3=threads $4=size -> one CSV row
	local tag="$1" w="$2" t="$3" s="$4" last=$(( $3 - 1 ))
	(( last >= NCPU )) && last=$((NCPU-1))
	LD_LIBRARY_PATH="/tmp/lib_$tag" \
	    numactl --physcpubind=0-"$last" --localalloc -- \
	    "/tmp/bench_main_$tag" -a umem -w "$w" -t "$t" -n "$OPS" \
	    -s "$s" -r "$RUNS" -W "$WARM" -c 2>/dev/null | grep "^umem," | tail -1
}

# Column numbers are 1-based over the row bench_print_csv_row() emits:
# 7 ops_per_sec, 9 lat_p50, 11 lat_p99, 25 ops_cov, 27 unstable,
# 28 ops_floor_raised.
ops_of()   { echo "$1" | awk -F, '{print $7}'; }
field_of() { echo "$2" | awk -F, -v n="$1" '{print $n}'; }

# One point: REPLICATES alternating pairs, median of the paired deltas.
measure_point() {  # $1=tagA $2=tagB $3=workload $4=threads $5=size $6=label
	local A="$1" B="$2" w="$3" t="$4" s="$5" label="$6"
	local i rowA rowB oa ob d deltas=() flags=""
	log "  $label  workload=$w threads=$t sizes=$s  (${REPLICATES} pairs)"
	for i in $(seq 1 "$REPLICATES"); do
		rowA="$(run_one "$A" "$w" "$t" "$s")"
		rowB="$(run_one "$B" "$w" "$t" "$s")"
		[ -z "$rowA" ] || [ -z "$rowB" ] && { log "    pair $i: missing row"; continue; }
		echo "$A,$label,$i,$rowA" >> "$OUT"
		echo "$B,$label,$i,$rowB" >> "$OUT"
		oa="$(ops_of "$rowA")"; ob="$(ops_of "$rowB")"
		d="$(awk -v a="$oa" -v b="$ob" 'BEGIN{printf "%.3f",(b-a)/a*100}')"
		deltas+=("$d")
		[ "$(field_of 28 "$rowA")" != 0 ] && flags="$flags floorA:$i"
		[ "$(field_of 28 "$rowB")" != 0 ] && flags="$flags floorB:$i"
		printf '    pair %d: %-8s %10.3f Mops (cov %5.2f%%)   %-8s %10.3f Mops (cov %5.2f%%)   delta %+7.2f%%\n' \
		    "$i" "$A" "$(awk -v v="$oa" 'BEGIN{print v/1e6}')" \
		    "$(awk -v v="$(field_of 25 "$rowA")" 'BEGIN{print v*100}')" \
		    "$B" "$(awk -v v="$ob" 'BEGIN{print v/1e6}')" \
		    "$(awk -v v="$(field_of 25 "$rowB")" 'BEGIN{print v*100}')" \
		    "$d" | tee -a "$LOG"
	done
	printf '%s\n' "${deltas[@]}" | sort -g | awk -v l="$label" -v f="$flags" '
	    {v[NR]=$1}
	    END{ if(NR==0){printf "    %s: no data\n",l; exit}
	         med=(NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2
	         printf "    => %s: median delta %+.2f%%  (min %+.2f%%, max %+.2f%%, n=%d)%s\n",
	                l, med, v[1], v[NR], NR, (f==""?"":"  FLAGS:"f) }' | tee -a "$LOG"
}

log "# P5.4 mangle A/B  vcpu=$NCPU  $(date -u +%FT%TZ)"
log "# $(grep '^sha=' ISOLATED_PROVENANCE 2>/dev/null || echo 'sha=unknown')"
log "# runs=$RUNS warmups=$WARM total_ops=$OPS replicates=$REPLICATES gcc=$(gcc -dumpversion)"
log "# instance=$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null || echo unknown)"
log ""

log "## builds"
build_into nomangle "-DUMEM_NO_LINK_MANGLE" || { log "nomangle build FAILED"; exit 1; }
build_into mangle   ""                      || { log "mangle build FAILED"; exit 1; }
# A second build of the SAME source as nomangle: the null control's other arm.
build_into nullB    "-DUMEM_NO_LINK_MANGLE" || { log "nullB build FAILED"; exit 1; }

# The arms must not be byte-identical: if they are, CPPFLAGS never reached the
# compile and the A/B is one library against itself, which reads as 0% cost.
if cmp -s "/tmp/lib_nomangle/$SONAME" "/tmp/lib_mangle/$SONAME"; then
	log "FATAL: mangle and nomangle are byte-identical -- -DUMEM_NO_LINK_MANGLE"
	log "       did not reach the compile, so no comparison is possible."
	exit 1
fi
log "  mangle vs nomangle differ: yes (required)"
log "  nullB vs nomangle differ: $(cmp -s "/tmp/lib_nullB/$SONAME" "/tmp/lib_nomangle/$SONAME" && echo 'no (bit-identical rebuild)' || echo 'yes (layout-only)')"
for a in nomangle mangle nullB; do
	log "  $a resolves: $(LD_LIBRARY_PATH=/tmp/lib_$a ldd /tmp/bench_main_$a | grep umem | tr -s ' ')"
done
log ""

printf 'arm,label,replicate,' >> "$OUT"
"/tmp/bench_main_mangle" -H 2>/dev/null | tr -d '\n' >> "$OUT"
printf '\n' >> "$OUT"

# NULL CONTROL FIRST, so the noise floor is on the record before any mangle
# number is read.  Same protocol, two builds of identical source.
log "## NULL CONTROL: nomangle vs an independent build of the SAME source"
log "##   Any delta here is build/layout/placement noise.  A mangle delta"
log "##   inside this spread is not evidence of a cost."
measure_point nomangle nullB single 1  16:64  "null-single-1-16:64"
measure_point nomangle nullB multi  8  64:256 "null-multi-8-64:256"
log ""

# Small size classes are the exposed ones: the caches without UMF_HASH, i.e.
# the ones whose bufctl lives inside the user buffer.
log "## MANGLE vs NOMANGLE"
for s in 16:64 64:256; do
	measure_point nomangle mangle single 1 "$s" "single-1-$s"
	for t in 8 32 96 192; do
		[ "$t" -gt "$NCPU" ] && continue
		measure_point nomangle mangle multi "$t" "$s" "multi-$t-$s"
	done
done

# prodcons crosses threads, so a freed buffer is likelier to reach the slab
# layer than to die in the freeing thread's own magazine.  The mangling is on
# the slab path, so this is where a cost should show if anywhere.
for t in 8 32; do
	[ "$t" -gt "$NCPU" ] && continue
	measure_point nomangle mangle prodcons "$t" 64:256 "prodcons-$t-64:256"
done

log ""
log "A/B done -> $OUT / $LOG"

#!/usr/bin/env bash
# P5.4 A/B: freelist-link mangling on vs off, same commit, same compiler.
#
# The control arm is the SAME source built with -DUMEM_NO_LINK_MANGLE, which
# reduces UMEM_LINK_MANGLE() to a cast.  That isolates the mangling from every
# other difference: one tree, one configure, one gcc, one binary layout.
# Comparing against a different commit would also carry whatever else changed.
#
# Methodology (test/bench/README.md, the P2.1/P2.2 protocol):
#   - -n is a TOTAL budget, divided by thread count exactly once, 100k/thread
#     floor.  Every reported point is rejected if ops_floor_raised is true.
#   - A/B ALTERNATES at the innermost loop, so compared points are adjacent in
#     time and slow drift cannot masquerade as an effect.
#   - median-of-5 with 1 warm-up discarded; CoV reported for every point.
#   - pinned to a contiguous CPU set sized to the thread count.
#
# Usage: scripts/ec2/p54_mangle_ab.sh   (run through job.sh on intel-hi)
set +e
NCPU=$(nproc)
OUT="${OUT:-docs/results/p54-mangle-ab.csv}"
LOG="${LOG:-docs/results/p54-mangle-ab.txt}"
mkdir -p "$(dirname "$OUT")"
: > "$OUT"
: > "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

RUNS="${RUNS:-5}"
WARM="${WARM:-1}"
OPS="${OPS:-40000000}"

# Resolve the real soname rather than hard-coding one.  scripts/ec2/d2_ab.sh
# hard-codes libumem.so.0.0.0, which stopped existing at the 1:0:0
# version-info bump -- so it silently copies nothing and measures whatever
# library happens to be there.  Fail loudly instead.
#
# Each arm gets its OWN directory with its own soname symlinks, and the run
# points LD_LIBRARY_PATH at that directory.  Overwriting one shared
# .libs/libumem.so.* in place (what d2_ab.sh does) means the file under test
# depends on which build ran last, and a failed cp leaves the previous arm's
# library in place while the log says otherwise.
SONAME=""

build_arm() {  # $1 = mangle|nomangle
	local arm="$1" extra=""
	[ "$arm" = nomangle ] && extra="-DUMEM_NO_LINK_MANGLE"
	./scripts/ec2/clean-regen.sh >/dev/null 2>&1
	./configure CPPFLAGS="$extra" >/dev/null 2>&1 || return 1
	make -j"$NCPU" >/dev/null 2>&1
	make -j"$NCPU" test/bench/bench_main >/dev/null 2>&1
	[ -x test/bench/.libs/bench_main ] || return 1
	local solib
	solib="$(ls .libs/libumem.so.*.*.* 2>/dev/null | head -1)"
	[ -n "$solib" ] || { log "  no .libs/libumem.so.*.*.* after building $arm"; return 1; }
	# The soname the binaries actually ask the loader for.
	SONAME="$(objdump -p "$solib" | awk '/SONAME/{print $2}')"
	[ -n "$SONAME" ] || { log "  no SONAME in $solib"; return 1; }
	rm -rf "/tmp/lib_$arm" && mkdir -p "/tmp/lib_$arm" || return 1
	cp "$solib" "/tmp/lib_$arm/$SONAME" || return 1
	cp test/bench/.libs/bench_main "/tmp/bench_main_$arm" || return 1
	# Prove the arms really differ: the mangling symbol is referenced only
	# by the mangling build.  A silently identical pair would make the
	# whole measurement meaningless and is exactly the kind of thing that
	# reads as "0% cost".
	local n
	n=$(nm -D "/tmp/lib_$arm/$SONAME" 2>/dev/null | grep -c umem_link_cookie)
	log "  built $arm: umem_link_cookie symbol refs=$n  soname=$SONAME  sha256=$(sha256sum "/tmp/lib_$arm/$SONAME" | cut -c1-16)"
	return 0
}

run_one() {  # $1=arm $2=workload $3=threads $4=size -> one CSV row
	local arm="$1" w="$2" t="$3" s="$4" last=$(( $3 - 1 ))
	(( last >= NCPU )) && last=$((NCPU-1))
	LD_LIBRARY_PATH="/tmp/lib_$arm" \
	    numactl --physcpubind=0-"$last" --localalloc -- \
	    "/tmp/bench_main_$arm" -a umem -w "$w" -t "$t" -n "$OPS" \
	    -s "$s" -r "$RUNS" -W "$WARM" -c 2>/dev/null | grep "^umem," | tail -1
}

emit() {  # $1=arm label; row on stdin
	local arm="$1" row
	row="$(cat)"
	if [ -z "$row" ]; then log "    $arm: (no row)"; return; fi
	echo "$arm,$row" >> "$OUT"
	echo "$row" | awk -F, -v a="$arm" \
	  '{printf "    %-9s mops=%9.3f  cov=%6.2f%%  p50=%6s p99=%8s  floor_raised=%s unstable=%s\n", \
	     a, $7/1e6, $25*100, $9, $11, $28, $27}' | tee -a "$LOG"
}

log "# P5.4 mangle A/B  vcpu=$NCPU  $(date -u +%FT%TZ)"
log "# commit=$(cat ISOLATED_PROVENANCE 2>/dev/null | grep '^sha=' || echo 'sha=unknown')"
log "# runs=$RUNS warmups=$WARM total_ops=$OPS  gcc=$(gcc -dumpversion)"
log ""
log "## builds"
build_arm nomangle || { log "nomangle build FAILED"; exit 1; }
build_arm mangle   || { log "mangle build FAILED"; exit 1; }

# The two arms must not be byte-identical: if they are, CPPFLAGS never reached
# the compile and the whole A/B is measuring one library against itself, which
# would read as a reassuring 0% cost.
if cmp -s "/tmp/lib_nomangle/$SONAME" "/tmp/lib_mangle/$SONAME"; then
	log "FATAL: the two arms are byte-identical -- -DUMEM_NO_LINK_MANGLE did"
	log "       not reach the compile, so no comparison is possible."
	exit 1
fi
# And each binary must resolve to its OWN arm's library.
for a in nomangle mangle; do
	log "  $a resolves: $(LD_LIBRARY_PATH=/tmp/lib_$a ldd /tmp/bench_main_$a | grep umem | tr -s ' ')"
done
log ""

printf 'arm,' >> "$OUT"
"/tmp/bench_main_mangle" -H >> "$OUT" 2>/dev/null

# Small size classes are the exposed ones: they are the caches without
# UMF_HASH, i.e. the ones whose bufctl lives in the user buffer.
for s in 16:64 64:256; do
	log "## single-thread, sizes $s"
	for t in 1; do
		log "  threads=$t"
		run_one nomangle single "$t" "$s" | emit nomangle
		run_one mangle   single "$t" "$s" | emit mangle
	done
	log "## multi-thread, sizes $s"
	for t in 8 32 96 192; do
		[ "$t" -gt "$NCPU" ] && continue
		log "  threads=$t"
		run_one nomangle multi "$t" "$s" | emit nomangle
		run_one mangle   multi "$t" "$s" | emit mangle
	done
done

# prodcons crosses threads, so a freed buffer is more likely to reach the
# slab layer rather than dying in the freeing thread's own magazine -- the
# mangling is on the slab path, so this is where cost should show if anywhere.
log "## prodcons 64:256 (cross-thread frees reach the slab layer)"
for t in 8 32; do
	[ "$t" -gt "$NCPU" ] && continue
	log "  threads=$t"
	run_one nomangle prodcons "$t" 64:256 | emit nomangle
	run_one mangle   prodcons "$t" 64:256 | emit mangle
done

log ""
log "A/B done -> $OUT / $LOG"

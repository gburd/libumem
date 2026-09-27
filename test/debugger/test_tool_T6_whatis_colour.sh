#!/usr/bin/env bash
#
# T6 of docs/reviews/2026-09-26-tool-review.md: `umemctl whatis` on the freed
# 96 KiB buffer names a small cache (umem_magazine_1, umem_bufctl_cache...).
# slab_of() accepts addr - slab_base < cache_slabsize, and slab_base carries
# the colour offset, so a coloured 4 KiB slab's range runs `colour` bytes
# into the next page; the smallest-slabsize candidate then wins.
#
# The layout varies per run (5 of 6 fresh runs wrong in the review), so this
# probes up to 5 fresh targets and reproduces on the first misattribution.
#
# Positive control: held_96k resolves to umem_alloc_98304 in every run.
#
# Exit: 0 fixed, 1 finding reproduces, 77 no --enable-introspect, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_introspect
CTL=$ROOT/tools/umemctl
export UMEM_INTROSPECT_SOCK=$TMPD/umem.sock
unset UMEM_DEBUG UMEM_LOGGING
cache_of() { "$CTL" "$TPID" whatis "$1" 2>&1 | awk '$1 == "cache" { print $2 }'; }

for run in 1 2 3 4 5; do
	UMEM_OPTIONS=introspect=1 start_target "$TMPD/info.$run" 3 30
	for _ in $(seq 1 50); do
		"$CTL" "$TPID" stats >/dev/null 2>&1 && break
		sleep 0.1
	done
	h=$(cache_of "$held_96k")
	f=$(cache_of "$freed_96k")
	echo "run $run: held_96k=$held_96k -> '$h'  freed_96k=$freed_96k -> '$f'"
	[[ $h == umem_alloc_98304 ]] ||
		{ echo "ERROR: control: held_96k -> '$h'"; exit 99; }
	if [[ $f != umem_alloc_98304 ]]; then
		echo "T6 REPRODUCES: freed_96k attributed to '$f'"
		exit 1
	fi
	kill -9 "$TPID" "$BGPID" 2>/dev/null
	wait 2>/dev/null
	TPID= BGPID=
done
echo "T6 FIXED: freed_96k -> umem_alloc_98304 in 5 fresh runs"
exit 0

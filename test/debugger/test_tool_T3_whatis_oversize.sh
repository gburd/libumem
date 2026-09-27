#!/usr/bin/env bash
#
# T3 (code half) of docs/reviews/2026-09-26-tool-review.md: `umemctl whatis`
# on a live oversize (256 KiB, vmem-routed) allocation says "not a umem
# buffer".  It must at least identify it as a umem allocation.
#
# Not covered: the "state cached" report for a magazine-cached buffer.  Under
# audit a freed buffer reads "free" (buftag), and without audit freed_96k was
# not found in the magazine set by tools/umem either (review, target A), so
# no target here puts a buffer where that branch can be shown to change.
#
# Positive control: held_4k resolves to a umem_alloc_ cache.
#
# Exit: 0 fixed, 1 finding reproduces, 77 no --enable-introspect, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_introspect
CTL=$ROOT/tools/umemctl
export UMEM_INTROSPECT_SOCK=$TMPD/umem.sock
unset UMEM_DEBUG UMEM_LOGGING
UMEM_OPTIONS=introspect=1 start_target "$TMPD/info" 3 30

ctl=
for _ in $(seq 1 50); do
	ctl=$("$CTL" "$TPID" whatis "$held_4k" 2>&1) && [[ -n $ctl ]] && break
	sleep 0.1
done
echo "$ctl"
[[ $ctl == *"cache umem_alloc_"* ]] ||
	{ echo "ERROR: control: whatis held_4k did not name a umem_alloc_ cache"; exit 99; }

out=$("$CTL" "$TPID" whatis "$held_big" 2>&1)
echo "$out"
if [[ $out == *"not a umem buffer"* ]]; then
	echo "T3 REPRODUCES: whatis held_big ($held_big, 256 KiB) not found"
	exit 1
fi
echo "T3 FIXED: whatis identifies the oversize allocation"
exit 0

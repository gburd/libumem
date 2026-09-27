#!/usr/bin/env bash
#
# T3 (code half) of docs/reviews/2026-09-26-tool-review.md: `umemctl whatis`
# on a live oversize (256 KiB, vmem-routed) allocation says "not a umem
# buffer".  It must at least identify it as a umem allocation.
#
# Second half: under UMEM_DEBUG=audit PTC is off, so freed_96k sits in a
# depot magazine; umemctl reads "state held" for it where tools/umem reads
# CACHED.  It must read "cached".  A buffer in a PTC bin (non-audit) is not
# tested: thread-local bins cannot be read soundly from the server thread.
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
kill -9 "$TPID" "$BGPID" 2>/dev/null
wait 2>/dev/null
UMEM_DEBUG=audit UMEM_OPTIONS=introspect=1 start_target "$TMPD/info.audit" 3 30
for _ in $(seq 1 50); do
	"$CTL" "$TPID" stats >/dev/null 2>&1 && break
	sleep 0.1
done
out=$("$CTL" "$TPID" whatis "$freed_96k" 2>&1)
echo "$out"
[[ $out == *"cache umem_alloc_98304"* ]] ||
	{ echo "ERROR: control: audit whatis freed_96k not in umem_alloc_98304"; exit 99; }
if [[ $out == *"state held"* ]]; then
	echo "T3 REPRODUCES: magazine-cached freed_96k reads 'state held'"
	exit 1
fi
echo "T3 FIXED: whatis identifies the oversize allocation"
exit 0

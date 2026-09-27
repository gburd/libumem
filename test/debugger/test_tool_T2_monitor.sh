#!/usr/bin/env bash
#
# T2 of docs/reviews/2026-09-26-tool-review.md: `umemctl <pid> monitor`
# always shows "bufs in-use 0".  The stats reply carries the held count as
# `bufs_held`; monitor parses a different key, so the field stays 0.
#
# Positive control: `umemctl stats` on the same target must report
# bufs_held > 0 (tool_target holds a known set), else a 0 from monitor would
# prove nothing.
#
# Exit: 0 fixed, 1 finding reproduces, 77 no --enable-introspect, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_introspect

CTL=$ROOT/tools/umemctl
export UMEM_INTROSPECT_SOCK=$TMPD/umem.sock
UMEM_OPTIONS=introspect=1 start_target "$TMPD/info" 3 30

stats=
for _ in $(seq 1 50); do
	stats=$("$CTL" "$TPID" stats 2>&1) && [[ $stats == *bufs_held* ]] && break
	sleep 0.1
done
echo "$stats"
held=$(awk '$1 == "bufs_held" { print $2 }' <<<"$stats")
[[ ${held:-0} =~ ^[0-9]+$ ]] && (( held > 0 )) ||
	{ echo "ERROR: control: stats bufs_held='${held:-}' (want > 0)"; exit 99; }

mon=$("$CTL" "$TPID" monitor --once 2>&1)
echo "$mon"
inuse=$(awk '/bufs in-use/ { print $NF }' <<<"$mon")
[[ ${inuse:-} =~ ^[0-9]+$ ]] ||
	{ echo "ERROR: no 'bufs in-use' line in monitor --once"; exit 99; }

if (( inuse == 0 )); then
	echo "T2 REPRODUCES: monitor bufs in-use 0, stats bufs_held $held"
	exit 1
fi
echo "T2 FIXED: monitor bufs in-use $inuse (stats bufs_held $held)"
exit 0

#!/usr/bin/env bash
#
# T10 of docs/reviews/2026-09-26-tool-review.md: `umem --pid P walk log` on a
# target without a transaction log prints nothing, so tools/umem reports
# "gdb produced no report" and exits 2.  `log` on the same target prints
# "transaction log disabled"; `walk log` must say the same and exit 0.
#
# Positive control: `umem --pid P log -n 1` prints "transaction log disabled"
# (the attach works and the target really has no log).
#
# Exit: 0 fixed, 1 finding reproduces, 77 no gdb / no ptrace, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_gdb
skip_unless_ptrace
UMEM=$ROOT/tools/umem
[[ -x $UMEM ]] || { echo "ERROR: $UMEM not built"; exit 99; }
unset UMEM_DEBUG UMEM_LOGGING UMEM_OPTIONS
start_target "$TMPD/info" 1 60

ctl=$(timeout 60 "$UMEM" --pid "$TPID" log -n 1 2>&1)
echo "$ctl"
[[ $ctl == *"transaction log disabled"* ]] ||
	{ echo "ERROR: control: log did not say 'transaction log disabled'"; exit 99; }

out=$(timeout 60 "$UMEM" --pid "$TPID" walk log -n 3 2>&1)
rc=$?
echo "$out"
echo "rc=$rc"
if (( rc != 0 )) || [[ $out != *"transaction log disabled"* ]]; then
	echo "T10 REPRODUCES: walk log rc=$rc, no 'transaction log disabled'"
	exit 1
fi
echo "T10 FIXED: walk log says the log is disabled, rc=0"
exit 0

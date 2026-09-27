#!/usr/bin/env bash
#
# T12 of docs/reviews/2026-09-26-tool-review.md: re-arming `umemctl break`
# while a thread is parked replaces the predicate but leaves that thread
# parked on the retired one until `continue`.
#
# One churn thread, so once it is parked nothing else allocates.  Arm
# size=98304, wait for its BREAK line, re-arm seq=3.  Fixed: re-arming
# releases the parked thread, it allocates again and seq=3 parks it, which
# logs a second BREAK line.  Bug: no second BREAK line.
#
# Positive control: the first predicate parks the thread (a BREAK line with
# size=98304 in the allverbose log within 5 s).
#
# Exit: 0 fixed, 1 finding reproduces, 77 no --enable-introspect, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_introspect

CTL=$ROOT/tools/umemctl
LOG=$TMPD/target.log
export UMEM_INTROSPECT_SOCK=$TMPD/umem.sock
unset UMEM_LOGGING
UMEM_DEBUG=allverbose UMEM_OPTIONS=introspect=1 start_target "$TMPD/info" 1 60
for _ in $(seq 1 50); do
	"$CTL" "$TPID" stats >/dev/null 2>&1 && break
	sleep 0.1
done
nbreak() { grep -c 'BREAK:' "$LOG"; }

"$CTL" "$TPID" break size=98304
for _ in $(seq 1 50); do
	grep -q 'BREAK:.*size=98304 ' "$LOG" && break
	sleep 0.1
done
n1=$(nbreak)
if ! grep -q 'BREAK:.*size=98304 ' "$LOG"; then
	"$CTL" "$TPID" continue
	echo "ERROR: control: break size=98304 parked no thread in 5 s"
	exit 99
fi

"$CTL" "$TPID" break seq=3
for _ in $(seq 1 30); do
	(( $(nbreak) > n1 )) && break
	sleep 0.1
done
n2=$(nbreak)
"$CTL" "$TPID" continue
grep 'BREAK:' "$LOG"

if (( n2 == n1 )); then
	echo "T12 REPRODUCES: after re-arm the thread stayed parked ($n1 BREAK lines)"
	exit 1
fi
echo "T12 FIXED: re-arm released the parked thread, seq=3 fired ($n2 BREAK lines)"
exit 0

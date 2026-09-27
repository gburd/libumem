#!/usr/bin/env bash
#
# T7 of docs/reviews/2026-09-26-tool-review.md: without UMEM_DEBUG=audit the
# server refuses `learn` ("learn: requires UMEM_DEBUG=audit"), but
# `umemctl record --learn-leaks <file>` still exits 0, prints "wrote leak set",
# and leaves the refusal line in <file> as if it were a leak set.
#
# Positive control: `umemctl leaks` on the same non-audit target must print
# "requires UMEM_DEBUG=audit", proving the target really is non-audit and the
# channel works.
#
# Exit: 0 fixed, 1 finding reproduces, 77 no --enable-introspect, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_introspect

CTL=$ROOT/tools/umemctl
unset UMEM_DEBUG UMEM_LOGGING
export UMEM_INTROSPECT_SOCK=$TMPD/umem.sock
UMEM_OPTIONS=introspect=1 start_target "$TMPD/info" 3 30

leaks=
for _ in $(seq 1 50); do
	leaks=$("$CTL" "$TPID" leaks 2>&1) && [[ -n $leaks ]] && break
	sleep 0.1
done
echo "$leaks"
[[ $leaks == *"requires UMEM_DEBUG=audit"* ]] ||
	{ echo "ERROR: control: leaks did not refuse on a non-audit target"; exit 99; }

out=$("$CTL" "$TPID" record --learn-leaks "$TMPD/leak.set" 2>&1)
rc=$?
echo "$out"
echo "rc=$rc"

if (( rc == 0 )) || [[ $out == *"wrote leak set"* ]]; then
	echo "T7 REPRODUCES: learn refused but umemctl rc=$rc and claims a leak set"
	exit 1
fi
echo "T7 FIXED: umemctl rc=$rc, no leak set claimed"
exit 0

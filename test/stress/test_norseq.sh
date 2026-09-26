#!/bin/sh
# P8.5b: UMEM_OPTIONS=norseq is the runtime off switch for the routed rseq
# per-CPU fast path.  This regression proves both directions:
#   * without norseq  -> the layer is enabled and actually serves
#                        (rseq_enabled=1 AND a nonzero rseq_alloc column), and
#   * with    norseq  -> the layer is off (rseq_enabled=0), so no allocation
#                        is served through it (rseq_alloc column all zero).
#
# SKIPs (77) on a host/build where rseq is unavailable in the first place
# (rseq_enabled=0 even without the option): there is no fast path to switch
# off, so the escape hatch has nothing to demonstrate here.
set -u
PROBE="${PROBE:-test/stress/probe_norseq}"
# libtool wrapper vs .libs binary: prefer whatever exists.
[ -x "$PROBE" ] || PROBE="test/stress/.libs/probe_norseq"
if [ ! -x "$PROBE" ]; then
	echo "SKIP: $PROBE not built"; exit 77
fi

on_out=$(env -u UMEM_OPTIONS "$PROBE" 2>/dev/null)
off_out=$(UMEM_OPTIONS=norseq "$PROBE" 2>/dev/null)

on_enabled=$(printf '%s\n' "$on_out"  | sed -n 's/.*rseq_enabled=\([0-9]*\).*/\1/p' | head -1)
off_enabled=$(printf '%s\n' "$off_out" | sed -n 's/.*rseq_enabled=\([0-9]*\).*/\1/p' | head -1)

if [ "${on_enabled:-0}" != "1" ]; then
	echo "SKIP: rseq not enabled on this host even without norseq"
	echo "  (rseq_enabled=${on_enabled:-?}); nothing to switch off here"
	exit 77
fi

# rseq_alloc is column 8 (1-based) of each cache row; sum it.
sum_rseq_alloc() {
	printf '%s\n' "$1" | awk '/^umem_/ { s += $8 } END { print s+0 }'
}
on_rseq=$(sum_rseq_alloc "$on_out")
off_rseq=$(sum_rseq_alloc "$off_out")

echo "without norseq: rseq_enabled=$on_enabled  rseq_alloc(total)=$on_rseq"
echo "with    norseq: rseq_enabled=$off_enabled  rseq_alloc(total)=$off_rseq"

rc=0
if [ "$on_rseq" -le 0 ]; then
	echo "FAIL: layer enabled but served 0 allocations (routing not firing)"; rc=1
fi
if [ "${off_enabled:-1}" != "0" ]; then
	echo "FAIL: UMEM_OPTIONS=norseq did not disable rseq (rseq_enabled=$off_enabled)"; rc=1
fi
if [ "$off_rseq" -ne 0 ]; then
	echo "FAIL: UMEM_OPTIONS=norseq still served $off_rseq allocations through rseq"; rc=1
fi
[ "$rc" -eq 0 ] && echo "PASS: norseq switches the rseq fast path off; default leaves it serving"
exit $rc

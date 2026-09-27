#!/usr/bin/env bash
#
# T8 of docs/reviews/2026-09-26-tool-review.md: umemctl `break size=|cache=`
# fires only on _umem_alloc's cache-backed slow path, so a predicate that
# matches umem_zalloc(777), umem_cache_alloc(tt_user) or an oversize
# umem_alloc(200000) -- all of which tool_target's churn threads call every
# iteration -- never parks a thread.
#
# A parked thread is seen by its "umem: BREAK: ... size=N cache=C" line,
# which UMEM_DEBUG=allverbose sends to stderr (the target log).
# Positive control: `break size=1500` (the one path that works pre-fix).
# Exit: 0 fixed, 1 finding reproduces, 77 no introspect, 99 control failed.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_introspect

CTL=$ROOT/tools/umemctl
export UMEM_INTROSPECT_SOCK=$TMPD/umem.sock
UMEM_OPTIONS=introspect=1 UMEM_DEBUG=allverbose start_target "$TMPD/info" 3 120
for _ in $(seq 1 50); do
	"$CTL" "$TPID" stats >/dev/null 2>&1 && break
	sleep 0.1
done

# parks <predicate> <regex on the BREAK line>: arm, wait up to 5 s, continue.
parks() {
	local n0 n rc=1
	n0=$(grep -cE "BREAK: .*$2" "$TMPD/target.log")
	"$CTL" "$TPID" break "$1" >/dev/null 2>&1 ||
		{ echo "ERROR: umemctl break $1 failed"; exit 99; }
	for _ in $(seq 1 50); do
		n=$(grep -cE "BREAK: .*$2" "$TMPD/target.log")
		(( n > n0 )) && { rc=0; break; }
		sleep 0.1
	done
	"$CTL" "$TPID" continue >/dev/null 2>&1
	sleep 0.2
	echo "break $1: $([[ $rc == 0 ]] && echo parked || echo 'NOT parked in 5 s')"
	return $rc
}

parks size=1500 'size=1500 ' ||
	{ echo "ERROR: control: break size=1500 never parked"; exit 99; }
fail=0
parks size=777 'size=777 ' || fail=1
parks cache=tt_user 'cache=tt_user' || fail=1
parks size=200000 'size=200000 ' || fail=1
(( fail )) && { echo "T8 REPRODUCES"; exit 1; }
echo "T8 FIXED: zalloc, user-cache and oversize predicates all park"
exit 0

#!/usr/bin/env bash
#
# lifecycle_stress.sh -- the LONG lifecycle coverage for P2.7.
#
# `make check` runs test/unit/test_lifecycle_churn at a small, deterministic
# scale.  This script runs the same coverage at stress scale plus the parts
# that cannot live in a quick suite, and states for each one whether it is a
# gate or an observation.
#
# Exit status (P2.4):
#   0   every GATE case passed (observations are reported, never gate)
#   1   a gate case failed
#   77  automake SKIP -- a prerequisite binary is missing, so nothing ran
#
# Cases:
#   1 thread churn + cache churn, stress scale       GATE
#   2 cross-thread free under churn (same binary)    GATE (in case 1)
#   3 fork under multithreaded allocation load       GATE
#     -> reuses test/integration/test_fork_mt_load (P1.2).  Not reimplemented.
#   4 malloc-interposed lifecycle churn (LD_PRELOAD) GATE
#   5 magazine resize under load                     OBSERVATION
#     -> exercises P1.3b/P1.3c, which are OPEN in the readiness plan.  A
#        failure here is an expected open defect, not a regression from this
#        workstream, so it is reported and does not set the exit status.
#   6 debug/reclaim reuse                            GATE
#     -> reuses test/unit/repro_reclaim_reuse (P1.5).  Not reimplemented.
#
# Invocation (from the repo root, after `make`):
#   ./test/stress/lifecycle_stress.sh            # ~2-4 min at default scale
#   LIFECYCLE_SCALE=heavy ./test/stress/lifecycle_stress.sh   # ~15 min
# On EC2, run it under job.sh (see AGENTS.md §4), never a foreground
# run-remote.sh.
set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

find_bin() {
	for cand in "$ROOT/$1/.libs/$2" "$ROOT/$1/$2"; do
		[[ -x $cand ]] && { echo "$cand"; return 0; }
	done
	return 1
}

SCALE="${LIFECYCLE_SCALE:-default}"
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)
case "$SCALE" in
	heavy)   ROUNDS=600; THREADS=$NCPU; CACHES=400; FORKS=600 ;;
	default) ROUNDS=200; THREADS=$(( NCPU > 16 ? 16 : NCPU )); CACHES=150; FORKS=300 ;;
	quick)   ROUNDS=40;  THREADS=4; CACHES=40; FORKS=60 ;;
	*) echo "LIFECYCLE_SCALE must be quick|default|heavy (got '$SCALE')"; exit 2 ;;
esac

CHURN=$(find_bin test/unit test_lifecycle_churn) || {
	echo "SKIP: test_lifecycle_churn not built (run make) -- nothing executed"
	exit 77
}

# Provenance with the result, per P2.5: a number without its identity is not
# a result.  ISOLATED_PROVENANCE is written by scripts/ec2/verify-isolated.sh.
echo "=== lifecycle stress (P2.7) ==="
echo "scale=$SCALE rounds=$ROUNDS threads=$THREADS caches=$CACHES forks=$FORKS ncpu=$NCPU"
if [[ -r $ROOT/ISOLATED_PROVENANCE ]]; then
	sed 's/^/  provenance: /' "$ROOT/ISOLATED_PROVENANCE"
else
	echo "  provenance: sha=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown) (working tree, NOT isolated)"
fi
echo "  uname: $(uname -srm)"
# Identity of the library that actually ran, resolved through the symlink --
# .libs/libumem.so is a link, and digesting the link name rather than the file
# reported "n/a" (observed on the first 192-vCPU run).
LIBUMEM_SO=$(readlink -f "$ROOT/.libs/libumem.so" 2>/dev/null || true)
[[ -r ${LIBUMEM_SO:-} ]] || LIBUMEM_SO=$(ls "$ROOT"/.libs/libumem.so.*.*.* 2>/dev/null | head -1)
if [[ -r ${LIBUMEM_SO:-} ]]; then
	echo "  libumem: $LIBUMEM_SO"
	echo "  digest: $( { sha256sum "$LIBUMEM_SO" 2>/dev/null || shasum -a 256 "$LIBUMEM_SO" 2>/dev/null; } | awk '{print $1}')"
else
	echo "  libumem: NOT FOUND -- results below cannot be attributed to a binary"
	echo "  digest: unavailable"
fi
echo ""

rc=0
gate() {
	local label=$1; shift
	echo "== GATE: $label =="
	"$@"
	local s=$?
	case $s in
		0)  echo "   PASS: $label" ;;
		77) echo "   SKIP: $label (prerequisite unmet -- NOT a pass)" ;;
		*)  echo "   FAIL: $label (exit $s)"; rc=1 ;;
	esac
	echo ""
}
observe() {
	local label=$1; shift
	echo "== OBSERVATION (does not gate): $label =="
	"$@"
	local s=$?
	if [[ $s -eq 0 ]]; then
		echo "   observed: clean (exit 0)"
	else
		echo "   observed: exit $s -- see the case notes above; this is"
		echo "             reported, not gated (P1.3b/P1.3c are OPEN)"
	fi
	echo ""
}

# 1+2. thread churn, cache create/destroy churn, cross-thread free.
gate "thread churn + cache churn + cross-thread free" \
	env LIFECYCLE_ROUNDS=$ROUNDS LIFECYCLE_THREADS=$THREADS \
	    LIFECYCLE_CACHES=$CACHES LIFECYCLE_DEADLINE=900 "$CHURN"

# 3. fork under multithreaded allocation load -- REUSED from P1.2.
if FORKBIN=$(find_bin test/integration test_fork_mt_load); then
	gate "fork under multithreaded allocation load (test_fork_mt_load)" \
		env FORK_MT_FORKS=$FORKS FORK_MT_THREADS=$THREADS \
		    FORK_MT_DEADLINE=600 "$FORKBIN"
else
	echo "== GATE: fork under load =="
	echo "   SKIP: test_fork_mt_load not built (NOT a pass)"
	echo ""
fi

# 4. malloc-interposed operation: the same lifecycle churn with the
#    interposer actually loaded, so malloc/free go through libumem.
PRELOAD=""
for cand in "$ROOT/.libs/libumem_malloc.so" "$ROOT/.libs/libumem_malloc.so.1"; do
	[[ -f $cand ]] && { PRELOAD=$cand; break; }
done
if [[ -n $PRELOAD ]]; then
	gate "lifecycle churn under malloc interposition (LD_PRELOAD)" \
		env LD_PRELOAD="$PRELOAD" LIFECYCLE_ROUNDS=$(( ROUNDS / 2 )) \
		    LIFECYCLE_THREADS=$THREADS LIFECYCLE_CACHES=$(( CACHES / 2 )) \
		    LIFECYCLE_DEADLINE=900 "$CHURN"
else
	echo "== GATE: malloc interposition =="
	echo "   SKIP: libumem_malloc.so not built (NOT a pass)"
	echo ""
fi

# 5. magazine resize under load (P1.3b/P1.3c: OPEN).  magazine_tune=1 lets the
#    depot reschedule magazine capacity while threads are inside the magazine
#    path, which is precisely the window those two open items describe.
observe "magazine resize under load (UMEM_OPTIONS=magazine_tune=1)" \
	env UMEM_OPTIONS=magazine_tune=1 LIFECYCLE_ROUNDS=$ROUNDS \
	    LIFECYCLE_THREADS=$THREADS LIFECYCLE_CACHES=$CACHES \
	    LIFECYCLE_DEADLINE=900 "$CHURN"

# 6. debug / reclaim reuse -- REUSED from P1.5.
if REUSE=$(find_bin test/unit repro_reclaim_reuse); then
	gate "debug/reclaim reuse, HASH guards (repro_reclaim_reuse)" \
		env UMEM_DEBUG=default,guards UMEM_OPTIONS=reclaim=1 "$REUSE" hash_guards
	gate "debug/reclaim reuse, large quantum (repro_reclaim_reuse)" \
		env UMEM_OPTIONS=reclaim=1 "$REUSE" big_quantum
else
	echo "== GATE: debug/reclaim reuse =="
	echo "   SKIP: repro_reclaim_reuse not built (NOT a pass)"
	echo ""
fi

echo "==================================="
if [[ $rc -eq 0 ]]; then
	echo "RESULT: PASS (all gate cases passed; see observations above)"
else
	echo "RESULT: FAIL (a gate case failed)"
fi
exit $rc

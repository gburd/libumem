#!/usr/bin/env bash
#
# oracle_control.sh -- prove the concurrency oracle DISCRIMINATES.
#
# WHY THIS EXISTS (P2.3)
#   Until 2026-09-22 stress_concurrency_oracle could PASS on an allocator
#   that returned NULL for every request: allocation failures were counted as
#   completed work and the verdict looked only at the corruption flag.  The
#   fix is easy to assert and worthless unasserted -- an oracle nobody has
#   watched fail is not evidence.  So this script runs the oracle against
#   DELIBERATELY BROKEN allocators and requires it to FAIL, plus a clean run
#   it must PASS.  Any of the four cases coming out the wrong way is a FAIL
#   of this script.
#
#   Cases:
#     1. clean                 -> expect PASS (no false positive)
#     2. inject=null           -> expect FAIL (alloc)      [the P2.3 defect]
#     3. inject=corrupt        -> expect FAIL (corruption) [the original job]
#     4. inject=null + legacy  -> expect PASS  ... which is the pre-fix
#                                 behaviour reproduced on demand.  Case 4 is
#                                 the pre-fix failure the plan's ground rule 2
#                                 requires: same injected defect, old
#                                 accounting passes, new accounting fails.
#
# Exit status: 0 all four behaved as required, 1 any did not,
#              77 (automake SKIP) binary not built -- never 0 for "not run".
set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
BIN=$ROOT/test/stress/.libs/stress_concurrency_oracle
[[ -x $BIN ]] || BIN=$ROOT/test/stress/stress_concurrency_oracle

if [[ ! -x $BIN ]]; then
	echo "SKIP: $BIN not built (run make)"
	exit 77
fi

export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Small and quick: the point is the verdict, not the volume.  --iters is per
# thread; the injections trip well inside it.
ARGS=(--threads=4 --iters=20000 --size-class=mixed --pattern=churn)

rc=0
run_case() {
	local label=$1 want=$2 env_assign=$3
	local out status
	echo "== $label (expect exit $want) =="
	if [[ -n $env_assign ]]; then
		out=$(env $env_assign "$BIN" "${ARGS[@]}" 2>&1)
	else
		out=$("$BIN" "${ARGS[@]}" 2>&1)
	fi
	status=$?
	printf '%s\n' "$out" | sed 's/^/    /'
	if [[ $status -ne $want ]]; then
		echo "  FAIL: $label exited $status, required $want"
		rc=1
	else
		echo "  ok: $label exited $status as required"
	fi
}

# 1. Control: unbroken allocator must PASS (no false positives).
run_case "clean" 0 ""

# 2. Broken allocator (returns NULL forever after 1000 allocations).
#    Pre-fix this PASSED; it must now FAIL.
run_case "inject=null" 1 "ORACLE_INJECT=null:1000"

# 3. Silent corruption: one byte of one live buffer flipped after stamping.
#    This is the oracle's original job; it must still catch it.
run_case "inject=corrupt" 1 "ORACLE_INJECT=corrupt:1000"

# 4. The SAME broken allocator as case 2 under the pre-fix accounting and
#    verdict.  It PASSES -- that is the defect, demonstrated rather than
#    described.  If this ever starts failing, the legacy path no longer
#    reproduces the bug and this script's claim is void.
run_case "inject=null + ORACLE_LEGACY_VERDICT=1 (pre-fix behaviour)" 0 \
	"ORACLE_INJECT=null:1000 ORACLE_LEGACY_VERDICT=1"

echo ""
if [[ $rc -eq 0 ]]; then
	echo "PASS: oracle discriminates (clean PASS; null and corrupt both FAIL;"
	echo "      pre-fix accounting reproduces the false PASS on the same defect)"
else
	echo "FAIL: oracle did not discriminate as required"
fi
exit $rc

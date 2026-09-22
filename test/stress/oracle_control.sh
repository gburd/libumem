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
#   it must PASS.  Any case coming out the wrong way fails this script.
#
#   Two independent ways of breaking things, because they prove different
#   things:
#
#   A. ORACLE_INJECT=... (in-process knob).  Breaks the allocation inside the
#      oracle's own wrapper.  Cheap, deterministic, always available.
#   B. LD_PRELOAD=oracle_null_shim.so (a genuinely broken ALLOCATOR).
#      Replaces umem_alloc underneath, so the oracle is judging an allocator
#      that really misbehaves and has no idea anything was injected.  This is
#      the stronger control: (A) is the oracle checking a defect it created
#      itself, (B) is the oracle checking someone else's.
#
#   Cases:
#     1. clean                      -> PASS (no false positive)
#     2. inject=null                -> FAIL (alloc)
#     3. inject=corrupt             -> FAIL (corruption)
#     4. inject=null + LEGACY       -> PASS, reproducing the pre-fix false
#                                      PASS on the very same defect.  This is
#                                      the demonstrated pre-fix failure the
#                                      plan's ground rule 2 requires.
#     5. shim=null  (broken alloc)  -> FAIL (alloc)
#     6. shim=alias (broken alloc)  -> FAIL (corruption/aliasing)
#     7. shim=null + LEGACY         -> PASS: the pre-fix verdict passing a
#                                      REAL broken allocator, not an injected
#                                      one.  The headline result.
#
# Exit status: 0 every case behaved as required, 1 any did not,
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

# ---- the stronger control: a genuinely broken ALLOCATOR underneath -------
#
# Build the shim against the same headers/compiler as the tree.  If it cannot
# be built the in-process cases above still ran, but say so plainly rather
# than quietly claiming the full experiment.
SHIM_SRC=$ROOT/test/stress/oracle_null_shim.c
SHIM=""
if [[ -r $SHIM_SRC ]]; then
	SHIM=$(mktemp -t oracle_shim_XXXXXX.so)
	if ! ${CC:-cc} -std=c11 -O1 -fPIC -shared -D_GNU_SOURCE \
	    -o "$SHIM" "$SHIM_SRC" -ldl 2>/tmp/oracle_shim_build.log; then
		echo "WARNING: could not build the broken-allocator shim:"
		sed 's/^/    /' /tmp/oracle_shim_build.log
		echo "         the in-process injection cases above still ran, but the"
		echo "         broken-allocator cases (5-7) did NOT."
		rm -f "$SHIM"; SHIM=""
		rc=1   # an incomplete experiment is not a passing one
	fi
else
	echo "WARNING: $SHIM_SRC missing; broken-allocator cases did NOT run"
	rc=1
fi

if [[ -n $SHIM ]]; then
	# 5. A real allocator that stops allocating.  Pre-fix: PASS.  Now: FAIL.
	run_case "shim=null (broken allocator returns NULL)" 1 \
		"LD_PRELOAD=$SHIM ORACLE_SHIM=null:1000"

	# 6. A real allocator that hands one live buffer to several owners.  This
	#    is the aliasing the oracle was built for, produced by the allocator.
	run_case "shim=alias (broken allocator double-allocates)" 1 \
		"LD_PRELOAD=$SHIM ORACLE_SHIM=alias:1000"

	# 7. THE HEADLINE: the pre-fix verdict against the same real broken
	#    allocator as case 5.  It passes.  That is what "the oracle could
	#    pass on failure" meant, shown rather than asserted.
	run_case "shim=null + ORACLE_LEGACY_VERDICT=1 (pre-fix PASSES a broken allocator)" 0 \
		"LD_PRELOAD=$SHIM ORACLE_SHIM=null:1000 ORACLE_LEGACY_VERDICT=1"

	rm -f "$SHIM"
fi

echo ""
if [[ $rc -eq 0 ]]; then
	echo "PASS: the oracle discriminates."
	echo "      clean run PASSes (no false positive);"
	echo "      injected NULL and injected corruption both FAIL;"
	echo "      a genuinely broken allocator (LD_PRELOAD shim) FAILs for both"
	echo "        returning NULL and for double-allocating;"
	echo "      and under the pre-fix accounting that same broken allocator"
	echo "        PASSes -- which is the defect P2.3 describes, reproduced."
else
	echo "FAIL: oracle did not discriminate as required"
fi
exit $rc

#!/usr/bin/env bash
#
# Run the interposer regressions with libumem_malloc.so actually preloaded.
#
# Two of the P1.1/P1.7 defects only exist in the LD_PRELOAD path (the shared
# calloc bump buffer, the untracked aligned_alloc), so running these binaries
# without LD_PRELOAD tests the platform allocator and proves nothing about
# libumem.  This script runs each binary BOTH ways: the no-preload run is the
# control (the assertions are C11/POSIX requirements, so it must pass there
# too), and the preload run is the actual regression.
#
# Exit status: 0 only if every configuration passes.
set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)

PRELOAD=""
for cand in "$ROOT/.libs/libumem_malloc.so" "$ROOT/.libs/libumem_malloc.so.1"; do
	[[ -f $cand ]] && { PRELOAD=$cand; break; }
done
if [[ -z $PRELOAD ]]; then
	echo "FAIL: libumem_malloc.so not built in $ROOT/.libs (run make)"
	exit 1
fi

export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Keep the default make-check invocation quick; the sustained runs are in
# scripts/ec2/.
THREADS=${INTERPOSE_THREADS:-16}
ITERS=${INTERPOSE_ITERS:-20000}

find_bin() {
	local d=$1 n=$2
	for cand in "$ROOT/$d/.libs/$n" "$ROOT/$d/$n"; do
		[[ -x $cand ]] && { echo "$cand"; return 0; }
	done
	return 1
}

rc=0
run_case() {
	local label=$1 preload=$2
	shift 2
	echo "== $label =="
	if [[ -n $preload ]]; then
		LD_PRELOAD="$preload" "$@"
	else
		"$@"
	fi
	local s=$?
	if [[ $s -ne 0 ]]; then
		echo "FAIL: $label exited $s"
		rc=1
	fi
	return 0
}

RACE=$(find_bin test/stress repro_calloc_interpose_race) || {
	echo "FAIL: repro_calloc_interpose_race not built"; exit 1; }
ALIGN=$(find_bin test/unit test_aligned_contracts) || {
	echo "FAIL: test_aligned_contracts not built"; exit 1; }

run_case "calloc race (control, no preload)"  "" \
	"$RACE" --threads="$THREADS" --iters=2000
run_case "calloc race (LD_PRELOAD interposer)" "$PRELOAD" \
	"$RACE" --threads="$THREADS" --iters="$ITERS"
run_case "aligned contracts (control, no preload)" "" "$ALIGN"
run_case "aligned contracts (LD_PRELOAD interposer)" "$PRELOAD" "$ALIGN" --strict

# free() must scale with threads.  Pre-fix, every free took a global mutex to
# scan an empty 512-slot table, so aggregate throughput FELL as threads were
# added (~500x below the API path at 192 threads on metal).  A ratio >= 1.0 at
# 8 threads is the bar; the defect delivered ~0.3-0.7.
SCALE=$(find_bin test/stress repro_interpose_free_scaling) || {
	echo "FAIL: repro_interpose_free_scaling not built"; exit 1; }
run_case "free() thread scaling (LD_PRELOAD interposer)" "$PRELOAD" "$SCALE"

if [[ $rc -eq 0 ]]; then
	echo "PASS: interposer regressions"
else
	echo "FAIL: interposer regressions"
fi
exit $rc

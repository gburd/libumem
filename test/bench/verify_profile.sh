#!/bin/bash
set -euo pipefail

# Verify bench_profile_test produces a valid profile with expected
# characteristics: at least 5 caches, at least 2 phases, correct
# cache names, and distinct rate patterns across phases.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BENCH="$TOP_DIR/test/bench/.libs/bench_profile_test"
PROF="/tmp/claude-1000/verify_prof.ump"
DUMP="/tmp/claude-1000/verify_prof_dump.txt"

if [ ! -x "$BENCH" ]; then
    BENCH="$TOP_DIR/test/bench/bench_profile_test"
fi

if [ ! -x "$BENCH" ]; then
    echo "SKIP: bench_profile_test not built"
    exit 0
fi

mkdir -p /tmp/claude-1000
rm -f "$PROF" "$DUMP"

PASS=0
FAIL=0

check() {
    local desc="$1"
    shift
    if "$@"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "--- Step 1: Record profile (~30 seconds) ---"
UMEM_PROFILE="record:$PROF" LD_LIBRARY_PATH="${TOP_DIR}/.libs:${LD_LIBRARY_PATH:-}" \
    "$BENCH" > "$DUMP" 2>&1
echo "Benchmark finished. Output saved to $DUMP"

echo ""
echo "--- Step 2: Verify profile file ---"
check "profile file created" test -f "$PROF"
check "profile file non-empty" test -s "$PROF"

echo ""
echo "--- Step 3: Verify dump header ---"
check "header present" grep -q "=== umem profile ===" "$DUMP"

NCACHES=$(grep -oP 'caches:\s+\K[0-9]+' "$DUMP" || echo "0")
check "num_caches >= 5 (got $NCACHES)" [ "$NCACHES" -ge 5 ]

NPHASES=$(grep -oP 'phases:\s+\K[0-9]+' "$DUMP" || echo "0")
check "num_phases >= 2 (got $NPHASES)" [ "$NPHASES" -ge 2 ]

echo ""
echo "--- Step 4: Verify cache names ---"
for NAME in prof_32 prof_64 prof_128 prof_256 prof_512; do
    check "cache '$NAME' in summary" grep -q "^$NAME " "$DUMP"
done

echo ""
echo "--- Step 5: Verify phase patterns ---"
HAS_BURST=$(grep -cP 'alloc_rate=[0-9]{4,}' "$DUMP" || echo "0")
check "burst phase with alloc_rate >= 1000 ($HAS_BURST lines)" \
    [ "$HAS_BURST" -gt 0 ]

HAS_QUIET=$(grep -c 'alloc_rate=0\.0' "$DUMP" || echo "0")
check "quiescent phase with alloc_rate=0.0 ($HAS_QUIET lines)" \
    [ "$HAS_QUIET" -gt 0 ]

echo ""
echo "--- Results: $PASS passed, $FAIL failed ---"

rm -f "$PROF" "$DUMP"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

#!/usr/bin/env bash
#
# Track coverage progress for libumem
#
# This script runs tests with coverage and generates a progress report
# comparing current coverage to targets.
#

set -euo pipefail

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}libumem Coverage Progress Tracker${NC}"
echo "===================================="
echo ""

# Run tests
echo -e "${BLUE}Running test suite...${NC}"
make check > /dev/null 2>&1 || echo "Some standard tests failed"

if [[ -x "test/test_main" ]]; then
    ./test/test_main > /dev/null 2>&1 || echo "Some unit tests failed"
fi

if [[ -x "test/property/prop_alloc_free2" ]]; then
    ./test/property/prop_alloc_free2 > /dev/null 2>&1 || echo "Some property tests failed"
fi

if [[ -x "test/property/prop_cache" ]]; then
    ./test/property/prop_cache > /dev/null 2>&1 || echo "Some cache property tests failed"
fi

# Capture coverage
echo -e "${BLUE}Capturing coverage data...${NC}"
lcov --capture \
     --directory . \
     --output-file coverage_raw.info \
     --ignore-errors negative,inconsistent,deprecated \
     --rc branch_coverage=0 > /dev/null 2>&1

# Filter
lcov --remove coverage_raw.info \
     "/usr/*" "*/test/*" "*/nix/store/*" "*/munit.c" "*/qc.c" "*/tdigest.c" \
     --output-file coverage.info \
     --ignore-errors negative,inconsistent,deprecated,unused \
     --rc branch_coverage=0 > /dev/null 2>&1

# Extract overall stats
echo ""
echo "===================================="
echo -e "${GREEN}Overall Coverage${NC}"
echo "===================================="
lcov --summary coverage.info 2>&1 | grep -v "WARNING" | grep -E "(lines|functions)"

# Define coverage targets
declare -A TARGETS
TARGETS["umem.c"]=95
TARGETS["vmem.c"]=95
TARGETS["malloc.c"]=95
TARGETS["umem_fork.c"]=95
TARGETS["envvar.c"]=95
TARGETS["umem_audit.c"]=95
TARGETS["umem_fail.c"]=95
TARGETS["umem_hooks.c"]=95
TARGETS["vmem_sbrk.c"]=80
TARGETS["umem_update_thread.c"]=95
TARGETS["vmem_mmap.c"]=95
TARGETS["misc.c"]=85

# Show per-file progress
echo ""
echo "===================================="
echo -e "${BLUE}Per-File Progress${NC}"
echo "===================================="
printf "%-30s %10s %10s %10s\n" "File" "Current" "Target" "Status"
echo "--------------------------------------------------------------------"

for file in "${!TARGETS[@]}"; do
    target=${TARGETS[$file]}
    coverage=$(lcov --list coverage.info 2>/dev/null | grep "$file" | awk '{print $2}' | sed 's/%//' || echo "0")

    if (( $(echo "$coverage >= $target" | bc -l 2>/dev/null || echo 0) )); then
        status="✓"
        color=$GREEN
    elif (( $(echo "$coverage >= $target - 10" | bc -l 2>/dev/null || echo 0) )); then
        status="⚠"
        color=$YELLOW
    else
        status="✗"
        color=$RED
    fi

    printf "%-30s ${color}%9.1f%%${NC} %9d%% %10s\n" "$file" "$coverage" "$target" "$status"
done

# Calculate files meeting target
meeting_target=0
total_files=${#TARGETS[@]}

for file in "${!TARGETS[@]}"; do
    target=${TARGETS[$file]}
    coverage=$(lcov --list coverage.info 2>/dev/null | grep "$file" | awk '{print $2}' | sed 's/%//' || echo "0")

    if (( $(echo "$coverage >= $target" | bc -l 2>/dev/null || echo 0) )); then
        meeting_target=$((meeting_target + 1))
    fi
done

# Summary
echo ""
echo "===================================="
echo -e "${BLUE}Summary${NC}"
echo "===================================="
echo "Files meeting target: $meeting_target / $total_files"

percent_complete=$(echo "scale=1; $meeting_target * 100 / $total_files" | bc -l)
echo "Progress: $percent_complete%"

if [ "$meeting_target" == "$total_files" ]; then
    echo -e "${GREEN}✓ All files meet coverage targets!${NC}"
    exit 0
else
    remaining=$((total_files - meeting_target))
    echo -e "${YELLOW}⚠ $remaining file(s) still need work${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Review COVERAGE_REPORT.md for detailed gap analysis"
    echo "  2. Focus on files with ✗ status"
    echo "  3. Write tests for uncovered code paths"
    echo "  4. Run this script again to track progress"
    exit 1
fi

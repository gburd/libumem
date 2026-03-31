#!/usr/bin/env bash
#
# Generate comprehensive coverage report for libumem
#
# Usage:
#   ./scripts/generate-coverage.sh [--min-coverage N]
#
# Requirements:
#   - lcov and genhtml (from lcov package)
#   - gcc with --coverage support
#
# Output:
#   - HTML report in test/coverage/index.html
#   - Summary printed to stdout
#   - Exit code 1 if coverage below minimum
#

set -euo pipefail

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
MIN_COVERAGE="${MIN_COVERAGE:-80}"
COVERAGE_DIR="test/coverage"
BUILD_DIR="."

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --min-coverage)
            MIN_COVERAGE="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [--min-coverage N]"
            echo ""
            echo "Options:"
            echo "  --min-coverage N    Set minimum coverage percentage (default: 80)"
            echo "  --help              Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check for required tools
if ! command -v lcov &> /dev/null; then
    echo -e "${RED}Error: lcov not found${NC}"
    echo "Install with: sudo apt-get install lcov  # Debian/Ubuntu"
    echo "           or: sudo yum install lcov     # RHEL/CentOS"
    echo "           or: nix develop              # Use Nix dev shell"
    exit 1
fi

if ! command -v genhtml &> /dev/null; then
    echo -e "${RED}Error: genhtml not found (part of lcov package)${NC}"
    exit 1
fi

echo -e "${GREEN}Generating coverage report for libumem${NC}"
echo "========================================"
echo ""

# Step 1: Check if code was built with coverage
if [[ ! -f "umem.gcno" ]]; then
    echo -e "${YELLOW}Warning: Coverage data files (.gcno) not found${NC}"
    echo "Building with coverage enabled..."
    make clean
    ./configure --enable-coverage
    make -j$(nproc)
fi

# Step 2: Run test suite
echo -e "${BLUE}Running test suite...${NC}"
make check || {
    echo -e "${RED}Warning: Some tests failed, but continuing with coverage${NC}"
}

# Run additional tests if available
if [[ -x "test/test_main" ]]; then
    echo "Running test/test_main..."
    ./test/test_main || echo "test_main had failures"
fi

if [[ -x "test/property/prop_alloc_free2" ]]; then
    echo "Running property tests..."
    ./test/property/prop_alloc_free2 || echo "prop tests had failures"
fi

if [[ -x "test/property/prop_cache" ]]; then
    ./test/property/prop_cache || echo "cache prop tests had failures"
fi

if [[ -x "test/integration/test_multithreaded" ]]; then
    echo "Running integration tests..."
    ./test/integration/test_multithreaded || echo "multithread tests had failures"
fi

# Step 3: Capture coverage data
echo ""
echo -e "${BLUE}Capturing coverage data...${NC}"
lcov --capture \
     --directory . \
     --output-file coverage_raw.info \
     --rc lcov_branch_coverage=1

# Step 4: Filter out unwanted files
echo "Filtering coverage data..."
lcov --remove coverage_raw.info \
     '/usr/*' \
     '*/test/*' \
     '*/munit.c' \
     '*/qc.c' \
     '*/tdigest.c' \
     --output-file coverage.info \
     --rc lcov_branch_coverage=1

# Step 5: Generate HTML report
echo "Generating HTML report..."
mkdir -p "$COVERAGE_DIR"
genhtml coverage.info \
        --output-directory "$COVERAGE_DIR" \
        --title "libumem Coverage Report" \
        --num-spaces 4 \
        --sort \
        --legend \
        --rc lcov_branch_coverage=1 \
        --demangle-cpp

# Step 6: Calculate coverage percentages
echo ""
echo "========================================"
echo -e "${GREEN}Coverage Summary${NC}"
echo "========================================"
echo ""

COVERAGE_SUMMARY=$(lcov --summary coverage.info 2>&1)
echo "$COVERAGE_SUMMARY"

# Extract line coverage percentage
LINE_COV=$(echo "$COVERAGE_SUMMARY" | grep "lines" | awk '{print $2}' | sed 's/%//')
FUNC_COV=$(echo "$COVERAGE_SUMMARY" | grep "functions" | awk '{print $2}' | sed 's/%//')

# Step 7: Per-file coverage
echo ""
echo "========================================"
echo -e "${BLUE}Per-File Coverage${NC}"
echo "========================================"
echo ""

# Show coverage for key files
lcov --list coverage.info | grep -E "(umem\.c|vmem\.c|malloc\.c|umem_fork\.c)" || true

# Step 8: Identify low-coverage areas
echo ""
echo "========================================"
echo -e "${YELLOW}Files Below 90% Coverage${NC}"
echo "========================================"
echo ""

lcov --list coverage.info | awk '$2 != "" && $2+0 < 90.0 && $2+0 > 0 {print $0}' || \
    echo "All files have >90% coverage!"

# Step 9: Generate gap analysis
echo ""
echo "========================================"
echo -e "${BLUE}Coverage Gap Analysis${NC}"
echo "========================================"
echo ""

# Core files analysis
for file in umem.c vmem.c malloc.c umem_fork.c; do
    if [[ -f "$file" ]]; then
        COV=$(lcov --list coverage.info | grep "$file" | awk '{print $2}' | sed 's/%//' || echo "0")
        if (( $(echo "$COV < 95" | bc -l 2>/dev/null || echo 1) )); then
            GAP=$(echo "95 - $COV" | bc -l 2>/dev/null || echo "?")
            echo -e "${YELLOW}$file: ${COV}% (${GAP}% gap to 95% target)${NC}"
        else
            echo -e "${GREEN}$file: ${COV}% ✓${NC}"
        fi
    fi
done

# Step 10: Check against minimum
echo ""
echo "========================================"

if (( $(echo "$LINE_COV < $MIN_COVERAGE" | bc -l 2>/dev/null || echo 1) )); then
    echo -e "${RED}Coverage ${LINE_COV}% is below minimum ${MIN_COVERAGE}%${NC}"
    echo ""
    echo "To improve coverage:"
    echo "  1. Review docs/COVERAGE_ANALYSIS.md for identified gaps"
    echo "  2. Run: xdg-open $COVERAGE_DIR/index.html"
    echo "  3. Look for red (uncovered) and orange (partially covered) lines"
    echo "  4. Write tests for those code paths"
    echo ""
    exit 1
else
    echo -e "${GREEN}✓ Coverage ${LINE_COV}% meets requirement (>=${MIN_COVERAGE}%)${NC}"
fi

# Step 11: Cleanup
rm -f coverage_raw.info

# Final message
echo ""
echo "========================================"
echo -e "${GREEN}Coverage report generated${NC}"
echo "========================================"
echo ""
echo "View report:"
echo "  xdg-open $COVERAGE_DIR/index.html"
echo ""
echo "Or browse manually:"
echo "  file://$(pwd)/$COVERAGE_DIR/index.html"
echo ""

exit 0

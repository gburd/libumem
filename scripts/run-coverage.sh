#!/usr/bin/env bash
#
# Run test suite with coverage measurement
#

set -euo pipefail

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Configuration
BUILD_DIR="${BUILD_DIR:-build-coverage}"
COVERAGE_DIR="test/coverage"
MIN_COVERAGE="${MIN_COVERAGE:-80}"

echo -e "${GREEN}Running tests with coverage${NC}"
echo "======================================"

# Clean previous build
if [[ -d "$BUILD_DIR" ]]; then
    echo "Cleaning previous build..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with coverage
echo "Configuring with coverage enabled..."
../configure --enable-coverage

# Build
echo "Building..."
make -j$(nproc)

# Run tests
echo "Running tests..."
make check || {
    echo -e "${RED}Tests failed${NC}"
    exit 1
}

# Capture coverage data
echo "Capturing coverage data..."
lcov --capture --directory . --output-file coverage.info

# Remove system/test files from coverage
lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage_filtered.info

# Generate HTML report
mkdir -p "../$COVERAGE_DIR"
genhtml coverage_filtered.info --output-directory "../$COVERAGE_DIR"

# Calculate coverage percentage
COVERAGE=$(lcov --summary coverage_filtered.info 2>&1 | grep lines | awk '{print $2}' | sed 's/%//')

echo ""
echo "======================================"
echo -e "${GREEN}Coverage Report Generated${NC}"
echo "Location: $COVERAGE_DIR/index.html"
echo "Total Coverage: ${COVERAGE}%"

# Check if coverage meets minimum
if (( $(echo "$COVERAGE < $MIN_COVERAGE" | bc -l) )); then
    echo -e "${RED}Coverage ${COVERAGE}% is below minimum ${MIN_COVERAGE}%${NC}"
    exit 1
else
    echo -e "${GREEN}Coverage meets requirement (>=${MIN_COVERAGE}%)${NC}"
fi

# Print file-level summary
echo ""
echo "File-level coverage:"
lcov --list coverage_filtered.info

# Return to original directory
cd ..

echo ""
echo "To view the report:"
echo "  xdg-open $COVERAGE_DIR/index.html"

#!/usr/bin/env bash
#
# Run test suite with sanitizers
#

set -euo pipefail

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Configuration
SANITIZERS="${SANITIZERS:-asan ubsan}"

run_with_sanitizer() {
    local sanitizer=$1
    local build_dir="build-${sanitizer}"

    echo -e "${GREEN}Testing with ${sanitizer}${NC}"
    echo "======================================"

    # Clean previous build
    if [[ -d "$build_dir" ]]; then
        rm -rf "$build_dir"
    fi

    mkdir -p "$build_dir"
    cd "$build_dir"

    # Configure
    echo "Configuring with --enable-${sanitizer}..."
    if ! ../configure --enable-${sanitizer}; then
        echo -e "${RED}Configuration failed${NC}"
        cd ..
        return 1
    fi

    # Build
    echo "Building..."
    if ! make -j$(nproc); then
        echo -e "${RED}Build failed${NC}"
        cd ..
        return 1
    fi

    # Run tests
    echo "Running tests..."
    local result=0

    # Set sanitizer options
    case "$sanitizer" in
        asan)
            export ASAN_OPTIONS="detect_leaks=1:check_initialization_order=1:strict_init_order=1"
            ;;
        ubsan)
            export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
            ;;
        tsan)
            export TSAN_OPTIONS="history_size=7:second_deadlock_stack=1"
            ;;
    esac

    if ! make check; then
        echo -e "${RED}Tests failed with ${sanitizer}${NC}"
        result=1
    else
        echo -e "${GREEN}Tests passed with ${sanitizer}${NC}"
    fi

    cd ..
    return $result
}

# Track overall result
overall_result=0

# Run each sanitizer
for sanitizer in $SANITIZERS; do
    if ! run_with_sanitizer "$sanitizer"; then
        overall_result=1
    fi
    echo ""
done

# Summary
echo "======================================"
if [[ $overall_result -eq 0 ]]; then
    echo -e "${GREEN}All sanitizer tests passed${NC}"
else
    echo -e "${RED}Some sanitizer tests failed${NC}"
fi

exit $overall_result

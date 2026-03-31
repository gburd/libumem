#!/bin/bash
# Standalone test runner for prop_fragmentation.c

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Compile the test
echo "Compiling prop_fragmentation test..."
gcc -Wall -Wextra -I"${PROJECT_ROOT}" -I"${PROJECT_ROOT}/test" -pthread \
    -o "${SCRIPT_DIR}/prop_fragmentation_test" \
    "${SCRIPT_DIR}/prop_fragmentation.c" \
    "${PROJECT_ROOT}/test/qc.c" \
    -L"${PROJECT_ROOT}/.libs" \
    -lumem -lumem_malloc -ldl -lm

if [ $? -eq 0 ]; then
    echo "Compilation successful"
    echo
    echo "Running fragmentation property tests..."
    LD_LIBRARY_PATH="${PROJECT_ROOT}/.libs:${LD_LIBRARY_PATH:-}" \
        "${SCRIPT_DIR}/prop_fragmentation_test"
    exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo
        echo "All tests passed!"
    else
        echo
        echo "Tests failed with exit code: $exit_code"
    fi

    exit $exit_code
else
    echo "Compilation failed"
    exit 1
fi

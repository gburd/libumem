#!/bin/bash
#
# Test script for experimental optimization features
#
# This script tests RSEQ, NUMA, and HTM feature detection and configuration.
#

set -e

echo "========================================"
echo "Experimental Features Test Script"
echo "========================================"
echo

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
PASSED=0
FAILED=0
SKIPPED=0

test_pass() {
    echo -e "${GREEN}✓ PASS${NC}: $1"
    ((PASSED++))
}

test_fail() {
    echo -e "${RED}✗ FAIL${NC}: $1"
    ((FAILED++))
}

test_skip() {
    echo -e "${YELLOW}⊘ SKIP${NC}: $1"
    ((SKIPPED++))
}

# 1. Check for RSEQ support
echo "=== Testing RSEQ Support ==="
echo

# Check for Linux kernel version
if [ -f /proc/version ]; then
    KERNEL_VERSION=$(uname -r | cut -d. -f1-2)
    KERNEL_MAJOR=$(echo "$KERNEL_VERSION" | cut -d. -f1)
    KERNEL_MINOR=$(echo "$KERNEL_VERSION" | cut -d. -f2)

    if [ "$KERNEL_MAJOR" -gt 4 ] || \
       ([ "$KERNEL_MAJOR" -eq 4 ] && [ "$KERNEL_MINOR" -ge 18 ]); then
        test_pass "Kernel version $KERNEL_VERSION supports RSEQ (4.18+ required)"
    else
        test_skip "Kernel version $KERNEL_VERSION too old for RSEQ (4.18+ required)"
    fi
else
    test_skip "Not a Linux system, RSEQ not available"
fi

# Check for rseq in kernel
if [ -d /sys/kernel/rseq ] || [ -f /sys/kernel/rseq/supported ]; then
    test_pass "Kernel RSEQ interface detected in sysfs"
else
    test_skip "Kernel RSEQ interface not found in sysfs"
fi

# Check for linux/rseq.h header
if [ -f /usr/include/linux/rseq.h ]; then
    test_pass "linux/rseq.h header found"
else
    test_skip "linux/rseq.h header not found"
fi

echo

# 2. Check for NUMA support
echo "=== Testing NUMA Support ==="
echo

# Check for libnuma
if ldconfig -p | grep -q libnuma; then
    test_pass "libnuma library found"
else
    test_fail "libnuma library not found (install libnuma-dev)"
fi

# Check for numa.h header
if [ -f /usr/include/numa.h ]; then
    test_pass "numa.h header found"
else
    test_fail "numa.h header not found (install libnuma-dev)"
fi

# Check if system has multiple NUMA nodes
if [ -f /sys/devices/system/node/online ]; then
    NUMA_NODES=$(cat /sys/devices/system/node/online | tr '-' ' ' | awk '{print $NF+1}')
    if [ "$NUMA_NODES" -gt 1 ]; then
        test_pass "System has $NUMA_NODES NUMA nodes (multi-socket)"
    else
        test_skip "System has only 1 NUMA node (single-socket)"
    fi
else
    test_skip "Cannot detect NUMA topology"
fi

# Check for numactl utility
if command -v numactl &> /dev/null; then
    test_pass "numactl utility available"
else
    test_skip "numactl utility not found (optional)"
fi

echo

# 3. Check for HTM support
echo "=== Testing HTM (Intel TSX) Support ==="
echo

# Check CPU for TSX support
if [ -f /proc/cpuinfo ]; then
    if grep -q " rtm " /proc/cpuinfo; then
        test_pass "CPU supports Intel RTM (Restricted Transactional Memory)"
    else
        test_skip "CPU does not support Intel RTM"
    fi

    if grep -q " hle " /proc/cpuinfo; then
        test_pass "CPU supports Intel HLE (Hardware Lock Elision)"
    else
        test_skip "CPU does not support Intel HLE"
    fi
else
    test_skip "Cannot detect CPU features (not on Linux)"
fi

# Check for immintrin.h header
if [ -f /usr/lib/gcc/x86_64-linux-gnu/*/include/immintrin.h ] || \
   [ -f /usr/include/immintrin.h ]; then
    test_pass "immintrin.h header found"
else
    test_skip "immintrin.h header not found (TSX intrinsics unavailable)"
fi

# Check for TSX in compiler
echo "int main() { return 0; }" | gcc -x c - -o /tmp/test-tsx -march=native 2>&1 | \
    grep -q "error" || test_pass "Compiler supports target architecture"

echo

# 4. Test build configuration
echo "=== Testing Build Configuration ==="
echo

# Check if configure script exists
if [ ! -f configure ]; then
    echo "Running autoreconf to generate configure script..."
    autoreconf -fi || test_fail "autoreconf failed"
fi

if [ -f configure ]; then
    test_pass "configure script exists"

    # Check if our options are in configure
    if grep -q "enable-rseq" configure; then
        test_pass "configure has --enable-rseq option"
    else
        test_fail "configure missing --enable-rseq option"
    fi

    if grep -q "enable-numa" configure; then
        test_pass "configure has --enable-numa option"
    else
        test_fail "configure missing --enable-numa option"
    fi

    if grep -q "enable-htm" configure; then
        test_pass "configure has --enable-htm option"
    else
        test_fail "configure missing --enable-htm option"
    fi
else
    test_fail "configure script not found"
fi

echo

# 5. Check source files
echo "=== Checking Source Files ==="
echo

FILES=(
    "umem_rseq.h"
    "umem_rseq.c"
    "umem_numa.h"
    "umem_numa.c"
    "umem_htm.h"
    "umem_htm.c"
)

for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        test_pass "Source file $file exists"
    else
        test_fail "Source file $file missing"
    fi
done

echo

# 6. Summary
echo "========================================"
echo "Test Summary"
echo "========================================"
echo -e "${GREEN}Passed:${NC}  $PASSED"
echo -e "${RED}Failed:${NC}  $FAILED"
echo -e "${YELLOW}Skipped:${NC} $SKIPPED"
echo

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All critical tests passed!${NC}"
    echo
    echo "To build with experimental features:"
    echo "  ./configure --enable-rseq --enable-numa --enable-htm"
    echo "  make"
    exit 0
else
    echo -e "${RED}Some tests failed. Check above for details.${NC}"
    exit 1
fi

#!/bin/bash
# Integration verification script
# Validates that all components are properly integrated

set -euo pipefail

echo "==================================================="
echo "  libumem Build System Integration Verification"
echo "==================================================="
echo

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

FAILED=0

check_file() {
    if [ -f "$1" ]; then
        echo -e "${GREEN}✓${NC} $1 exists"
    else
        echo -e "${RED}✗${NC} $1 missing"
        FAILED=1
    fi
}

check_dir() {
    if [ -d "$1" ]; then
        echo -e "${GREEN}✓${NC} $1/ exists"
    else
        echo -e "${RED}✗${NC} $1/ missing"
        FAILED=1
    fi
}

echo "Checking core implementation files..."
check_file "umem_hooks.c"
check_file "umem_hooks.h"
check_file "umem_audit.c"
echo

echo "Checking architecture support..."
check_dir "riscv64"
check_file "riscv64/umem_genasm.c"
check_dir "aarch64"
check_file "aarch64/umem_genasm.c"
echo

echo "Checking test infrastructure..."
check_dir "test"
check_file "test/test_main.c"
check_file "test/munit.c"
check_file "test/munit.h"
check_file "test/qc.c"
check_file "test/qc.h"
check_file "test/tdigest.c"
check_file "test/common.c"
check_file "test/common.h"
echo

echo "Checking test suites..."
check_dir "test/unit"
check_file "test/unit/test_umem_alloc.c"
check_file "test/unit/test_umem_cache.c"
check_file "test/unit/test_umem_align.c"
check_file "test/unit/test_umem_debug.c"
check_file "test/unit/test_vmem.c"
echo

echo "Checking property-based tests..."
check_dir "test/property"
check_file "test/property/prop_alloc_free2.c"
echo

echo "Checking benchmarks..."
check_dir "test/bench"
check_file "test/bench/bench_main.c"
check_file "test/bench/bench_framework.c"
check_file "test/bench/bench_framework.h"
check_file "test/bench/allocators.c"
echo

echo "Checking debugger extensions..."
check_dir "tools"
check_dir "tools/gdb"
check_file "tools/gdb/umem_gdb.py"
check_dir "tools/lldb"
check_file "tools/lldb/umem_lldb.py"
echo

echo "Checking scripts..."
check_dir "scripts"
check_file "scripts/run-coverage.sh"
check_file "scripts/run-sanitizers.sh"
echo

echo "Checking examples..."
check_dir "examples"
check_file "examples/palloc_integration.c"
check_file "examples/README.md"
echo

echo "Checking documentation..."
check_file "NIX_USAGE.md"
check_file "IMPLEMENTATION_STATUS.md"
check_file "umem_alloc.3"
check_file "umem_cache_create.3"
check_file "umem_debug.3"
echo

echo "Checking build system files..."
check_file "Makefile.am"
check_file "configure.ac"
check_file "flake.nix"
echo

echo "Checking CI configuration..."
check_file ".github/workflows/test.yml"
echo

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}==================================================="
    echo "  ✓ All integration checks passed!"
    echo -e "===================================================${NC}"
    echo
    echo "Next steps:"
    echo "  1. Run: ./autogen.sh"
    echo "  2. Run: ./configure"
    echo "  3. Run: make check"
    echo "  4. Run: nix build"
    echo "  5. Run: nix flake check"
    exit 0
else
    echo -e "${RED}==================================================="
    echo "  ✗ Integration verification failed"
    echo -e "===================================================${NC}"
    exit 1
fi

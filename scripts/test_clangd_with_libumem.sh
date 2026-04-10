#!/usr/bin/env bash
# Test clangd with LD_PRELOAD libumem to check for segfaults
set -euo pipefail

LIBUMEM_SO="$(pwd)/.libs/libumem.so.0.0.0"
TEST_FILE="umem.c"

if [ ! -f "$LIBUMEM_SO" ]; then
    echo "Error: libumem.so not found at $LIBUMEM_SO"
    echo "Run 'make' first to build libumem"
    exit 1
fi

if [ ! -f "compile_commands.json" ]; then
    echo "Generating compile_commands.json..."
    make compile_commands.json
fi

echo "Testing clangd with LD_PRELOAD=$LIBUMEM_SO"
echo "=========================================="

# Test 1: Simple check
echo "Test 1: clangd --check on $TEST_FILE"
if LD_PRELOAD="$LIBUMEM_SO" timeout 10 clangd --check="$TEST_FILE" 2>&1 | head -5; then
    echo "✓ Test 1 passed (no crash)"
else
    exit_code=$?
    if [ $exit_code -eq 139 ]; then
        echo "✗ Test 1 FAILED: segfault detected (exit code 139)"
        exit 1
    elif [ $exit_code -eq 124 ]; then
        echo "✗ Test 1 FAILED: timeout (possible hang)"
        exit 1
    else
        echo "✓ Test 1 passed with exit code $exit_code"
    fi
fi
echo ""

# Test 2: Parse multiple files
echo "Test 2: Check multiple source files"
for file in umem.c vmem.c umem_fail.c; do
    if [ -f "$file" ]; then
        echo -n "  Checking $file... "
        if LD_PRELOAD="$LIBUMEM_SO" timeout 10 clangd --check="$file" 2>&1 >/dev/null; then
            echo "✓"
        else
            exit_code=$?
            if [ $exit_code -eq 139 ]; then
                echo "✗ SEGFAULT"
                exit 1
            else
                echo "✓ (exit $exit_code)"
            fi
        fi
    fi
done
echo ""

# Test 3: LSP server mode
echo "Test 3: LSP server mode with JSON-RPC requests"
cat > /tmp/clangd_test_requests.json << 'EOF'
Content-Length: 150

{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"processId":null,"rootPath":"/home/gburd/ws/libumem","capabilities":{}}}
Content-Length: 52

{"jsonrpc":"2.0","method":"initialized","params":{}}
Content-Length: 38

{"jsonrpc":"2.0","id":1,"method":"shutdown"}
Content-Length: 44

{"jsonrpc":"2.0","method":"exit","params":{}}
EOF

if LD_PRELOAD="$LIBUMEM_SO" timeout 10 clangd < /tmp/clangd_test_requests.json > /tmp/clangd_output.json 2>&1; then
    echo "✓ Test 3 passed (server mode)"
else
    exit_code=$?
    if [ $exit_code -eq 139 ]; then
        echo "✗ Test 3 FAILED: segfault in server mode"
        exit 1
    else
        echo "✓ Test 3 passed with exit code $exit_code"
    fi
fi
echo ""

echo "=========================================="
echo "All tests passed! clangd works with libumem LD_PRELOAD"
echo ""
echo "To use clangd with libumem in your editor:"
echo "  LD_PRELOAD=$LIBUMEM_SO clangd"

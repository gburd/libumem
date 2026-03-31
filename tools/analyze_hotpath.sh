#!/bin/bash
# Analyze malloc hot path overhead

set -e

echo "=== libumem Malloc Hot Path Analysis ==="
echo

# Check if library is built
if [ ! -f .libs/libumem_malloc.so ]; then
    echo "Error: .libs/libumem_malloc.so not found"
    echo "Run 'make' first"
    exit 1
fi

echo "1. Checking symbol exports..."
echo "   umem_ptc_enabled: $(readelf -s .libs/libumem.so.0.0.0 | grep -c umem_ptc_enabled || echo 0) entries"
echo "   umem_flags:       $(readelf -s .libs/libumem.so.0.0.0 | grep -c umem_flags || echo 0) entries"
echo "   umem_ready:       $(readelf -s .libs/libumem.so.0.0.0 | grep -c umem_ready || echo 0) entries"
echo

echo "2. Analyzing malloc() hot path assembly..."
objdump -d .libs/libumem_malloc.so.0.0.0 | awk '
/<malloc>:/ { in_malloc=1; count=0 }
in_malloc && count < 40 { print; count++ }
count >= 40 { in_malloc=0 }
' | grep -E "(mov|test|cmp|call|je|jne)" | head -20
echo

echo "3. Counting TLS accesses in malloc()..."
TLS_COUNT=$(objdump -d .libs/libumem_malloc.so.0.0.0 | awk '/<malloc>:/,/<free>:/' | grep -c "%fs:" || echo 0)
echo "   TLS accesses in malloc(): $TLS_COUNT"
echo

echo "4. Checking PTC fast path..."
if objdump -t .libs/libumem.so.0.0.0 | grep -q "umem_genasm"; then
    echo "   ✓ PTC code generation present"
else
    echo "   ✗ PTC code generation NOT found"
fi
echo

echo "5. Checking recursion guard overhead..."
if objdump -d .libs/libumem_malloc.so.0.0.0 | awk '/<malloc>:/,/<free>:/' | grep -q "umem_malloc_recursion_depth"; then
    echo "   ✓ Recursion guard ACTIVE (adds overhead)"
else
    echo "   ✗ Recursion guard NOT active"
fi
echo

echo "6. Measuring overhead with microbenchmark..."
if [ -x tools/measure_overhead ]; then
    echo "   Running libc malloc..."
    tools/measure_overhead 1000 | grep "Size 64 bytes" -A 6
    echo
    echo "   Running umem malloc..."
    UMEM_DEBUG="" LD_PRELOAD=.libs/libumem_malloc.so tools/measure_overhead 1000 | grep "Size 64 bytes" -A 6
else
    echo "   Skipping: tools/measure_overhead not built"
fi
echo

echo "=== Analysis Complete ==="

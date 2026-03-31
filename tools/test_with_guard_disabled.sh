#!/bin/bash
# Test performance with recursion guard disabled

set -e

echo "=== Testing Performance Impact of Recursion Guard ==="
echo

# Save current state
echo "1. Testing current build (with recursion guard)..."
UMEM_DEBUG="" LD_PRELOAD=.libs/libumem_malloc.so tools/quick_test 2>&1 | grep Average

echo
echo "2. To test without recursion guard, rebuild with:"
echo "   CFLAGS=\"-DDISABLE_RECURSION_GUARD\" make clean && make"
echo
echo "3. Then run:"
echo "   UMEM_DEBUG=\"\" LD_PRELOAD=.libs/libumem_malloc.so tools/quick_test"
echo

echo "Expected improvements:"
echo "  - Current:  ~9000 ns per malloc+free"
echo "  - No guard: ~5000 ns per malloc+free (45% improvement)"
echo "  - Target:   ~400 ns per malloc+free (with PTC enabled)"
echo

echo "Note: Disabling recursion guard may cause issues on some platforms"
echo "where pthread_create calls malloc internally."

#!/usr/bin/env bash
# Isolate the ownership mt test that aborts under ASan (scope check only).
set +e
ASAN_DIR="$(dirname "$(gcc -print-file-name=libasan.so)")"
LINK="$(mktemp -d)"
ln -sf "${ASAN_DIR}/libasan.so.6.0.0" "${LINK}/libasan.so.6"
export LD_LIBRARY_PATH=".libs:${LINK}:${ASAN_DIR}"
echo "=== run only /umem_own/mt/move_transfer ==="
ASAN_OPTIONS=abort_on_error=1 test/.libs/test_main --no-fork \
    /umem_own/mt/move_transfer 2>&1 | head -45
echo "exit=${PIPESTATUS[0]}"

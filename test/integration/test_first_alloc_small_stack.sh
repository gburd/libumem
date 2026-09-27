#!/usr/bin/env bash
# Driver for test_first_alloc_small_stack (see the .c header): the first
# allocation on a small-stack thread must not crash.
#   1. umem_alloc() API, no preload                      -- the regression
#   2. malloc(), no preload (glibc)                      -- control
#   3. malloc() under LD_PRELOAD=libumem_malloc.so       -- the interposer
# Each case prints where umem_init ran.  Exit 0 only if all pass.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
BIN="$ROOT/test/integration/.libs/test_first_alloc_small_stack"
[[ -x $BIN ]] || BIN="$ROOT/test/integration/test_first_alloc_small_stack"
[[ -x $BIN ]] || { echo "FAIL: test_first_alloc_small_stack not built"; exit 1; }
PRELOAD="$ROOT/.libs/libumem_malloc.so"
[[ -f $PRELOAD ]] || { echo "FAIL: $PRELOAD not built"; exit 1; }
export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
rc=0
run() {
	local label=$1; shift
	env "$@" "$BIN" "${ARGS[@]}"; local s=$?
	if [[ $s -eq 77 ]]; then echo "SKIP: $label"
	elif [[ $s -ne 0 ]]; then echo "FAIL: $label exited $s"; rc=1
	fi
}
ARGS=(api);    run "umem_alloc API"            X_=1
ARGS=(malloc); run "glibc malloc (control)"    X_=1
ARGS=(malloc); run "libumem_malloc.so preload" LD_PRELOAD="$PRELOAD"
[[ $rc -eq 0 ]] && echo "PASS: first allocation on a small stack" \
	|| echo "FAIL: first allocation on a small stack"
exit $rc

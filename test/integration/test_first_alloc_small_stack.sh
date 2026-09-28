#!/usr/bin/env bash
# Driver for test_first_alloc_small_stack (see the .c header): the first
# allocation on a small-stack thread must not crash.
#   1. umem_alloc() API, no preload                      -- the regression
#   2. malloc(), no preload (glibc)                      -- control
#   3. malloc() under LD_PRELOAD=libumem_malloc.so       -- the interposer
#   4. umem_alloc() API with an unrecognized UMEM_OPTIONS item: umem_init
#      then reaches log_message() (misc.c), whose 4 KiB stack buffer alone
#      still SEGVs a 16 KiB x86 thread once init_lib.c's 64 KiB one is gone
#      (docs/results/2026-09-27-arm-dlopen-stack-crash.md section 9)
#   5. TIGHT budget (4 KiB usable below the alloc): umem_alloc() API. glibc
#      survives 4 KiB; umem's first call cost 5.2-5.4 KB before O7 (the eager
#      umem_stacktrace_init in umem_init), so this SEGVs on master and passes
#      after the fix.  SKIPs if the platform floor won't leave 4 KiB usable.
#   6. TIGHT budget glibc control -- must pass in both.
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
ARGS=(api);    run "umem_alloc API, bad UMEM_OPTIONS item" \
	UMEM_OPTIONS=first_alloc_small_stack_bogus_item
ARGS=(api 4096);    run "umem_alloc API, 4 KiB budget"          X_=1
ARGS=(malloc 4096); run "glibc malloc, 4 KiB budget (control)" X_=1
[[ $rc -eq 0 ]] && echo "PASS: first allocation on a small stack" \
	|| echo "FAIL: first allocation on a small stack"
exit $rc

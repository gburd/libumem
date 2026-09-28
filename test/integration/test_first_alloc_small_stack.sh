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
#   5. Per-arch ENFORCED depth cap on the first umem_alloc (cap:<bytes>),
#      measured on a large stack.  arm64 cap 3800 B (post-fix ~3.57 KB PASS,
#      pre-fix ~5.36 KB FAIL); x86_64 cap 5400 B (post-fix ~5.2 KB PASS, red if
#      the x86 first-alloc depth grows).  Both green now, both red on
#      regression.  See the .c head for why O7 helps arm but not x86.
#   6. arm64 only: survival of a first umem_alloc on a 4 KiB budget (the real
#      target).  Passes post-fix, SEGV'd pre-fix.  x86 does not meet 4 KiB
#      (~5.2 KB floor), so this arm is arm64-only; the x86 bound is the cap.
#   7. TIGHT budget glibc control -- must pass in both.
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
ARGS=(malloc 4096); run "glibc malloc, 4 KiB budget (control)" X_=1
# Per-arch enforced depth cap (green now, red if first-alloc depth grows).
if [[ $(uname -m) == aarch64 ]]; then
	ARGS=(api cap:3800); run "umem_alloc first-alloc depth <= 3800 B (arm64)" X_=1
	ARGS=(api 4096);     run "umem_alloc API, 4 KiB budget survival (arm64)" X_=1
else
	ARGS=(api cap:5400); run "umem_alloc first-alloc depth <= 5400 B (x86_64)" X_=1
fi
[[ $rc -eq 0 ]] && echo "PASS: first allocation on a small stack" \
	|| echo "FAIL: first allocation on a small stack"
exit $rc

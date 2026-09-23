#!/usr/bin/env bash
#
# P5.9 driver: runs test/security/test_stack_bounds with UMEM_DEBUG=audit,
# which is what makes getpcstack() run at all.  Without audit the binary
# exercises nothing and would report a vacuous PASS, so the environment is set
# here rather than left to whoever invokes it.
#
# See test/security/test_stack_bounds.c for the defect, the attacker position,
# and what each arm asserts.
#
# Exit: 0 pass, 1 fail, 77 not built.

set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)

BIN=""
for cand in "$ROOT/test/security/.libs/test_stack_bounds" \
            "$ROOT/test/security/test_stack_bounds"; do
	[[ -x $cand ]] && { BIN=$cand; break; }
done
if [[ -z $BIN ]]; then
	echo "SKIP: test/security/test_stack_bounds not built"
	exit 77
fi

export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# audit is the flag that turns on stack capture (umem.c's UMEM_AUDIT macro ->
# getpcstack).  Assert it took effect rather than assuming: a typo here would
# silently make every arm vacuous.
export UMEM_DEBUG=audit
export UMEM_LOGGING=transaction

# ASAN_OPTIONS: we want ASan to REPORT and exit non-zero, and arm A deliberately
# leaks a thread stack mapping, which is not what is under test.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:abort_on_error=0:exitcode=1"

out=$("$BIN" 2>&1)
rc=$?
echo "$out"

if ! grep -q '100 audited allocations' <<<"$out"; then
	echo "test_stack_bounds.sh: FAIL -- the control arm did not run;"
	echo "  the arms cannot be trusted (is UMEM_DEBUG=audit taking effect?)"
	exit 1
fi

# An ASan report anywhere in the output is a failure even if the binary's own
# accounting was happy: the whole point of the ASan run is to catch a read that
# lands in a mapped page and therefore does not SEGV.
if grep -qE 'ERROR: AddressSanitizer|SUMMARY: AddressSanitizer' <<<"$out"; then
	echo "test_stack_bounds.sh: FAIL -- AddressSanitizer reported an error"
	exit 1
fi

exit $rc

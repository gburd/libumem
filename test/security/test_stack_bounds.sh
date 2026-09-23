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

# audit is the flag that turns on stack capture in the allocator.  This test
# calls getpcstack() directly (see its header for why: through umem_alloc the
# library's own -O2 frames carry no frame pointer and the walk never reaches
# the frame under test), so audit is not strictly required -- but it is set
# anyway so the run matches the configuration P5.9 is about.
export UMEM_DEBUG=audit
export UMEM_LOGGING=transaction

# ASAN_OPTIONS: we want ASan to REPORT and exit non-zero, and arm A deliberately
# leaks a thread stack mapping, which is not what is under test.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:abort_on_error=0:exitcode=1"

out=$("$BIN" 2>&1)
rc=$?
echo "$out"

# The binary itself reports SKIP (77) when its build cannot drive the frame walk
# -- an ASan build, where ASan's frame rewriting both breaks the
# frame-pointer-slot assumption and limits the walk.  Propagate that, rather
# than treating a legitimate "this build cannot test it" as a failure.
if [[ $rc -eq 77 ]]; then
	echo "test_stack_bounds.sh: SKIP propagated from the binary"
	exit 77
fi

# The test's own control arm is the vacuity guard (it fails the run if the walk
# cannot produce a multi-frame chain).  Check for its line here too, so a build
# that silently stopped running the arms cannot report green through this
# wrapper.
if ! grep -q 'uncorrupted walk returned depth' <<<"$out"; then
	if grep -q '^SKIP:' <<<"$out"; then
		echo "test_stack_bounds.sh: skipping (platform has no frame walk)"
		exit 77
	fi
	echo "test_stack_bounds.sh: FAIL -- the control arm did not report;"
	echo "  the arms cannot be trusted"
	exit 1
fi

# An ASan report is a failure even if the binary's own accounting was happy:
# the point of the ASan run is to catch a read that lands in a mapped page and
# therefore does not SEGV.  Arms whose premise ASan invalidates are skipped
# inside the binary, so a report here is about the LIBRARY.
if grep -qE 'ERROR: AddressSanitizer|SUMMARY: AddressSanitizer' <<<"$out"; then
	echo "test_stack_bounds.sh: FAIL -- AddressSanitizer reported an error"
	exit 1
fi

exit $rc

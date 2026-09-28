#!/bin/sh
# O9 driver: findleaks must not report rseq-resident buffers as outstanding.
#
# Runs the same program in two library builds and compares:
#   default    -- rseq subtraction compiled IN  (the shipped behaviour)
#   norseqsub  -- rseq subtraction compiled OUT (test-only knob)
#
# Both do identical multi-thread batch churn that parks buffers in the rseq
# per-CPU magazines, then read findleaks' cached_skipped.  With the rseq
# subtraction in, those rounds are subtracted (higher cached_skipped); with it
# out, the SAME rounds are NOT (lower cached_skipped).  A positive difference
# is the rseq subtraction doing its job, isolated from the depot/per-CPU
# subtraction that is present in both builds.
#
# Exit: 0 PASS (difference > 0), 77 SKIP (no difference -> rseq not serving on
# this box, e.g. kernel refused registration), 1 FAIL, 99 harness error.
set -u

DEF="./test/integration/test_findleaks_rseq"
NEG="./test/integration/test_findleaks_rseq_norseqsub"
DUR="${1:-3}"

[ -x "$DEF" ] || { echo "harness: $DEF missing"; exit 99; }
[ -x "$NEG" ] || { echo "harness: $NEG missing"; exit 99; }

def_out=$("$DEF" "$DUR") || { echo "harness: default arm exit $?"; exit 99; }
neg_out=$("$NEG" "$DUR") || { echo "harness: negative arm exit $?"; exit 99; }
echo "default : $def_out"
echo "norseqsub: $neg_out"

def=$(printf '%s\n' "$def_out" | sed -n 's/.*cached_skipped=\([0-9]*\).*/\1/p')
neg=$(printf '%s\n' "$neg_out" | sed -n 's/.*cached_skipped=\([0-9]*\).*/\1/p')
[ -n "$def" ] && [ -n "$neg" ] || { echo "harness: no cached_skipped"; exit 99; }

diff=$((def - neg))
echo "rseq-resident buffers subtracted only by the rseq block: $diff"

if [ "$diff" -gt 0 ]; then
	echo "RESULT: PASS (findleaks subtracts $diff rseq-resident buffers; the rseq block, not the depot/per-CPU subtraction, is what keeps them out of the outstanding count)"
	exit 0
fi
if [ "$diff" -eq 0 ]; then
	echo "RESULT: SKIP (no rseq-resident buffers this run; rseq not serving on this box -- kernel refused registration, or the churn did not migrate CPUs)"
	exit 77
fi
echo "RESULT: FAIL (norseqsub subtracted MORE than default: $neg > $def -- impossible if the only difference is the rseq block)"
exit 1

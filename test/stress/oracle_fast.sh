#!/usr/bin/env bash
#
# Fast, make check-friendly invocation of the concurrency oracle.
# Small threads/iters so CI stays quick; the heavy sustained runs
# (128+ threads, 60s, ASan, across Intel/Graviton) live in
# scripts/ec2/README.md and docs/results/2026-07-24-concurrency-oracle-findings.md.
#
# Exit status (P2.4 -- PASS/SKIP/FAIL must be distinguishable):
#   0   PASS   the oracle ran and found no corruption and no alloc failures
#   1   FAIL   the oracle ran and its verdict was FAIL
#   77  SKIP   the binary is not built, so nothing was executed.  This used
#              to `exit 1` (a FAIL for a missing prerequisite); automake
#              reports 77 as SKIP, which is what "never ran" means.  Never
#              exit 0 here for a run that did not happen.
set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
BIN=$ROOT/test/stress/.libs/stress_concurrency_oracle
[[ -x $BIN ]] || BIN=$ROOT/test/stress/stress_concurrency_oracle

if [[ ! -x $BIN ]]; then
	echo "SKIP: $BIN not built (run make) -- oracle NOT executed"
	exit 77
fi

export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# The control knobs exist to break the run on purpose (oracle_control.sh).
# If they leak in from the environment this is no longer a clean gate.
if [[ -n ${ORACLE_INJECT:-} || -n ${ORACLE_LEGACY_VERDICT:-} ]]; then
	echo "FAIL: ORACLE_INJECT/ORACLE_LEGACY_VERDICT set in the environment;"
	echo "      this is the clean gate, not the control experiment."
	exit 1
fi

# ~1-2s: enough threads to migrate + hit the depot, small iters.
"$BIN" --threads=8 --iters=50000 --size-class=mixed --pattern=all
status=$?
if [[ $status -eq 0 ]]; then
	echo "PASS: oracle clean"
else
	echo "FAIL: oracle exited $status"
fi
exit $status

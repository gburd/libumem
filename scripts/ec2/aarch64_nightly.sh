#!/usr/bin/env bash
# scripts/ec2/aarch64_nightly.sh - end-to-end aarch64 correctness run:
# launch arm-lo -> bootstrap -> build+test -> terminate, always.
#
# Driven by .forgejo/workflows/aarch64-nightly.yml on a schedule trigger
# (Forgejo CI has no arm64 runner here -- see tests.yml's header). Can also
# be run by hand: AWS_PROFILE=bene ./scripts/ec2/aarch64_nightly.sh
#
# Exit code is the build+test exit code (or the launch/bootstrap failure's),
# NOT swallowed -- the workflow step that calls this can fail the job. The
# instance is terminated in a trap so a build/test failure (or this script
# being killed) never leaves an arm-lo box running.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROLE="arm-lo"
RC=1

cleanup() {
	echo "[aarch64-nightly] terminating $ROLE (always-run trap)" >&2
	"$DIR/terminate.sh" "$ROLE" || true
}
trap cleanup EXIT

"$DIR/launch.sh" "$ROLE" || { echo "[aarch64-nightly] launch failed" >&2; exit 1; }
"$DIR/bootstrap.sh" "$ROLE" || { echo "[aarch64-nightly] bootstrap failed" >&2; exit 1; }

# build+test: fresh autotools regen (cross-arch worktree sync leaves stale
# host-toolchain artifacts, see clean-regen.sh) + the fast correctness suite.
# This is exactly the class of bug the v2.0.0 rseq SIGSEGV was: a crash that
# only reproduces on real aarch64 hardware, never on the x86_64 CI runner.
"$DIR/run-remote.sh" "$ROLE" \
	'bash scripts/ec2/clean-regen.sh && make -j$(nproc) && make check TESTS="umem_test umem_test2 umem_test3 umem_ptc_fork_test test/test_debug" && LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork'
RC=$?

exit "$RC"

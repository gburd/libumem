#!/usr/bin/env bash
# test/limits/run_all.sh -- run one Phase 6 hard-limit probe (both builds).
#
#   test/limits/run_all.sh <probe> [args...]
#
# Runs test/limits/<probe> (libumem API) and test/limits/<probe>_glibc (plain
# libc) with the same args, back to back, each under its own `timeout`, and
# prints both with a banner so one job log holds the pair.  Honours
# LIM_TIMEOUT (seconds, default 1800) and LIM_ONLY=umem|glibc to run one arm.
#
# Never rsync'd results, never make check: this is what a job.sh command
# invokes from an isolated verify-isolated.sh tree.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
probe="${1:?probe name}"; shift
tmo="${LIM_TIMEOUT:-1800}"
only="${LIM_ONLY:-}"

run() {
	local bin="$1"; shift
	echo "=== $(basename "$bin") $* @ $(date -u +%FT%TZ) sha=$(cat "$here/ISOLATED_PROVENANCE" 2>/dev/null | awk -F= '/^sha/{print substr($2,1,12)}') ==="
	[ -x "$bin" ] || { echo "MISSING $bin"; return 1; }
	/usr/bin/time -f "  wall=%es maxrss=%MkB rc=%x" timeout -s KILL "$tmo" "$bin" "$@"
	echo "=== end $(basename "$bin") rc=$? ==="
}

cd "$here"
[ "$only" = glibc ] || run "test/limits/$probe" "$@"
if [ -x "test/limits/${probe}_glibc" ]; then
	[ "$only" = umem ] || run "test/limits/${probe}_glibc" "$@"
fi

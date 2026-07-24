#!/usr/bin/env bash
#
# Fast, make check-friendly invocation of the concurrency oracle.
# Small threads/iters so CI stays quick; the heavy sustained runs
# (128+ threads, 60s, ASan, across Intel/Graviton) live in
# scripts/ec2/README.md and docs/results/2026-07-24-concurrency-oracle-findings.md.
set -euo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
BIN=$ROOT/test/stress/.libs/stress_concurrency_oracle
[[ -x $BIN ]] || BIN=$ROOT/test/stress/stress_concurrency_oracle

if [[ ! -x $BIN ]]; then
	echo "FAIL: $BIN not built (run make)"
	exit 1
fi

export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ~1-2s: enough threads to migrate + hit the depot, small iters.
"$BIN" --threads=8 --iters=50000 --size-class=mixed --pattern=all

#!/usr/bin/env bash
# test/bench/bench_gate.sh - soft, non-blocking CI benchmark regression
# annotation (WS-C3). Runs the same short single-thread + 2-thread config
# used to capture test/bench/baseline/x86_64-ci.toml and diffs against it
# via the existing bench_compare_history() (>10% ops/sec drop or >10% p99
# rise = "REGRESSION" line on stdout). Always exits 0: this is a shared,
# untuned Forgejo docker runner -- variance here is too high to hard-fail a
# build on. Authoritative gating is the EC2 matrix (test/bench/matrix.sh),
# not this job.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."   # repo root
BENCH="test/bench/.libs/bench_main"
[[ -x "$BENCH" ]] || BENCH="test/bench/bench_main"
if [[ ! -x "$BENCH" ]]; then
    echo "::warning::bench_gate: $BENCH not built, skipping" >&2
    exit 0
fi

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:}.libs"
mkdir -p test/bench/results
cp test/bench/baseline/x86_64-ci.toml test/bench/results/history.toml

OUT="$("$BENCH" -a umem -w single -n 2000000 -s 16:256 -r7 -W1 --compare 2>&1)"
echo "$OUT"
OUT2="$("$BENCH" -a umem -w multi -t 2 -n 2000000 -s 16:256 -r7 -W1 --compare 2>&1)"
echo "$OUT2"

REGRESSIONS="$(printf '%s\n%s\n' "$OUT" "$OUT2" | grep -c 'REGRESSION:' || true)"
if [[ "$REGRESSIONS" -gt 0 ]]; then
    echo "::warning::bench-gate: $REGRESSIONS directional regression(s) vs test/bench/baseline/x86_64-ci.toml (shared runner, non-authoritative -- see scripts/ec2/README.md for the real EC2 gate)"
else
    echo "::notice::bench-gate: no directional regression vs test/bench/baseline/x86_64-ci.toml"
fi

# Never fail the build from here.
exit 0

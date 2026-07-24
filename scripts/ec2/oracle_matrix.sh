#!/usr/bin/env bash
# Remote oracle matrix driver (synced to EC2, invoked by run-remote.sh).
# Usage: oracle_matrix.sh <threads> <duration_sec> [asan]
set -e
THREADS="${1:-16}"
DUR="${2:-60}"
MODE="${3:-default}"

if [ "$MODE" = "asan" ]; then
	./scripts/ec2/clean-regen.sh --enable-asan >/dev/null
else
	./scripts/ec2/clean-regen.sh >/dev/null
fi
make -j"$(nproc)" test/stress/stress_concurrency_oracle >/dev/null 2>&1
echo "BUILD_OK mode=$MODE ncpu=$(nproc) threads=$THREADS dur=${DUR}s"

BIN=./test/stress/.libs/stress_concurrency_oracle
run() {
	echo "--- oracle $* ---"
	LD_LIBRARY_PATH=.libs "$BIN" "$@"
	echo "exit=$?"
}

run --threads="$THREADS" --duration="$DUR" --size-class=small --pattern=multi
run --threads="$THREADS" --duration="$DUR" --size-class=mag   --pattern=multi
run --threads="$THREADS" --duration="$DUR" --size-class=mixed --pattern=all
run --threads="$THREADS" --duration="$DUR" --size-class=large --pattern=churn

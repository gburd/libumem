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
# AL2023 gcc's libasan has no libasan.so.6 soname symlink (see
# scripts/ec2/remote_run_asan.sh); preload the real .so.6.0.0 file
# directly and disable leak detection -- umem intentionally retains
# process-lifetime TLS/init allocations that LSAN misreports as leaks
# (same convention as remote_run_asan.sh / oob_validate.sh).
ASAN="$(gcc -print-file-name=libasan.so.6.0.0)"
[ -f "$ASAN" ] || ASAN="$(ls /usr/lib/gcc/*/*/libasan.so.6.0.0 2>/dev/null | head -1)"
run() {
	echo "--- oracle $* ---"
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 \
	    LD_PRELOAD="$ASAN" LD_LIBRARY_PATH=.libs "$BIN" "$@"
	echo "exit=$?"
}

run --threads="$THREADS" --duration="$DUR" --size-class=small --pattern=multi
run --threads="$THREADS" --duration="$DUR" --size-class=mag   --pattern=multi
run --threads="$THREADS" --duration="$DUR" --size-class=mixed --pattern=all
run --threads="$THREADS" --duration="$DUR" --size-class=large --pattern=churn

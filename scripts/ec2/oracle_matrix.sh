#!/usr/bin/env bash
# Remote oracle matrix driver (synced to EC2, invoked via job.sh).
# Usage: oracle_matrix.sh <threads> <duration_sec> [asan]
#
# Exit status (P2.4): 0 every oracle run PASSed; 1 any run FAILed; 77 the
# oracle could not be built or the ASan runtime could not be found, so nothing
# was measured.  This script used to `set -e` with each run's status
# unexamined, so a failing oracle could end the script without a verdict and a
# missing libasan silently produced an unsanitized run reported as an ASan one.
set -uo pipefail
THREADS="${1:-16}"
DUR="${2:-60}"
MODE="${3:-default}"

if [ "$MODE" = "asan" ]; then
	./scripts/ec2/clean-regen.sh --enable-asan >/dev/null || {
		echo "SKIP: clean-regen --enable-asan failed; nothing measured"; exit 77; }
else
	./scripts/ec2/clean-regen.sh >/dev/null || {
		echo "SKIP: clean-regen failed; nothing measured"; exit 77; }
fi
if ! make -j"$(nproc)" test/stress/stress_concurrency_oracle >/dev/null 2>&1; then
	echo "SKIP: oracle did not build; nothing measured"
	exit 77
fi
echo "BUILD_OK mode=$MODE ncpu=$(nproc) threads=$THREADS dur=${DUR}s"

BIN=./test/stress/.libs/stress_concurrency_oracle
[ -x "$BIN" ] || { echo "SKIP: $BIN missing after build"; exit 77; }

# The control knobs deliberately break the run (see oracle_control.sh).  If
# they leak in from the environment this matrix measures nothing.
if [ -n "${ORACLE_INJECT:-}" ] || [ -n "${ORACLE_LEGACY_VERDICT:-}" ]; then
	echo "FAIL: ORACLE_INJECT/ORACLE_LEGACY_VERDICT set; this is the real"
	echo "      matrix, not the control experiment."
	exit 1
fi

# AL2023 gcc's libasan has no libasan.so.6 soname symlink (see
# scripts/ec2/remote_run_asan.sh); preload the real .so.6.0.0 file
# directly and disable leak detection -- umem intentionally retains
# process-lifetime TLS/init allocations that LSAN misreports as leaks
# (same convention as remote_run_asan.sh / oob_validate.sh).
ASAN=""
if [ "$MODE" = "asan" ]; then
	ASAN="$(gcc -print-file-name=libasan.so.6.0.0)"
	[ -f "$ASAN" ] || ASAN="$(ls /usr/lib/gcc/*/*/libasan.so.6.0.0 2>/dev/null | head -1)"
	if [ ! -f "${ASAN:-}" ]; then
		# Running unsanitized and calling it ASan would be a false result.
		echo "SKIP: asan mode requested but libasan.so.6.0.0 not found;"
		echo "      refusing to report an unsanitized run as an ASan run"
		exit 77
	fi
fi

rc=0
run() {
	echo "--- oracle $* ---"
	if [ -n "$ASAN" ]; then
		ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 \
		    LD_PRELOAD="$ASAN" LD_LIBRARY_PATH=.libs "$BIN" "$@"
	else
		LD_LIBRARY_PATH=.libs "$BIN" "$@"
	fi
	s=$?
	echo "exit=$s"
	if [ "$s" -ne 0 ]; then
		echo "FAIL: oracle $* exited $s"
		rc=1
	fi
}

run --threads="$THREADS" --duration="$DUR" --size-class=small --pattern=multi
run --threads="$THREADS" --duration="$DUR" --size-class=mag   --pattern=multi
run --threads="$THREADS" --duration="$DUR" --size-class=mixed --pattern=all
run --threads="$THREADS" --duration="$DUR" --size-class=large --pattern=churn

echo ""
if [ "$rc" -eq 0 ]; then
	echo "RESULT: PASS (all 4 oracle configurations)"
else
	echo "RESULT: FAIL (at least one oracle configuration)"
fi
exit "$rc"

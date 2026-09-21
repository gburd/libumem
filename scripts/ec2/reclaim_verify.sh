#!/usr/bin/env bash
# P1.4/P1.5 verification: build, run the reclaim regressions, then the
# standard gates (make check, test_main --no-fork, concurrency oracle).
# Never exits early: every gate must report, so a later failure cannot hide
# an earlier one, and a gate that was never executed is visible as such.
set -u
./scripts/ec2/clean-regen.sh >/dev/null 2>&1 || { echo "clean-regen FAILED"; exit 1; }
CONFIGURE_ARGS="${CONFIGURE_ARGS:-}"
./configure $CONFIGURE_ARGS > /tmp/conf.log 2>&1 || { echo "configure FAILED"; tail -20 /tmp/conf.log; exit 1; }
make -j"$(nproc)" > /tmp/build.log 2>&1 || { echo "build FAILED"; tail -40 /tmp/build.log; exit 1; }
echo "=== BUILD OK (CONFIGURE_ARGS='$CONFIGURE_ARGS') ==="
export LD_LIBRARY_PATH=.libs
RECL="reclaim=1,reclaim_delay=0,reap_interval=1"

run() { local n="$1"; shift; echo "--- $n ---"; "$@"; echo "rc=$?"; }

run "P1.4 destroy" timeout 120 ./test/unit/.libs/repro_reclaim_destroy
run "P1.5a hash_guards" env UMEM_DEBUG=guards UMEM_OPTIONS="$RECL" timeout 120 ./test/unit/.libs/repro_reclaim_reuse hash_guards
run "P1.5b big_quantum" env UMEM_OPTIONS="$RECL" timeout 120 ./test/unit/.libs/repro_reclaim_reuse big_quantum
run "P1.5c race" env UMEM_OPTIONS="$RECL" timeout 200 ./test/unit/.libs/repro_reclaim_reuse race
run "P1.5c reap_reentry" env UMEM_OPTIONS="$RECL" timeout 200 ./test/unit/.libs/repro_reclaim_reuse reap_reentry

echo "--- make check ---"
make check > /tmp/check.log 2>&1; echo "rc=$?"
grep -E "^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)" /tmp/check.log || tail -25 /tmp/check.log

echo "--- test_main --no-fork ---"
timeout 1200 ./test/.libs/test_main --no-fork > /tmp/tm.log 2>&1; echo "rc=$?"
echo "OK=$(grep -c '\[ OK' /tmp/tm.log) FAIL=$(grep -c '\[ FAIL' /tmp/tm.log) SKIP=$(grep -c '\[ SKIP' /tmp/tm.log) ERROR=$(grep -c '\[ ERROR' /tmp/tm.log)"
grep -E "FAIL|ERROR" /tmp/tm.log | head -20
tail -3 /tmp/tm.log

echo "--- concurrency oracle ---"
timeout 900 ./test/stress/.libs/stress_concurrency_oracle > /tmp/oracle.log 2>&1; echo "rc=$?"
tail -10 /tmp/oracle.log

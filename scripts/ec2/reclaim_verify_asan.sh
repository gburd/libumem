#!/usr/bin/env bash
# ASan gate for the reclaim regressions + oracle, plus an RSS observation.
set -u
./scripts/ec2/clean-regen.sh --enable-asan >/dev/null 2>&1 || { echo "clean-regen FAILED"; exit 1; }
make -j"$(nproc)" > /tmp/build.log 2>&1 || { echo "build FAILED"; tail -40 /tmp/build.log; exit 1; }
echo "=== ASAN BUILD OK ==="
ASAN="$(gcc -print-file-name=libasan.so.6.0.0)"
[ -f "$ASAN" ] || ASAN="$(ls /usr/lib/gcc/*/*/libasan.so.6.0.0 2>/dev/null | head -1)"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
export LD_PRELOAD="$ASAN" LD_LIBRARY_PATH=.libs
RECL="reclaim=1,reclaim_delay=0,reap_interval=1"
run() { local n="$1"; shift; echo "--- $n ---"; "$@"; echo "rc=$?"; }
run "asan P1.4 destroy" timeout 300 ./test/unit/.libs/repro_reclaim_destroy
run "asan P1.5a hash_guards" env UMEM_DEBUG=guards UMEM_OPTIONS="$RECL" timeout 300 ./test/unit/.libs/repro_reclaim_reuse hash_guards
run "asan P1.5b big_quantum" env UMEM_OPTIONS="$RECL" timeout 300 ./test/unit/.libs/repro_reclaim_reuse big_quantum
run "asan P1.5c race" env UMEM_OPTIONS="$RECL" timeout 400 ./test/unit/.libs/repro_reclaim_reuse race
run "asan P1.5c reap_reentry" env UMEM_OPTIONS="$RECL" timeout 400 ./test/unit/.libs/repro_reclaim_reuse reap_reentry
echo "--- asan concurrency oracle ---"
timeout 900 ./test/stress/.libs/stress_concurrency_oracle --threads=8 --duration=20 --size-class=mixed --pattern=all > /tmp/o.log 2>&1; echo "rc=$?"
tail -8 /tmp/o.log

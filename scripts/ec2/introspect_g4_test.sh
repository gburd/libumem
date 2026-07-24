#!/usr/bin/env bash
# scripts/ec2/introspect_g4_test.sh — G4: dependency-free TUI monitor.
# Asserts the scriptable --once mode renders the expected panels against a
# running stress process.
set -e

./scripts/ec2/clean-regen.sh --enable-introspect >/dev/null 2>&1
make -j"$(nproc)" libumem.la tools/umemctl test/integration/introspect_churn \
    >/tmp/build.log 2>&1 || { echo BUILD_FAIL; tail -20 /tmp/build.log; exit 1; }
echo "BUILD_OK"

CTL="./tools/umemctl"
CHURN="$(find . -name introspect_churn -type f -perm -u+x | head -1)"
export LD_LIBRARY_PATH=.libs

UMEM_OPTIONS=introspect=1 "$CHURN" 20 &
PID=$!
sleep 3

echo "===== monitor --once ====="
OUT="$($CTL "$PID" monitor --once)"
echo "$OUT"

echo "===== assertions ====="
fail=0
for pat in "libumem monitor" "caches" "bufs in-use" "slabs created" \
           "depot contention" "magazine reloads" "RSS" "cache"; do
    if echo "$OUT" | grep -qi "$pat"; then
        echo "OK: panel '$pat'"
    else
        echo "MISSING: panel '$pat'"; fail=1
    fi
done

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
[ "$fail" -eq 0 ] && echo "G4_MONITOR_OK" || echo "G4_MONITOR_FAIL"
exit "$fail"

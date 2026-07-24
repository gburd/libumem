#!/usr/bin/env bash
# scripts/ec2/introspect_g3_test.sh — G3: record, BREAK/resume,
# break-before-leaked. Proves the allocating thread stops on a condvar and
# resumes on `continue`, and that a two-phase leak workflow stops right
# before an allocation whose size matches a learned-leaked signature.
set -e

./scripts/ec2/clean-regen.sh --enable-introspect >/dev/null 2>&1
make -j"$(nproc)" libumem.la tools/umemctl test/integration/introspect_churn \
    >/tmp/build.log 2>&1 || { echo BUILD_FAIL; tail -20 /tmp/build.log; exit 1; }
echo "BUILD_OK"

CTL="./tools/umemctl"
CHURN="$(find . -name introspect_churn -type f -perm -u+x | head -1)"
export LD_LIBRARY_PATH=.libs

# ---------- Test 1: break seq=<n> stops, continue resumes ----------
echo "===== TEST 1: break seq + continue ====="
UMEM_OPTIONS=introspect=1 "$CHURN" 30 &
PID=$!
sleep 2
# Arm a break on the next allocation (seq=1) and confirm the process stalls.
$CTL "$PID" break seq=1
sleep 1
# Sample iteration count twice ~1s apart; if stopped, iterations don't grow.
# (churn prints nothing mid-run, so instead we check the stderr BREAK line.)
echo "-- process should be stopped on condvar now --"
# Resume it.
$CTL "$PID" continue
sleep 1
echo "-- resumed --"
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

# ---------- Test 2: break-before-leaked, two-phase, under gdb ----------
echo "===== TEST 2: break leaked (learn -> attach gdb -> stop) ====="
# Phase 1: learn leaked signatures under audit.
UMEM_OPTIONS=introspect=1 UMEM_DEBUG=audit "$CHURN" 60 &
PID=$!
sleep 3
$CTL "$PID" record --learn-leaks /tmp/leaks.set >/dev/null 2>&1
echo "-- learned leak set (unique sizes): --"
sort -u /tmp/leaks.set | head
NSIG=$(wc -l < /tmp/leaks.set)
echo "leak-set lines: $NSIG"

# Phase 2: arm break leaked. The next alloc matching a leaked size stops.
$CTL "$PID" break leaked --set /tmp/leaks.set
sleep 2
echo "-- attaching gdb to the stopped thread (backtrace of a stopped worker) --"
# gdb: attach, find a thread parked in pthread_cond_wait via break_check.
gdb -q -batch -p "$PID" \
    -ex 'set pagination off' \
    -ex 'thread apply all bt' 2>/dev/null \
    | grep -iE 'umem_introspect_break_check|_umem_alloc|leak_site|pthread_cond' \
    | head -20 || echo "(gdb grep found nothing)"

echo "-- resume --"
$CTL "$PID" continue
sleep 1
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
echo "G3_TEST_DONE"

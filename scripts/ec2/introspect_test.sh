#!/usr/bin/env bash
# scripts/ec2/introspect_test.sh — end-to-end G1/G2 functional test.
set -e

./scripts/ec2/clean-regen.sh --enable-introspect >/dev/null 2>&1
make -j"$(nproc)" libumem.la tools/umemctl test/integration/introspect_churn \
    >/tmp/build.log 2>&1 || { echo BUILD_FAIL; tail -20 /tmp/build.log; exit 1; }
echo "BUILD_OK"

CTL="./tools/umemctl"
CHURN="$(find . -name introspect_churn -type f -perm -u+x | head -1)"
export LD_LIBRARY_PATH=.libs

# ---------- run 1: plain introspect, stats/caches/logtail ----------
UMEM_OPTIONS=introspect=1 "$CHURN" 15 &
PID=$!
sleep 2
echo "===== stats ====="
$CTL "$PID" stats
echo "===== caches (in-use, head) ====="
$CTL "$PID" caches | awk 'NR==1 || $3>0' | head -8
echo "===== whatis on a live slab base ====="
# grab a cache name with buffers, then dump it and probe its slab addr
CN=$($CTL "$PID" caches | awk 'NR>1 && $3>0 {print $1; exit}')
SLAB=$($CTL "$PID" cache "$CN" 2>/dev/null; true)
echo "===== logtail (2s) ====="
timeout 2 $CTL "$PID" logtail || true
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

# ---------- run 2: audit, leaks ----------
UMEM_OPTIONS=introspect=1 UMEM_DEBUG=audit "$CHURN" 15 &
PID=$!
sleep 3
echo "===== audit caches (odd flags = UMF_AUDIT set) ====="
$CTL "$PID" caches | awk 'NR==1 || (index($5,"0x")==1)' | head -6
echo "===== leaks (head) ====="
$CTL "$PID" leaks | head -18
echo "===== leak count ====="
$CTL "$PID" leaks | grep -c '^leak ' | cat
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
echo "TEST_DONE"

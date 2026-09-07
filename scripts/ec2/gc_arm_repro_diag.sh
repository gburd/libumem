#!/usr/bin/env bash
# Larger-sample ASan repro loop with diagnostics; reports every failure, does not stop early.
set -u
ASANLIB=$(gcc -print-file-name=libasan.so)
n="${1:-30}"
fails=0
for i in $(seq 1 "$n"); do
  ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD=$ASANLIB LD_LIBRARY_PATH=.libs timeout 60 \
    ./test/property/.libs/prop_gc --threads=16 --rounds=800 >/tmp/asanN.$i 2>&1
  rc=$?
  if grep -q "STW SOUNDNESS BUG\|RUNAWAY CHAIN" /tmp/asanN.$i; then
    fails=$((fails+1))
    echo "--- run $i FAIL (rc=$rc) ---"
    grep -E "STW SOUNDNESS BUG|RUNAWAY CHAIN|diag:" /tmp/asanN.$i
  fi
done
echo "TOTAL: fails=$fails / $n"

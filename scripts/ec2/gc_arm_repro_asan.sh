#!/usr/bin/env bash
# Loop prop_gc under ASan until the STW soundness bug reproduces.
set -u
ASANLIB=$(gcc -print-file-name=libasan.so)
for i in $(seq 1 40); do
  ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD=$ASANLIB LD_LIBRARY_PATH=.libs timeout 60 \
    ./test/property/.libs/prop_gc --threads=16 --rounds=800 >/tmp/asan.$i 2>&1
  rc=$?
  if grep -q "STW SOUNDNESS BUG" /tmp/asan.$i; then
    echo "=== FAILED on iteration $i (rc=$rc) ==="
    tail -12 /tmp/asan.$i
    exit 0
  fi
done
echo "no failure in 40 runs"

#!/usr/bin/env bash
# Loop prop_gc until the STW soundness bug reproduces, then dump details.
set -u
for i in $(seq 1 40); do
  LD_LIBRARY_PATH=.libs timeout 60 ./test/property/.libs/prop_gc --threads=16 --rounds=800 >/tmp/plain.$i 2>&1
  rc=$?
  if grep -q "STW SOUNDNESS BUG" /tmp/plain.$i; then
    echo "=== FAILED on iteration $i (rc=$rc) ==="
    tail -10 /tmp/plain.$i
    exit 0
  fi
done
echo "no failure in 40 runs"

#!/usr/bin/env bash
# r9f3 gate 4 (soundness) from a committed-ref clone of r9f3 tip (3c5a247).
# OFF: p85b_fix_lo_gate.sh 30 (make check 0 FAIL, 8t oracle x4, resize-leak,
#      the rseq 6-bug suite, drain/commit tests). ON: re-run make check, the
#      two drain tests, resize-leak, and the 8t oracle matrix with
#      UMEM_OPTIONS=owner_recycle=1 to prove the feature is sound when enabled.
set -u
POST=3c5a247
BASE=~/libumem-g4-r9f3b
rm -rf "$BASE"; mkdir -p "$BASE"; cd "$BASE"
git clone --quiet https://codeberg.org/gregburd/libumem.git repo 2>/dev/null || \
  git clone --quiet ssh://codeberg.org/gregburd/libumem.git repo
cd repo && git fetch --quiet origin && git checkout --quiet "$POST"
sudo sysctl -w vm.max_map_count=65530 >/dev/null 2>&1

echo "########## GATE 4 OFF: p85b_fix_lo_gate.sh 30 ##########"
./scripts/ec2/p85b_fix_lo_gate.sh 30; LO_RC=$?
echo "P85B_LO_GATE_RC=$LO_RC"

echo "########## GATE 4 ON: UMEM_OPTIONS=owner_recycle=1 ##########"
export LD_LIBRARY_PATH="$PWD/.libs"
S=./test/stress/.libs; I=./test/integration/.libs
rc=0
gate() { local n="$1"; shift; if UMEM_OPTIONS=owner_recycle=1 "$@" >"on-$n.log" 2>&1; then echo "GATE_ON $n PASS"; else echo "GATE_ON $n FAIL rc=$?"; tail -20 "on-$n.log"; rc=1; fi; }
UMEM_OPTIONS=owner_recycle=1 make -j"$(nproc)" check >on-make_check.log 2>&1 && echo "GATE_ON make_check PASS" || { echo "GATE_ON make_check FAIL"; rc=1; }
grep -E "^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)" test-suite.log 2>/dev/null
grep -E "^(FAIL|ERROR):" on-make_check.log | head
gate fork_drain    "$I/test_fork_ptc_drain"
gate exit_drain    "$I/test_ptc_thread_exit_drain"
gate resize_leak   "$S/repro_rseq_resize_leak"
gate oracle_small  "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=small --pattern=multi
gate oracle_mag    "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=mag   --pattern=multi
gate oracle_mixed  "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=mixed --pattern=all
gate oracle_large  "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=large --pattern=churn
echo "GATE4_ON_RC=$rc"
echo "GATE4_SUMMARY: lo_gate_off_rc=$LO_RC on_rc=$rc"

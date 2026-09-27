#!/usr/bin/env bash
# scripts/ec2/p85b_fix_lo_gate.sh -- the P8.5b-fix lo gate, run from the root
# of an isolated tree (verify-isolated.sh).  Prints one GATE line per check.
#   make check (vm.max_map_count=65530, as the P8.5b gates), the 6-bug rseq
#   suite, repro_rseq_trailing_store, the ported commit tests, N runs of
#   repro_reload_commit_abort, the resize-leak test, the e2e alias repro, and
#   the 8t/10s mixed/all oracle x 4 matrix.
# Exit 0 all PASS, 1 any FAIL, 77 build failed.
set -u
N_ABORT="${1:-200}"
sudo sysctl -w vm.max_map_count=65530 >/dev/null 2>&1
./scripts/ec2/clean-regen.sh --enable-rseq CFLAGS="-O2 -g" >regen.log 2>&1 \
	|| { echo "SKIP: regen"; tail -20 regen.log; exit 77; }
make -j"$(nproc)" >make.log 2>&1 || { echo "SKIP: build"; tail -30 make.log; exit 77; }
export LD_LIBRARY_PATH="$PWD/.libs"
S=./test/stress/.libs
rc=0
gate() { # name, then command
	local n="$1"; shift
	if "$@" >"gate-$n.log" 2>&1; then echo "GATE $n PASS"
	else echo "GATE $n FAIL rc=$?"; tail -15 "gate-$n.log"; rc=1; fi
}
gate make_check make -j"$(nproc)" check
grep -E "^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)" test-suite.log
grep -E "^(FAIL|ERROR|SKIP):" gate-make_check.log | head -20
gate rseq_fastpath_6bug ./test/unit/.libs/test_rseq_fastpath
gate trailing_store "$S/repro_rseq_trailing_store"
gate commit_safe "$S/repro_reload_commit_safe"
gate resize_leak_16 "$S/repro_rseq_resize_leak" 16
gate resize_leak_32 "$S/repro_rseq_resize_leak" 32
gate alias_e2e "$S/repro_rseq_reload_alias" 30
f=0
for i in $(seq 1 "$N_ABORT"); do
	"$S/repro_reload_commit_abort" 2000000 5 >abort.log 2>&1 || { f=$((f+1)); cat abort.log; }
done
echo "GATE commit_abort $([ $f -eq 0 ] && echo PASS || echo FAIL) fails=$f/$N_ABORT"
[ "$f" -eq 0 ] || rc=1
gate oracle_small  "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=small --pattern=multi
gate oracle_mag    "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=mag --pattern=multi
gate oracle_mixed  "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=mixed --pattern=all
gate oracle_large  "$S/stress_concurrency_oracle" --threads=8 --duration=10 --size-class=large --pattern=churn
echo "LO_GATE rc=$rc"
exit $rc

#!/usr/bin/env bash
# scripts/ec2/r6arm_gate.sh <mode> -- r6arm lo gate, isolated tree.
#   mode=regress  build, run only test_first_alloc_small_stack.sh
#   mode=full     build, make check (vm.max_map_count=65530), the regression
#                 log, and the first-allocation depth/survival re-measure
set -u
MODE="${1:-full}"
sudo sysctl -w vm.max_map_count=65530 >/dev/null 2>&1
./scripts/ec2/clean-regen.sh --enable-rseq CFLAGS="-O2 -g" >regen.log 2>&1 \
	|| { echo "SKIP: regen"; tail -20 regen.log; exit 77; }
make -j"$(nproc)" >make.log 2>&1 || { echo "SKIP: build"; tail -30 make.log; exit 77; }
cat ISOLATED_PROVENANCE; uname -rm; gcc --version | head -1; ldd --version | head -1
rc=0
if [ "$MODE" = regress ]; then
	make check TESTS=test/integration/test_first_alloc_small_stack.sh >chk.log 2>&1 || rc=1
	grep -E "^# (TOTAL|PASS|FAIL)|^(PASS|FAIL|ERROR):" chk.log
	cat test/integration/test_first_alloc_small_stack.log
	exit $rc
fi
make -j"$(nproc)" check >chk.log 2>&1 || rc=1
echo "make-check rc=$rc"
grep -E "^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)" test-suite.log
grep -E "^(FAIL|ERROR|SKIP|XFAIL):" chk.log
echo "== regression log =="; cat test/integration/test_first_alloc_small_stack.log
L="$PWD/.libs"; export LD_LIBRARY_PATH="$L"
gcc -O2 -g -o probe test/integration/probe_first_alloc_stack.c -I. -L"$L" -lumem -ldl -pthread || exit 1
echo "== first-call depth, default thread =="
./probe umem 0 m | tail -1
UMEM_OPTIONS=bogus_item ./probe umem 0 m 2>&1 | tail -1
./probe malloc 0 m | tail -1
echo "== survival with N bytes of stack left =="
for n in 4096 5120 6144 8192; do
	printf '%s=' $n; PROBE_LEAVE=$n ./probe umem 0 >/dev/null 2>&1; r1=$?
	PROBE_LEAVE=$n UMEM_OPTIONS=bogus_item ./probe umem 0 >/dev/null 2>&1; printf 'umem:%s,umem-badopt:%s ' $r1 $?
done; echo
for k in 16 32 64; do printf 'umem %sK=' $k; ./probe umem $k >/dev/null 2>&1; echo -n "$? "; done; echo
exit $rc

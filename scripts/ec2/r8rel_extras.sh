#!/usr/bin/env bash
# scripts/ec2/r8rel_extras.sh -- r8rel qualification steps 2,3,5 in one job.
# Run from the root of an isolated 744f639 tree (verify-isolated.sh).
# Arg $1 = arch tag: "x86" or "arm" (controls the x86-only frame-pointers substep).
set -u
ARCH="${1:?arch: x86|arm}"
J=$(nproc)
counts() { grep -E "^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)" test-suite.log 2>/dev/null; }
regr() { # name: report PASS/SKIP/FAIL/XFAIL for a specific test in test-suite.log
  local t="$1"
  grep -E "^(PASS|SKIP|FAIL|XFAIL|XPASS|ERROR): .*${t}( |$)" test-suite.log 2>/dev/null | head -3
}

echo "############ STEP 2: --enable-introspect ############"
./scripts/ec2/clean-regen.sh --enable-introspect CFLAGS="-O2 -g" >regen2.log 2>&1 || { echo "REGEN2_FAIL"; tail -20 regen2.log; }
make -j$J >make2.log 2>&1; echo "build2_rc=$?"
make -j$J check >check2.log 2>&1; echo "check2_rc=$?"
echo "-- introspect counts --"; counts
echo "-- FAIL/ERROR lines --"; grep -E "^(FAIL|ERROR|XPASS):" test-suite.log | head
echo "-- T1 --"; regr test_introspect_g1; regr "T1"; grep -E "^(PASS|SKIP|FAIL): .*(introspect_g1|_t1_|_T1_)" test-suite.log
echo "-- T8 --"; grep -E "^(PASS|SKIP|FAIL): .*(introspect_g8|_t8_|_T8_)" test-suite.log
echo "-- T9 --"; grep -E "^(PASS|SKIP|FAIL): .*(introspect_g9|leak_stack|_t9_|_T9_)" test-suite.log
echo "-- all introspect_g tests --"; grep -E "^(PASS|SKIP|FAIL|XFAIL): .*introspect" test-suite.log

if [ "$ARCH" = "x86" ]; then
  echo "############ STEP 2b: x86 --enable-introspect --enable-frame-pointers (T9 must PASS) ############"
  ./scripts/ec2/clean-regen.sh --enable-introspect --enable-frame-pointers CFLAGS="-O2 -g" >regen2b.log 2>&1 || { echo "REGEN2b_FAIL"; tail -20 regen2b.log; }
  make -j$J >make2b.log 2>&1; echo "build2b_rc=$?"
  make -j$J check >check2b.log 2>&1; echo "check2b_rc=$?"
  echo "-- fp counts --"; counts
  echo "-- T9 (fp) --"; grep -E "^(PASS|SKIP|FAIL): .*(introspect_g9|leak_stack|_t9_|_T9_)" test-suite.log
  echo "-- all introspect_g (fp) --"; grep -E "^(PASS|SKIP|FAIL|XFAIL): .*introspect" test-suite.log
fi

echo "############ STEP 3: --disable-rseq ############"
./scripts/ec2/clean-regen.sh --disable-rseq CFLAGS="-O2 -g" >regen3.log 2>&1 || { echo "REGEN3_FAIL"; tail -20 regen3.log; }
make -j$J >make3.log 2>&1; echo "build3_rc=$?"
sudo sysctl -w vm.max_map_count=65530 >/dev/null 2>&1
make -j$J check >check3.log 2>&1; echo "check3_rc=$?"
echo "-- norseq counts --"; counts
echo "-- FAIL/ERROR --"; grep -E "^(FAIL|ERROR|XPASS):" test-suite.log | head

echo "############ STEP 5: round-8 regressions (default build) ############"
./scripts/ec2/clean-regen.sh CFLAGS="-O2 -g" >regen5.log 2>&1 || { echo "REGEN5_FAIL"; tail -20 regen5.log; }
make -j$J >make5.log 2>&1; echo "build5_rc=$?"
sudo sysctl -w vm.max_map_count=65530 >/dev/null 2>&1
make -j$J check >check5.log 2>&1; echo "check5_rc=$?"
echo "-- default counts --"; counts
echo "== round-8 regression outcomes =="
for t in test_ptc_exit_full_mag test_cpu_hint_spread test_slab_interior_free test_findleaks_rseq test_first_alloc_small_stack test_ptc_freeonly_trim; do
  echo "  [$t]"; regr "$t"
done
echo "############ EXTRAS DONE ############"

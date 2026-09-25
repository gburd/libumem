#!/bin/bash
# scripts/ec2/exit_criteria_gate.sh -- the readiness plan's exit criteria, in one
# runnable check.  Run it through verify-isolated.sh so it tests committed
# content rather than whatever is in the worktree:
#
#   ./scripts/ec2/verify-isolated.sh intel-lo@w HEAD gate 3600 \
#       'bash scripts/ec2/exit_criteria_gate.sh'
#
# Covers: default build, make check, the comprehensive unit suite, every
# property test, the concurrency oracle, a clean `make dist` tarball that must
# pass its OWN make check, and `make install` into a temp prefix with an
# external consumer compiled against only the installed headers.
#
# NOTE: every $? is captured IMMEDIATELY. An intervening echo/printf overwrites
# it -- that is how an aborting prop_fragmentation (rc=134) was first reported
# here as rc=0, the same defect class as oracle_matrix.sh's `echo "exit=$?"`.
# Exit status is the number of gate failures, so it is usable as a gate.
r() { printf '\n=== %s ===\n' "$*"; }
fails=0
note_fail() { fails=$((fails+1)); echo "GATE-FAIL: $*"; }

r "1. default build + make check"
./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && make -j$(nproc) >/dev/null 2>&1
rc=$?; echo "build_rc=$rc"; [ $rc -ne 0 ] && note_fail "build"
make check >/tmp/ck.log 2>&1; rc=$?; echo "check_rc=$rc"
grep -E '^# (TOTAL|PASS|FAIL|SKIP|ERROR)' /tmp/ck.log
grep -E '^(FAIL|ERROR):' /tmp/ck.log | head -5
[ $rc -ne 0 ] && note_fail "make check"

r "2. comprehensive unit suite"
LD_LIBRARY_PATH=.libs ./test/.libs/test_main --no-fork >/tmp/tm.log 2>&1
rc=$?; echo "test_main_rc=$rc"; tail -2 /tmp/tm.log | head -2
[ $rc -ne 0 ] && note_fail "test_main"

r "3. property tests (each rc captured immediately)"
for p in test/property/.libs/prop_alloc_free2 test/property/.libs/prop_cache test/property/.libs/prop_fragmentation test/property/.libs/prop_ownership; do
  [ -x "$p" ] || continue
  LD_LIBRARY_PATH=.libs timeout 180 "$p" >/tmp/p.log 2>&1
  rc=$?
  echo "$(basename $p) rc=$rc"
  # No allowances here.  Every property test must pass; if one starts failing,
  # that is a gate failure, not a footnote.  (prop_fragmentation used to be
  # exempted for a vmem abort -- that was fixed, so the exemption is gone;
  # docs/results/2026-09-22-prop-fragmentation-vmem-abort.md has the record.)
  [ $rc -ne 0 ] && note_fail "$(basename $p) rc=$rc"
done

r "4. concurrency oracle"
LD_LIBRARY_PATH=.libs timeout 200 ./test/stress/.libs/stress_concurrency_oracle --threads=8 --duration=25 --size-class=mixed --pattern=all >/tmp/or.log 2>&1
rc=$?; tail -2 /tmp/or.log; echo "oracle_rc=$rc"; [ $rc -ne 0 ] && note_fail "oracle"

r "5. clean source tarball"
make dist >/tmp/dist.log 2>&1; rc=$?; echo "dist_rc=$rc"; [ $rc -ne 0 ] && note_fail "make dist"
TB=$(ls -t umem-*.tar.* 2>/dev/null | head -1); echo "tarball=$TB"
if [ -n "$TB" ]; then
  rm -rf /tmp/tb && mkdir -p /tmp/tb && tar -xf "$TB" -C /tmp/tb
  D=$(ls -d /tmp/tb/*/ | head -1)
  ( cd "$D" && ./configure >/tmp/tbc.log 2>&1 && make -j$(nproc) >/tmp/tbm.log 2>&1 && make check >/tmp/tbk.log 2>&1 )
  rc=$?; echo "tarball_rc=$rc"; grep -E '^# (TOTAL|PASS|FAIL)' /tmp/tbk.log 2>/dev/null
  grep -E '^FAIL:' /tmp/tbk.log 2>/dev/null | head -3
  [ $rc -ne 0 ] && note_fail "tarball build/check"
fi

r "6. installed prefix"
rm -rf /tmp/pfx && make install DESTDIR=/tmp/pfx >/tmp/inst.log 2>&1
rc=$?; echo "install_rc=$rc"; [ $rc -ne 0 ] && note_fail "make install"
printf '#include <umem.h>\nint main(void){ void *p = umem_alloc(64, UMEM_DEFAULT); if(!p) return 1; umem_free(p,64); return 0; }\n' > /tmp/ext.c
INC=$(find /tmp/pfx -name umem.h -printf '%h\n' | head -1)
LIB=$(find /tmp/pfx -name 'libumem.so' -printf '%h\n' | head -1)
gcc /tmp/ext.c -I"$INC" -L"$LIB" -lumem -o /tmp/ext 2>/tmp/ext.err
rc=$?; echo "external_compile_rc=$rc"; [ $rc -ne 0 ] && { note_fail "external compile"; head -3 /tmp/ext.err; }
LD_LIBRARY_PATH="$LIB" /tmp/ext; rc=$?; echo "external_run_rc=$rc"; [ $rc -ne 0 ] && note_fail "external run"
rm -rf -f /tmp/ext /tmp/ext.c 2>/dev/null

r "7. RELEASE configuration: -O3 -DNDEBUG, the binary that actually ships"
# .forgejo/workflows/release.yml builds the tarball with CFLAGS='-O3 -g -DNDEBUG'
# and ran NO tests on it.  Under NDEBUG every ASSERT() is compiled out
# (misc.h:138), so that binary is a different program from the one every
# regression above ran against -- and it had never been through any of them
# (production-readiness review 2026-09-24, section 3.1).  Steps 1-4 again on
# exactly that configuration.  A failure here that steps 1-4 did not show is
# an ASSERT that was load-bearing.
./scripts/ec2/clean-regen.sh CFLAGS='-O3 -g -DNDEBUG' >/dev/null 2>&1 && make -j$(nproc) >/dev/null 2>&1
rc=$?; echo "ndebug_build_rc=$rc"; [ $rc -ne 0 ] && note_fail "ndebug build"
grep -q 'CFLAGS.*NDEBUG' Makefile || note_fail "ndebug configure did not take (Makefile CFLAGS lacks NDEBUG)"
make check >/tmp/ck2.log 2>&1; rc=$?; echo "ndebug_check_rc=$rc"
grep -E '^# (TOTAL|PASS|FAIL|SKIP|ERROR)' /tmp/ck2.log
grep -E '^(FAIL|ERROR):' /tmp/ck2.log | head -5
[ $rc -ne 0 ] && note_fail "ndebug make check"
LD_LIBRARY_PATH=.libs ./test/.libs/test_main --no-fork >/tmp/tm2.log 2>&1
rc=$?; echo "ndebug_test_main_rc=$rc"; tail -2 /tmp/tm2.log | head -2
[ $rc -ne 0 ] && note_fail "ndebug test_main"
LD_LIBRARY_PATH=.libs timeout 200 ./test/stress/.libs/stress_concurrency_oracle --threads=8 --duration=25 --size-class=mixed --pattern=all >/tmp/or2.log 2>&1
rc=$?; tail -1 /tmp/or2.log; echo "ndebug_oracle_rc=$rc"; [ $rc -ne 0 ] && note_fail "ndebug oracle"

r "GATE RESULT"
echo "gate_failures=$fails"
[ $fails -eq 0 ] && echo "GATE: PASS" || echo "GATE: FAIL"
exit $fails

#!/usr/bin/env bash
#
# The `abort` option must exist and must work.
#
# THE DEFECT: malloc_interpose.c and README.md told users that
# `UMEM_OPTIONS=abort=1` restores glibc-like abort-on-invalid-free under
# LD_PRELOAD, where the interposer clears umem_abort.  No such option existed:
# the tables had only `noabort` (ITEM_CLEARFLAG, and in the UMEM_DEBUG table,
# not UMEM_OPTIONS), nothing could SET the flag, and `abort=1` would have been
# rejected by the flag parser for carrying a value anyway.  The first attempt
# at this fix put `abort` next to `noabort` -- in UMEM_DEBUG -- and this test
# caught it: UMEM_OPTIONS=abort parsed nothing and the arm still exited 0.  So the one documented way to make a preloaded program fail
# loudly on a forged or foreign free did nothing, silently, and the program
# went on logging where the user believed it would die.
#
# WHAT THIS TESTS, both arms against the SAME input (test_forged_free, which
# hands the interposer's free() a forged MALLOC_MAGIC header on the stack):
#
#   default          -> process_free refuses, logs, and the test completes
#                       (exit 0 or 77 -- its own verdict, not ours)
#   UMEM_OPTIONS=abort -> the same refusal now aborts: killed by SIGABRT
#                       (exit 134) before the test can finish
#
# The default arm is the control: it proves the forgery is reached and refused.
# Without it, an abort arm that died for an unrelated reason would "pass".
#
# ASan intercepts free() ahead of the interposer, so under --enable-asan the
# forgery never reaches process_free and neither arm means anything: SKIP.
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/test_forged_free"
[[ -x $bin ]] || { echo "SKIP: test_forged_free not built"; exit 77; }

if ldd "$bin" 2>/dev/null | grep -q libasan; then
	echo "SKIP: ASan build; free() is intercepted before the interposer"
	exit 77
fi

echo "[control] default: forged free must be refused and the run must complete"
out=$("$bin" 2>&1); rc=$?
if [[ $rc -ne 0 && $rc -ne 77 ]]; then
	echo "FAIL: control arm exited $rc; cannot attribute the abort arm"
	echo "$out" | tail -5
	exit 1
fi
if ! grep -q "refused\|not a libumem allocation" <<<"$out"; then
	echo "FAIL: control arm did not report a refused forgery -- the input is not reaching process_free"
	echo "$out" | tail -5
	exit 1
fi
echo "  ok: refused, logged, completed (rc=$rc)"

echo "[test] UMEM_OPTIONS=abort: the same refusal must abort the process"
UMEM_OPTIONS=abort "$bin" >/dev/null 2>&1; rc=$?
if [[ $rc -eq 134 ]] || (( rc > 128 && rc - 128 == 6 )); then
	echo "  ok: SIGABRT (rc=$rc)"
	echo "RESULT: PASS (abort option exists and arms umem_abort under the interposer)"
	exit 0
fi
echo "FAIL: with UMEM_OPTIONS=abort the process exited $rc, not SIGABRT -- the option is not armed"
exit 1

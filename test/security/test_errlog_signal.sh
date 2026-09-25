#!/usr/bin/env bash
# The error log must not deadlock against a signal handler that frees a bad
# pointer.  Pre-fix the helper hangs; this wrapper bounds it.
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/test_errlog_signal"
[[ -x $bin ]] || bin="$here/.libs/test_errlog_signal"
[[ -x $bin ]] || { echo "SKIP: helper not built"; exit 77; }
timeout 10 "$bin"; rc=$?
if [[ $rc -eq 124 ]]; then
	echo "RESULT: FAIL (helper hung for 10 s: a signal handler's free() deadlocked on the error-log lock)"
	exit 1
fi
exit $rc

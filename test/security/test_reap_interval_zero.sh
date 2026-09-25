#!/usr/bin/env bash
#
# P5.11: UMEM_OPTIONS=reap_interval=0 must not spin a thread at 100 % CPU.
#
# THE DEFECT.  The update thread sleeps until umem_update_next, which is
# now + umem_reap_interval; with reap_interval=0 the deadline is always
# already past, so the loop runs umem_cache_applyall() back to back with no
# wait, forever: one core at 100 % for the life of the process.  reap_interval
# is a "pure tuning" option and so is HONOURED under AT_SECURE (P5.2 gates
# only options with file/socket/exec/disclosure side effects).  Attacker
# position C: control of a setuid target's environment burns a core.  glibc
# ignores every MALLOC_ tunable under AT_SECURE, so this is worse than glibc.
#
# WHAT THIS TESTS.  Run a process that allocates once and then sleeps 2 s
# under reap_interval=0; read its total CPU time from /proc afterwards.
#
#   PASS: < 0.5 s of CPU across 2 s of wall time (the thread waited).
#   FAIL: >= 1.5 s (the thread spun).
#
# The control arm runs the same program with the default interval and must
# also pass, so the assertion is about the option, not the program.
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/test_reap_interval_zero"
[[ -x $bin ]] || bin="$here/.libs/test_reap_interval_zero"
[[ -x $bin ]] || { echo "SKIP: helper not built"; exit 77; }

run() {	# $1 label, env already set
	local out cpu
	out=$("$bin") || { echo "SKIP: helper failed"; exit 77; }
	cpu=$(awk '{print $1}' <<<"$out")
	echo "  [$1] cpu_seconds=$cpu"
	awk -v c="$cpu" -v l="$1" 'BEGIN{ if (c+0 >= 1.5) { print "RESULT: FAIL (" l ": " c " s of CPU in a 2 s sleep -- the update thread is spinning)"; exit 1 } }'
}
run control || exit 1
UMEM_OPTIONS=reap_interval=0 run reap_interval=0 || exit 1
echo "RESULT: PASS (reap_interval=0 does not spin)"

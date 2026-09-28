#!/bin/bash
# cpuhint_runs.sh <tag> [old.c]: run the built test_cpu_hint_spread in cwd 20x
# idle and 20x at +64 busy loops.  With old.c (the pre-r8hint unpinned test),
# build it against the same libumem and run it under the same two loads.
# The busy loops are this script's children; job.sh kills the group on timeout.
cat ISOLATED_PROVENANCE; nproc; grep PRETTY_NAME /etc/os-release; uname -m
export LD_LIBRARY_PATH=.libs
T=test/integration/.libs/test_cpu_hint_spread
if [ -n "${2:-}" ]; then
	gcc -DHAVE_CONFIG_H -I. -std=c17 -D_GNU_SOURCE -O2 -pthread "$2" \
	    .libs/libumem.so -o /tmp/oldch && O=/tmp/oldch || echo "old test build failed"
fi
run() { # tag load binary
	local p=0 f=0 s=0 rc i
	for i in $(seq 20); do
		"$3" >/tmp/ch.$$ 2>&1; rc=$?
		case $rc in 0) p=$((p+1));; 77) s=$((s+1));; *) f=$((f+1));; esac
		echo "  rc=$rc $(grep -E '^(cc_alloc|distinct)' /tmp/ch.$$ | tr '\n' ' ')"
	done
	[ -n "$S" ] || { S=1; cat /tmp/ch.$$; }
	echo "SUMMARY $1 $2 $(basename "$3"): PASS $p FAIL $f SKIP $s /20"
}
S=
for load in idle +64busy; do
	[ $load = +64busy ] && { for i in $(seq 64); do ( while :; do :; done ) & done; sleep 1; }
	run "$1" $load $T
	[ -n "${O:-}" ] && run "$1" $load $O
done
kill $(jobs -p) 2>/dev/null; wait 2>/dev/null
echo done

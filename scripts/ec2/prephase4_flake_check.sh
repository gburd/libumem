#!/usr/bin/env bash
# Is the --disable-rseq 12/12 test_inspect_e2e.sh failure pre-existing, or did
# this workstream cause it?  Copy the working tree aside, restore the
# pre-Phase-4 Makefile.am and umem_rseq.h into the copy, build it the same
# way, and run the same script.
#
# At 5c7fd76 --disable-rseq did not build at all (the rseq repros were
# ungated -- a separate pre-existing bug), so build only the targets the test
# needs, which is enough to measure the assertion.
set -u
D=/tmp/prebase.$$
mkdir -p "$D"
tar -cf - --exclude .git --exclude '*.o' --exclude '*.lo' --exclude .libs . \
	| (cd "$D" && tar xf -)
cd "$D" || exit 1
cp tmp/prefix-control/old_Makefile.am Makefile.am
cp tmp/prefix-control/old_umem_rseq.h  umem_rseq.h
cp tmp/prefix-control/old_configure.ac configure.ac
if ! ./scripts/ec2/clean-regen.sh --disable-rseq >/tmp/pre-cfg.log 2>&1; then
	echo PRE_CONFIG_FAIL; tail -12 /tmp/pre-cfg.log; exit 1
fi
if ! make -j"$(nproc)" libumem.la tools/umem test/test_inspect_live \
	>/tmp/pre-make.log 2>&1; then
	echo PRE_MAKE_FAIL; grep -m6 'error:' /tmp/pre-make.log; exit 1
fi
f=0; n=8
for i in $(seq 1 $n); do
	if ! ./test/debugger/test_inspect_e2e.sh >/tmp/pre-run.log 2>&1; then
		f=$((f+1))
		echo "PRE-PHASE4 run $i FAIL: $(grep -o 'expected 2 cached, got [0-9]*' /tmp/pre-run.log | head -1)"
	fi
done
echo "== PRE-PHASE-4 tree, --disable-rseq: $f/$n failures"

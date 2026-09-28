#!/bin/bash
# bench_main -a <X> for an allocator that did not load must exit NONZERO.
# It used to exit 0 with no rows, so a comparison silently lost jemalloc when
# its dlopen hit the static-TLS limit (round 7, docs/results/r7samp/).
# Exit: 0 fixed, 1 finding reproduces, 99 harness broken.
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/.libs/bench_main"; [[ -x $bin ]] || bin="$here/bench_main"
[[ -x $bin ]] || { echo "ERROR: bench_main not built"; exit 99; }
export LD_LIBRARY_PATH="$here/../../.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Positive control: umem is always linked in and must produce a row, rc 0.
out=$("$bin" -a umem -w single -n 1000 -s 16:16 -c 2>/dev/null); rc=$?
if [[ $rc -ne 0 ]] || ! grep -q '^umem,' <<<"$out"; then
	echo "ERROR: control -a umem rc=$rc, no row"; exit 99
fi
# umem-preload is available only when libumem_malloc.so is LD_PRELOADed, so
# without it the allocator is deterministically unavailable here, whatever
# else the box has installed.  An unknown name must fail too.
env -u LD_PRELOAD "$bin" -a umem-preload -w single -n 1000 -s 16:16 -c \
	>/dev/null 2>&1; rc1=$?
"$bin" -a no-such-allocator -w single -n 1000 -s 16:16 -c >/dev/null 2>&1; rc2=$?
echo "unavailable umem-preload rc=$rc1; unknown allocator rc=$rc2"
if [[ $rc1 -eq 0 || $rc2 -eq 0 ]]; then
	echo "RESULT: FAIL (a named allocator that cannot run exits 0 with no rows)"
	exit 1
fi
echo "RESULT: PASS"; exit 0

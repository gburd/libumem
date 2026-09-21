#!/usr/bin/env bash
# Diagnose the two failures from verify_phase4.sh:
#
#   1. --disable-rseq fails to build.  Is that pre-existing, or did the Phase 4
#      umem_rseq.h guard change cause it?  Tested by checking out the file as
#      it was before Phase 4 touched it and rebuilding the same way.
#
#   2. test/debugger/test_inspect_e2e.sh failed under --enable-avx2 but passed
#      in every other configuration.  Is it AVX2-specific or flaky?  Tested by
#      running it repeatedly in BOTH configurations and capturing its output.
set -u
echo "=== 1a. --disable-rseq with the CURRENT umem_rseq.h ==="
./scripts/ec2/clean-regen.sh --disable-rseq >/tmp/r1.log 2>&1 && echo CONFIG_OK
make -j"$(nproc)" >/tmp/m1.log 2>&1 && echo "MAKE_OK (current)" || {
	echo "MAKE_FAIL (current) -- first errors:"
	grep -m8 'error:' /tmp/m1.log
	echo "failing objects:"
	grep -o 'Makefile:[0-9]*: [^ ]*\.o' /tmp/m1.log | sort -u
}

echo
echo "=== 1b. --disable-rseq with the PRE-Phase-4 umem_rseq.h ==="
# tmp/prefix-control/old_umem_rseq.h is staged locally (the remote tree has
# no .git), so this compares the header, nothing else.
if [ -f tmp/prefix-control/old_umem_rseq.h ]; then
	cp umem_rseq.h /tmp/umem_rseq.h.phase4
	cp tmp/prefix-control/old_umem_rseq.h umem_rseq.h
	make -j"$(nproc)" >/tmp/m2.log 2>&1 && echo "MAKE_OK (pre-Phase-4 header)" || {
		echo "MAKE_FAIL (pre-Phase-4 header too) -- first errors:"
		grep -m8 'error:' /tmp/m2.log
	}
	cp /tmp/umem_rseq.h.phase4 umem_rseq.h
else
	echo "SKIP: tmp/prefix-control/old_umem_rseq.h not staged"
fi

echo
echo "=== 2. test_inspect_e2e.sh: AVX2-specific or flaky? ==="
for cfg in "" "--enable-avx2"; do
	label="${cfg:-default}"
	echo "--- configuration: $label"
	./scripts/ec2/clean-regen.sh $cfg >/dev/null 2>&1
	make -j"$(nproc)" >/dev/null 2>&1
	for i in 1 2 3 4 5; do
		out="$(./test/debugger/test_inspect_e2e.sh 2>&1)"; rc=$?
		echo "  run $i: rc=$rc"
		if [ $rc -ne 0 ]; then
			echo "  --- failing output (run $i, $label) ---"
			echo "$out" | tail -30
			echo "  --- end ---"
		fi
	done
done

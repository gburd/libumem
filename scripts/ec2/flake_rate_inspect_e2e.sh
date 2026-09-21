#!/usr/bin/env bash
# Measure test/debugger/test_inspect_e2e.sh's failure rate in --disable-rseq
# vs default, to establish whether the flake is rseq-off-specific (which
# would make it a real regression risk) or configuration-independent.
set -u
for cfg in "--disable-rseq" ""; do
	label="${cfg:-default}"
	./scripts/ec2/clean-regen.sh $cfg >/dev/null 2>&1
	make -j"$(nproc)" >/dev/null 2>&1
	f=0; n=12
	for i in $(seq 1 $n); do
		out="$(./test/debugger/test_inspect_e2e.sh 2>&1)"
		if [ $? -ne 0 ]; then
			f=$((f+1))
			echo "$label run $i FAIL: $(echo "$out" | grep -o 'expected 2 cached, got [0-9]*' | head -1)"
		fi
	done
	echo "== $label: $f/$n failures"
done

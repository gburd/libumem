#!/usr/bin/env bash
# scripts/ec2/p85b_fix_metal_gate.sh <mode> -- P8.5b-fix metal gate, run from
# the root of an isolated tree (verify-isolated.sh) on a Debian metal box.
#   mode=default  build --enable-rseq -O2 -g; 192t/60s mixed/all oracle; then
#                 sustained_load.sh umem 180 192 with rseq ON and with
#                 UMEM_OPTIONS=norseq (same binary), 3 windows + 1 warmup.
#   mode=asan     build --enable-rseq --enable-asan -O1 -g; 192t/60s mixed/all
#                 oracle under ASan (vm.max_map_count raised).
# Prints GATE lines.  Exit 0 PASS, 1 FAIL, 77 build failed.
set -u
MODE="${1:?default|asan}"
sudo sysctl -w vm.max_map_count=1966080 >/dev/null 2>&1
if [ "$MODE" = asan ]; then CONF="--enable-rseq --enable-asan"; CF="-O1 -g"
else CONF="--enable-rseq"; CF="-O2 -g"; fi
# shellcheck disable=SC2086
./scripts/ec2/clean-regen.sh $CONF CFLAGS="$CF" >regen.log 2>&1 \
	|| { echo "SKIP: regen"; tail -20 regen.log; exit 77; }
make -j"$(nproc)" >make.log 2>&1 || { echo "SKIP: build"; tail -30 make.log; exit 77; }
export LD_LIBRARY_PATH="$PWD/.libs"
cat ISOLATED_PROVENANCE 2>/dev/null; uname -r; nproc; gcc --version | head -1
rc=0
O=test/stress/.libs/stress_concurrency_oracle
if [ "$MODE" = asan ]; then
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:allocator_may_return_null=0 \
	    "$O" --threads=192 --duration=60 --size-class=mixed --pattern=all >oracle.log 2>&1
else
	"$O" --threads=192 --duration=60 --size-class=mixed --pattern=all >oracle.log 2>&1
fi
s=$?
tail -8 oracle.log
grep -q "ERROR: AddressSanitizer" oracle.log && s=99
echo "GATE oracle_192t_60s_$MODE $([ $s -eq 0 ] && echo PASS || echo FAIL) rc=$s"
[ $s -eq 0 ] || rc=1
if [ "$MODE" = default ]; then
	export SUSTAINED_WINDOWS=3 SUSTAINED_WARMUPS=1 SUSTAINED_FRAG_SIZES=64:256
	for arm in on off; do
		echo "=== sustained rseq $arm ==="
		if [ $arm = off ]; then export UMEM_OPTIONS=norseq; else unset UMEM_OPTIONS; fi
		SUSTAINED_OUT=sustained-rseq-$arm.toml ./scripts/ec2/sustained_load.sh umem 180 192 2>&1 \
		    | grep -E "prodcons-sustained|frag-sustained|CRASH|ALLOC_FAIL|done:"
	done
	unset UMEM_OPTIONS
fi
echo "METAL_GATE mode=$MODE rc=$rc"
exit $rc

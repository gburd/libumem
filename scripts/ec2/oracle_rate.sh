#!/usr/bin/env bash
# scripts/ec2/oracle_rate.sh <logdir> <runs> <threads> <duration_s>
#
# Failure-RATE measurement for stress_concurrency_oracle (run on EC2 via
# job.sh / verify-isolated.sh, from the root of an isolated source tree).
# Builds the tree once with the same flags as the P8.5b gates
# (--enable-rseq CFLAGS="-O2 -g"), then runs the oracle <runs> times.
#
# Every failing run keeps its COMPLETE stdout+stderr in <logdir>/fail-<i>.log
# (the ORACLE FAILURE block: stage, addr, size, offset, owner tid+seq).  An
# earlier hammer loop kept only the "Result:" line and so discarded the only
# diagnostic a 1/20 failure produced.  Passing runs keep their Result line.
#
# Env is passed through, so UMEM_OPTIONS=norseq selects the rseq-off arm.
# Exit: 0 all PASS, 1 any FAIL, 77 build failed (nothing measured).
set -u
LOG="${1:?logdir}"; N="${2:-100}"; T="${3:-8}"; D="${4:-10}"
mkdir -p "$LOG"
./scripts/ec2/clean-regen.sh --enable-rseq CFLAGS="-O2 -g" >"$LOG/regen.log" 2>&1 \
	|| { echo "SKIP: regen failed"; tail -20 "$LOG/regen.log"; exit 77; }
make -j"$(nproc)" test/stress/stress_concurrency_oracle >"$LOG/make.log" 2>&1 \
	|| { echo "SKIP: build failed"; tail -20 "$LOG/make.log"; exit 77; }
B=test/stress/.libs/stress_concurrency_oracle
export LD_LIBRARY_PATH="$PWD/.libs"
sudo sysctl -w vm.max_map_count=65530 >/dev/null 2>&1
{
	cat ISOLATED_PROVENANCE 2>/dev/null
	echo "UMEM_OPTIONS=${UMEM_OPTIONS:-<unset>} runs=$N threads=$T dur=$D"
	uname -r; nproc; gcc --version | head -1
	grep -E '^#define (UMEM_RSEQ|HAVE_LINUX_RSEQ)' config.h
	sha256sum "$B" .libs/libumem.so.*.*.* 2>/dev/null
	ldd "$B" | grep umem
} >"$LOG/provenance.txt" 2>&1
cat "$LOG/provenance.txt"
fail=0
for i in $(seq 1 "$N"); do
	out="$($B --threads="$T" --duration="$D" --size-class=mixed --pattern=all 2>&1)"
	rc=$?
	if [ "$rc" -ne 0 ]; then
		fail=$((fail + 1))
		printf '%s\n' "$out" >"$LOG/fail-$i.log"
		echo "run$i FAIL rc=$rc"
		grep -A7 'ORACLE FAILURE' "$LOG/fail-$i.log" | head -9
	else
		echo "run$i PASS"
	fi
	echo "run$i rc=$rc $(printf '%s\n' "$out" | grep 'Result:')" >>"$LOG/summary.txt"
done
echo "FAILS=$fail/$N UMEM_OPTIONS=${UMEM_OPTIONS:-<unset>}"
[ "$fail" -eq 0 ]

#!/usr/bin/env bash
#
# check_budget.sh -- end-to-end check that the operation budget is divided
# EXACTLY ONCE, from the command line through to the reported total (P2.1).
#
# WHY A SEPARATE, END-TO-END CHECK
#   test/bench/test_bench_accounting pins bench_framework's arithmetic by
#   calling the workloads directly.  It cannot catch the defect that was
#   actually published, because that defect was a COMPOSITION: matrix.sh
#   divided by the thread count, and bench_main.c divided the already-divided
#   value again.  Each half looked locally defensible.  Only running the real
#   binary the way the real sweep runs it shows the product.
#
#   Concretely, `bench_main -w multi -t T -n N` must complete ~N operations
#   for every T.  Pre-fix it completed N/T (bench_main's own division), and
#   matrix.sh's extra division made the sweep's effective budget N/T^2.
#
# Exit: 0 pass, 1 fail, 77 SKIP (binary not built -- never 0 for "not run").
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."   # repo root
BENCH="test/bench/.libs/bench_main"
[[ -x $BENCH ]] || BENCH="test/bench/bench_main"
if [[ ! -x $BENCH ]]; then
	echo "SKIP: bench_main not built -- budget check NOT executed"
	exit 77
fi
export LD_LIBRARY_PATH="$(pwd)/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Above 4 x BENCH_MIN_OPS_PER_THREAD so the work floor never fires and the
# comparison is about the division alone.
FLOOR=$(grep -oE 'BENCH_MIN_OPS_PER_THREAD[[:space:]]+[0-9]+' \
	test/bench/bench_framework.h | awk '{print $2}' | head -1)
: "${FLOOR:=100000}"
N=$(( FLOOR * 8 ))

rc=0
# Column 4 (1-based) is total_ops, column 5 is ops_per_thread, column 3 is
# threads -- read them from the header rather than hard-coding, so a future
# column change cannot silently make this check read the wrong field.
HDR=$("$BENCH" -H)
col() { echo "$HDR" | tr ',' '\n' | grep -n "^$1$" | cut -d: -f1; }
C_THREADS=$(col threads); C_TOTAL=$(col total_ops)
C_PER=$(col ops_per_thread); C_FLOOR=$(col ops_floor_raised)
if [[ -z $C_TOTAL || -z $C_PER || -z $C_THREADS || -z $C_FLOOR ]]; then
	echo "FAIL: cannot locate the expected columns in the CSV header:"
	echo "      $HDR"
	exit 1
fi

echo "=== budget check: -n is TOTAL ops, divided exactly once ==="
echo "    n=$N (floor=$FLOOR), workloads: multi, prodcons, frag"

for wl in multi prodcons frag; do
	for t in 1 2 4; do
		row=$("$BENCH" -a libc -w "$wl" -t "$t" -n "$N" -s 64:64 -c 2>/dev/null \
			| grep '^libc,' | tail -1)
		if [[ -z $row ]]; then
			echo "  FAIL: $wl t=$t produced no row"
			rc=1
			continue
		fi
		threads=$(echo "$row" | cut -d, -f"$C_THREADS")
		total=$(echo "$row" | cut -d, -f"$C_TOTAL")
		per=$(echo "$row" | cut -d, -f"$C_PER")
		raised=$(echo "$row" | cut -d, -f"$C_FLOOR")
		printf '  %-9s t=%-2s threads=%-3s total_ops=%-10s ops_per_thread=%-10s floor=%s\n' \
			"$wl" "$t" "$threads" "$total" "$per" "$raised"

		if [[ $raised != 0 ]]; then
			echo "    FAIL: the work floor fired; this check is misconfigured"
			rc=1
			continue
		fi
		# prodcons: only the producer half allocates, so its completed-op
		# count is ~2N (each buffer is allocated then freed).  Bound it
		# loosely but still tightly enough to catch a factor of t.
		lo=$(( N * 80 / 100 )); hi=$(( N * 250 / 100 ))
		if (( total < lo || total > hi )); then
			echo "    FAIL: total_ops=$total outside [$lo,$hi] for a"
			echo "          total budget of $N -- the budget is divided"
			echo "          the wrong number of times (pre-fix: N/t)"
			rc=1
		fi
		if (( threads != t )); then
			echo "    FAIL: reported threads=$threads for -t $t"
			rc=1
		fi
	done
done

echo ""
if [[ $rc -eq 0 ]]; then
	echo "PASS: -n is a total budget at every thread count and workload"
else
	echo "FAIL: budget accounting is wrong"
fi
exit $rc

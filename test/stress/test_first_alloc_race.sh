#!/usr/bin/env bash
#
# Threads that make a process's first allocations concurrently must all be
# served by umem (see test/stress/repro_first_alloc_race.c for the defect).
#
# Runs repro_first_alloc_race under LD_PRELOAD=libumem_malloc.so at 2, 4 and
# 16 threads, malloc and memalign arms, UMEM_FIRST_ALLOC_RUNS times each
# (default 5), every run SIGKILLed after 60 s.  A control run against the
# platform allocator (no preload) must pass first.
#
# On glibc Linux this passes with or without the fix (the C runtime
# allocates on the main thread before main()); the pre-fix failure is
# demonstrated on illumos, where it fails every run.
#
# Exit: 0 pass, 1 the defect reproduces, 77 prerequisite missing, 99 harness.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PRELOAD=""
for c in "$ROOT"/.libs/libumem_malloc.so "$ROOT"/.libs/libumem_malloc.so.*; do
	[[ -f $c ]] && { PRELOAD=$c; break; }
done
[[ -n $PRELOAD ]] || { echo "SKIP: libumem_malloc.so not built"; exit 77; }
BIN=""
for c in "$ROOT"/test/stress/.libs/repro_first_alloc_race \
    "$ROOT"/test/stress/repro_first_alloc_race; do
	[[ -x $c ]] && { BIN=$c; break; }
done
[[ -n $BIN ]] || { echo "SKIP: repro_first_alloc_race not built"; exit 77; }

# illumos runs 64-bit programs with LD_PRELOAD_64; set both.
RUNS=${UMEM_FIRST_ALLOC_RUNS:-5}

# run_bounded <preload|""> <args...>: run once, kill -9 after 60 s (timeout(1)
# has been seen to leave a hung LD_PRELOAD child alive).  Returns the exit
# status, or 124 when killed.
run_bounded() {
	local pre=$1 pid t
	shift
	if [[ -n $pre ]]; then
		LD_PRELOAD="$pre" LD_PRELOAD_64="$pre" "$BIN" "$@" >"$OUT" 2>&1 &
	else
		"$BIN" "$@" >"$OUT" 2>&1 &
	fi
	pid=$!
	for ((t = 0; t < 600; t++)); do
		kill -0 "$pid" 2>/dev/null || { wait "$pid"; return $?; }
		sleep 0.1
	done
	kill -9 "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	return 124
}

OUT=$(mktemp) || exit 99
trap 'rm -f "$OUT"' EXIT

echo "[control] platform allocator, no preload"
for arm in "" --align; do
	run_bounded "" --threads=4 $arm
	rc=$?
	if [[ $rc -ne 0 ]]; then
		echo "HARNESS: control ${arm:-malloc} exited $rc"; cat "$OUT"; exit 99
	fi
done
echo "  ok"

fail=0
echo "[test] LD_PRELOAD=$(basename "$PRELOAD")"
for arm in "" --align; do
	for n in 2 4 16; do
		bad=0; first=""
		for ((i = 0; i < RUNS; i++)); do
			run_bounded "$PRELOAD" --threads=$n $arm
			rc=$?
			if [[ $rc -ne 0 ]]; then
				bad=$((bad + 1))
				[[ -n $first ]] || first="rc=$rc: $(grep -E 'NULL|FAIL' "$OUT" | head -1)"
			fi
		done
		printf '  %-8s threads=%-2d %d/%d failed%s\n' "${arm:---malloc}" \
		    "$n" "$bad" "$RUNS" "${first:+  ($first)}"
		(( bad )) && fail=1
	done
done

if (( fail )); then
	echo "RESULT: FAIL (a thread racing the first allocation was not served by umem)"
	exit 1
fi
echo "RESULT: PASS"
exit 0

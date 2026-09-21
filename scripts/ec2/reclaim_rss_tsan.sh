#!/usr/bin/env bash
# P1.4/P1.5: RSS/retention observation, then a TSAN usability probe.
#
# Part 1 is an observation with a control (default delay, delay=0,
# reclaim=0), not a benchmark.
#
# Part 2 records honestly whether TSAN is usable here.  AL2023's gcc-11 ships
# libtsan.so.0.0.0 with no libtsan.so.0 soname symlink, so the TSAN runtime
# has to be preloaded by file path -- the same problem
# scripts/ec2/remote_run_asan.sh documents for libasan.
set -u
echo "######## PART 1: RSS/retention observation (default build) ########"
./scripts/ec2/clean-regen.sh >/dev/null 2>&1
./configure >/dev/null 2>&1 && make -j"$(nproc)" >/dev/null 2>&1 && echo "BUILD OK"
export LD_LIBRARY_PATH=.libs
for mode in "default:" "delay0:reclaim=1,reclaim_delay=0,reap_interval=1" "off:reclaim=0"; do
	name="${mode%%:*}"; opts="${mode#*:}"
	echo "--- reclaim mode: $name (UMEM_OPTIONS='$opts') ---"
	if [ -n "$opts" ]; then
		UMEM_OPTIONS="$opts" timeout 300 ./test/bench/.libs/probe_reclaim_rss
	else
		timeout 300 ./test/bench/.libs/probe_reclaim_rss
	fi
	echo "rc=$?"
done

echo "######## PART 2: TSAN usability ########"
./scripts/ec2/clean-regen.sh --enable-tsan >/dev/null 2>&1
if ! make -j"$(nproc)" > /tmp/tsanbuild.log 2>&1; then
	echo "TSAN BUILD FAILED"; tail -30 /tmp/tsanbuild.log; exit 0
fi
echo "TSAN BUILD OK"
TSAN_LIB="$(gcc -print-file-name=libtsan.so.0.0.0)"
[ -f "$TSAN_LIB" ] || TSAN_LIB="$(ls /usr/lib/gcc/*/*/libtsan.so.0.0.0 2>/dev/null | head -1)"
if [ ! -f "$TSAN_LIB" ]; then
	echo "TSAN RUNTIME NOT FOUND -- reporting unusable rather than skipping silently"; exit 0
fi
echo "tsan runtime: $TSAN_LIB"
export LD_PRELOAD="$TSAN_LIB"
export TSAN_OPTIONS=halt_on_error=0:exitcode=0:history_size=4
for case in race reap_reentry; do
	echo "--- tsan $case ---"
	UMEM_OPTIONS=reclaim=1,reclaim_delay=0,reap_interval=1 \
	    timeout 600 ./test/unit/.libs/repro_reclaim_reuse "$case" \
	    > "/tmp/tsan_$case.log" 2>&1
	echo "rc=$?"
	echo "tsan_warnings=$(grep -c 'WARNING: ThreadSanitizer' "/tmp/tsan_$case.log")"
	echo "top reported sites:"
	grep -oE '(umem|vmem)[a-z_]*\.c:[0-9]+' "/tmp/tsan_$case.log" | sort | uniq -c | sort -rn | head -15
	echo "slab_state / reclaim mentions:"
	grep -n 'slab_state\|umem_slab_reclaim\|umem_cache_reclaim_pages' "/tmp/tsan_$case.log" | head -8
	echo "first report:"; sed -n '/WARNING: ThreadSanitizer/,/^$/p' "/tmp/tsan_$case.log" | head -25
done

#!/usr/bin/env bash
# TSAN post-fix run for P1.5c, at the SAME sensitivity as the pre-fix baseline
# (scripts/ec2/reclaim_tsan_baseline.sh): 30 s, reap_interval=0 so the reclaim
# limiter is off and the RECLAIMING->CLEAN publication runs continuously.
#
# Matching the baseline's settings is the whole point.  The first attempt at
# this ran 3 s with reap_interval=1 and reported 0 races for the PRE-FIX code
# too, i.e. the run never sampled the window.
set -u
./scripts/ec2/clean-regen.sh --enable-tsan >/dev/null 2>&1
make -j"$(nproc)" > /tmp/b.log 2>&1 || { echo "TSAN BUILD FAILED"; tail -30 /tmp/b.log; exit 1; }
echo "TSAN BUILD OK (post-fix)"
TSAN_LIB="$(ls /usr/lib/gcc/*/*/libtsan.so.0.0.0 2>/dev/null | head -1)"
[ -f "$TSAN_LIB" ] || { echo "no TSAN runtime"; exit 1; }
export LD_PRELOAD="$TSAN_LIB" LD_LIBRARY_PATH=.libs
export TSAN_OPTIONS=halt_on_error=0:exitcode=0:history_size=7
for case in race reap_reentry; do
	echo "--- tsan $case (30s, reap_interval=0) ---"
	RECLAIM_RACE_SECONDS=30 UMEM_OPTIONS=reclaim=1,reclaim_delay=0,reap_interval=0 \
	    timeout 600 ./test/unit/.libs/repro_reclaim_reuse "$case" > "/tmp/post_$case.log" 2>&1
	echo "rc=$?"
	echo "tsan_warnings=$(grep -c 'WARNING: ThreadSanitizer' "/tmp/post_$case.log")"
	echo "reports naming umem_slab_reclaim: $(grep -c 'umem_slab_reclaim' "/tmp/post_$case.log")"
	echo "reports naming umem_cache_reclaim_pages: $(grep -c 'umem_cache_reclaim_pages' "/tmp/post_$case.log")"
	echo "SUMMARY lines (the function each race is attributed to):"
	grep 'SUMMARY: ThreadSanitizer' "/tmp/post_$case.log" | sed 's|.*/||' | sort | uniq -c | sort -rn
done

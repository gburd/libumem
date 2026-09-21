#!/usr/bin/env bash
set -u
echo "######## PART 1: RSS/retention observation (default build) ########"
./scripts/ec2/clean-regen.sh >/dev/null 2>&1
./configure >/dev/null 2>&1 && make -j"$(nproc)" >/dev/null 2>&1 && echo "BUILD OK"
export LD_LIBRARY_PATH=.libs
echo "--- default reclaim (delay=30) ---"
timeout 300 ./test/bench/.libs/probe_reclaim_rss; echo "rc=$?"
echo "--- reclaim_delay=0 (pages actually discarded within the run) ---"
UMEM_OPTIONS=reclaim=1,reclaim_delay=0,reap_interval=1 timeout 300 ./test/bench/.libs/probe_reclaim_rss; echo "rc=$?"
echo "--- reclaim disabled (control) ---"
UMEM_OPTIONS=reclaim=0 timeout 300 ./test/bench/.libs/probe_reclaim_rss; echo "rc=$?"

echo "######## PART 2: TSAN usability ########"
./scripts/ec2/clean-regen.sh --enable-tsan >/dev/null 2>&1
if make -j"$(nproc)" > /tmp/tsanbuild.log 2>&1; then
  echo "TSAN BUILD OK"
  export LD_LIBRARY_PATH=.libs TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1
  echo "--- tsan race ---"
  UMEM_OPTIONS=reclaim=1,reclaim_delay=0,reap_interval=1 timeout 400 ./test/unit/.libs/repro_reclaim_reuse race > /tmp/tsan_race.log 2>&1; echo "rc=$?"
  echo "warnings=$(grep -c 'WARNING: ThreadSanitizer' /tmp/tsan_race.log)"
  echo "distinct race sites:"; grep -A3 'WARNING: ThreadSanitizer: data race' /tmp/tsan_race.log | grep -oE 'umem[a-z_]*\.c:[0-9]+' | sort | uniq -c | sort -rn | head -20
  echo "--- slab_state specifically ---"; grep -n "slab_state\|umem_slab_reclaim\|umem_cache_reclaim_pages" /tmp/tsan_race.log | head -10
  echo "--- head of log ---"; head -40 /tmp/tsan_race.log
else
  echo "TSAN BUILD FAILED"; tail -30 /tmp/tsanbuild.log
fi

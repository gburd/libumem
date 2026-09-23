#!/usr/bin/env bash
# P6.8: freed slab memory must return to the OS with NO umem_reap() call.
#
# reap_interval=1 and reclaim_delay=2 compress the documented cycle (two
# working-set passes, depot reap, slab idle for reclaim_delay, madvise) from
# ~70 s of defaults into a few seconds.  Both are documented UMEM_OPTIONS
# tunables; nothing here is test-only.
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/test_reclaim_returns"
[[ -x $bin ]] || bin="$here/.libs/test_reclaim_returns"
[[ -x $bin ]] || { echo "SKIP: test_reclaim_returns not built"; exit 77; }
UMEM_OPTIONS="reap_interval=1,reclaim_delay=2${UMEM_OPTIONS:+,$UMEM_OPTIONS}" exec "$bin"

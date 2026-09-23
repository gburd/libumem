#!/usr/bin/env bash
# P6.9: a forked child must have a working update thread.  Same compressed
# cycle as test_reclaim_returns.sh (documented tunables).
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/test_fork_child_reclaim"
[[ -x $bin ]] || bin="$here/.libs/test_fork_child_reclaim"
[[ -x $bin ]] || { echo "SKIP: test_fork_child_reclaim not built"; exit 77; }
UMEM_OPTIONS="reap_interval=1,reclaim_delay=2${UMEM_OPTIONS:+,$UMEM_OPTIONS}" exec "$bin"

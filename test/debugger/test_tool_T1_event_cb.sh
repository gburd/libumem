#!/usr/bin/env bash
#
# T1 of docs/reviews/2026-09-26-tool-review.md: umem_inspect_set_event_cb()
# callbacks never run, because umem_inspect_notify() has no caller in the
# allocator.  The gdb/lldb `umem break alloc|free|error` / `umem events`
# commands break on the umem_event_* stubs reached only from that fan-out, so
# they are dead for the same reason.
#
# Exit: 0 fixed (alloc and free callbacks both ran), 1 finding reproduces,
# 99 harness broken (binary missing, output unparsable, or it made no
# allocations -- then zero callbacks proves nothing).
# TOOL_EVENT_CB overrides the binary (used to prove the 99 path).
set -u
. "$(dirname "$0")/tool_common.sh"

BIN=${TOOL_EVENT_CB:-$ROOT/test/debugger/test_event_cb}
[[ -x $BIN ]] || { echo "ERROR: $BIN not built"; exit 99; }

out=$(LD_LIBRARY_PATH="$ROOT/.libs" "$BIN" 2>&1)
echo "$out"
re='alloc=([0-9]+) free=([0-9]+) \(([0-9]+) allocations made\)'
[[ $out =~ $re ]] || { echo "ERROR: unparsable test_event_cb output"; exit 99; }
alloc=${BASH_REMATCH[1]} free=${BASH_REMATCH[2]} made=${BASH_REMATCH[3]}
(( made > 0 )) || { echo "ERROR: control: 0 allocations made"; exit 99; }

if (( alloc == 0 || free == 0 )); then
	echo "T1 REPRODUCES: $made allocations, alloc callbacks=$alloc free=$free"
	exit 1
fi
echo "T1 FIXED: alloc callbacks=$alloc free=$free for $made allocations"
exit 0

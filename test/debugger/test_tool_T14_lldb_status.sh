#!/usr/bin/env bash
#
# T14 of docs/reviews/2026-09-26-tool-review.md: lldb `umem status` prints the
# report and then "error: library call failed: error: unknown error", so
# `lldb --batch` stops the rest of the script.  lldb 19 flags every (void)
# expression result as Fail() (0x1001, no result).
#
# Positive control: the status report itself (umem_alloc_ rows) printed.
#
# Exit: 0 fixed, 1 finding reproduces, 77 no lldb / no ptrace, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_lldb
skip_unless_ptrace
unset UMEM_DEBUG UMEM_LOGGING UMEM_OPTIONS
start_target "$TMPD/info" 1 60

# The marker is built at run time: lldb echoes each -o command, so a literal
# would appear even if the command never ran.
out=$(timeout 90 lldb --batch -p "$TPID" \
    -o "command script import $ROOT/tools/lldb/umem_lldb.py" \
    -o 'umem status' -o 'script print("T14_" + "AFTER")' \
    -o 'process detach' 2>&1)
rc=$?
grep -v '^umem_\|^[[:space:]]*[0-9]' <<<"$out" | tail -20
echo "rc=$rc"
[[ $out == *umem_alloc_* ]] ||
	{ echo "ERROR: control: umem status printed no cache rows"; exit 99; }

if [[ $out == *"library call failed"* || $out != *T14_AFTER* ]]; then
	echo "T14 REPRODUCES: umem status ended in an error / aborted the batch"
	exit 1
fi
echo "T14 FIXED: umem status clean, batch continued"
exit 0

#!/usr/bin/env bash
#
# T13 of docs/reviews/2026-09-26-tool-review.md: gdb `umem break error -c CODE`
# ignores CODE.  `-c` is parsed as a cache name and dropped for `error`, so
# the breakpoint is unconditional (lldb gets this right).
#
# tool_target dupfree:5 raises UMERR_DUPFREE (code 2) under UMEM_DEBUG=default.
# With `-c 99` gdb must not stop on it.
#
# Positive control: plain `umem break error` on a fresh target stops in
# umem_event_error.
#
# Exit: 0 fixed, 1 finding reproduces, 77 no gdb / no ptrace, 99 harness.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_gdb
skip_unless_ptrace
export UMEM_DEBUG=default
unset UMEM_LOGGING UMEM_OPTIONS

# run_gdb <tag> [break args...]: fresh target, gdb output in $TMPD/<tag>.out
run_gdb() {
	local tag=$1
	shift
	start_target "$TMPD/info.$tag" 1 30 dupfree:5
	timeout 60 gdb -q -nx -batch -p "$TPID" \
	    -ex "source $ROOT/tools/gdb/umem_gdb.py" \
	    -ex "umem break error $*" -ex continue -ex 'print code' \
	    >"$TMPD/$tag.out" 2>&1
	kill -9 "$TPID" "$BGPID" 2>/dev/null
	wait 2>/dev/null
	cat "$TMPD/$tag.out"
}
hit='hit Breakpoint .*umem_event_error'

run_gdb ctl
grep -q "$hit" "$TMPD/ctl.out" ||
	{ echo "ERROR: control: plain 'umem break error' did not stop"; exit 99; }

run_gdb code99 -c 99
if grep -q "$hit" "$TMPD/code99.out"; then
	echo "T13 REPRODUCES: 'umem break error -c 99' stopped on another code"
	exit 1
fi
echo "T13 FIXED: 'umem break error -c 99' did not stop on code 2"
exit 0

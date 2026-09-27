#!/usr/bin/env bash
#
# T1 of docs/reviews/2026-09-26-tool-review.md at the debugger level: gdb
# `umem break alloc` / `umem break free` (tools/gdb/umem_gdb.py) break on
# umem_event_alloc / umem_event_free, reached only through
# umem_inspect_notify().  Pre-fix nothing calls it, so the breakpoint is set
# and never hit.
#
# Each gdb session attaches, runs its commands, `continue`s, and is knocked
# out by SIGUSR2 after 8 s if nothing stopped it (tool_target ignores
# SIGUSR2).  Positive control: `break _umem_alloc` must hit.
# Exit: 0 fixed, 1 finding reproduces, 77 no gdb / ptrace refused, 99 control.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_gdb
if [[ $(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0) != 0 &&
      $(id -u) != 0 ]]; then
	echo "SKIP: kernel.yama.ptrace_scope != 0"; exit 77
fi
start_target "$TMPD/info" 3 120

# gdb_run <tag> <gdb command>... ; output in $TMPD/gdb.<tag>
gdb_run() {
	local tag=$1 c; shift
	local args=(-ex 'set pagination off' -ex 'set confirm off'
	    -ex "source $ROOT/tools/gdb/umem_gdb.py")
	for c in "$@"; do args+=(-ex "$c"); done
	args+=(-ex continue -ex 'bt 3' -ex detach)
	( sleep 8; kill -USR2 "$TPID" 2>/dev/null ) &
	local kp=$!
	timeout 60 gdb -nx -batch -p "$TPID" "${args[@]}" >"$TMPD/gdb.$tag" 2>&1
	kill "$kp" 2>/dev/null; wait "$kp" 2>/dev/null
	echo "--- gdb $tag"; grep -E 'Breakpoint|SIGUSR2|^#' "$TMPD/gdb.$tag"
}
hit() { grep -qE "Breakpoint [0-9]+, (0x[0-9a-f]+ in )?$2 " "$TMPD/gdb.$1"; }

gdb_run control 'break _umem_alloc'
hit control _umem_alloc ||
	{ echo "ERROR: control: break _umem_alloc never hit"; cat "$TMPD/gdb.control"; exit 99; }

gdb_run alloc 'umem break alloc'
gdb_run free 'umem break free'
fail=0
hit alloc umem_event_alloc || { echo "T1 REPRODUCES: umem break alloc never stopped"; fail=1; }
hit free umem_event_free || { echo "T1 REPRODUCES: umem break free never stopped"; fail=1; }
(( fail )) && exit 1
echo "T1 FIXED: gdb umem break alloc and free both stopped"
exit 0

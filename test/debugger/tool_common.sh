# test/debugger/tool_common.sh -- sourced by test/debugger/test_tool_T*.sh,
# the regressions for docs/reviews/2026-09-26-tool-review.md.
#
# Exit-code contract for every test_tool_T*.sh:
#   0  fixed (the finding no longer reproduces)
#   1  the finding reproduces (these are in XFAIL_TESTS)
#   77 prerequisite missing (automake SKIP)
#   99 harness broken (automake ERROR): the positive control failed, so the
#      absence of the finding would mean nothing
#
# Sets ROOT (repo root) and TMPD (per-run scratch dir, removed on exit).
# TOOL_TARGET overrides the target binary (used to prove the 99 path).

cd "$(dirname "$0")/../.." || exit 99
ROOT=$(pwd)
TMPD=$(mktemp -d) || exit 99
TPID=
BGPID=
TOOL_TARGET=${TOOL_TARGET:-$ROOT/test/debugger/tool_target}

tool_cleanup() {
	kill -9 ${TPID:+"$TPID"} ${BGPID:+"$BGPID"} 2>/dev/null
	wait 2>/dev/null
	rm -rf "$TMPD"
}
trap tool_cleanup EXIT

skip_unless_gdb() {
	command -v gdb >/dev/null 2>&1 || { echo "SKIP: gdb not installed"; exit 77; }
}

skip_unless_lldb() {
	command -v lldb >/dev/null 2>&1 || { echo "SKIP: lldb not installed"; exit 77; }
}

# config.h is the authority (same test as test_introspect_sock_path.sh): when
# off it holds "/* #undef UMEM_INTROSPECT */", which a bare grep would match.
skip_unless_introspect() {
	grep -qE '^[[:space:]]*#[[:space:]]*define[[:space:]]+UMEM_INTROSPECT[[:space:]]+1' \
	    "$ROOT/config.h" 2>/dev/null ||
		{ echo "SKIP: built without --enable-introspect"; exit 77; }
	[[ -x $ROOT/tools/umemctl ]] ||
		{ echo "SKIP: tools/umemctl not built"; exit 77; }
}

# start_target <infofile> [threads] [secs] [extra] -- runs tool_target in the
# background with the caller's environment, waits up to 5 s for the info file
# (renamed into place only once complete), sources it, exports TPID.
start_target() {
	local info=$1
	[[ -x $TOOL_TARGET ]] || { echo "ERROR: $TOOL_TARGET not built"; exit 99; }
	LD_LIBRARY_PATH="$ROOT/.libs" "$TOOL_TARGET" "$info" "${2:-3}" \
	    "${3:-30}" ${4:+"$4"} >"$TMPD/target.log" 2>&1 &
	BGPID=$!
	for _ in $(seq 1 50); do
		[[ -s $info ]] && break
		sleep 0.1
	done
	if [[ ! -s $info ]]; then
		echo "ERROR: tool_target did not write $info in 5 s"
		cat "$TMPD/target.log"
		exit 99
	fi
	# shellcheck disable=SC1090
	. "$info"
	# The info pid, not $!: tool_target may be a libtool wrapper.
	TPID=${pid:-}
	[[ -n $TPID ]] && kill -0 "$TPID" 2>/dev/null ||
		{ echo "ERROR: target pid '${TPID}' from $info not alive"; exit 99; }
	export TPID
}

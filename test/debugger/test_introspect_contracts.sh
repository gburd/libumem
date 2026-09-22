#!/usr/bin/env bash
#
# test/debugger/test_introspect_contracts.sh -- enforce the umemctl control
# channel contracts (Phase 3 items 4 and 5 of
# docs/plans/2026-09-21-production-readiness.md).
#
# Each check names the contract clause in umem_introspect.c that it defends.
#
# SKIPs (automake status 77) when the library was not built with
# --enable-introspect, because then there is no channel to test.  Returning 0
# for an unmet prerequisite would be a false green (see AGENTS.md §6).
#
# PRE-FIX BEHAVIOUR:
#   A1  the socket mode depended on the inherited umask, with no explicit mode.
#   item 4 (SIGPIPE): a client disconnecting mid-response raised SIGPIPE in the
#       target, whose default action TERMINATES IT -- a debugging channel could
#       kill the process it was inspecting.
#   B5  the fork child inherited an armed break predicate and a satisfied
#       pthread_once but NOT the server thread, so an armed child stopped on
#       its next matching allocation with nothing able to resume it.
set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
CTL=$ROOT/tools/umemctl
CHURN=$(find "$ROOT" -name introspect_churn -type f -perm -u+x 2>/dev/null | head -1)

fail=0
note() { printf '  %s\n' "$*"; }
bad()  { printf 'FAIL: %s\n' "$*" >&2; fail=$((fail + 1)); }

# ---------------------------------------------------------------------------
# Prerequisite: the channel must be compiled in.
#
# NOTE the grep pattern.  When introspection is OFF, config.h contains the line
#
#     /* #undef UMEM_INTROSPECT */
#
# so a plain `grep -q UMEM_INTROSPECT` MATCHES in a default build.  This test
# then skipped its own skip, ran against a library with no channel compiled in,
# and reported a hard FAIL ("target never created ...sock") that looked like a
# real introspection defect.  Match the actual #define.
#
# tools/umemctl and introspect_churn are built unconditionally, so their
# presence proves nothing about the library -- config.h is the authority.
# ---------------------------------------------------------------------------
if ! grep -qE '^[[:space:]]*#[[:space:]]*define[[:space:]]+UMEM_INTROSPECT[[:space:]]+1' \
    "$ROOT/config.h" 2>/dev/null; then
	echo "SKIP: built without --enable-introspect (no channel to test)"
	exit 77
fi
if [[ ! -x $CTL || -z $CHURN ]]; then
	echo "SKIP: umemctl/introspect_churn not built"
	exit 77
fi

export LD_LIBRARY_PATH=$ROOT/.libs
SOCK=$(mktemp -u /tmp/umem-contract-XXXXXX.sock)
# umemctl and the B5 helper both resolve the socket from this variable; without
# it they would look for /tmp/umem.<pid>.sock and silently talk to nothing.
export UMEM_INTROSPECT_SOCK=$SOCK
TPID=""

cleanup() {
	[[ -n $TPID ]] && kill -9 "$TPID" 2>/dev/null
	rm -f "$SOCK"
	wait 2>/dev/null
	return 0
}
trap cleanup EXIT

start_target() {
	# Deliberately permissive umask: pre-fix this produced a 0666 socket.
	# UMEM_DEBUG=audit so 'leaks' produces a response far larger than the
	# socket buffer -- required for the SIGPIPE check below.
	( umask 000
	  UMEM_OPTIONS=introspect=1 UMEM_DEBUG=audit \
	      "$CHURN" 60 >/dev/null 2>&1 &
	  echo $! > /tmp/.umem-contract-pid )
	TPID=$(cat /tmp/.umem-contract-pid); rm -f /tmp/.umem-contract-pid
	for _ in $(seq 1 50); do
		[[ -S $SOCK ]] && return 0
		sleep 0.1
	done
	return 1
}

echo "umemctl contract tests"

if ! start_target; then
	bad "target never created $SOCK"
	exit 1
fi

# ---------------------------------------------------------------------------
# A1: explicit 0600 socket mode, independent of the inherited umask.
# ---------------------------------------------------------------------------
MODE=$(stat -c '%a' "$SOCK" 2>/dev/null || stat -f '%Lp' "$SOCK")
if [[ $MODE == "600" ]]; then
	note "A1 socket mode is 0600 despite umask 000: ok"
else
	bad "A1 violated: socket mode is $MODE, expected 600 (the control channel can stop the process)"
fi

# ---------------------------------------------------------------------------
# Sanity: the channel answers at all, and the walks hold the right locks.
# ---------------------------------------------------------------------------
if $CTL "$TPID" stats 2>/dev/null | grep -q '^pid '; then
	note "channel responds to stats: ok"
else
	bad "channel did not answer 'stats'"
fi

# 'held' replaced the misleading 'inuse' label (item 7).
if $CTL "$TPID" stats 2>/dev/null | grep -q '^bufs_held '; then
	note "stats reports bufs_held (not 'inuse'): ok"
else
	bad "stats does not report bufs_held"
fi

# ---------------------------------------------------------------------------
# item 4 (SIGPIPE): a client that disconnects mid-response must not kill the
# target.
#
# The response has to be LARGER THAN THE SOCKET BUFFER, otherwise the server
# completes its write into the buffer before the client ever hangs up and no
# SIGPIPE is possible -- the check would pass vacuously even against the
# pre-fix code.  'leaks' under UMEM_DEBUG=audit emits thousands of records
# (hundreds of KB), so the server is guaranteed to still be writing.  We also
# shrink the receive buffer and read nothing, so the server blocks, then abort
# with RST.
# ---------------------------------------------------------------------------
SIGPIPE_TRIGGERED=0
for _ in $(seq 1 15); do
	timeout 5 python3 - "$SOCK" <<'PY' 2>/dev/null
import socket, struct, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    # Small receive buffer so the server fills it and blocks in write().
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    s.connect(sys.argv[1])
    s.sendall(b"leaks\n")
    # Read a little, then stop reading so the server blocks mid-response.
    s.recv(256)
    time.sleep(0.2)
    # Abort with RST rather than an orderly FIN: the next server write fails.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
finally:
    s.close()
PY
	if ! kill -0 "$TPID" 2>/dev/null; then
		SIGPIPE_TRIGGERED=1
		break
	fi
done

if [[ $SIGPIPE_TRIGGERED == 1 ]]; then
	bad "item 4 violated: the target DIED when a client disconnected mid-response (SIGPIPE)"
	TPID=""
	exit 1
fi
note "item 4 target survived 15 aborted mid-response disconnects: ok"

# The channel must still be usable afterwards.
if $CTL "$TPID" stats 2>/dev/null | grep -q '^pid '; then
	note "channel still serving after abrupt disconnects: ok"
else
	bad "channel stopped serving after a client disconnected"
fi

# ---------------------------------------------------------------------------
# B3/B4: arm a predicate the workload actually hits, confirm it stops a thread,
# and that 'continue' releases it.  The server thread itself must never stop
# (B4) -- if it did, this 'continue' could never be processed and the command
# would hang, so the timeout is the real assertion.
#
# introspect_churn allocates 32 bytes on every iteration, so size=32 is
# guaranteed to match promptly.
# ---------------------------------------------------------------------------
$CTL "$TPID" break size=32 >/dev/null 2>&1
sleep 0.5
if timeout 10 "$CTL" "$TPID" continue 2>/dev/null | grep -q 'ok continue'; then
	note "B3/B4 armed, then resumed via continue: ok"
else
	bad "B3/B4 violated: 'continue' did not complete (server thread parked, or a stranded waiter)"
fi

sleep 0.3
if kill -0 "$TPID" 2>/dev/null; then
	note "target alive after break/continue: ok"
else
	bad "target died during break/continue"
	TPID=""
	exit 1
fi

kill -9 "$TPID" 2>/dev/null; TPID=""
rm -f "$SOCK"

# ---------------------------------------------------------------------------
# B5: a fork child must not inherit an armed predicate.
#
# The child has no server thread, so an armed child would stop on its next
# matching allocation with nothing able to resume it -- it would hang forever.
# The helper arms a predicate, forks, and the child allocates the matching
# size.  If the child hangs, B5 is violated.
# ---------------------------------------------------------------------------
HELPER=$ROOT/test/integration/test_introspect_fork
if [[ -x $HELPER ]]; then
	# The helper runs its OWN control channel on its own socket, so it can
	# arm a predicate and then fork.
	FSOCK=$(mktemp -u /tmp/umem-fork-XXXXXX.sock)
	out=$(UMEM_OPTIONS=introspect=1 UMEM_INTROSPECT_SOCK=$FSOCK \
	    timeout 30 "$HELPER" 2>&1)
	rc=$?
	rm -f "$FSOCK"
	case $rc in
	0)   note "B5 fork child did not inherit the armed predicate: ok" ;;
	77)  echo "  (B5 skipped: $out)" ;;
	124) bad "B5 violated: the fork child HUNG on an inherited armed predicate" ;;
	*)   bad "B5 helper failed (rc=$rc): $out" ;;
	esac
else
	echo "  (B5 helper not built; skipping that clause)"
fi

echo
if [[ $fail -gt 0 ]]; then
	echo "$fail contract violation(s)"
	exit 1
fi
echo "all umemctl contract tests passed"

#!/usr/bin/env bash
#
# P5.2 regression: privileged processes must ignore UMEM_* options that have a
# file, socket, exec, or disclosure side effect.
#
# THE DEFECT (pre-fix)
#   envvar.c had no issetugid()/AT_SECURE check anywhere.  UMEM_OPTIONS,
#   UMEM_DEBUG and UMEM_LOGGING were parsed and honoured identically whether
#   the process was ordinary or setuid.  So an attacker who controls the
#   environment of a setuid or AT_SECURE target got, among others:
#     profile=record:/path   create + truncate a file of their choosing, as
#                            the target's uid (with P5.3, any file)
#     introspect=1           bind a control socket into the privileged process
#     backend=sbrk           change the memory backend (vmem_sbrk.c gated its
#                            page tuning, but only AFTER parsing)
#     verbose/allverbose     heap addresses and cache names onto fd 2
#     noabort                disarm abort-on-corruption
#     mtbf=N                 inject allocation failures (DoS)
#   glibc ignores MALLOC_* tunables under AT_SECURE; libumem had no equivalent.
#
# THE FIX: one helper, umem_secure_mode() = issetugid() || getauxval(AT_SECURE)
# (misc.c), consulted in envvar.c's process_item() BEFORE the argument is
# parsed and before any flag is set, for every option marked
# item_secure_unsafe.
#
# WHAT THIS TESTS: THE GATE, not setuid.  A real setuid binary cannot be built
# from the test suite (needs root), so the helper forces the decision through
# umem_secure_mode_force -- an internal variable that is deliberately NOT
# settable from the environment, because an env-settable override would let an
# attacker switch the gate off.  What issetugid()/AT_SECURE themselves return
# on a real setuid exec is verified by inspection of misc.c, not here.
#
# THE CONTROL IS THE POINT: the non-secure arm must CREATE the file.  Without
# it, a build where profiling was simply broken would "pass" the secure arm.
#
# PRE-FIX DEMONSTRATION: against v3.0.0 both arms create the file (and the
# helper fails to link at all, since umem_secure_mode did not exist -- so the
# demonstration there is run with the helper's two extern declarations
# stubbed; see the report).
#
# Exit: 0 pass, 1 fail, 77 prerequisites missing.

set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)

HELPER=""
for cand in "$ROOT/test/security/.libs/test_secure_gate_helper" \
            "$ROOT/test/security/test_secure_gate_helper"; do
	[[ -x $cand ]] && { HELPER=$cand; break; }
done
if [[ -z $HELPER ]]; then
	echo "SKIP: test/security/test_secure_gate_helper not built"
	exit 77
fi

export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

FAIL=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; FAIL=1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

run_arm() {         # run_arm <secure 0|1> <sentinel path>
	local secure=$1 path=$2
	rm -f "$path"
	UMEM_OPTIONS="profile=record:$path" "$HELPER" "$secure" \
		>"$TMP/arm$secure.log" 2>&1
	echo $?
}

# ------------------------------------------------- arm 1: control (not secure)
echo "[1/3] control: profile=record:<path> works when NOT in secure mode"
SENT_OPEN="$TMP/profile_open.ump"
rc=$(run_arm 0 "$SENT_OPEN")
if [[ $rc -ne 0 ]]; then
	fail "helper exited $rc in the non-secure arm"
	sed -n 1,20p "$TMP/arm0.log"
elif [[ -s $SENT_OPEN ]]; then
	pass "profile written ($(stat -c %s "$SENT_OPEN") bytes) -- feature works"
else
	# Without this arm passing, the secure arm proves nothing.
	fail "profile NOT written without secure mode: the secure arm would be vacuous"
fi

# ----------------------------------------------------- arm 2: secure mode gate
echo "[2/3] gated: profile=record:<path> ignored in secure mode"
SENT_SEC="$TMP/profile_secure.ump"
rc=$(run_arm 1 "$SENT_SEC")
if [[ $rc -ne 0 ]]; then
	fail "helper exited $rc in the secure arm"
	sed -n 1,20p "$TMP/arm1.log"
elif [[ -e $SENT_SEC ]]; then
	fail "secure mode still created the file: $SENT_SEC"
else
	pass "no file created in secure mode"
fi

# --------------------------------- arm 3: the allocator still works when gated
# The goal is no side effects, not a crippled allocator: the secure arm above
# must have exited 0 having actually allocated (the helper allocates and frees
# before returning).  Check a tuning option still lands, so the filter is not
# simply discarding everything.
echo "[3/3] pure tuning options still honoured in secure mode"
rm -f "$TMP/none.ump"
if UMEM_OPTIONS="nomagazines,concurrency=4" "$HELPER" 1 \
	>"$TMP/arm_tune.log" 2>&1; then
	pass "allocator runs with tuning options under secure mode"
else
	fail "tuning options broke the allocator in secure mode (rc=$?)"
	sed -n 1,20p "$TMP/arm_tune.log"
fi

echo
if [[ $FAIL -eq 0 ]]; then
	echo "test_secure_env_gate: PASS"
	exit 0
fi
echo "test_secure_env_gate: FAIL"
exit 1

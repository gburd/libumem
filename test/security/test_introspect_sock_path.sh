#!/usr/bin/env bash
#
# P5.6 regression: the introspection control socket must not be placed at a
# predictable name in a shared directory, and reclaiming a stale path must not
# act on a filesystem entry it only looked at.
#
# THE DEFECT (pre-fix, umem_introspect.c:907 and :975-990)
#   The path was /tmp/umem.<pid>.sock -- a guessable name in a world-writable
#   sticky directory -- and the reclaim sequence was
#
#       stat(path) -> probe connect() -> unlink(path) -> bind(path)
#
#   stat(2) FOLLOWS SYMLINKS.  So a symlink at that path aiming at some other
#   process's socket satisfied S_ISSOCK, the probe connect() to it could fail
#   (nothing listening on a dead peer's socket), and the target then
#   unlink()ed a path it had never created -- removing an attacker-chosen
#   directory entry as the target's uid.  For a setuid target that uid is the
#   privileged one.
#
#   Independently: because the name was predictable, an attacker could also
#   bind it FIRST.  The target then finds EADDRINUSE with a live listener and
#   (correctly) refuses -- but `umemctl <pid>` resolves the same predictable
#   path, so the operator's whatis/break/continue commands go to the attacker's
#   socket and the attacker's replies come back looking like the allocator's.
#
# THE FIX
#   The socket lives in a directory only this euid can write to
#   ($XDG_RUNTIME_DIR when it qualifies, else /tmp/umem-<euid> created with
#   mkdir(0700) and verified), so no other user can pre-create the path at all.
#   Reclaim never unlinks the caller's path: it binds a private temporary name
#   in the same directory and rename()s it over, which is atomic and follows no
#   symlink.  lstat() replaced stat(), so a symlink is never mistaken for our
#   own stale socket.
#
# WHAT THIS ASSERTS
#   A. SENTINEL SURVIVAL (the P5.6 exposure).  Pre-create the socket path as a
#      symlink to a sentinel FILE and start the server.  The sentinel and its
#      directory entry must both still exist afterwards.  Pre-fix the target
#      unlinked the symlink; with a socket as the symlink target it unlinked
#      that instead.  Uses UMEM_INTROSPECT_SOCK to name the path, because the
#      whole point of the fix is that the DEFAULT path can no longer be
#      pre-created by anyone else -- so the TOCTOU itself must be provoked
#      through the developer override, which is the only remaining way to aim
#      the server at a directory an attacker can write to.
#   B. NOT IN A SHARED DIRECTORY.  With no override, the socket must not appear
#      at /tmp/umem.<pid>.sock, and the directory it does appear in must be
#      mode 0700 and owned by us.
#   C. CONTROL.  The channel must actually work at the new location, so A and B
#      are not passing because introspection is simply broken.
#
# SKIPs (77) without --enable-introspect: there is no channel to test.
#
# Exit: 0 pass, 1 fail, 77 prerequisites missing.

set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)

# config.h is the authority on whether the LIBRARY has the channel; umemctl
# and introspect_churn are built unconditionally.  Match the actual #define:
# when introspection is off config.h holds "/* #undef UMEM_INTROSPECT */",
# which a plain grep for the name would match.
if ! grep -qE '^[[:space:]]*#[[:space:]]*define[[:space:]]+UMEM_INTROSPECT[[:space:]]+1' \
    "$ROOT/config.h" 2>/dev/null; then
	echo "SKIP: built without --enable-introspect (no channel to test)"
	exit 77
fi

CTL=$ROOT/tools/umemctl
CHURN=$(find "$ROOT" -name introspect_churn -type f -perm -u+x 2>/dev/null | head -1)
if [[ ! -x $CTL || -z $CHURN ]]; then
	echo "SKIP: umemctl/introspect_churn not built"
	exit 77
fi

export LD_LIBRARY_PATH="$ROOT/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

FAIL=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; FAIL=1; }

TMP=$(mktemp -d)
PIDS=()
cleanup() {
	local p
	for p in "${PIDS[@]:-}"; do
		[[ -n $p ]] && kill -9 "$p" 2>/dev/null
	done
	rm -rf "$TMP"
	wait 2>/dev/null
	return 0
}
trap cleanup EXIT

CANARY='DO-NOT-UNLINK-ME: this entry belongs to someone else'

# start_churn <seconds> [env assignments...] -- echoes the pid.
start_churn() {
	local secs=$1; shift
	env "$@" UMEM_OPTIONS=introspect=1 "$CHURN" "$secs" \
	    >>"$TMP/churn.log" 2>&1 &
	local p=$!
	PIDS+=("$p")
	echo "$p"
}

# -------------------------------------------------- A: sentinel must survive
#
# The pre-fix unlink() removed whatever directory entry the path named.  Two
# sentinels, because which one dies depends on what the symlink points at:
#   - the symlink ENTRY itself (unlink removes the link, not the target), and
#   - a victim socket, which is what stat()-follows-symlink made the target
#     believe it was reclaiming.
echo "[1/3] a symlink at the socket path must not cost anyone a directory entry"
VICTIM=$TMP/victim_file
printf '%s\n' "$CANARY" > "$VICTIM"

# A victim SOCKET with no listener: this is precisely what the pre-fix probe
# connect() saw as "a stale socket, safe to remove".
python3 - "$TMP/victim_socket" <<'PY' >/dev/null 2>&1
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
# Deliberately do NOT listen(): a connect() probe then fails, which is how the
# pre-fix code concluded the path was reclaimable.
s.close()
PY
if [[ ! -S $TMP/victim_socket ]]; then
	echo "SKIP: could not create a victim AF_UNIX socket (python3?)"
	exit 77
fi

LINK=$TMP/squatted.sock
ln -s "$TMP/victim_socket" "$LINK"

TPID=$(start_churn 10 "UMEM_INTROSPECT_SOCK=$LINK")
sleep 2

if [[ ! -L $LINK ]]; then
	fail "the symlink's own directory entry was removed (target unlinked a path it did not create)"
elif [[ ! -S $TMP/victim_socket ]]; then
	fail "the victim socket was unlinked -- stat() followed the symlink and the target reclaimed someone else's entry"
else
	pass "symlink entry and its victim socket both intact"
fi

# The plain-file sentinel, same mechanism, different target type.  This arm is
# a NON-REGRESSION check, not a pre-fix failure: pre-fix, stat() on a symlink
# to a plain file reported a non-socket, so the unlink was already skipped.
# It is here because the fix changed which syscall makes that decision.
LINK2=$TMP/squatted2.sock
ln -s "$VICTIM" "$LINK2"
TPID2=$(start_churn 10 "UMEM_INTROSPECT_SOCK=$LINK2")
sleep 2
if [[ ! -L $LINK2 ]]; then
	fail "the symlink to a plain file had its entry removed"
elif ! grep -q "DO-NOT-UNLINK-ME" "$VICTIM" 2>/dev/null; then
	fail "the plain-file sentinel was clobbered"
else
	pass "symlink to a plain file left alone (a symlink is never our stale socket)"
fi

kill -9 "$TPID" "$TPID2" 2>/dev/null

# ------------------------------------------- B: not a predictable shared path
echo "[2/3] the default socket location must not be a predictable /tmp name"
TPID3=$(start_churn 15)
sleep 2

LEGACY=/tmp/umem.$TPID3.sock
if [[ -e $LEGACY ]]; then
	fail "socket still at the predictable shared path $LEGACY"
else
	pass "nothing at the old predictable path $LEGACY"
fi

# Find where it actually went, and check the directory's properties.
SOCKDIR=""
for d in "${XDG_RUNTIME_DIR:-}" "/tmp/umem-$(id -u)"; do
	[[ -n $d && -S $d/umem.$TPID3.sock ]] && { SOCKDIR=$d; break; }
done
if [[ -z $SOCKDIR ]]; then
	fail "socket not found in XDG_RUNTIME_DIR or /tmp/umem-$(id -u)"
	sed -n 1,20p "$TMP/churn.log"
else
	mode=$(stat -c '%a' "$SOCKDIR")
	owner=$(stat -c '%u' "$SOCKDIR")
	if [[ $owner == "$(id -u)" ]]; then
		pass "socket directory $SOCKDIR is owned by us"
	else
		fail "socket directory $SOCKDIR is owned by uid $owner, not $(id -u)"
	fi
	# A directory nobody else can write to is the whole point: an attacker
	# who cannot create an entry there cannot win the race at all.
	if [[ ${mode: -2} == "00" ]]; then
		pass "socket directory mode is $mode (no group/other access)"
	else
		fail "socket directory mode is $mode: others can create entries there"
	fi
	smode=$(stat -c '%a' "$SOCKDIR/umem.$TPID3.sock")
	if [[ $smode == 600 ]]; then
		pass "socket mode is 0600 (A1 not regressed)"
	else
		fail "socket mode is $smode, expected 600"
	fi
fi

# --------------------------------------------------------------- C: control
echo "[3/3] control: the channel still answers at the new location"
if $CTL "$TPID3" stats 2>>"$TMP/churn.log" | grep -q '^pid '; then
	pass "umemctl reached the target without UMEM_INTROSPECT_SOCK"
else
	fail "umemctl could not reach the target: arms A and B may be vacuous"
	sed -n 1,20p "$TMP/churn.log"
fi

kill -9 "$TPID3" 2>/dev/null

echo
if [[ $FAIL -eq 0 ]]; then
	echo "test_introspect_sock_path: PASS"
	exit 0
fi
echo "test_introspect_sock_path: FAIL"
exit 1

#!/usr/bin/env bash
#
# P5.3 regression: library file writers must not follow a symlink.
#
# THE DEFECT (pre-fix)
#   umem_profile.c:437   open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644)
#   umem_inspect.c:2071  fopen(path, "wb")
#   umem_inspect.c:2172  fopen(path, "w")
#   No O_EXCL and no O_NOFOLLOW anywhere in the library's writers -- the only
#   O_EXCL in the whole tree was examples/umem_palloc.c:514.  So if `path` was
#   a symlink, the library truncated the symlink's TARGET, as the process's
#   own uid.  Combined with P5.2 (no privilege gate on UMEM_OPTIONS) that is
#   arbitrary file truncation in a privileged target; on its own, a predictable
#   snapshot path in a shared directory is a symlink target.
#
# THE FIX: one writer, misc.c:umem_open_write(), used by both the profile and
# the snapshot writers.  O_NOFOLLOW; no O_TRUNC in the open (truncation is
# ftruncate() on the fd, AFTER the checks, so a hardlinked victim is not
# destroyed before being inspected); rejects non-regular files, st_nlink != 1,
# and any file not owned by geteuid().
#
# WHAT THIS ASSERTS
#   A. symlink: point the profile path at a symlink to a sentinel file with
#      known content.  The sentinel must still hold its content afterwards,
#      and the profile must not have been written.
#   B. hardlink: point it at a second link to the sentinel.  Same.
#   C. control: an ordinary path must still be written, so A and B are not
#      passing merely because profiling is broken.
#
# PRE-FIX DEMONSTRATION: against v3.0.0, (A) leaves the sentinel truncated to
# 0 bytes (or overwritten with profile data) -- the test fails.
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

CANARY='DO-NOT-TRUNCATE-ME: this file belongs to someone else'

# write_profile_to <path>: run the helper (secure mode OFF, so profiling is
# live) with profile=record:<path>.
write_profile_to() {
	UMEM_OPTIONS="profile=record:$1" "$HELPER" 0 >>"$TMP/run.log" 2>&1
	echo $?
}

# ------------------------------------------------------------------ A: symlink
echo "[1/3] symlink at the profile path must not truncate its target"
VICTIM="$TMP/victim_symlink"
printf '%s\n' "$CANARY" > "$VICTIM"
LINK="$TMP/profile_via_symlink.ump"
ln -s "$VICTIM" "$LINK"

rc=$(write_profile_to "$LINK")
if [[ $rc -ne 0 ]]; then
	fail "helper exited $rc in the symlink arm"
elif ! grep -q "DO-NOT-TRUNCATE-ME" "$VICTIM" 2>/dev/null; then
	fail "symlink target was clobbered ($(stat -c %s "$VICTIM" 2>/dev/null) bytes)"
else
	pass "symlink target intact ($(stat -c %s "$VICTIM") bytes, content preserved)"
fi

# ----------------------------------------------------------------- B: hardlink
# O_NOFOLLOW does nothing about hardlinks; the st_nlink check is what covers
# this, and it is checked separately because it is a separate mechanism.
echo "[2/3] hardlink at the profile path must not truncate the other link"
VICTIM2="$TMP/victim_hardlink"
printf '%s\n' "$CANARY" > "$VICTIM2"
HLINK="$TMP/profile_via_hardlink.ump"
ln "$VICTIM2" "$HLINK"

rc=$(write_profile_to "$HLINK")
if [[ $rc -ne 0 ]]; then
	fail "helper exited $rc in the hardlink arm"
elif ! grep -q "DO-NOT-TRUNCATE-ME" "$VICTIM2" 2>/dev/null; then
	fail "hardlinked file was clobbered ($(stat -c %s "$VICTIM2" 2>/dev/null) bytes)"
else
	pass "hardlinked file intact ($(stat -c %s "$VICTIM2") bytes, content preserved)"
fi

# ------------------------------------------------------------------- C: control
echo "[3/3] control: an ordinary path is still written"
PLAIN="$TMP/profile_plain.ump"
rm -f "$PLAIN"
rc=$(write_profile_to "$PLAIN")
if [[ $rc -ne 0 ]]; then
	fail "helper exited $rc in the control arm"
elif [[ -s $PLAIN ]]; then
	pass "profile written to an ordinary path ($(stat -c %s "$PLAIN") bytes)"
	# Also assert the tightened mode: a profile holds heap addresses.
	mode=$(stat -c %a "$PLAIN")
	if [[ $mode == 600 ]]; then
		pass "profile mode is 0600"
	else
		fail "profile mode is $mode, expected 600"
	fi
else
	fail "profile NOT written to an ordinary path: arms A and B are vacuous"
	sed -n 1,20p "$TMP/run.log"
fi

echo
if [[ $FAIL -eq 0 ]]; then
	echo "test_writer_nofollow: PASS"
	exit 0
fi
echo "test_writer_nofollow: FAIL"
exit 1

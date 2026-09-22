#!/usr/bin/env bash
#
# P5.1 regression: the library must not contain a PATH-resolving exec.
#
# THE DEFECT (pre-fix, umem_stacktrace.c:159 and :211)
#   umem_stacktrace_init() is called unconditionally from umem_init(), i.e.
#   before main() in any process that allocates.  It called
#   execlp("addr2line", ...), and execlp resolves through $PATH.  A process
#   whose PATH an attacker influences -- including a setuid binary *linked*
#   against libumem, which glibc's AT_SECURE does not protect, since AT_SECURE
#   blocks LD_PRELOAD and not linkage -- therefore ran an attacker-chosen
#   "addr2line" as the elevated user.  The only gate was
#   getenv("UMEM_STACKTRACE_ADDR2LINE") with no privilege check.
#
#   The feature could not work anyway: it passed `-e /proc/self/exe`, which
#   after the exec names addr2line itself.  Measured with the identical
#   fork/exec sequence on x86_64: `-e /proc/self/exe` gives "?? ??:0", the real
#   executable path gives "main at demo.c:38".  So it was attack surface with
#   no benefit, and it was deleted rather than gated.
#
# WHAT THIS ASSERTS, two independent ways:
#
#   A. BEHAVIOUR.  Put a hostile executable named "addr2line" first on PATH
#      which touches a sentinel file, then run an allocating program with the
#      trigger env var set (UMEM_STACKTRACE_ADDR2LINE=1) under UMEM_DEBUG=audit
#      so the stack-trace machinery is live.  The sentinel must NOT appear.
#
#      READ THIS BEFORE TRUSTING ARM A: it is only non-vacuous on a build
#      WITHOUT libdw.  umem_stacktrace_init() tries init_libdw() first and
#      returns as soon as it succeeds, so on a box with elfutils installed the
#      pre-fix code never reached the execlp at all -- verified: against
#      v3.0.0 on a libdw build this arm passes.  It is not useless, because a
#      build without elfutils is an ordinary deployment (elfutils-devel is not
#      a runtime dependency) and that is exactly the configuration where the
#      exec fired; the pre-fix demonstration was run that way
#      (ac_cv_lib_dw_dwfl_begin=no) and the sentinel DID appear.  This script
#      prints which case it is in so a green result cannot be misread.
#
#   B. STRUCTURE.  No PATH-resolving exec symbol (execlp/execvp/execvpe/
#      system/popen) may be undefined-referenced by libumem.so at all.  This
#      is the arm that bites unconditionally: it fails against v3.0.0 on ANY
#      build, libdw or not, and it fails if the call is reintroduced on a path
#      this script does not exercise.
#
# PRE-FIX DEMONSTRATION (both verified against d22bf03 = v3.0.0, x86_64):
#   B always: nm shows `U execlp@GLIBC_2.2.5`.
#   A with libdw disabled: sentinel created, "HOSTILE addr2line ran: --version".
#
# Exit: 0 all pass, 1 any fail, 77 prerequisites missing (a missing
# prerequisite is a SKIP, never a silent pass -- AGENTS.md 6).

set -uo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)

LIB=""
for cand in "$ROOT/.libs/libumem.so.1" "$ROOT/.libs/libumem.so" "$ROOT/.libs/libumem.so.1.0.0"; do
	[[ -f $cand ]] && { LIB=$cand; break; }
done
if [[ -z $LIB ]]; then
	echo "SKIP: libumem.so not built in $ROOT/.libs (run make first)"
	exit 77
fi

# An allocating program that also drives the stack-trace path.  test_debug
# runs under UMEM_DEBUG and allocates; umem_env_helper's "alive" probe forces
# init and allocates.  Either is enough: the exec sat on the init path.
HELPER=""
for cand in "$ROOT/test/unit/.libs/umem_env_helper" "$ROOT/test/unit/umem_env_helper"; do
	[[ -x $cand ]] && { HELPER=$cand; break; }
done
if [[ -z $HELPER ]]; then
	echo "SKIP: test/unit/umem_env_helper not built"
	exit 77
fi

FAIL=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; FAIL=1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---------------------------------------------------------------- A: behaviour
SENTINEL="$TMP/pwned"
mkdir -p "$TMP/hostile"
cat > "$TMP/hostile/addr2line" <<EOF
#!/bin/sh
# Stand-in for an attacker's addr2line.  A real one would run as the target's
# uid; touching a file is enough to prove it ran.
echo "HOSTILE addr2line ran: \$*" > "$SENTINEL"
# Behave enough like addr2line that the caller's --version probe succeeds,
# so a library that DOES exec us proceeds to the real resolve call too.
echo "GNU addr2line (hostile) 2.40"
exit 0
EOF
chmod +x "$TMP/hostile/addr2line"

echo "[1/2] hostile addr2line first on PATH must not be executed"
env -i \
	PATH="$TMP/hostile:/usr/bin:/bin" \
	HOME="$HOME" \
	LD_LIBRARY_PATH="$ROOT/.libs" \
	UMEM_STACKTRACE_ADDR2LINE=1 \
	UMEM_DEBUG=audit \
	UMEM_LOGGING=transaction \
	"$HELPER" alive >"$TMP/run.log" 2>&1
rc=$?

if [[ $rc -ne 0 ]]; then
	# The helper failing is not the thing under test, but it would make the
	# sentinel check vacuous, so it is a hard failure here.
	fail "helper exited $rc under hostile PATH (check is vacuous otherwise)"
	sed -n 1,20p "$TMP/run.log"
fi

if [[ -e $SENTINEL ]]; then
	fail "library EXECUTED the hostile addr2line: $(cat "$SENTINEL")"
else
	# Say which case this is, so the pass is not over-read.  grep, not a
	# compiled-in define: this script must work against any build tree.
	if grep -qs '^#define HAVE_LIBDW 1' "$ROOT/config.h"; then
		pass "no hostile addr2line execution (sentinel absent) -- NOTE:" \
		     "this build has libdw, which pre-fix also short-circuited" \
		     "before the exec, so arm A is WEAK here; arm B below is the" \
		     "binding one"
	else
		pass "no hostile addr2line execution (sentinel absent) -- build" \
		     "has no libdw, i.e. exactly the pre-fix configuration that" \
		     "DID exec; this arm is meaningful"
	fi
fi

# --------------------------------------------------------------- B: structure
echo "[2/2] no PATH-resolving exec referenced by libumem.so"
if ! command -v nm >/dev/null 2>&1; then
	echo "SKIP: nm unavailable, cannot run the structural check"
	[[ $FAIL -eq 0 ]] && exit 77 || exit 1
fi

# Undefined ('U') references only: a defined local symbol containing these
# letters is not a call out to the libc PATH resolvers.  Strip the @GLIBC_x.y
# version suffix first -- without that, `nm` prints "execlp@GLIBC_2.2.5" and an
# anchored match silently finds nothing.  (This bug made the check pass against
# v3.0.0 on its first run; the symbol was there all along.)
BAD=$(nm -D --undefined-only "$LIB" 2>/dev/null | \
	awk '{print $NF}' | sed 's/@.*//' | \
	grep -E '^(execlp|execvp|execvpe|system|popen)$' | sort -u || true)

if [[ -n $BAD ]]; then
	fail "libumem.so references PATH-resolving exec: $(echo "$BAD" | tr '\n' ' ')"
else
	pass "no execlp/execvp/execvpe/system/popen references"
fi

echo
if [[ $FAIL -eq 0 ]]; then
	echo "test_no_path_exec: PASS"
	exit 0
fi
echo "test_no_path_exec: FAIL"
exit 1

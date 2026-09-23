#!/usr/bin/env bash
# scripts/ec2/p5_6_9_prefix_demo.sh -- PRE-FIX demonstrations for P5.6, P5.7,
# P5.8 and P5.9.
#
# Runs INSIDE an isolated checkout of a committed ref (see verify-isolated.sh),
# reverts ONE fix at a time by patching the shipped source in place, rebuilds,
# and asserts the corresponding regression FAILS.  A hardening change whose
# regression cannot be shown to fail beforehand is not a demonstrated fix
# (AGENTS.md 7a), and reverting only one thing at a time is what keeps each
# "it fails" attributable to one defect.
#
# The patches restore the v3.0.0 code exactly; each is checked for having
# applied (a silently-unapplied patch would make the arm report a false
# pre-fix PASS, which is the failure mode this script exists to avoid).
#
# Usage: ./scripts/ec2/p5_6_9_prefix_demo.sh
# Exit 0 = every arm demonstrated its pre-fix failure.

set -uo pipefail

ROOT=$(pwd)
NPROC=$(nproc)
RESULT_DIR="$ROOT/docs/results"
mkdir -p "$RESULT_DIR"
LOG="$RESULT_DIR/$(date +%F)-p5.6-p5.9-prefix-demo-$(uname -m).txt"

fails=0
: > "$LOG"
say() { echo "$@" | tee -a "$LOG"; }

say "P5.6/P5.7/P5.8/P5.9 pre-fix demonstrations"
say "ref:  $(git rev-parse HEAD 2>/dev/null || echo '(archive, no git)')"
say "arch: $(uname -m)  $(gcc --version | head -1)"
say ""

# Snapshot the fixed sources so each arm starts from the same place.
cp getpcstack.c /tmp/getpcstack.fixed
cp malloc.c /tmp/malloc.fixed
cp umem_introspect.c /tmp/umem_introspect.fixed

restore() {
	cp /tmp/getpcstack.fixed getpcstack.c
	cp /tmp/malloc.fixed malloc.c
	cp /tmp/umem_introspect.fixed umem_introspect.c
}
trap restore EXIT

build() {
	make -j"$NPROC" >/tmp/build.log 2>&1 || {
		say "  BUILD FAILED; last lines:"
		tail -20 /tmp/build.log | tee -a "$LOG"
		return 1
	}
	return 0
}

# expect_fail <label> <test command...>
# The regression MUST exit non-zero.  Exit 77 (skip) is NOT a demonstration:
# a skipped test proves nothing, so it is reported as a failure of this script.
expect_fail() {
	local label=$1; shift
	local out rc
	out=$("$@" 2>&1); rc=$?
	if [[ $rc -eq 77 ]]; then
		say "  INCONCLUSIVE: $label SKIPPED (rc=77) -- proves nothing"
		say "$out" | sed 's/^/    /'
		fails=$((fails + 1))
	elif [[ $rc -ne 0 ]]; then
		say "  PRE-FIX FAILURE DEMONSTRATED: $label exits $rc"
		say "$out" | grep -E 'FAIL|MUTATED|signal|AddressSanitizer|unlink' \
		    | head -8 | sed 's/^/    /' | tee -a /dev/null
	else
		say "  NOT DEMONSTRATED: $label still PASSES with the fix reverted"
		say "$out" | tail -15 | sed 's/^/    /'
		fails=$((fails + 1))
	fi
}

# ===========================================================================
say "### P5.9 -- frame walk with no stack bounds"
say "Revert: drop the umem_stack_bounds() consultation, restoring the"
say "16 MiB-ceiling-only check that v3.0.0 had."
restore
# Make umem_stack_bounds() always report "unknown", which is exactly the
# pre-fix state: alignment + monotonic + 16 MiB ceiling and nothing else.
python3 - <<'PY'
import re
s = open('getpcstack.c').read()
# Force the "no bounds available" path for every caller.
old = "\tif (cached_hi == 0) {\n\t\tif (in_lookup)"
new = "\treturn (0);\t/* PREFIX DEMO: pretend bounds are unavailable */\n\tif (cached_hi == 0) {\n\t\tif (in_lookup)"
assert old in s, "P5.9 patch anchor not found"
s = s.replace(old, new, 1)
open('getpcstack.c','w').write(s)
PY
if grep -q 'PREFIX DEMO' getpcstack.c; then
	say "  patch applied"
	if build; then
		expect_fail "test_stack_bounds.sh" ./test/security/test_stack_bounds.sh
	else
		fails=$((fails + 1))
	fi
else
	say "  PATCH DID NOT APPLY -- arm is meaningless"; fails=$((fails + 1))
fi
say ""

# ===========================================================================
say "### P5.8 -- process_free() decodes and mutates before validating"
say "Revert: make umem_may_own() always say yes and move the stat-word"
say "poisoning back before the size check, as v3.0.0 had it."
restore
python3 - <<'PY'
s = open('malloc.c').read()
old = "\tif (a == 0 || len == 0)\n\t\treturn (0);"
new = "\treturn (1);\t/* PREFIX DEMO: no ownership check, as v3.0.0 */\n\tif (a == 0 || len == 0)\n\t\treturn (0);"
assert old in s, "P5.8 ownership anchor not found"
s = s.replace(old, new, 1)

# Restore the pre-fix write-before-validate: poison at the top of `validate:`,
# before the size/ownership test.
old2 = """validate:"""
new2 = """validate:
\t/* PREFIX DEMO: v3.0.0 poisoned the stat word inside the switch, i.e.
\t * before any size validation.  Same observable effect. */
\tif (do_free) {
\t\tint pi;
\t\tfor (pi = 0; pi < npoison; pi++)
\t\t\tpoison_lo[pi].malloc_stat = UMEM_FREE_PATTERN_32;
\t}"""
assert old2 in s, "P5.8 validate anchor not found"
s = s.replace(old2, new2, 1)
open('malloc.c','w').write(s)
PY
if [[ $(grep -c 'PREFIX DEMO' malloc.c) -eq 2 ]]; then
	say "  both patches applied"
	if build; then
		expect_fail "test_forged_free" ./test/security/test_forged_free
	else
		fails=$((fails + 1))
	fi
else
	say "  PATCHES DID NOT APPLY -- arm is meaningless"; fails=$((fails + 1))
fi
say ""

# ===========================================================================
# P5.6 and P5.7 need the control channel compiled in.
if ! grep -qE '^[[:space:]]*#[[:space:]]*define[[:space:]]+UMEM_INTROSPECT[[:space:]]+1' \
    config.h 2>/dev/null; then
	say "### P5.6/P5.7 -- SKIPPED: this build has no --enable-introspect"
	say "    (re-run this script from an --enable-introspect build)"
	fails=$((fails + 1))
else
	say "### P5.7 -- SO_PEERCRED accepted the real uid"
	say "Revert: put the getuid() term back in the authorization rule."
	restore
	python3 - <<'PY'
s = open('umem_introspect.c').read()
old = "\treturn (peer == euid || peer == 0);"
new = "\t/* PREFIX DEMO: v3.0.0's rule, with the real uid accepted. */\n\treturn (peer == getuid() || peer == euid || peer == 0);"
assert old in s, "P5.7 anchor not found"
s = s.replace(old, new, 1)
open('umem_introspect.c','w').write(s)
PY
	if grep -q 'PREFIX DEMO' umem_introspect.c; then
		say "  patch applied"
		if build; then
			# The unit test uses the LIVE getuid() as its "real uid",
			# so the reinstated getuid() term accepts that peer on any
			# machine -- arm 4 then fails.
			expect_fail "test_introspect_peer_uid" \
			    ./test/security/test_introspect_peer_uid
		else
			fails=$((fails + 1))
		fi
	else
		say "  PATCH DID NOT APPLY -- arm is meaningless"; fails=$((fails + 1))
	fi
	say ""

	say "### P5.6 -- predictable socket path and the stat/unlink TOCTOU"
	say "Revert: restore /tmp/umem.<pid>.sock and the"
	say "stat -> probe -> unlink -> bind reclaim sequence."
	restore
	python3 - <<'PY'
s = open('umem_introspect.c').read()

# 1. Predictable path in a shared directory.
old = """\tif (sock_dir(dir, sizeof (dir)) != 0)
\t\treturn (NULL);"""
new = """\t/* PREFIX DEMO: v3.0.0's predictable shared-directory path. */
\tsnprintf(buf, n, "/tmp/umem.%ld.sock", (long)getpid());
\treturn (buf);
\tif (sock_dir(dir, sizeof (dir)) != 0)
\t\treturn (NULL);"""
assert old in s, "P5.6 path anchor not found"
s = s.replace(old, new, 1)

# 2. stat() instead of lstat(), and unlink+rebind instead of rename.
old2 = "\t\tif (lstat(path, &sb) == 0 && S_ISSOCK(sb.st_mode) &&"
new2 = "\t\t/* PREFIX DEMO: stat() follows symlinks, as v3.0.0 did. */\n\t\tif (stat(path, &sb) == 0 && S_ISSOCK(sb.st_mode) &&"
assert old2 in s, "P5.6 lstat anchor not found"
s = s.replace(old2, new2, 1)

old3 = """\t\t\tif (!live)
\t\t\t\trc = rebind_over_stale(lfd, path);"""
new3 = """\t\t\tif (!live) {
\t\t\t\t/* PREFIX DEMO: v3.0.0's unlink-then-bind. */
\t\t\t\t(void) unlink(path);
\t\t\t\trc = bind(lfd, (struct sockaddr *)&addr,
\t\t\t\t    sizeof (addr));
\t\t\t}"""
assert old3 in s, "P5.6 unlink anchor not found"
s = s.replace(old3, new3, 1)
open('umem_introspect.c','w').write(s)
PY
	if [[ $(grep -c 'PREFIX DEMO' umem_introspect.c) -eq 3 ]]; then
		say "  all three patches applied"
		if build; then
			expect_fail "test_introspect_sock_path.sh" \
			    ./test/security/test_introspect_sock_path.sh
		else
			fails=$((fails + 1))
		fi
	else
		say "  PATCHES DID NOT APPLY -- arm is meaningless"; fails=$((fails + 1))
	fi
fi

restore
say ""
say "=========================================================="
if [[ $fails -eq 0 ]]; then
	say "ALL PRE-FIX FAILURES DEMONSTRATED"
	say "log: $LOG"
	exit 0
fi
say "$fails arm(s) did not demonstrate a pre-fix failure"
say "log: $LOG"
exit 1

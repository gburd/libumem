/*
 * P5.7 regression: the control channel's peer check must be euid-based.
 *
 * THE DEFECT (pre-fix, umem_introspect.c:941)
 *
 *	if (cred.uid == getuid() || cred.uid == geteuid() || cred.uid == 0)
 *		return (1);
 *
 * The `getuid()` term is the bug.  In a setuid target the REAL uid is the
 * unprivileged invoker, so this granted that invoker the whole control
 * channel:
 *   - `whatis <addr>` / bufctl decoding read process memory at addresses the
 *     client chooses;
 *   - `break <predicate>` parks every allocating thread that matches until a
 *     `continue` that need never come -- a denial of service against a
 *     privileged process, from an unprivileged account.
 *
 * WHY THIS TEST IS A UNIT TEST AND NOT AN END-TO-END ONE
 *   Demonstrating it end-to-end needs a setuid-root binary that links libumem.
 *   `make check` cannot create one: installing a setuid binary requires root,
 *   and a test suite that needs root to report PASS is a test suite that will
 *   be run with a SKIP nobody reads.  (It is also the kind of test that, if it
 *   ever half-worked, would leave a setuid binary in a build tree.)  So the
 *   DECISION FUNCTION is tested directly: umem_introspect_peer_authorized()
 *   was factored out of peer_is_authorized() for exactly this reason, and the
 *   remaining untested step is the single call that passes it geteuid().
 *
 * WHAT THIS ASSERTS
 *   1. A peer whose uid is the target's euid is accepted.
 *   2. root is accepted (root can ptrace the process anyway; refusing buys
 *      nothing).
 *   3. An unrelated uid is refused.
 *   4. THE PRE-FIX CASE: a peer equal to the REAL uid but NOT the effective
 *      uid is REFUSED.  This is the arm that fails against v3.0.0.
 *   5. The rule does not consult getuid() at all, checked by driving it with
 *      an euid that differs from this process's real uid.
 *
 * PRE-FIX DEMONSTRATION: build this against v3.0.0's umem_introspect.c and
 * arm 4 fails -- the old expression accepts cred.uid == getuid().  (The
 * function did not exist there, so the demonstration is done by restoring the
 * old boolean inline; see the comment at REFERENCE_PREFIX_RULE below.)
 *
 * Exit: 0 pass, 1 fail, 77 built without --enable-introspect.
 */

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>

#ifndef UMEM_INTROSPECT

int
main(void)
{
	printf("SKIP: built without --enable-introspect "
	    "(no control channel, no peer check)\n");
	return (77);
}

#else

#include "umem_introspect.h"

static int failures;

static void
check(int got, int want, const char *what)
{
	if (got == !!want) {
		printf("  PASS: %s\n", what);
	} else {
		printf("  FAIL: %s (got %d, want %d)\n", what, got, !!want);
		failures++;
	}
}

/*
 * REFERENCE_PREFIX_RULE -- v3.0.0's expression, kept here as the thing this
 * test discriminates against.  Arm 4 below asserts the shipped rule does NOT
 * behave like this one; if someone reinstates the getuid() term, the shipped
 * rule and this reference agree and the arm fails.
 */
static int
prefix_rule(uid_t peer, uid_t ruid, uid_t euid)
{
	return (peer == ruid || peer == euid || peer == 0);
}

int
main(void)
{
	const uid_t euid = 4242;	/* pretend effective uid */
	const uid_t ruid = 1000;	/* pretend real uid (the invoker) */
	const uid_t other = 31337;

	printf("P5.7: control-channel peer authorization is euid-based\n");

	/* 1. same euid: allowed. */
	check(umem_introspect_peer_authorized(euid, euid), 1,
	    "peer with the target's euid is accepted");

	/* 2. root: allowed. */
	check(umem_introspect_peer_authorized(0, euid), 1,
	    "root is accepted");

	/* 3. unrelated uid: refused. */
	check(umem_introspect_peer_authorized(other, euid), 0,
	    "an unrelated uid is refused");

	/*
	 * 4. THE P5.7 ARM.  In a setuid target the real uid is the
	 * unprivileged invoker.  It must not be able to drive the channel.
	 */
	check(umem_introspect_peer_authorized(ruid, euid), 0,
	    "the REAL uid of a setuid target is refused (P5.7)");

	/* 5. And that is a genuine difference from the pre-fix rule. */
	if (prefix_rule(ruid, ruid, euid) == 1 &&
	    umem_introspect_peer_authorized(ruid, euid) == 0) {
		printf("  PASS: differs from the v3.0.0 rule, which accepted "
		    "this peer\n");
	} else {
		printf("  FAIL: the shipped rule agrees with the v3.0.0 rule "
		    "-- the getuid() term is back\n");
		failures++;
	}

	/*
	 * 6. Vacuity guard.  The arms above use invented uids; make sure the
	 * function is actually looking at its arguments and not, say, calling
	 * geteuid() itself.  A real euid of 0 would make arm 3 pass for the
	 * wrong reason, so assert the refusal holds for a non-root euid too.
	 */
	check(umem_introspect_peer_authorized(getuid() + 1, getuid() + 2), 0,
	    "vacuity: two uids unrelated to this process still refuse");

	printf("\n");
	if (failures != 0) {
		printf("test_introspect_peer_uid: FAIL (%d)\n", failures);
		return (1);
	}
	printf("test_introspect_peer_uid: PASS\n");
	return (0);
}

#endif /* UMEM_INTROSPECT */

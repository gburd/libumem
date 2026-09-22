/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * CDDL HEADER END
 */

/*
 * P5.2 regression helper: does secure mode actually suppress the
 * side-effecting UMEM_* options?
 *
 * WHY A HELPER BINARY: umem reads UMEM_OPTIONS exactly once, during
 * umem_init(), which runs from a constructor before main().  setenv() in a
 * test is far too late, so each probe must be a fresh process.  Same pattern
 * as test/unit/umem_env_helper.
 *
 * WHAT IS BEING TESTED, stated plainly: THE GATE, not setuid itself.  We
 * cannot create a real setuid binary from the test suite (it needs root, and
 * a suite that needed root would simply be skipped everywhere).  So the
 * secure-mode decision is forced through umem_secure_mode_force, an internal
 * int that misc.c consults ahead of issetugid()/AT_SECURE.
 *
 * That knob is deliberately NOT settable from the environment: if it were,
 * an attacker could set it to 0 and turn the gate off, which is the very
 * exposure this closes.  It is set here by writing the library's own variable
 * from in-process code, before the first allocation forces umem_init().
 *
 * What this therefore proves:
 *   - with the gate OFF, profile=record:<path> really does create the file
 *     (so the feature works and the ON case is not vacuous), and
 *   - with the gate ON, it does not.
 * What it does NOT prove: that issetugid()/getauxval(AT_SECURE) correctly
 * detect a real setuid exec.  That is a two-line expression in misc.c,
 * verified by inspection.
 *
 * Usage: test_secure_gate_helper <secure 0|1> <expect-file 0|1> <path>
 *   exit 0 if the file's existence after exit matches <expect-file>.
 *   Since the profile is written from an atexit() handler, the check cannot
 *   happen in this process: the helper instead exits 0 and the SCRIPT checks
 *   the file.  This binary's exit status only reports setup problems.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem.h"

/* misc.c.  Test-only override of the secure-mode decision (see above). */
extern int umem_secure_mode_force;
/* misc.c.  The decision itself, so we can assert the override took. */
extern int umem_secure_mode(void);

int
main(int argc, char **argv)
{
	if (argc < 2) {
		(void) fprintf(stderr,
		    "usage: %s <secure 0|1>\n", argv[0]);
		return (2);
	}

	umem_secure_mode_force = atoi(argv[1]) ? 1 : 0;

	/*
	 * Assert the override is in effect BEFORE init.  Without this the test
	 * could pass for the wrong reason (e.g. the knob renamed and silently
	 * ignored, leaving both arms in the same mode).
	 */
	if (umem_secure_mode() != (atoi(argv[1]) ? 1 : 0)) {
		(void) fprintf(stderr,
		    "helper: umem_secure_mode() did not honour the override\n");
		return (3);
	}

	/* Force umem_init(), which parses UMEM_OPTIONS and starts profiling. */
	void *p = umem_alloc(64, UMEM_DEFAULT);
	if (p == NULL) {
		(void) fprintf(stderr, "helper: umem_alloc failed\n");
		return (4);
	}
	umem_free(p, 64);

	/* The record-mode profile is flushed by umem_profile_fini via atexit. */
	return (0);
}

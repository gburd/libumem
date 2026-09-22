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
 * P5.5 regression: a failed allocation must leave errno describing WHY.
 *
 * THE DEFECT (pre-fix, vmem_mmap.c)
 *   vmem_mmap_alloc() saved errno on entry and restored it on every exit,
 *   including failures.  Two distinct restores did this:
 *
 *     1. the MAP_FIXED failure branch (the one P5.5 named), and
 *     2. the function's final `errno = old_errno`, which is also reached with
 *        ret == NULL whenever vmem_alloc(src) fails -- and since src imports
 *        through vmem_mmap_top_alloc(), that IS the address-space-exhaustion
 *        path.  So the ENOMEM that v3.0.0 deliberately preserved in
 *        vmem_mmap_top_alloc() was overwritten one frame later, by its own
 *        caller.
 *
 *   v3.0.0 fixed vmem_mmap_top_alloc() only and its release notes said
 *   "failure paths now leave errno alone".  That was false for the sibling,
 *   and because of (2) the announced fix did not even take effect for the
 *   heap-ceiling failure it was written for ("FIRST FAILURE at 8269MB
 *   (errno=0 Success)", docs/results/2026-09-22-umem-heap-ceiling-vma.md).
 *
 * HOW THIS FORCES A REAL FAILURE
 *   setrlimit(RLIMIT_AS) caps the process's address space, so mmap() inside
 *   the backend genuinely fails with ENOMEM.  That is the same kernel refusal
 *   as the vm.max_map_count ceiling, reached without needing 8 GB of RAM or a
 *   particular sysctl -- so this runs anywhere, deterministically.
 *
 *   errno is set to a distinctive sentinel (EDOM) immediately before the
 *   allocation.  The assertion is not merely "errno is ENOMEM" but "errno is
 *   no longer the sentinel": if the library restores the entry value we see
 *   EDOM back, which is precisely the defect, and the failure message says so.
 *
 * VACUITY GUARD: if no allocation ever fails under the cap, the test SKIPs
 * (77) rather than passing.  An assertion that never ran is not evidence
 * (AGENTS.md 6).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "umem.h"

/* Big enough to force fresh spans from the backend, small enough that many
 * fit under the cap before it bites. */
#define CHUNK		(64 * 1024)
#define MAX_ALLOCS	200000
#define AS_CAP		(256UL * 1024 * 1024)

#define SENTINEL	EDOM	/* never produced by an allocation failure */

int
main(void)
{
	struct rlimit rl;

	/*
	 * Touch the allocator first: umem_init() itself maps a good deal, and
	 * capping before init can fail the constructor rather than an
	 * individual allocation, which tests nothing.
	 */
	void *warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm == NULL) {
		(void) fprintf(stderr, "SKIP: allocator unusable before cap\n");
		return (77);
	}
	umem_free(warm, 64);

	if (getrlimit(RLIMIT_AS, &rl) != 0) {
		(void) fprintf(stderr, "SKIP: getrlimit(RLIMIT_AS): %s\n",
		    strerror(errno));
		return (77);
	}
	if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < AS_CAP) {
		(void) fprintf(stderr, "SKIP: RLIMIT_AS hard cap already %lu\n",
		    (unsigned long)rl.rlim_max);
		return (77);
	}
	rl.rlim_cur = AS_CAP;
	if (setrlimit(RLIMIT_AS, &rl) != 0) {
		(void) fprintf(stderr, "SKIP: setrlimit(RLIMIT_AS): %s\n",
		    strerror(errno));
		return (77);
	}

	void **held = calloc(MAX_ALLOCS, sizeof (void *));
	if (held == NULL) {
		(void) fprintf(stderr, "SKIP: bookkeeping alloc failed\n");
		return (77);
	}

	int failed_at = -1;
	int observed_errno = 0;
	size_t n = 0;

	for (; n < MAX_ALLOCS; n++) {
		errno = SENTINEL;
		held[n] = umem_alloc(CHUNK, UMEM_DEFAULT);
		if (held[n] == NULL) {
			observed_errno = errno;
			failed_at = (int)n;
			break;
		}
	}

	if (failed_at < 0) {
		(void) fprintf(stderr,
		    "SKIP: no allocation failed under a %luMB RLIMIT_AS after "
		    "%zu x %dKB -- cannot test the failure path\n",
		    AS_CAP / (1024 * 1024), n, CHUNK / 1024);
		return (77);
	}

	(void) printf("first failure at allocation %d (%d MB requested), "
	    "errno=%d (%s)\n", failed_at,
	    (int)((size_t)failed_at * CHUNK / (1024 * 1024)),
	    observed_errno, strerror(observed_errno));

	int rc = 0;

	if (observed_errno == SENTINEL) {
		(void) printf("FAIL: errno was RESTORED to the pre-call "
		    "sentinel (%d) over a failed allocation -- this is the "
		    "P5.5 defect: the caller sees NULL with no reason\n",
		    SENTINEL);
		rc = 1;
	} else if (observed_errno == 0) {
		(void) printf("FAIL: errno is 0 after a failed allocation "
		    "(\"NULL, Success\")\n");
		rc = 1;
	} else if (observed_errno != ENOMEM) {
		/*
		 * Not automatically wrong -- EAGAIN is a legitimate umem
		 * failure code -- but under an address-space cap the honest
		 * answer is ENOMEM, and anything else means the reason came
		 * from somewhere other than the refusal we forced.
		 */
		(void) printf("FAIL: expected ENOMEM (%d) under RLIMIT_AS, "
		    "got %d (%s)\n", ENOMEM, observed_errno,
		    strerror(observed_errno));
		rc = 1;
	} else {
		(void) printf("PASS: errno is ENOMEM after a forced "
		    "vmem_mmap_alloc failure\n");
	}

	/*
	 * Deliberately not freeing `held`: the process is about to exit, the
	 * address space is capped, and a free loop here has itself caused
	 * secondary failures that obscure the result.
	 */
	return (rc);
}

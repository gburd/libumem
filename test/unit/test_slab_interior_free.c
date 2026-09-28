/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * test_slab_interior_free.c -- the CONSEQUENCE of the UMEM_SLAB_MEMBER bound
 * that 080ef6f fixed.
 *
 * test_slab_member.c showed the predicate accepted the byte past a slab's
 * last chunk.  This shows what that let through on the free path: before the
 * fix, umem_slab_free() classified a buffer by UMEM_SLAB_MEMBER (a range
 * test), not by a chunk start, so freeing an INTERIOR pointer of a live
 * buffer -- buf + chunksize/2, still inside the slab -- was accepted.  The
 * bufctl it derives from that address (UMEM_BUFCTL: buf + cache_bufctl) then
 * pointed into the middle of a live object, and the object went back on the
 * free list while its owner still held it: a later umem_cache_alloc() could
 * return it a second time.
 *
 * After 080ef6f umem_slab_free() uses UMEM_SLAB_CHUNK, which also requires
 * (buf - slab_base) % chunksize == 0, so an interior free is refused with
 * UMERR_BADADDR.
 *
 * HOW IT OBSERVES REFUSAL.  A refused free reaches umem_err_recoverable(),
 * which calls umem_do_abort() when umem_abort > 0 (the default; misc.h).
 * umem_abort_info is file-local to umem.c, so the test cannot read it; the
 * observable signal is the abort.  So each free runs in a forked child with
 * UMEM_OPTIONS=abort:
 *   - refused  -> child aborts (WIFSIGNALED, SIGABRT)  -> POST-fix behaviour
 *   - accepted -> child exits 0                        -> PRE-fix behaviour
 * A control frees a real chunk start and must NOT abort, in both builds, so
 * the test is not merely detecting "any abort".
 *
 * A cache with UMC_NOMAGAZINE sends every free straight to umem_slab_free
 * (no magazine, no PTC, no rseq layer in between), so the slab check is the
 * one under test.
 *
 * Exit: 0 the interior free was REFUSED (fixed); 1 it was ACCEPTED (the
 * pre-fix defect: the control also reports whether a second alloc aliased);
 * 99 harness broken; 77 skipped.
 *
 * _UMEM_INTERNAL build: reads umem_cache_t/umem_slab_t like
 * test_slab_member.c.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <umem.h>
#include "umem_base.h"
#include "umem_impl.h"

/*
 * Run one free in a child so an abort does not take the test down.
 * kind 0 = free a real chunk start (control, must NOT abort)
 * kind 1 = free buf+chunksize/2 of a live buffer (interior; the subject)
 * Returns: 1 = child aborted (free refused), 0 = child exited cleanly
 * (free accepted), -1 = harness error.
 */
static int
child_free_aborts(int kind)
{
	pid_t pid = fork();

	if (pid < 0)
		return (-1);
	if (pid == 0) {
		umem_cache_t *cp = umem_cache_create("t_interior", 200, 0,
		    NULL, NULL, NULL, NULL, NULL, UMC_NODEBUG | UMC_NOMAGAZINE);
		void *keep[8];
		int i;
		size_t chunk;
		void *victim, *target;

		if (cp == NULL)
			_exit(99);
		/* Hold several so the slab stays live across the free. */
		for (i = 0; i < 8; i++) {
			keep[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
			if (keep[i] == NULL)
				_exit(99);
		}
		chunk = cp->cache_chunksize;
		victim = keep[3];
		target = (kind == 0) ? victim
		    : (void *)((uintptr_t)victim + chunk / 2);
		/*
		 * The free under test.  If refused, umem aborts here and the
		 * child never reaches _exit(0).
		 */
		umem_cache_free(cp, target);
		_exit(0);
	}
	{
		int st;
		if (waitpid(pid, &st, 0) < 0)
			return (-1);
		if (WIFSIGNALED(st))
			return (1);		/* aborted: free refused */
		if (WIFEXITED(st) && WEXITSTATUS(st) == 99)
			return (-1);
		return (0);			/* exited: free accepted */
	}
}

/*
 * Second observation, run only when an interior free is ACCEPTED (pre-fix):
 * does a subsequent alloc alias the live buffer?  Done in one child so the
 * corrupt free list stays in that child.  Prints the two addresses.
 * Returns 1 if a later alloc landed on the victim's chunk, else 0/-1.
 */
static int
child_interior_aliases(void)
{
	pid_t pid = fork();

	if (pid < 0)
		return (-1);
	if (pid == 0) {
		umem_cache_t *cp = umem_cache_create("t_interior2", 200, 0,
		    NULL, NULL, NULL, NULL, NULL, UMC_NODEBUG | UMC_NOMAGAZINE);
		void *keep[8], *reload[16];
		int i;
		size_t chunk;
		uintptr_t vbase;

		if (cp == NULL)
			_exit(99);
		for (i = 0; i < 8; i++)
			if ((keep[i] = umem_cache_alloc(cp, UMEM_DEFAULT))
			    == NULL)
				_exit(99);
		chunk = cp->cache_chunksize;
		vbase = (uintptr_t)keep[3];
		umem_cache_free(cp, (void *)(vbase + chunk / 2));
		for (i = 0; i < 16; i++) {
			reload[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
			if (reload[i] != NULL &&
			    (uintptr_t)reload[i] == vbase) {
				fprintf(stderr,
				    "  ALIAS: alloc returned %p, still held "
				    "as keep[3]\n", reload[i]);
				_exit(0);
			}
		}
		_exit(2);		/* accepted, but no alias observed */
	}
	{
		int st;
		if (waitpid(pid, &st, 0) < 0)
			return (-1);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
			return (1);
		return (0);
	}
}

int
main(void)
{
	int ctrl, interior;

	/* Control: freeing a real chunk start must be accepted (no abort). */
	ctrl = child_free_aborts(0);
	if (ctrl < 0) {
		fprintf(stderr, "harness: control free failed\n");
		return (99);
	}
	if (ctrl == 1) {
		fprintf(stderr, "harness: a VALID free aborted -- the test "
		    "cannot distinguish refusal from a broken cache\n");
		return (99);
	}
	printf("control (free a chunk start): accepted, no abort -- ok\n");

	interior = child_free_aborts(1);
	if (interior < 0) {
		fprintf(stderr, "harness: interior free child failed\n");
		return (99);
	}
	if (interior == 1) {
		printf("interior free (buf + chunksize/2): REFUSED (abort)\n");
		printf("RESULT: PASS (umem_slab_free rejects an interior "
		    "pointer; UMEM_SLAB_CHUNK)\n");
		return (0);
	}

	/* Accepted: the pre-fix defect.  Show the consequence. */
	printf("interior free (buf + chunksize/2): ACCEPTED\n");
	if (child_interior_aliases() == 1)
		printf("RESULT: FAIL (interior free accepted AND a later alloc "
		    "aliased the live buffer -- double allocation)\n");
	else
		printf("RESULT: FAIL (interior free accepted; no alias seen "
		    "in 16 reallocs, but the free list took an interior "
		    "pointer)\n");
	return (1);
}

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
 * P5.4 regression: a slab freelist link inside a freed user buffer must not
 * be usable as an arbitrary-address primitive.
 *
 * THE EXPOSURE (attacker position D: controls allocation patterns and buffer
 * contents, not the environment and not the code).
 *
 * For every cache without UMF_HASH -- the default for small objects --
 * umem_cache_create() sets cache_bufctl = chunksize - UMEM_ALIGN
 * (umem.c), so UMEM_BUFCTL(cp, buf) lands on the last UMEM_ALIGN bytes of
 * the USER BUFFER.  bc_next, the slab freelist link, therefore lives inside
 * memory the application writes to.  umem_slab_free() stores sp->slab_head
 * there and umem_slab_alloc() pops it back off and hands the result out as a
 * fresh buffer.
 *
 * So an ordinary one-buffer heap overflow that runs off the end of a live
 * buffer into the tail of the adjacent FREED one overwrites bc_next, and the
 * allocation after next returns (attacker value - cache_bufctl): an
 * arbitrary address, chosen by the attacker, delivered by the allocator as a
 * normal allocation.  glibc has mangled these links since 2.32
 * (safe-linking); libumem did not, which made it WORSE than glibc for the
 * most common heap-bug class.
 *
 * WHAT THIS TEST DOES
 *
 *   attack    Allocate two adjacent buffers of one size class, free the
 *             second, then write a chosen value into the freed buffer's tail
 *             link -- exactly what an overflow from the first buffer does --
 *             and allocate again.  The allocator must not return the
 *             attacker's address.
 *             PRE-FIX: the second allocation IS the attacker's address.
 *
 *   detect    The same corruption must be REPORTED through umem_error()
 *             (UMERR_BADADDR, "invalid free: buffer not in cache" text plus
 *             "heap corruption detected") rather than silently dereferenced.
 *             Checked by capturing what the library writes to stderr with
 *             umem_output on.
 *
 * Both cases run with umem_abort = 0 so the recoverable error does not kill
 * the test process; with the default umem_abort = 1 the same corruption
 * aborts, which is the stronger response.
 *
 * Build the library with -DUMEM_NO_LINK_MANGLE to get the pre-fix behaviour
 * in an otherwise identical binary; this test then fails, which is what
 * makes it a regression and not a tautology, and is the control arm the
 * throughput A/B uses.
 *
 * Exit: 0 pass, 1 fail, 77 skip (cache shape not the one under test).
 */

#include "umem_base.h"		/* umem_cache_t, umem_abort, umem_output */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The address the attacker wants handed to them.  Static storage, aligned
 * generously so that pre-fix the allocator returns it exactly rather than
 * tripping over some unrelated alignment check on the way.
 */
static char attacker_target[256] __attribute__((aligned(64)));

#define	BUFSIZE		64

/*
 * Allocate two chunks that are adjacent in one slab, so the second one's
 * tail is what an overflow from the first one reaches.  Returns 0 on
 * success.  Both buffers belong to the caller afterwards.
 */
static int
alloc_adjacent_pair(umem_cache_t *cp, void **first, void **second)
{
	void *bufs[64];
	int n, i, j;
	size_t chunksize = cp->cache_chunksize;

	for (n = 0; n < 64; n++) {
		bufs[n] = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (bufs[n] == NULL)
			break;
	}
	if (n < 2) {
		for (i = 0; i < n; i++)
			umem_cache_free(cp, bufs[i]);
		return (-1);
	}

	/* Find any adjacent pair; free everything else. */
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			if (i == j || bufs[i] == NULL || bufs[j] == NULL)
				continue;
			if ((char *)bufs[j] == (char *)bufs[i] + chunksize) {
				*first = bufs[i];
				*second = bufs[j];
				bufs[i] = bufs[j] = NULL;
				for (i = 0; i < n; i++)
					if (bufs[i] != NULL)
						umem_cache_free(cp, bufs[i]);
				return (0);
			}
		}
	}

	for (i = 0; i < n; i++)
		if (bufs[i] != NULL)
			umem_cache_free(cp, bufs[i]);
	return (-1);
}

/*
 * The cache shape the exposure needs: no UMF_HASH (bufctl inside the
 * buffer), no magazine layer (so a free reaches the slab freelist directly
 * instead of stopping in a magazine), and a bufctl offset that really is
 * inside the user buffer.
 */
static umem_cache_t *
make_victim_cache(const char *name)
{
	umem_cache_t *cp = umem_cache_create(name, BUFSIZE, 0,
	    NULL, NULL, NULL, NULL, NULL, UMC_NOMAGAZINE);

	if (cp == NULL)
		return (NULL);
	if ((cp->cache_flags & UMF_HASH) != 0 ||
	    cp->cache_bufctl == 0 || cp->cache_bufctl >= BUFSIZE) {
		printf("SKIP: cache_flags=0x%x cache_bufctl=%lu: not the "
		    "in-buffer-bufctl shape this test targets\n",
		    cp->cache_flags, (unsigned long)cp->cache_bufctl);
		umem_cache_destroy(cp);
		return (NULL);
	}
	return (cp);
}

/*
 * Case "attack": the allocator must not hand back the attacker's address.
 */
static int
run_attack(void)
{
	umem_cache_t *cp;
	void *first = NULL, *second = NULL;
	void **link;
	void *want, *p1, *p2;
	int rc = 0;

	cp = make_victim_cache("p54_attack");
	if (cp == NULL)
		return (77);

	if (alloc_adjacent_pair(cp, &first, &second) != 0) {
		printf("SKIP: could not obtain two adjacent chunks\n");
		umem_cache_destroy(cp);
		return (77);
	}

	/* Free the second buffer: its tail now holds the freelist link. */
	umem_cache_free(cp, second);

	/*
	 * The overflow.  A write that runs past the end of `first` lands
	 * here; writing it directly is the same bytes with less ceremony.
	 * The allocator returns UMEM_BUF(cp, bcp) == bcp - cache_bufctl, so
	 * aim the link at attacker_target + cache_bufctl.
	 */
	link = (void **)((char *)second + cp->cache_bufctl);
	*link = (void *)(attacker_target + cp->cache_bufctl);
	want = (void *)attacker_target;

	/*
	 * First allocation pops the corrupted bufctl (returns `second`
	 * pre-fix, NULL post-fix because the bad link is caught here);
	 * the second one is the arbitrary-address allocation.
	 */
	p1 = umem_cache_alloc(cp, UMEM_DEFAULT);
	p2 = umem_cache_alloc(cp, UMEM_DEFAULT);

	printf("  attack: target=%p  alloc1=%p  alloc2=%p\n",
	    want, p1, p2);

	if (p1 == want || p2 == want) {
		printf("FAIL: allocator returned the attacker-chosen address "
		    "%p (arbitrary-address write primitive)\n", want);
		rc = 1;
	} else {
		printf("ok: attacker-chosen address %p was not returned\n",
		    want);
	}

	/*
	 * Deliberately no cleanup of p1/p2/first and no umem_cache_destroy:
	 * the cache's freelist has been corrupted on purpose, and walking it
	 * again to be tidy would just re-report the corruption.  The process
	 * is about to exit.
	 */
	return (rc);
}

/*
 * Case "inslab": the target is INSIDE the victim slab, so containment passes
 * and only the mangling stands between the overwrite and a chosen-address
 * return.
 *
 * WHY THIS CASE EXISTS.  The "attack" case above aims at a static in the test
 * binary, far outside the slab, and umem_slab_link_valid()'s containment check
 * rejects it before the mangling is ever consulted -- verified by building with
 * -DUMEM_NO_LINK_MANGLE: "attack" still PASSES.  So "attack" proves the
 * combination of controls works; it does not prove mangling does.  This case
 * does: it must FAIL with mangling compiled out and PASS by default.
 *
 * THE TARGET is `first`, the live neighbour.  It is inside the slab (so
 * containment passes), UMEM_ALIGN-aligned (so the alignment check passes),
 * and currently allocated -- handing it out again is a double allocation, the
 * exact primitive a heap exploit wants.  Pre-fix/unmangled, an overwrite of
 * the freed neighbour's link with (first + cache_bufctl) makes the allocation
 * after next return `first` while it is still live.  Mangled, the same
 * overwritten bytes demangle to garbage that almost surely fails containment
 * or alignment and is reported instead.
 */
static int
run_inslab(void)
{
	umem_cache_t *cp;
	void *first = NULL, *second = NULL;
	void **link;
	void *p1, *p2;
	int rc = 0;

	cp = make_victim_cache("p54_inslab");
	if (cp == NULL)
		return (77);

	if (alloc_adjacent_pair(cp, &first, &second) != 0) {
		printf("SKIP: could not obtain two adjacent chunks\n");
		umem_cache_destroy(cp);
		return (77);
	}

	umem_cache_free(cp, second);

	/*
	 * Point the freed buffer's link at the LIVE neighbour.  UMEM_BUF()
	 * subtracts cache_bufctl, so aim at first + cache_bufctl to have the
	 * allocator hand back `first` itself.
	 */
	link = (void **)((char *)second + cp->cache_bufctl);
	*link = (void *)((char *)first + cp->cache_bufctl);

	p1 = umem_cache_alloc(cp, UMEM_DEFAULT);	/* pops `second` */
	p2 = umem_cache_alloc(cp, UMEM_DEFAULT);	/* follows the link */

	printf("  inslab: live=%p  alloc1=%p  alloc2=%p\n", first, p1, p2);

	if (p1 == first || p2 == first) {
		printf("FAIL: allocator returned the LIVE neighbour %p "
		    "(double allocation via an in-slab freelist overwrite; "
		    "containment cannot catch this, only mangling can)\n",
		    first);
		rc = 1;
	} else {
		printf("ok: in-slab target %p was not returned; mangling "
		    "defeated a containment-passing overwrite\n", first);
	}

	/* Corrupted cache, no cleanup -- see run_attack. */
	return (rc);
}

/*
 * Case "detect": the corruption must be reported, not dereferenced.
 *
 * Captures fd 2 into a temp file, because that is where umem_error() ->
 * umem_err_recoverable() -> umem_error_enter() writes when umem_output is
 * set, and it needs no access to any internal symbol beyond the two
 * tunables.
 */
static int
run_detect(void)
{
	umem_cache_t *cp;
	void *first = NULL, *second = NULL;
	void **link;
	char path[] = "/tmp/umem_p54_detectXXXXXX";
	char buf[8192];
	int fd, saved, n, rc = 0;

	cp = make_victim_cache("p54_detect");
	if (cp == NULL)
		return (77);

	if (alloc_adjacent_pair(cp, &first, &second) != 0) {
		printf("SKIP: could not obtain two adjacent chunks\n");
		umem_cache_destroy(cp);
		return (77);
	}

	umem_cache_free(cp, second);
	link = (void **)((char *)second + cp->cache_bufctl);
	*link = (void *)(attacker_target + cp->cache_bufctl);

	if ((fd = mkstemp(path)) < 0) {
		printf("SKIP: mkstemp failed\n");
		return (77);
	}
	(void) unlink(path);

	umem_output = 1;		/* send error text to fd 2 */
	saved = dup(2);
	(void) fflush(stdout);
	(void) dup2(fd, 2);

	(void) umem_cache_alloc(cp, UMEM_DEFAULT);

	(void) dup2(saved, 2);
	(void) close(saved);
	umem_output = 0;

	(void) lseek(fd, 0, SEEK_SET);
	n = (int)read(fd, buf, sizeof (buf) - 1);
	(void) close(fd);
	if (n < 0)
		n = 0;
	buf[n] = '\0';

	if (strstr(buf, "umem allocator:") != NULL &&
	    strstr(buf, "heap corruption detected") != NULL) {
		printf("ok: corruption reported through umem_error:\n    %.*s\n",
		    120, buf);
	} else {
		printf("FAIL: corrupted freelist link was not reported "
		    "through umem_error (captured %d bytes: \"%.200s\")\n",
		    n, buf);
		rc = 1;
	}

	/* Same reasoning as run_attack: no cleanup of a corrupted cache. */
	return (rc);
}

int
main(int argc, char **argv)
{
	const char *which = (argc > 1) ? argv[1] : "all";
	void *warm;
	int rc = 0, r;

	/* Force init so env-derived flags are in effect before we look. */
	warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, 64);

	/*
	 * A recoverable error must not abort the test process: the point is
	 * to observe what the allocator returns and reports, not to observe
	 * SIGABRT.  Default behaviour (umem_abort = 1) is to abort, which is
	 * strictly stronger.
	 */
	umem_abort = 0;

	if (strcmp(which, "attack") == 0)
		return (run_attack());
	if (strcmp(which, "detect") == 0)
		return (run_detect());
	if (strcmp(which, "inslab") == 0)
		return (run_inslab());
	if (strcmp(which, "all") != 0) {
		printf("usage: %s [attack|detect|inslab|all]\n", argv[0]);
		return (2);
	}

	/*
	 * Each case corrupts its own cache, so they are independent and can
	 * share a process.  A SKIP from either is reported but does not mask
	 * a failure from the other.
	 */
	if ((r = run_attack()) == 1)
		rc = 1;
	else if (r == 77 && rc == 0)
		rc = 77;
	if ((r = run_detect()) == 1)
		rc = 1;
	else if (r == 77 && rc == 0)
		rc = 77;
	if ((r = run_inslab()) == 1)
		rc = 1;
	else if (r == 77 && rc == 0)
		rc = 77;

	if (rc == 0)
		printf("PASS: freelist links are not an arbitrary-address "
		    "primitive\n");
	return (rc);
}

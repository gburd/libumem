/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * P1.1 regression: interposer calloc() storage ownership under concurrency.
 *
 * This program must be run with the interposer active
 * (LD_PRELOAD=libumem_malloc.so); see test/stress/interpose_regress.sh.
 * Run without LD_PRELOAD it exercises the platform allocator and acts as a
 * control (it must pass there too).
 *
 * THE DEFECT IT REPRODUCES
 *
 *   malloc_interpose.c used a process-global `in_calloc` flag as a
 *   *recursion* guard and set it on every ordinary calloc().  Thread A
 *   setting the flag therefore made every concurrent thread B believe it was
 *   recursing, so B was served out of a shared 2 KiB static bump buffer
 *   (`calloc_buffer`).  When A finished it reset `calloc_buffer_used = 0`
 *   while B's allocation was still live, so a later calloc() handed out --
 *   and memset() -- the exact same bytes.  Two live allocations, one
 *   address range.
 *
 * THE ORACLE
 *
 *   Two independent checks, either of which fails on the defect:
 *
 *   1. OVERLAP.  Every live allocation is registered in a shared table
 *      (fixed-size, no allocation of its own) as [base, base+size).  A new
 *      allocation that intersects any live entry is a hard failure -- that is
 *      the defect stated directly, with no reliance on timing of the
 *      corruption becoming visible.
 *
 *   2. CONTENT INTEGRITY.  Each allocation is filled with a byte pattern
 *      unique to (thread, sequence), kept live across further
 *      calloc/realloc/free churn by other threads, and re-read before
 *      release.  A byte that changed means someone else wrote into our
 *      allocation.
 *
 *   calloc()'s zero guarantee is checked too: the whole allocation must read
 *   as zero before we write our pattern.
 *
 *   The overlap table deliberately does NOT use malloc: under LD_PRELOAD the
 *   test's own bookkeeping would otherwise re-enter the allocator being
 *   tested and could mask or perturb the race.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_THREADS	64
#define LIVE_PER_THREAD	12
#define MAX_LIVE	(MAX_THREADS * LIVE_PER_THREAD)

static int	g_threads = 16;
static long	g_iters = 20000;
static int	g_churn = 1;

/* ---------------------------------------------------------------- oracle 1 */

/*
 * Live-allocation registry.  Protected by g_reg_lock; entries with base ==
 * NULL are free slots.  Statically sized so the oracle never allocates.
 */
struct live_ent {
	unsigned char	*base;
	size_t		size;
	int		owner;		/* thread index, for diagnostics */
	unsigned char	pattern;
};

static struct live_ent	g_live[MAX_LIVE];
static pthread_mutex_t	g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static atomic_long	g_overlaps;
static atomic_long	g_corruptions;
static atomic_long	g_nonzero;	/* calloc returned non-zeroed storage */
static atomic_long	g_allocs;
static atomic_long	g_failures;	/* calloc/realloc returned NULL */
static atomic_long	g_reg_full;

/* Report at most a few of each kind; a race can produce thousands. */
static void
complain(const char *what, const void *a, size_t asz, int aown,
    const void *b, size_t bsz, int bown)
{
	static atomic_int shown;

	if (atomic_fetch_add(&shown, 1) >= 8)
		return;
	(void) fprintf(stderr,
	    "FAIL(%s): [%p,+%zu) owner=%d overlaps live [%p,+%zu) owner=%d\n",
	    what, a, asz, aown, b, bsz, bown);
}

/*
 * Register [base,base+size) as live.  Records an overlap failure if it
 * intersects any other live allocation.  Returns the slot index, or -1 if
 * the table is full (counted, not fatal).
 */
static int
live_insert(void *base, size_t size, int owner, unsigned char pattern)
{
	int i, slot = -1;

	(void) pthread_mutex_lock(&g_reg_lock);
	for (i = 0; i < MAX_LIVE; i++) {
		if (g_live[i].base == NULL) {
			if (slot < 0)
				slot = i;
			continue;
		}
		/* Half-open interval intersection. */
		if ((unsigned char *)base < g_live[i].base + g_live[i].size &&
		    g_live[i].base < (unsigned char *)base + size) {
			atomic_fetch_add(&g_overlaps, 1);
			complain("overlap", base, size, owner,
			    g_live[i].base, g_live[i].size, g_live[i].owner);
		}
	}
	if (slot >= 0) {
		g_live[slot].base = base;
		g_live[slot].size = size;
		g_live[slot].owner = owner;
		g_live[slot].pattern = pattern;
	} else {
		atomic_fetch_add(&g_reg_full, 1);
	}
	(void) pthread_mutex_unlock(&g_reg_lock);
	return (slot);
}

static void
live_remove(int slot)
{
	if (slot < 0)
		return;
	(void) pthread_mutex_lock(&g_reg_lock);
	g_live[slot].base = NULL;
	g_live[slot].size = 0;
	(void) pthread_mutex_unlock(&g_reg_lock);
}

static void
live_update(int slot, void *base, size_t size)
{
	if (slot < 0)
		return;
	(void) pthread_mutex_lock(&g_reg_lock);
	g_live[slot].base = base;
	g_live[slot].size = size;
	(void) pthread_mutex_unlock(&g_reg_lock);
}

/* ---------------------------------------------------------------- oracle 2 */

static void
check_zeroed(const unsigned char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (p[i] != 0) {
			atomic_fetch_add(&g_nonzero, 1);
			complain("calloc-not-zeroed", p, n, -1, p + i, 1, -1);
			return;
		}
	}
}

static void
check_pattern(const unsigned char *p, size_t n, unsigned char pat, int owner)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (p[i] != pat) {
			atomic_fetch_add(&g_corruptions, 1);
			complain("content-clobbered", p, n, owner, p + i, 1,
			    (int)p[i]);
			return;
		}
	}
}

/* ----------------------------------------------------------------- workload */

struct slot {
	unsigned char	*p;
	size_t		size;
	unsigned char	pat;
	int		reg;
};

struct targ {
	int	idx;
	long	iters;
};

/* Sizes chosen to straddle the old 2 KiB static buffer's granularity. */
static const size_t g_sizes[] = { 8, 16, 24, 48, 96, 130, 256, 700, 1500 };
#define NSIZES (sizeof (g_sizes) / sizeof (g_sizes[0]))

static void
release(struct slot *s)
{
	if (s->p == NULL)
		return;
	check_pattern(s->p, s->size, s->pat, -1);
	live_remove(s->reg);
	free(s->p);
	s->p = NULL;
	s->size = 0;
	s->reg = -1;
}

static void *
worker(void *arg)
{
	struct targ *ta = arg;
	struct slot live[LIVE_PER_THREAD];
	unsigned int seed = (unsigned int)(ta->idx * 2654435761u + 12345u);
	long it;
	int i;

	for (i = 0; i < LIVE_PER_THREAD; i++) {
		live[i].p = NULL;
		live[i].reg = -1;
	}

	for (it = 0; it < ta->iters; it++) {
		int k = (int)(rand_r(&seed) % LIVE_PER_THREAD);
		struct slot *s = &live[k];
		size_t sz;
		unsigned char pat;

		/* Evict whatever is there, verifying it first. */
		release(s);

		sz = g_sizes[rand_r(&seed) % NSIZES];
		s->p = calloc(1, sz);
		atomic_fetch_add(&g_allocs, 1);
		if (s->p == NULL) {
			atomic_fetch_add(&g_failures, 1);
			continue;
		}

		/* calloc contract: zeroed storage. */
		check_zeroed(s->p, sz);

		pat = (unsigned char)(0x40 + (ta->idx & 0x3f));
		if (pat == 0)
			pat = 0xa5;
		memset(s->p, pat, sz);
		s->size = sz;
		s->pat = pat;
		s->reg = live_insert(s->p, sz, ta->idx, pat);

		/*
		 * Every few iterations, grow one live allocation via realloc.
		 * realloc must preserve our bytes and must hand back storage
		 * that free() also recognizes.
		 */
		if ((it % 7) == 0) {
			size_t nsz = sz + 64;
			unsigned char *np = realloc(s->p, nsz);

			if (np == NULL) {
				atomic_fetch_add(&g_failures, 1);
			} else {
				check_pattern(np, sz, pat, ta->idx);
				memset(np + sz, pat, nsz - sz);
				s->p = np;
				s->size = nsz;
				live_update(s->reg, np, nsz);
			}
		}

		/*
		 * Re-verify a different live slot, giving other threads a
		 * window in which to clobber it.
		 */
		{
			struct slot *o = &live[(k + 1) % LIVE_PER_THREAD];

			if (o->p != NULL)
				check_pattern(o->p, o->size, o->pat, ta->idx);
		}
	}

	for (i = 0; i < LIVE_PER_THREAD; i++)
		release(&live[i]);

	return (NULL);
}

/*
 * Thread churn: pthread_create()/join() is the path that drives the real
 * TLS-setup calloc() recursion the interposer's guard exists for, so the
 * regression must exercise it concurrently with the calloc workload rather
 * than only at startup.
 */
static void *
churn_worker(void *arg)
{
	long n = (long)(intptr_t)arg;
	long i;

	for (i = 0; i < n; i++) {
		void *p = calloc(1, 64);

		if (p == NULL)
			atomic_fetch_add(&g_failures, 1);
		free(p);
	}
	return (NULL);
}

static int
run_churn(int rounds, int per_round)
{
	int r, i;

	for (r = 0; r < rounds; r++) {
		pthread_t t[8];
		int n = per_round > 8 ? 8 : per_round;

		for (i = 0; i < n; i++) {
			if (pthread_create(&t[i], NULL, churn_worker,
			    (void *)(intptr_t)200) != 0) {
				(void) fprintf(stderr,
				    "FAIL: pthread_create failed in churn "
				    "round %d (%s)\n", r, strerror(errno));
				while (--i >= 0)
					(void) pthread_join(t[i], NULL);
				return (-1);
			}
		}
		for (i = 0; i < n; i++)
			(void) pthread_join(t[i], NULL);
	}
	return (0);
}

int
main(int argc, char **argv)
{
	pthread_t tid[MAX_THREADS];
	struct targ ta[MAX_THREADS];
	int i, rc = 0;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--threads=", 10) == 0)
			g_threads = atoi(argv[i] + 10);
		else if (strncmp(argv[i], "--iters=", 8) == 0)
			g_iters = atol(argv[i] + 8);
		else if (strcmp(argv[i], "--no-churn") == 0) {
			/*
			 * The thread-churn phase is the harshest expression of
			 * the defect -- pre-fix it exhausts the shared buffer and
			 * pthread_create() dies with "cannot allocate memory for
			 * thread-local data" -- but that abort happens before the
			 * overlap oracle gets to run.  This flag skips it so the
			 * overlap/corruption oracle can be observed directly.
			 */
			g_churn = 0;
		} else {
			(void) fprintf(stderr,
			    "usage: %s [--threads=N] [--iters=N] [--no-churn]\n",
			    argv[0]);
			return (2);
		}
	}
	if (g_threads < 2)
		g_threads = 2;
	if (g_threads > MAX_THREADS)
		g_threads = MAX_THREADS;

	(void) printf("calloc-interpose-race: threads=%d iters=%ld\n",
	    g_threads, g_iters);
	(void) fflush(stdout);

	if (g_churn && run_churn(3, 8) != 0)
		rc = 1;

	for (i = 0; i < g_threads; i++) {
		ta[i].idx = i;
		ta[i].iters = g_iters;
		if (pthread_create(&tid[i], NULL, worker, &ta[i]) != 0) {
			(void) fprintf(stderr,
			    "FAIL: pthread_create %d failed: %s\n", i,
			    strerror(errno));
			g_threads = i;
			rc = 1;
			break;
		}
	}
	for (i = 0; i < g_threads; i++)
		(void) pthread_join(tid[i], NULL);

	if (g_churn && run_churn(3, 8) != 0)
		rc = 1;

	(void) printf("  allocs=%ld overlaps=%ld clobbered=%ld "
	    "not-zeroed=%ld alloc-failures=%ld registry-full=%ld\n",
	    atomic_load(&g_allocs), atomic_load(&g_overlaps),
	    atomic_load(&g_corruptions), atomic_load(&g_nonzero),
	    atomic_load(&g_failures), atomic_load(&g_reg_full));

	if (atomic_load(&g_overlaps) != 0) {
		(void) printf("FAIL: %ld overlapping live allocations\n",
		    atomic_load(&g_overlaps));
		rc = 1;
	}
	if (atomic_load(&g_corruptions) != 0) {
		(void) printf("FAIL: %ld clobbered allocations\n",
		    atomic_load(&g_corruptions));
		rc = 1;
	}
	if (atomic_load(&g_nonzero) != 0) {
		(void) printf("FAIL: %ld calloc results were not zeroed\n",
		    atomic_load(&g_nonzero));
		rc = 1;
	}
	/*
	 * An allocation failure here is a failure, not a skip: these are
	 * small requests on an unloaded machine.  The pre-fix code returned
	 * NULL once the shared 2 KiB buffer was exhausted.
	 */
	if (atomic_load(&g_failures) != 0) {
		(void) printf("FAIL: %ld allocation failures for small "
		    "requests\n", atomic_load(&g_failures));
		rc = 1;
	}
	if (atomic_load(&g_reg_full) != 0) {
		(void) printf("FAIL: oracle registry overflowed (%ld); the "
		    "oracle was not fully applied\n",
		    atomic_load(&g_reg_full));
		rc = 1;
	}

	(void) printf("%s\n", rc == 0 ? "PASS" : "FAIL");
	return (rc);
}

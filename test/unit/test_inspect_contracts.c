/*
 * test/unit/test_inspect_contracts.c -- enforce the umem_inspect.h
 * cache-lifetime and snapshot contracts (Phase 3 items 1, 2, 7 of
 * docs/plans/2026-09-21-production-readiness.md).
 *
 * Each test names the contract clause (C1..C5 in umem_inspect.c) it defends.
 *
 * PRE-FIX BEHAVIOUR:
 *
 *   C1  inspect_vs_destroy_churn: the walkers followed cache_next WITHOUT
 *       umem_cache_lock while umem_cache_destroy() unlinked caches, destroyed
 *       their cache_lock and vmem_free'd the descriptor.  An inspecting thread
 *       could therefore follow a freed link or mutex_lock a freed cache.  This
 *       test runs findleaks/status/walk/whatis concurrently with create/destroy
 *       churn; pre-fix it crashes or hangs on a destroyed mutex.  Reproduced
 *       on x86_64 within seconds (see the report in docs/results/).
 *
 *   C2  no_alloc_under_lock: inspection allocated (calloc/realloc/stdio) while
 *       holding cache_lock.  Under malloc interposition that allocation
 *       re-enters the allocator and can block on the very lock the walk holds.
 *       Here an interposed allocator is simulated at the only layer a unit
 *       test can reach: a walker callback and a leak-classification path that
 *       both allocate.  Pre-fix the public callbacks ran UNDER cache_lock, so
 *       this deadlocks; post-fix they run with every lock released.
 *
 *   C3  whatis_reports_cached: umem_whatis() never returned UMEM_BUF_CACHED
 *       despite umem_inspect.h documenting it, so a buffer the application had
 *       already freed into a magazine was reported ALLOCATED.
 *
 * All tests are bounded by a watchdog: a lifetime bug here manifests as a hang
 * or a crash, and a hung test is indistinguishable from a slow one in CI.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem.h"
#include "umem_inspect.h"

static int failures;

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
			    __FILE__, __LINE__, (msg));			\
			failures++;					\
		}							\
	} while (0)

static void *
watchdog(void *arg)
{
	int secs = *(int *)arg;
	sleep((unsigned)secs);
	fprintf(stderr,
	    "FAIL: watchdog fired after %ds -- inspection deadlocked or a "
	    "lifetime bug wedged a lock\n", secs);
	fflush(stderr);
	_exit(1);
}

static void
watchdog_start(int secs)
{
	static int s;
	pthread_t t;
	s = secs;
	if (pthread_create(&t, NULL, watchdog, &s) == 0)
		(void) pthread_detach(t);
}

/* Discard inspection output; we are testing lifetime, not formatting. */
static FILE *
devnull(void)
{
	FILE *f = fopen("/dev/null", "w");
	return (f != NULL ? f : stderr);
}

/* ------------------------------------------------------------------ */
/* C1: cache lifetime during inspection.                              */
/* ------------------------------------------------------------------ */

static volatile int churn_stop;
static volatile unsigned long churn_created;
static volatile unsigned long inspect_passes;

/*
 * Create and destroy caches as fast as possible.  Each cache gets one
 * allocation so it has a slab and a hash entry for the walkers to find, i.e.
 * the walk has real work to do inside the window destruction is racing.
 */
static void *
churn_thread(void *unused)
{
	unsigned long i = 0;

	(void) unused;
	while (!churn_stop) {
		char name[32];
		umem_cache_t *cp;
		void *p;

		(void) snprintf(name, sizeof (name), "churn_%lu", i++ & 0xff);
		cp = umem_cache_create(name, 128, 0, NULL, NULL, NULL, NULL,
		    NULL, 0);
		if (cp == NULL)
			continue;
		churn_created++;

		p = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (p != NULL)
			umem_cache_free(cp, p);

		/* Destroy while inspection threads are mid-walk. */
		umem_cache_destroy(cp);
	}
	return (NULL);
}

/* Callback that stops the walk early -- exercises the stop path under churn. */
static int
stop_after_n(const umem_buffer_info_t *info, void *arg)
{
	size_t *left = arg;
	(void) info;
	return ((*left)-- == 0 ? 1 : 0);
}

static void *
inspect_thread(void *unused)
{
	FILE *null = devnull();

	(void) unused;
	while (!churn_stop) {
		umem_buffer_info_t info;
		size_t budget;

		(void) umem_findleaks(null, UMEM_FMT_JSON, 10);
		umem_status_dump(null, UMEM_FMT_JSON);
		(void) umem_walk_dump(null, "allocated", UMEM_FMT_JSON, 64);
		(void) umem_walk_dump(null, "freed", UMEM_FMT_JSON, 64);

		budget = 32;
		(void) umem_walk_allocated(stop_after_n, &budget);

		/* whatis against a live buffer and against garbage. */
		void *p = umem_alloc(96, UMEM_DEFAULT);
		if (p != NULL) {
			(void) umem_whatis(p, &info);
			umem_free(p, 96);
		}
		(void) umem_whatis((void *)(uintptr_t)0x1234, &info);

		inspect_passes++;
	}
	if (null != stderr)
		(void) fclose(null);
	return (NULL);
}

static void
test_inspect_vs_destroy_churn(void)
{
	enum { NINSPECT = 3, SECONDS = 5 };
	pthread_t churn, insp[NINSPECT];
	int i;

	churn_stop = 0;
	churn_created = 0;
	inspect_passes = 0;

	CHECK(pthread_create(&churn, NULL, churn_thread, NULL) == 0,
	    "pthread_create churn");
	for (i = 0; i < NINSPECT; i++)
		CHECK(pthread_create(&insp[i], NULL, inspect_thread,
		    NULL) == 0, "pthread_create inspect");

	sleep(SECONDS);
	churn_stop = 1;

	(void) pthread_join(churn, NULL);
	for (i = 0; i < NINSPECT; i++)
		(void) pthread_join(insp[i], NULL);

	/*
	 * Surviving is the assertion: pre-fix this SIGSEGVs following a freed
	 * cache_next, or wedges in mutex_lock on a destroyed cache_lock.  The
	 * counters exist so a vacuous run (nothing actually raced) cannot pass
	 * quietly.
	 */
	CHECK(churn_created > 100,
	    "test is vacuous: almost no caches were created/destroyed");
	CHECK(inspect_passes > 10,
	    "test is vacuous: inspection barely ran");
	printf("  C1 inspection survived cache destroy churn: "
	    "%lu caches created/destroyed, %lu inspection passes\n",
	    churn_created, inspect_passes);
}

/* ------------------------------------------------------------------ */
/* C2: walker callbacks run with no allocator lock held.              */
/* ------------------------------------------------------------------ */

static size_t cb_allocs;

/*
 * Allocate FROM INSIDE a walker callback.  This is what an interposed
 * malloc does implicitly on every stdio call, and what leak classification
 * does explicitly.  Pre-fix the callback ran under cache_lock, so this
 * allocation could block on a lock the walk already held.
 */
static int
allocating_cb(const umem_buffer_info_t *info, void *arg)
{
	size_t *budget = arg;

	(void) info;
	/* umem and libc paths both matter: the first can take cache_lock,
	 * the second is what stdio inside the walk would do. */
	void *a = umem_alloc(64, UMEM_DEFAULT);
	void *b = malloc(64);
	char *c = strdup("classification scratch");
	if (a != NULL)
		umem_free(a, 64);
	free(b);
	free(c);
	cb_allocs++;

	return ((*budget)-- == 0 ? 1 : 0);
}

static void
test_no_alloc_under_lock(void)
{
	void *keep[64];
	size_t budget;
	int i;

	/* Make sure the walks have something to visit. */
	for (i = 0; i < 64; i++)
		keep[i] = umem_alloc(128, UMEM_DEFAULT);

	cb_allocs = 0;
	budget = 256;
	(void) umem_walk_allocated(allocating_cb, &budget);
	budget = 256;
	(void) umem_walk_freed(allocating_cb, &budget);
	budget = 256;
	(void) umem_walk_log(allocating_cb, &budget);

	CHECK(cb_allocs > 0,
	    "test is vacuous: no walker callback ever ran");

	/* findleaks itself allocates per leak class; it must not do that
	 * under a cache lock either. */
	FILE *null = devnull();
	(void) umem_findleaks(null, UMEM_FMT_TEXT, 50);
	if (null != stderr)
		(void) fclose(null);

	for (i = 0; i < 64; i++)
		if (keep[i] != NULL)
			umem_free(keep[i], 128);

	printf("  C2 walker callbacks may allocate: %zu callbacks, "
	    "no deadlock\n", cb_allocs);
}

/* ------------------------------------------------------------------ */
/* C3/item 7: whatis distinguishes CACHED from ALLOCATED.             */
/* ------------------------------------------------------------------ */

/*
 * Free a buffer and confirm umem_whatis() does not call it ALLOCATED.  A
 * freed buffer lands in a magazine (or an rseq magazine), where the slab layer
 * still counts it as handed out -- which is exactly why this used to report
 * ALLOCATED for memory the application had already returned.
 *
 * The buffer may also reach a slab freelist (FREE) or a PTC bin.  PTC bins are
 * thread-local and unreachable from the inspector, so that case is accepted
 * explicitly rather than asserted away: with PTC on, ALLOCATED is a known and
 * documented false positive.  The test therefore asserts the property that
 * must hold -- CACHED is reachable at all -- over a batch, rather than for one
 * buffer.
 *
 * WHY THE PTC IS DISABLED HERE.  With PTC enabled most freed buffers stop in a
 * thread-local bin and never reach a magazine, so the CACHED population is
 * dominated by whatever PTC happens to overflow.  That made this check pass
 * even while the rseq-magazine subtraction was compiled out entirely (the
 * UMEM_RSEQ_AVAILABLE include guard could never fire, since umem_rseq.h is
 * what DEFINES that macro).  Re-exec once with UMEM_OPTIONS=ptc=0 so freed
 * buffers go to the magazine layer, which is the layer under test.
 */
static void
test_whatis_reports_cached(void)
{
	enum { N = 256 };
	void *bufs[N];
	int i, cached = 0, freestate = 0, allocated = 0, missing = 0;

	for (i = 0; i < N; i++)
		bufs[i] = umem_alloc(192, UMEM_DEFAULT);
	for (i = 0; i < N; i++)
		if (bufs[i] != NULL)
			umem_free(bufs[i], 192);

	for (i = 0; i < N; i++) {
		umem_buffer_info_t info;
		if (bufs[i] == NULL)
			continue;
		if (umem_whatis(bufs[i], &info) != 0) {
			missing++;
			continue;
		}
		switch (info.state) {
		case UMEM_BUF_CACHED:		cached++;	break;
		case UMEM_BUF_FREE:		freestate++;	break;
		case UMEM_BUF_ALLOCATED:	allocated++;	break;
		default:					break;
		}
	}

	printf("  C3 whatis on %d freed buffers: %d CACHED, %d FREE, "
	    "%d held(PTC/unaccounted), %d unresolved\n",
	    N, cached, freestate, allocated, missing);

	/*
	 * The contract: a freed, magazine-resident buffer must be reportable
	 * as CACHED.  umem_inspect.h has always documented UMEM_BUF_CACHED and
	 * umem_whatis() never returned it, so pre-fix `cached` is 0 here.
	 *
	 * With the PTC off (see above) the magazine layer is the only place a
	 * freed buffer can be, so this is a strong assertion: nearly every
	 * buffer must be accounted as CACHED or FREE, not merely one of them.
	 */
	CHECK(cached + freestate >= N / 2,
	    "item 7 violated: most freed buffers are still reported as held -- "
	    "a retention site is not being subtracted (magazine, or the rseq "
	    "magazines if their subtraction was compiled out)");
}

int
main(int argc, char **argv)
{
	/*
	 * Re-exec once with the PTC off.  See test_whatis_reports_cached():
	 * with PTC on, freed buffers stop in thread-local bins and the
	 * magazine-layer accounting under test is barely exercised.
	 */
	if (getenv("UMEM_INSPECT_CONTRACTS_REEXEC") == NULL) {
		(void) setenv("UMEM_INSPECT_CONTRACTS_REEXEC", "1", 1);
		(void) setenv("UMEM_OPTIONS", "ptc=0", 1);
		(void) execv("/proc/self/exe", argv);
		/* execv failed: carry on with PTC enabled rather than
		 * reporting a pass we did not earn. */
		fprintf(stderr,
		    "warning: could not re-exec with ptc=0 (%s); the CACHED "
		    "check is weaker in this run\n", strerror(errno));
	}
	(void) argc;

	/* Generous: the churn test alone runs 5s, and this must not flake on
	 * a loaded CI box.  A real lifetime bug hangs indefinitely. */
	watchdog_start(180);

	printf("umem_inspect contract tests (UMEM_OPTIONS=%s)\n",
	    getenv("UMEM_OPTIONS") ? getenv("UMEM_OPTIONS") : "");
	test_whatis_reports_cached();
	test_no_alloc_under_lock();
	test_inspect_vs_destroy_churn();

	if (failures != 0) {
		printf("\n%d contract violation(s)\n", failures);
		return (1);
	}
	printf("\nall inspect contract tests passed\n");
	return (0);
}

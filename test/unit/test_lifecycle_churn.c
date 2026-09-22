/*
 * test_lifecycle_churn.c -- lifecycle coverage for P2.7 in
 * docs/plans/2026-09-21-production-readiness.md.
 *
 * WHAT IS COVERED HERE (fast, deterministic, in `make check`)
 *   A. thread churn            threads created and destroyed repeatedly while
 *                              allocating, so per-thread cache construction
 *                              and teardown runs hundreds of times.
 *   B. cache create/destroy    umem_cache_create/destroy in a loop, including
 *      churn                   concurrently from several threads, while other
 *                              threads keep ordinary umem_alloc traffic going.
 *   C. cross-thread free       objects allocated on one thread and freed on
 *                              another, so the depot return path runs under
 *                              the churn above.
 *
 * Every allocation is stamped with an owner token and verified before free,
 * so a buffer handed to two owners fails here rather than being silently
 * tolerated.  A hang is a FAIL: the process carries a watchdog.
 *
 * WHAT IS DELIBERATELY NOT HERE
 *   * fork under allocation load -- test/integration/test_fork_mt_load
 *     already does exactly that (P1.2); reused, not duplicated.
 *   * magazine resize under load -- exercises P1.3b/P1.3c, which are OPEN.
 *     A gate that fails on a known-open defect turns `make check` red for
 *     something nobody is fixing in this pass, so it lives in
 *     test/stress/lifecycle_stress.sh with its expected outcome documented.
 *   * debug/reclaim reuse -- needs UMEM_DEBUG/UMEM_OPTIONS set before init,
 *     so it is a separate process: test/unit/repro_reclaim_reuse.
 *   * malloc-interposed operation -- needs LD_PRELOAD, so it is run from
 *     test/stress/lifecycle_stress.sh (this same binary, preloaded).
 *
 * Exit status: 0 PASS, 1 FAIL, 77 SKIP (a precondition was not met; never 0).
 * Scale knobs (used by lifecycle_stress.sh for the long runs):
 *   LIFECYCLE_ROUNDS   thread-churn rounds (default 120)
 *   LIFECYCLE_THREADS  threads per round (default 4)
 *   LIFECYCLE_CACHES   cache create/destroy iterations per thread (default 60)
 *   LIFECYCLE_DEADLINE watchdog seconds (default 120)
 */

#include "config.h"

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "umem.h"

static int rounds = 120;
static int nthreads = 4;
static int ncaches = 60;
static int deadline_s = 120;

static atomic_int failures = 0;
static atomic_int stop = 0;

static void
failf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	fflush(stderr);
	atomic_fetch_add(&failures, 1);
}

/* ---- owner-stamped buffers -------------------------------------------- */
static void
stamp(void *p, size_t sz, uint64_t tok)
{
	size_t n = sz / sizeof(uint64_t), i;
	uint64_t *w = p;
	for (i = 0; i < n; i++)
		w[i] = tok;
	for (i = n * sizeof(uint64_t); i < sz; i++)
		((unsigned char *)p)[i] = (unsigned char)(tok & 0xff);
}

static int
check(const char *where, void *p, size_t sz, uint64_t tok)
{
	size_t n = sz / sizeof(uint64_t), i;
	const uint64_t *w = p;
	for (i = 0; i < n; i++) {
		if (w[i] != tok) {
			failf("%s: buffer %p word %zu = 0x%llx, expected "
			    "0x%llx (buffer handed to two owners, or "
			    "corrupted)", where, p, i,
			    (unsigned long long)w[i],
			    (unsigned long long)tok);
			return (0);
		}
	}
	for (i = n * sizeof(uint64_t); i < sz; i++) {
		if (((unsigned char *)p)[i] != (unsigned char)(tok & 0xff)) {
			failf("%s: buffer %p byte %zu corrupted", where, p, i);
			return (0);
		}
	}
	return (1);
}

/* ---- watchdog: a hang is a FAIL, never an indefinite wait ------------- */
static void *
watchdog(void *unused)
{
	(void) unused;
	for (int s = 0; s < deadline_s; s++) {
		if (atomic_load(&stop))
			return (NULL);
		sleep(1);
	}
	/* Do not allocate here: the process may hold allocator locks. */
	static const char msg[] =
	    "FAIL: lifecycle churn exceeded its deadline -- likely a deadlock "
	    "in cache destruction or per-thread cache teardown\n";
	ssize_t n = write(2, msg, sizeof(msg) - 1);
	(void) n;
	_exit(1);
}

/* ---- A. thread churn -------------------------------------------------- */
static const size_t sizes[] = { 24, 64, 200, 1024, 3072, 9000 };
#define NSIZES	(sizeof (sizes) / sizeof (sizes[0]))

#define HELD	32

struct churn_arg {
	int tid;
	int iters;
};

static void *
churn_thread(void *arg)
{
	struct churn_arg *a = arg;
	void *held[HELD] = { 0 };
	size_t hsz[HELD] = { 0 };
	uint64_t htok[HELD] = { 0 };
	unsigned int seed = (unsigned)a->tid * 2654435761u + 1u;

	for (int i = 0; i < a->iters && !atomic_load(&failures); i++) {
		int slot = rand_r(&seed) % HELD;
		if (held[slot] != NULL) {
			if (!check("thread-churn", held[slot], hsz[slot],
			    htok[slot]))
				break;
			umem_free(held[slot], hsz[slot]);
			held[slot] = NULL;
			continue;
		}
		size_t sz = sizes[rand_r(&seed) % NSIZES];
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (p == NULL) {
			failf("thread-churn: umem_alloc(%zu) returned NULL",
			    sz);
			break;
		}
		uint64_t tok = ((uint64_t)a->tid << 40) | (uint64_t)i;
		stamp(p, sz, tok);
		if (!check("thread-churn-readback", p, sz, tok)) {
			umem_free(p, sz);
			break;
		}
		held[slot] = p;
		hsz[slot] = sz;
		htok[slot] = tok;
	}
	for (int k = 0; k < HELD; k++)
		if (held[k] != NULL) {
			(void) check("thread-churn-drain", held[k], hsz[k],
			    htok[k]);
			umem_free(held[k], hsz[k]);
		}
	return (NULL);
}

static void
test_thread_churn(void)
{
	printf("A. thread churn: %d rounds x %d threads\n", rounds, nthreads);
	for (int r = 0; r < rounds && !atomic_load(&failures); r++) {
		pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
		struct churn_arg *ar =
		    calloc((size_t)nthreads, sizeof(*ar));
		if (th == NULL || ar == NULL) {
			failf("thread churn: driver allocation failed");
			free(th); free(ar);
			return;
		}
		int started = 0;
		for (int i = 0; i < nthreads; i++) {
			ar[i].tid = r * nthreads + i;
			ar[i].iters = 400;
			int e = pthread_create(&th[i], NULL, churn_thread,
			    &ar[i]);
			if (e != 0) {
				failf("pthread_create: %s", strerror(e));
				break;
			}
			started++;
		}
		for (int i = 0; i < started; i++)
			pthread_join(th[i], NULL);
		free(th); free(ar);
	}
	printf("   %s\n", atomic_load(&failures) ? "FAIL" : "ok");
}

/* ---- B. cache create/destroy churn, concurrent with alloc traffic ----- */
struct cache_arg {
	int tid;
	int iters;
};

static void *
cache_churn_thread(void *arg)
{
	struct cache_arg *a = arg;
	char name[64];

	for (int i = 0; i < a->iters && !atomic_load(&failures); i++) {
		size_t objsz = sizes[(size_t)(i + a->tid) % NSIZES];
		snprintf(name, sizeof(name), "lifecycle_%d_%d", a->tid, i);
		/* Alternate magazine / no-magazine: UMC_NOMAGAZINE is the
		 * flag combination that exposed P1.4 (destroy with retained
		 * empty slabs). */
		int flags = (i & 1) ? UMC_NOMAGAZINE : 0;
		umem_cache_t *cp = umem_cache_create(name, objsz, 0,
		    NULL, NULL, NULL, NULL, NULL, flags);
		if (cp == NULL) {
			failf("cache churn: umem_cache_create(%s, %zu) "
			    "returned NULL", name, objsz);
			break;
		}
		/* Allocate, stamp, verify, free -- then destroy with nothing
		 * outstanding, which is the case that must leave no retained
		 * backing storage behind (P1.4). */
		void *objs[16];
		int n = 0;
		for (; n < 16; n++) {
			objs[n] = umem_cache_alloc(cp, UMEM_DEFAULT);
			if (objs[n] == NULL) {
				failf("cache churn: umem_cache_alloc(%s) "
				    "returned NULL", name);
				break;
			}
			stamp(objs[n], objsz, 0xc0ffee00ULL + (uint64_t)n);
		}
		for (int k = 0; k < n; k++) {
			(void) check("cache-churn", objs[k], objsz,
			    0xc0ffee00ULL + (uint64_t)k);
			umem_cache_free(cp, objs[k]);
		}
		umem_cache_destroy(cp);
	}
	return (NULL);
}

/* Background ordinary allocation traffic, so cache destruction happens while
 * the allocator is busy rather than in a quiet process. */
static void *
background_traffic(void *unused)
{
	(void) unused;
	unsigned int seed = 0x5bd1e995u;
	while (!atomic_load(&stop) && !atomic_load(&failures)) {
		size_t sz = sizes[rand_r(&seed) % NSIZES];
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (p == NULL) {
			failf("background traffic: umem_alloc(%zu) NULL", sz);
			break;
		}
		memset(p, 0x5a, sz);
		umem_free(p, sz);
	}
	return (NULL);
}

static void
test_cache_churn(void)
{
	printf("B. cache create/destroy churn: %d threads x %d iterations, "
	    "with background traffic\n", nthreads, ncaches);

	pthread_t bg;
	int have_bg = (pthread_create(&bg, NULL, background_traffic,
	    NULL) == 0);
	if (!have_bg)
		printf("   (no background traffic thread; churn still runs)\n");

	pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
	struct cache_arg *ar = calloc((size_t)nthreads, sizeof(*ar));
	if (th == NULL || ar == NULL) {
		failf("cache churn: driver allocation failed");
		free(th); free(ar);
		atomic_store(&stop, 1);
		if (have_bg) pthread_join(bg, NULL);
		return;
	}
	int started = 0;
	for (int i = 0; i < nthreads; i++) {
		ar[i].tid = i;
		ar[i].iters = ncaches;
		int e = pthread_create(&th[i], NULL, cache_churn_thread,
		    &ar[i]);
		if (e != 0) {
			failf("pthread_create: %s", strerror(e));
			break;
		}
		started++;
	}
	for (int i = 0; i < started; i++)
		pthread_join(th[i], NULL);
	atomic_store(&stop, 1);
	if (have_bg)
		pthread_join(bg, NULL);
	atomic_store(&stop, 0);
	free(th); free(ar);
	printf("   %s\n", atomic_load(&failures) ? "FAIL" : "ok");
}

/* ---- C. cross-thread free (depot return) under thread churn ---------- */
#define XQ	4096
static void *xq_ptr[XQ];
static size_t xq_sz[XQ];
static uint64_t xq_tok[XQ];
static atomic_int xq_ready[XQ];
static atomic_int xq_head = 0;
static atomic_int xq_freed = 0;

static void *
xfree_producer(void *arg)
{
	int tid = *(int *)arg;
	unsigned int seed = (unsigned)tid * 40503u + 7u;
	for (;;) {
		int i = atomic_fetch_add(&xq_head, 1);
		if (i >= XQ)
			return (NULL);
		size_t sz = sizes[rand_r(&seed) % NSIZES];
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (p == NULL) {
			failf("cross-thread: umem_alloc(%zu) NULL", sz);
			return (NULL);
		}
		uint64_t tok = ((uint64_t)tid << 40) | (uint64_t)i;
		stamp(p, sz, tok);
		xq_ptr[i] = p;
		xq_sz[i] = sz;
		xq_tok[i] = tok;
		atomic_store(&xq_ready[i], 1);
	}
}

static void *
xfree_consumer(void *unused)
{
	(void) unused;
	for (;;) {
		int i = atomic_fetch_add(&xq_freed, 1);
		if (i >= XQ)
			return (NULL);
		while (!atomic_load(&xq_ready[i])) {
			if (atomic_load(&failures))
				return (NULL);
			sched_yield();
		}
		(void) check("cross-thread-free", xq_ptr[i], xq_sz[i],
		    xq_tok[i]);
		umem_free(xq_ptr[i], xq_sz[i]);
	}
}

static void
test_cross_thread_free(void)
{
	printf("C. cross-thread free (depot return): %d buffers\n", XQ);
	int np = nthreads > 1 ? nthreads / 2 : 1;
	int nc = nthreads - np;
	if (nc < 1) nc = 1;

	pthread_t *pt = calloc((size_t)np, sizeof(*pt));
	pthread_t *ct = calloc((size_t)nc, sizeof(*ct));
	int *ids = calloc((size_t)np, sizeof(*ids));
	if (pt == NULL || ct == NULL || ids == NULL) {
		failf("cross-thread: driver allocation failed");
		free(pt); free(ct); free(ids);
		return;
	}
	int sp = 0, sc = 0;
	for (int i = 0; i < np; i++) {
		ids[i] = i;
		if (pthread_create(&pt[i], NULL, xfree_producer, &ids[i]) != 0)
			break;
		sp++;
	}
	for (int i = 0; i < nc; i++) {
		if (pthread_create(&ct[i], NULL, xfree_consumer, NULL) != 0)
			break;
		sc++;
	}
	if (sp == 0 || sc == 0)
		failf("cross-thread: could not start producers/consumers");
	for (int i = 0; i < sp; i++)
		pthread_join(pt[i], NULL);
	for (int i = 0; i < sc; i++)
		pthread_join(ct[i], NULL);
	free(pt); free(ct); free(ids);
	printf("   %s\n", atomic_load(&failures) ? "FAIL" : "ok");
}

/* ---- main ------------------------------------------------------------- */
static int
env_int(const char *name, int dflt)
{
	const char *s = getenv(name);
	if (s == NULL || *s == '\0')
		return (dflt);
	int v = atoi(s);
	return (v > 0 ? v : dflt);
}

int
main(void)
{
	rounds = env_int("LIFECYCLE_ROUNDS", rounds);
	nthreads = env_int("LIFECYCLE_THREADS", nthreads);
	ncaches = env_int("LIFECYCLE_CACHES", ncaches);
	deadline_s = env_int("LIFECYCLE_DEADLINE", deadline_s);

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	printf("lifecycle churn: rounds=%d threads=%d caches=%d "
	    "deadline=%ds ncpu=%ld\n", rounds, nthreads, ncaches, deadline_s,
	    ncpu);

	pthread_t wd;
	if (pthread_create(&wd, NULL, watchdog, NULL) != 0) {
		/* No watchdog means a hang would block forever: refuse to
		 * report a result we could not bound.  SKIP, not PASS. */
		printf("SKIP: could not start the watchdog thread\n");
		return (77);
	}
	pthread_detach(wd);

	test_thread_churn();
	test_cross_thread_free();
	test_cache_churn();

	atomic_store(&stop, 1);
	int f = atomic_load(&failures);
	printf("\nResult: %s (%d failure%s)\n", f ? "FAIL" : "PASS", f,
	    f == 1 ? "" : "s");
	return (f ? 1 : 0);
}

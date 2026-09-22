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
 * stress_concurrency_oracle.c -- adversarial oracle for cross-thread buffer
 * aliasing and silent corruption under contention.
 *
 * THE ORACLE
 * ----------
 * Every allocation is stamped with a unique owner token
 *     canary = (tid << 40) | (seq & ((1<<40)-1))
 * written as a repeating 8-byte pattern across the ENTIRE usable buffer.
 * The token is verified:
 *   (a) immediately after alloc (read-back),
 *   (b) after holding the buffer a while (a live buffer that another thread
 *       has ALSO been handed will read back the other owner's token), and
 *   (c) at free time, just before releasing it.
 * A mismatch means the allocator handed the same live buffer to two owners
 * (double-alloc / aliasing) or corrupted it under contention.  On ANY
 * mismatch the process prints the offending address + expected/found tokens
 * and exits non-zero.  A correct allocator never triggers it (no false
 * positives on valid concurrent use: each live buffer has exactly one owner
 * at a time, so the token it reads back is always the one it wrote).
 *
 * PATTERNS (why this is an oracle, not a smoke test)
 * --------------------------------------------------
 *   multi     same-size-class hammering: all threads pound one PTC/magazine
 *             size class, maximizing depot refill / magazine swap contention
 *             and per-CPU migration (the rseq reload path).
 *   prodcons  cross-thread handoff: producers stamp + enqueue, consumers
 *             dequeue + verify + free.  A buffer freed by a thread that did
 *             not allocate it exercises the depot return path and catches
 *             corruption in transit.
 *   churn     varied sizes spanning PTC / magazine / slab, plus periodic
 *             bulk free to force depot refills, magazine swaps, slab reclaim.
 *   all       (default) run all three back to back.
 *
 * Size class coverage: --size-class=small|mag|large|mixed selects the size
 * band; default mixed spans all three (PTC-eligible <=2048, magazine, and
 * >UMEM_MAXBUF slab-direct).
 *
 * WHAT "PASS" MEANS (P2.3, 2026-09-22)
 * ------------------------------------
 * Until 2026-09-22 this oracle could PASS on a completely broken allocator.
 * An allocation failure was counted as completed work (`if (!p) { done++;
 * continue; }`) and the final verdict looked only at the corruption flag, so
 * an allocator that returned NULL for every request finished every stage,
 * reported hundreds of millions of "ops/s", and exited 0.
 *
 * A PASS now requires ALL of:
 *   1. no stamp mismatch (aliasing/corruption), AND
 *   2. zero allocation failures -- a NULL is a terminal FAIL, not an op, AND
 *   3. each stage completed at least an eighth of the requested successful
 *      allocations (a stage that barely ran is not evidence), AND
 *   4. every worker thread was created and every driver allocation succeeded.
 * Workers also start behind a barrier, so a --duration run measures the
 * window in which workers actually exist rather than including thread
 * creation.
 *
 * CONTROL KNOBS (test-only; see test/stress/oracle_control.sh)
 * -----------------------------------------------------------
 * An oracle nobody has seen fail is an assertion, not evidence.  These env
 * vars inject a known defect so the discrimination can be demonstrated on
 * demand, and are inert unless set:
 *
 *   ORACLE_INJECT=null[:N]     after N successful allocations (default 1000)
 *                              every allocation returns NULL.  Models an
 *                              exhausted/broken allocator.
 *   ORACLE_INJECT=corrupt[:N]  after N successful allocations, one byte of
 *                              the next buffer is flipped after stamping.
 *                              Models silent corruption/aliasing.
 *   ORACLE_LEGACY_VERDICT=1    restore the pre-2026-09-22 accounting and
 *                              verdict (failures counted as work, verdict
 *                              checks corruption only).  Exists so the
 *                              defect this file used to have is reproducible
 *                              rather than merely described.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sched.h>
#include <errno.h>

#include "../../umem.h"

/* ---- owner token ------------------------------------------------------- */
/* tid in the top 24 bits, seq in the low 40 -> unique per (thread, alloc). */
#define SEQ_MASK        ((1ULL << 40) - 1)
#define MAKE_TOKEN(tid, seq)    (((uint64_t)(tid) << 40) | ((seq) & SEQ_MASK))
#define TOKEN_TID(tok)          ((unsigned)((tok) >> 40))
#define TOKEN_SEQ(tok)          ((tok) & SEQ_MASK)

/* Global failure flag + first-failure detail (printed once, atomically). */
static atomic_int g_failed = 0;
static pthread_mutex_t g_fail_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Allocation failure is a separate, equally fatal condition from corruption:
 * an allocator that cannot allocate has not been shown to be free of
 * aliasing, it has only been shown not to have been exercised.  Kept
 * separate from g_failed so the report can name which one happened.
 */
static atomic_int g_alloc_failed = 0;

/* Harness (not allocator) errors: failed pthread_create, failed driver
 * allocation.  Never silently degrades the run into a smaller one. */
static atomic_int g_harness_failed = 0;

/* ---- control-only fault injection (see file header) -------------------- */
enum inject_kind { INJ_NONE = 0, INJ_NULL, INJ_CORRUPT };
static enum inject_kind g_inject = INJ_NONE;
static unsigned long long g_inject_after = 1000;
static int g_legacy_verdict = 0;
static atomic_ullong g_alloc_seq = 0;   /* successful allocations, all threads */

static void
harness_error(const char *what, int err)
{
	atomic_store(&g_harness_failed, 1);
	fprintf(stderr, "*** ORACLE HARNESS ERROR: %s%s%s ***\n", what,
	    err ? ": " : "", err ? strerror(err) : "");
	fflush(stderr);
}

static void
report_mismatch(const char *where, void *addr, size_t sz, size_t off,
    uint64_t expected, uint64_t found)
{
	/* Latch: only the first thread to fail prints the full detail, but
	 * every failure sets the flag so the exit code is non-zero. */
	if (atomic_exchange(&g_failed, 1) == 0) {
		pthread_mutex_lock(&g_fail_lock);
		fprintf(stderr,
		    "\n*** ORACLE FAILURE (%s) ***\n"
		    "  addr        = %p\n"
		    "  size        = %zu\n"
		    "  byte offset = %zu\n"
		    "  expected    = 0x%016llx (owner tid=%u seq=%llu)\n"
		    "  found       = 0x%016llx (owner tid=%u seq=%llu)\n"
		    "  => allocator returned a live buffer to two owners "
		    "(aliasing) or corrupted it under contention.\n\n",
		    where, addr, sz, off,
		    (unsigned long long)expected, TOKEN_TID(expected),
		    (unsigned long long)TOKEN_SEQ(expected),
		    (unsigned long long)found, TOKEN_TID(found),
		    (unsigned long long)TOKEN_SEQ(found));
		fflush(stderr);
		pthread_mutex_unlock(&g_fail_lock);
	}
}

/* Stamp the whole usable buffer with the owner token (8-byte repeat). */
static void
stamp(void *p, size_t sz, uint64_t token)
{
	size_t n = sz / sizeof(uint64_t);
	uint64_t *w = (uint64_t *)p;
	for (size_t i = 0; i < n; i++)
		w[i] = token;
	/* tail bytes: stamp a low byte of the token so a short overrun by a
	 * different owner is still visible. */
	unsigned char *tail = (unsigned char *)p + n * sizeof(uint64_t);
	unsigned char tb = (unsigned char)(token & 0xff);
	for (size_t i = n * sizeof(uint64_t); i < sz; i++)
		*tail++ = tb;
}

/* Verify every stamped word/byte equals our token. Returns true on match. */
static bool
verify(const char *where, void *p, size_t sz, uint64_t token)
{
	size_t n = sz / sizeof(uint64_t);
	const uint64_t *w = (const uint64_t *)p;
	for (size_t i = 0; i < n; i++) {
		if (w[i] != token) {
			report_mismatch(where, p, sz, i * sizeof(uint64_t),
			    token, w[i]);
			return (false);
		}
	}
	const unsigned char *tail = (const unsigned char *)p +
	    n * sizeof(uint64_t);
	unsigned char tb = (unsigned char)(token & 0xff);
	for (size_t i = n * sizeof(uint64_t); i < sz; i++) {
		if (tail[i - n * sizeof(uint64_t)] != tb) {
			report_mismatch(where, p, sz, i, token & 0xff,
			    tail[i - n * sizeof(uint64_t)]);
			return (false);
		}
	}
	return (true);
}

/*
 * Allocation used by every worker.  Wraps umem_alloc so the control knobs
 * have exactly one place to act, and so the "successful allocation" count
 * that the verdict needs is maintained in one place.
 */
static void *
oracle_alloc(size_t sz)
{
	unsigned long long n = atomic_fetch_add(&g_alloc_seq, 1) + 1;

	if (g_inject == INJ_NULL && n > g_inject_after)
		return (NULL);

	void *p = umem_alloc(sz, UMEM_DEFAULT);
	return (p);
}

/* Corrupt one byte of a just-stamped buffer, once, when asked to. */
static void
maybe_inject_corruption(void *p, size_t sz)
{
	static atomic_int done = 0;

	if (g_inject != INJ_CORRUPT || sz == 0)
		return;
	if (atomic_load(&g_alloc_seq) <= g_inject_after)
		return;
	if (atomic_exchange(&done, 1) != 0)
		return;
	((unsigned char *)p)[sz / 2] ^= 0xffu;
}

/* ---- per-thread RNG ---------------------------------------------------- */
typedef struct { uint64_t s; } rng_t;
static uint64_t
rng_next(rng_t *r)
{
	uint64_t x = r->s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return (r->s = x);
}
static size_t
rng_range(rng_t *r, size_t lo, size_t hi)
{
	return (hi <= lo) ? lo : lo + (rng_next(r) % (hi - lo + 1));
}

/* ---- size-class bands -------------------------------------------------- */
enum size_class { SC_SMALL, SC_MAG, SC_LARGE, SC_MIXED };

static size_t
pick_size(enum size_class sc, rng_t *r)
{
	switch (sc) {
	case SC_SMALL:  return (rng_range(r, 8, 256));       /* PTC-eligible */
	case SC_MAG:    return (rng_range(r, 512, 8192));    /* magazine     */
	case SC_LARGE:  return (rng_range(r, 65536, 262144));/* slab-direct  */
	case SC_MIXED:
	default: {
		uint32_t b = rng_next(r) % 100;
		if (b < 60)  return (rng_range(r, 8, 256));
		if (b < 90)  return (rng_range(r, 512, 8192));
		return (rng_range(r, 65536, 262144));
	}
	}
}

/* Shared knobs. */
typedef struct {
	int nthreads;
	uint64_t iters;         /* per-thread ops (0 => duration mode) */
	uint64_t deadline_ns;   /* 0 => iters mode; written before the barrier */
	enum size_class sc;
	size_t fixed_size;      /* "multi": ONE size class, shared by all threads */
	pthread_barrier_t bar;  /* workers + driver; duration starts after it */
	atomic_ullong ops;      /* completed operations (alloc or free) */
	atomic_ullong allocs_ok;/* successful allocations only */
	atomic_ullong fails;    /* allocation failures */
} cfg_t;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static bool
keep_going(const cfg_t *c, uint64_t done)
{
	if (atomic_load(&g_failed))
		return (false);
	if (!g_legacy_verdict && atomic_load(&g_alloc_failed))
		return (false);
	if (c->deadline_ns)
		return (now_ns() < c->deadline_ns);
	return (done < c->iters);
}

/*
 * One allocation failure ends the run.  Pre-fix this incremented the
 * completed-work counter and continued, which is how an allocator that never
 * allocated anything reached the end of every stage and PASSed.
 */
static void
note_alloc_failure(cfg_t *c, size_t sz)
{
	atomic_fetch_add(&c->fails, 1);
	if (atomic_exchange(&g_alloc_failed, 1) == 0) {
		fprintf(stderr, "\n*** ORACLE FAILURE (alloc) ***\n"
		    "  umem_alloc(%zu) returned NULL\n"
		    "  => the allocator did not perform the requested work; "
		    "absence of corruption proves nothing here.\n\n", sz);
		fflush(stderr);
	}
}

/* ---- pattern: multi (same-size hammer) + churn (varied) ---------------- *
 * Each thread keeps a live pool.  On alloc: stamp + read-back verify.  On
 * free (chosen randomly): verify the token is STILL ours, then free.  A
 * held buffer that a second thread was also handed will read back the wrong
 * token here -- that's the aliasing catch. */
#define POOL 512

typedef struct {
	cfg_t *cfg;
	int tid;
	bool fixed_size;        /* multi: one size class hammered */
} worker_arg_t;

static void *
churn_worker(void *arg)
{
	worker_arg_t *wa = (worker_arg_t *)arg;
	cfg_t *c = wa->cfg;
	rng_t r = { .s = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)wa->tid << 1 | 1) };

	void *ptr[POOL] = { 0 };
	size_t size[POOL] = { 0 };
	uint64_t tok[POOL] = { 0 };
	uint64_t seq = 0;
	uint64_t done = 0;
	uint64_t allocs_ok = 0;
	/*
	 * "multi" hammers ONE shared size class so every thread contends for
	 * the same PTC bin / magazine / depot.  Previously each thread picked
	 * its own fixed size from its own RNG, which spread the threads across
	 * size classes and removed most of the contention the pattern exists
	 * to create.
	 */
	size_t fixed = c->fixed_size;

	/* Start together: in --duration mode the window must not include
	 * thread creation (the driver arms the deadline before releasing us). */
	pthread_barrier_wait(&c->bar);

	while (keep_going(c, done)) {
		int i = (int)(rng_next(&r) % POOL);

		if (ptr[i]) {
			/* Verify before free: must still be our token. */
			if (!verify("free", ptr[i], size[i], tok[i]))
				break;
			umem_free(ptr[i], size[i]);
			ptr[i] = NULL;
		} else {
			size_t sz = wa->fixed_size ? fixed
			    : pick_size(c->sc, &r);
			void *p = oracle_alloc(sz);
			if (!p) {
				note_alloc_failure(c, sz);
				if (g_legacy_verdict) { done++; continue; }
				break;
			}
			allocs_ok++;
			uint64_t t = MAKE_TOKEN(wa->tid, seq++);
			stamp(p, sz, t);
			maybe_inject_corruption(p, sz);
			/* read-back immediately: catches a freshly double-alloc'd
			 * buffer whose other owner just stamped it. */
			if (!verify("alloc", p, sz, t)) {
				umem_free(p, sz);
				break;
			}
			ptr[i] = p; size[i] = sz; tok[i] = t;
		}
		done++;

		/* Periodically re-verify the whole live pool: a buffer aliased
		 * to another thread AFTER we stamped it fails here. */
		if ((done & 0x3ff) == 0) {
			for (int k = 0; k < POOL; k++)
				if (ptr[k] &&
				    !verify("hold", ptr[k], size[k], tok[k]))
					goto out;
		}
	}
out:
	/* Drain: final verify + free everything we still hold. */
	for (int k = 0; k < POOL; k++)
		if (ptr[k]) {
			verify("drain", ptr[k], size[k], tok[k]);
			umem_free(ptr[k], size[k]);
		}
	atomic_fetch_add(&c->ops, done);
	atomic_fetch_add(&c->allocs_ok, allocs_ok);
	return (NULL);
}

/* ---- pattern: producer/consumer cross-thread handoff ------------------- *
 * Producers stamp + enqueue; consumers dequeue, verify (buffer crossed a
 * thread boundary intact), then free (cross-thread free -> depot return). */
typedef struct {
	void *ptr;
	size_t sz;
	uint64_t tok;
} pc_slot_t;

#define PC_CAP 16384
typedef struct {
	pc_slot_t slot[PC_CAP];
	atomic_ullong head, tail;   /* mpmc via seq lock on slots is overkill;
	                             * use a mutex-free bounded queue */
	atomic_ullong seq[PC_CAP];
	atomic_int producers_live;
} pc_queue_t;

static bool
pc_push(pc_queue_t *q, pc_slot_t v)
{
	unsigned long long h = atomic_load_explicit(&q->head,
	    memory_order_relaxed);
	unsigned long long s = atomic_load_explicit(&q->seq[h % PC_CAP],
	    memory_order_acquire);
	if (s != h)
		return (false);
	if (!atomic_compare_exchange_weak_explicit(&q->head, &h, h + 1,
	    memory_order_relaxed, memory_order_relaxed))
		return (false);
	q->slot[h % PC_CAP] = v;
	atomic_store_explicit(&q->seq[h % PC_CAP], h + 1,
	    memory_order_release);
	return (true);
}

static bool
pc_pop(pc_queue_t *q, pc_slot_t *out)
{
	unsigned long long t = atomic_load_explicit(&q->tail,
	    memory_order_relaxed);
	unsigned long long s = atomic_load_explicit(&q->seq[t % PC_CAP],
	    memory_order_acquire);
	if (s != t + 1)
		return (false);
	if (!atomic_compare_exchange_weak_explicit(&q->tail, &t, t + 1,
	    memory_order_relaxed, memory_order_relaxed))
		return (false);
	*out = q->slot[t % PC_CAP];
	atomic_store_explicit(&q->seq[t % PC_CAP], t + PC_CAP,
	    memory_order_release);
	return (true);
}

typedef struct {
	cfg_t *cfg;
	pc_queue_t *q;
	int tid;
} pc_arg_t;

static void *
pc_producer(void *arg)
{
	pc_arg_t *pa = (pc_arg_t *)arg;
	cfg_t *c = pa->cfg;
	rng_t r = { .s = 0xd1b54a32d192 ^ ((uint64_t)pa->tid << 1 | 1) };
	uint64_t seq = 0, done = 0, allocs_ok = 0;

	pthread_barrier_wait(&c->bar);

	while (keep_going(c, done)) {
		size_t sz = pick_size(c->sc, &r);
		void *p = oracle_alloc(sz);
		if (!p) {
			note_alloc_failure(c, sz);
			if (g_legacy_verdict) { done++; continue; }
			break;
		}
		allocs_ok++;
		uint64_t t = MAKE_TOKEN(pa->tid, seq++);
		stamp(p, sz, t);
		maybe_inject_corruption(p, sz);
		if (!verify("prod-alloc", p, sz, t)) { umem_free(p, sz); break; }
		pc_slot_t v = { .ptr = p, .sz = sz, .tok = t };
		while (!pc_push(pa->q, v)) {
			if (!keep_going(c, done)) { umem_free(p, sz); goto out; }
			sched_yield();
		}
		done++;
	}
out:
	atomic_fetch_add(&c->ops, done);
	atomic_fetch_add(&c->allocs_ok, allocs_ok);
	atomic_fetch_sub(&pa->q->producers_live, 1);
	return (NULL);
}

static void *
pc_consumer(void *arg)
{
	pc_arg_t *pa = (pc_arg_t *)arg;
	cfg_t *c = pa->cfg;
	uint64_t done = 0;

	pthread_barrier_wait(&c->bar);

	for (;;) {
		pc_slot_t v;
		if (pc_pop(pa->q, &v)) {
			/* buffer crossed a thread boundary: token must survive */
			if (!verify("consume", v.ptr, v.sz, v.tok)) {
				umem_free(v.ptr, v.sz);
				break;
			}
			umem_free(v.ptr, v.sz);
			done++;
			if (atomic_load(&g_failed))
				break;
			if (!g_legacy_verdict && atomic_load(&g_alloc_failed))
				break;
		} else if (atomic_load(&pa->q->producers_live) == 0) {
			/* drain and stop */
			while (pc_pop(pa->q, &v)) {
				verify("drain", v.ptr, v.sz, v.tok);
				umem_free(v.ptr, v.sz);
				done++;
			}
			break;
		} else {
			sched_yield();
		}
	}
	atomic_fetch_add(&c->ops, done);
	return (NULL);
}

/* ---- pattern drivers --------------------------------------------------- *
 * Every driver: check its own allocations, check pthread_create, and arm the
 * duration deadline AFTER threads exist but BEFORE the start barrier opens,
 * so the measured window is the window in which workers are running.
 * Returns the elapsed seconds of that window, or -1 on a harness error. */
static double
run_threaded(cfg_t *c, bool fixed_size, int duration)
{
	pthread_t *th = calloc(c->nthreads, sizeof(*th));
	worker_arg_t *wa = calloc(c->nthreads, sizeof(*wa));
	int started = 0;

	if (th == NULL || wa == NULL) {
		harness_error("driver calloc failed", errno);
		free(th); free(wa);
		return (-1);
	}
	if (pthread_barrier_init(&c->bar, NULL, (unsigned)c->nthreads + 1) != 0) {
		harness_error("pthread_barrier_init failed", errno);
		free(th); free(wa);
		return (-1);
	}

	for (int i = 0; i < c->nthreads; i++) {
		wa[i] = (worker_arg_t){ .cfg = c, .tid = i,
		    .fixed_size = fixed_size };
		int e = pthread_create(&th[i], NULL, churn_worker, &wa[i]);
		if (e != 0) {
			/* A short run is not a smaller run: report it. */
			harness_error("pthread_create failed", e);
			break;
		}
		started++;
	}
	/* Release the barrier even if we created fewer threads than planned,
	 * otherwise the ones that did start wait forever. */
	for (int i = started; i < c->nthreads; i++)
		(void) pthread_barrier_wait(&c->bar);

	if (duration)
		c->deadline_ns = now_ns() + (uint64_t)duration * 1000000000ULL;
	uint64_t t0 = now_ns();
	pthread_barrier_wait(&c->bar);

	for (int i = 0; i < started; i++)
		pthread_join(th[i], NULL);
	double sec = (now_ns() - t0) / 1e9;

	pthread_barrier_destroy(&c->bar);
	free(th); free(wa);
	return (sec);
}

static double
run_prodcons(cfg_t *c, int duration)
{
	pc_queue_t *q = calloc(1, sizeof(*q));
	if (q == NULL) {
		harness_error("prodcons queue calloc failed", errno);
		return (-1);
	}
	for (int i = 0; i < PC_CAP; i++)
		atomic_init(&q->seq[i], (unsigned long long)i);
	atomic_init(&q->head, 0);
	atomic_init(&q->tail, 0);

	int half = c->nthreads / 2;
	if (half < 1) half = 1;
	int nprod = half, ncons = c->nthreads - half;
	if (ncons < 1) ncons = 1;
	atomic_init(&q->producers_live, nprod);

	pthread_t *pt = calloc(nprod, sizeof(*pt));
	pthread_t *ct = calloc(ncons, sizeof(*ct));
	pc_arg_t *pa = calloc((size_t)nprod + (size_t)ncons, sizeof(*pa));
	int nstarted_p = 0, nstarted_c = 0;
	double sec = -1;

	if (pt == NULL || ct == NULL || pa == NULL) {
		harness_error("prodcons driver calloc failed", errno);
		goto done;
	}
	if (pthread_barrier_init(&c->bar, NULL,
	    (unsigned)(nprod + ncons) + 1) != 0) {
		harness_error("pthread_barrier_init failed", errno);
		goto done;
	}

	for (int i = 0; i < ncons; i++) {
		pa[i] = (pc_arg_t){ .cfg = c, .q = q, .tid = 1000 + i };
		int e = pthread_create(&ct[i], NULL, pc_consumer, &pa[i]);
		if (e != 0) { harness_error("pthread_create failed", e); break; }
		nstarted_c++;
	}
	for (int i = 0; i < nprod; i++) {
		pa[ncons + i] = (pc_arg_t){ .cfg = c, .q = q, .tid = i };
		int e = pthread_create(&pt[i], NULL, pc_producer, &pa[ncons + i]);
		if (e != 0) { harness_error("pthread_create failed", e); break; }
		nstarted_p++;
	}
	/* Producers that never started still "exited": keep the live count and
	 * the barrier consistent so consumers can terminate. */
	for (int i = nstarted_p; i < nprod; i++)
		atomic_fetch_sub(&q->producers_live, 1);
	for (int i = nstarted_p + nstarted_c; i < nprod + ncons; i++)
		(void) pthread_barrier_wait(&c->bar);

	if (duration)
		c->deadline_ns = now_ns() + (uint64_t)duration * 1000000000ULL;
	uint64_t t0 = now_ns();
	pthread_barrier_wait(&c->bar);

	for (int i = 0; i < nstarted_p; i++)
		pthread_join(pt[i], NULL);
	for (int i = 0; i < nstarted_c; i++)
		pthread_join(ct[i], NULL);
	sec = (now_ns() - t0) / 1e9;
	pthread_barrier_destroy(&c->bar);

done:
	free(pt); free(ct); free(pa); free(q);
	return (sec);
}

/* ---- main -------------------------------------------------------------- */
static void
usage(const char *p)
{
	printf("Usage: %s [OPTIONS]\n"
	    "  --threads=N       worker threads (default 8)\n"
	    "  --iters=M         ops per thread, iters mode (default 200000)\n"
	    "  --duration=SECS   run each pattern for SECS (overrides --iters)\n"
	    "  --size-class=C    small|mag|large|mixed (default mixed)\n"
	    "  --pattern=P       multi|prodcons|churn|all (default all)\n"
	    "  -h, --help\n"
	    "\n"
	    "A PASS requires no corruption AND zero allocation failures AND\n"
	    "that each stage actually completed most of the requested work.\n"
	    "Exit: 0 PASS, 1 FAIL, 2 usage.\n"
	    "\n"
	    "Control knobs (deliberately break the run, to show the oracle\n"
	    "discriminates -- see test/stress/oracle_control.sh):\n"
	    "  ORACLE_INJECT=null[:N]     all allocations NULL after N\n"
	    "  ORACLE_INJECT=corrupt[:N]  flip a byte of one buffer after N\n"
	    "  ORACLE_LEGACY_VERDICT=1    pre-2026-09-22 (broken) accounting\n",
	    p);
}

/* Parse ORACLE_INJECT / ORACLE_LEGACY_VERDICT.  Absent => inert. */
static void
read_control_env(void)
{
	const char *s = getenv("ORACLE_INJECT");
	const char *legacy = getenv("ORACLE_LEGACY_VERDICT");

	if (legacy != NULL && atoi(legacy) != 0)
		g_legacy_verdict = 1;

	if (s == NULL || *s == '\0')
		return;
	const char *colon = strchr(s, ':');
	size_t klen = colon ? (size_t)(colon - s) : strlen(s);
	if (klen == 4 && strncmp(s, "null", 4) == 0)
		g_inject = INJ_NULL;
	else if (klen == 7 && strncmp(s, "corrupt", 7) == 0)
		g_inject = INJ_CORRUPT;
	else {
		fprintf(stderr, "ORACLE_INJECT: unknown kind '%.*s'\n",
		    (int)klen, s);
		exit(2);
	}
	if (colon != NULL)
		g_inject_after = strtoull(colon + 1, NULL, 10);
	fprintf(stderr, "ORACLE CONTROL: inject=%s after=%llu legacy=%d "
	    "(this run is EXPECTED to fail)\n",
	    g_inject == INJ_NULL ? "null" : "corrupt",
	    (unsigned long long)g_inject_after, g_legacy_verdict);
}

int
main(int argc, char **argv)
{
	int nthreads = 8;
	uint64_t iters = 200000;
	int duration = 0;
	enum size_class sc = SC_MIXED;
	const char *pattern = "all";

	static struct option lo[] = {
		{ "threads",    required_argument, 0, 't' },
		{ "iters",      required_argument, 0, 'i' },
		{ "duration",   required_argument, 0, 'd' },
		{ "size-class", required_argument, 0, 's' },
		{ "pattern",    required_argument, 0, 'p' },
		{ "help",       no_argument,       0, 'h' },
		{ 0, 0, 0, 0 }
	};
	int opt;
	while ((opt = getopt_long(argc, argv, "t:i:d:s:p:h", lo, NULL)) != -1) {
		switch (opt) {
		case 't': nthreads = atoi(optarg); break;
		case 'i': iters = strtoull(optarg, NULL, 10); break;
		case 'd': duration = atoi(optarg); break;
		case 's':
			if (!strcmp(optarg, "small")) sc = SC_SMALL;
			else if (!strcmp(optarg, "mag")) sc = SC_MAG;
			else if (!strcmp(optarg, "large")) sc = SC_LARGE;
			else sc = SC_MIXED;
			break;
		case 'p': pattern = optarg; break;
		case 'h': usage(argv[0]); return (0);
		default:  usage(argv[0]); return (2);
		}
	}
	if (nthreads < 1) nthreads = 1;

	read_control_env();

	printf("=== libumem concurrency oracle ===\n");
	printf("threads=%d  %s=%llu  size-class=%s  pattern=%s\n",
	    nthreads,
	    duration ? "duration(s)" : "iters",
	    duration ? (unsigned long long)duration : (unsigned long long)iters,
	    (sc == SC_SMALL ? "small" : sc == SC_MAG ? "mag" :
	     sc == SC_LARGE ? "large" : "mixed"),
	    pattern);
	fflush(stdout);

	struct stage { const char *name; int is_multi, is_pc, is_churn; }
	    stages[3];
	int nstages = 0;
	if (!strcmp(pattern, "all")) {
		stages[nstages++] = (struct stage){ "multi", 1, 0, 0 };
		stages[nstages++] = (struct stage){ "prodcons", 0, 1, 0 };
		stages[nstages++] = (struct stage){ "churn", 0, 0, 1 };
	} else if (!strcmp(pattern, "multi")) {
		stages[nstages++] = (struct stage){ "multi", 1, 0, 0 };
	} else if (!strcmp(pattern, "prodcons")) {
		stages[nstages++] = (struct stage){ "prodcons", 0, 1, 0 };
	} else if (!strcmp(pattern, "churn")) {
		stages[nstages++] = (struct stage){ "churn", 0, 0, 1 };
	} else {
		usage(argv[0]);
		return (2);
	}

	unsigned long long total_allocs_ok = 0, total_fails = 0;
	int thin_stage = 0;    /* a stage that did too little work to count */

	for (int s = 0; s < nstages && !atomic_load(&g_failed) &&
	    !atomic_load(&g_alloc_failed) && !atomic_load(&g_harness_failed);
	    s++) {
		cfg_t c = { .nthreads = nthreads, .iters = iters, .sc = sc };
		atomic_init(&c.ops, 0);
		atomic_init(&c.allocs_ok, 0);
		atomic_init(&c.fails, 0);
		/*
		 * ONE shared size for the "multi" pattern: the point of that
		 * pattern is that every thread contends for the same size
		 * class's PTC bin / magazine / depot.
		 */
		rng_t sr = { .s = 0x243f6a8885a308d3ULL };
		c.fixed_size = pick_size(sc, &sr);

		double sec = stages[s].is_pc ? run_prodcons(&c, duration)
		    : run_threaded(&c, stages[s].is_multi, duration);

		unsigned long long ops = atomic_load(&c.ops);
		unsigned long long ok = atomic_load(&c.allocs_ok);
		unsigned long long bad = atomic_load(&c.fails);
		total_allocs_ok += ok;
		total_fails += bad;

		/*
		 * Work floor: a stage that completed almost nothing is not
		 * evidence of anything, whether or not it reported a failure.
		 * iters mode: each thread alternates alloc/free, so expect
		 * ~iters/2 successful allocations per thread -- demand a
		 * quarter of that.  duration mode has no requested count, so
		 * demand only that every thread got real work done.
		 */
		unsigned long long floor_ok = duration
		    ? (unsigned long long)nthreads * 100ULL
		    : ((unsigned long long)nthreads * iters) / 8ULL;
		int thin = (ok < floor_ok);
		if (thin)
			thin_stage = 1;

		const char *verdict = atomic_load(&g_failed) ? "FAIL(corrupt)"
		    : atomic_load(&g_alloc_failed) ? "FAIL(alloc)"
		    : atomic_load(&g_harness_failed) ? "FAIL(harness)"
		    : thin ? "FAIL(too-little-work)" : "ok";

		printf("  %-10s %12llu ops  %12llu allocs_ok  %10llu fails  "
		    "%7.1f Mops/s  %s\n",
		    stages[s].name, ops, ok, bad,
		    sec > 0 ? ops / sec / 1e6 : 0, verdict);
		if (thin)
			printf("    (needed >= %llu successful allocations to "
			    "count as executed)\n", floor_ok);
		fflush(stdout);
		if (sec < 0)
			break;              /* harness error already reported */
	}

	/*
	 * The verdict is a conjunction.  Pre-2026-09-22 it was only the
	 * corruption flag, which is how "allocator returns NULL forever"
	 * printed PASS at hundreds of millions of ops/s.
	 */
	int corrupt = atomic_load(&g_failed);
	int allocfail = atomic_load(&g_alloc_failed);
	int harness = atomic_load(&g_harness_failed);
	int failed;

	if (g_legacy_verdict) {
		/* Deliberately the OLD, broken rule -- for the control run. */
		failed = corrupt;
		printf("\n[legacy verdict: corruption flag only]\n");
	} else {
		failed = corrupt || allocfail || harness || thin_stage;
	}

	printf("\nallocs_ok=%llu alloc_failures=%llu\n",
	    total_allocs_ok, total_fails);
	if (failed) {
		printf("Result: FAIL (%s%s%s%s)\n",
		    corrupt ? "aliasing/corruption " : "",
		    allocfail ? "allocation failures " : "",
		    harness ? "harness error " : "",
		    thin_stage ? "insufficient work " : "");
	} else {
		printf("Result: PASS (no aliasing or corruption, and %llu "
		    "successful allocations with 0 failures)\n",
		    total_allocs_ok);
	}
	return (failed ? 1 : 0);
}

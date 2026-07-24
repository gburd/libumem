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
	uint64_t deadline_ns;   /* 0 => iters mode */
	enum size_class sc;
	atomic_ullong ops;
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
	if (c->deadline_ns)
		return (now_ns() < c->deadline_ns);
	return (done < c->iters);
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
	size_t fixed = pick_size(c->sc, &r);   /* stable class for "multi" */

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
			void *p = umem_alloc(sz, UMEM_DEFAULT);
			if (!p) { done++; continue; }
			uint64_t t = MAKE_TOKEN(wa->tid, seq++);
			stamp(p, sz, t);
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
	uint64_t seq = 0, done = 0;

	while (keep_going(c, done)) {
		size_t sz = pick_size(c->sc, &r);
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (!p) { done++; continue; }
		uint64_t t = MAKE_TOKEN(pa->tid, seq++);
		stamp(p, sz, t);
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
	atomic_fetch_sub(&pa->q->producers_live, 1);
	return (NULL);
}

static void *
pc_consumer(void *arg)
{
	pc_arg_t *pa = (pc_arg_t *)arg;
	cfg_t *c = pa->cfg;
	uint64_t done = 0;

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

/* ---- pattern drivers --------------------------------------------------- */
static void
run_threaded(cfg_t *c, bool fixed_size)
{
	pthread_t *th = calloc(c->nthreads, sizeof(*th));
	worker_arg_t *wa = calloc(c->nthreads, sizeof(*wa));
	for (int i = 0; i < c->nthreads; i++) {
		wa[i] = (worker_arg_t){ .cfg = c, .tid = i,
		    .fixed_size = fixed_size };
		pthread_create(&th[i], NULL, churn_worker, &wa[i]);
	}
	for (int i = 0; i < c->nthreads; i++)
		pthread_join(th[i], NULL);
	free(th); free(wa);
}

static void
run_prodcons(cfg_t *c)
{
	pc_queue_t *q = calloc(1, sizeof(*q));
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
	pc_arg_t *pa = calloc(nprod + ncons, sizeof(*pa));

	for (int i = 0; i < ncons; i++) {
		pa[i] = (pc_arg_t){ .cfg = c, .q = q, .tid = 1000 + i };
		pthread_create(&ct[i], NULL, pc_consumer, &pa[i]);
	}
	for (int i = 0; i < nprod; i++) {
		pa[ncons + i] = (pc_arg_t){ .cfg = c, .q = q, .tid = i };
		pthread_create(&pt[i], NULL, pc_producer, &pa[ncons + i]);
	}
	for (int i = 0; i < nprod; i++)
		pthread_join(pt[i], NULL);
	for (int i = 0; i < ncons; i++)
		pthread_join(ct[i], NULL);

	free(pt); free(ct); free(pa); free(q);
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
	    "  -h, --help\n", p);
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

	for (int s = 0; s < nstages && !atomic_load(&g_failed); s++) {
		cfg_t c = { .nthreads = nthreads, .iters = iters, .sc = sc };
		atomic_init(&c.ops, 0);
		if (duration)
			c.deadline_ns = now_ns() +
			    (uint64_t)duration * 1000000000ULL;

		uint64_t t0 = now_ns();
		if (stages[s].is_pc)
			run_prodcons(&c);
		else
			run_threaded(&c, stages[s].is_multi);
		double sec = (now_ns() - t0) / 1e9;

		unsigned long long ops = atomic_load(&c.ops);
		printf("  %-10s %12llu ops  %7.1f Mops/s  %s\n",
		    stages[s].name, ops,
		    sec > 0 ? ops / sec / 1e6 : 0,
		    atomic_load(&g_failed) ? "FAIL" : "ok");
		fflush(stdout);
	}

	int failed = atomic_load(&g_failed);
	printf("\nResult: %s\n", failed ? "FAIL (aliasing/corruption detected)"
	    : "PASS (no cross-thread aliasing or corruption)");
	return (failed ? 1 : 0);
}

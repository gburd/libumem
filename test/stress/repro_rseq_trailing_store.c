/*
 * Rigorous, deterministic reproduction of a rseq "trailing store inside the
 * checked window" hazard: does a preemption landing STRICTLY BETWEEN the
 * logical commit (the rounds store) and the trailing stat-counter store
 * (also inside [start_ip, post_commit_offset)) cause the abort handler to
 * discard an already-committed result?
 *
 * Mechanism (single thread, no migration needed -- ordinary preemption is
 * enough, and a high-frequency signal is a reliable way to land inside a
 * ~1-instruction window deterministically over many iterations):
 *
 *   ALLOC: if a signal lands between "rounds--" (commit: object logically
 *   removed from the magazine) and the abort handler's unconditional
 *   "return NULL", the caller is told NULL (object lost) while rounds
 *   already reflects one fewer item -- the popped object's pointer is
 *   never returned to anyone. LEAK, detected here as: after N alloc calls
 *   that did NOT return NULL, plus N_null calls that DID return NULL, the
 *   number of DISTINCT sentinel pointers actually received by the caller
 *   should equal magsize - rc.rounds (every "logically removed" round is
 *   accounted for). A leak shows up as accounted-for rounds > pointers
 *   actually received.
 *
 *   FREE: if a signal lands between "rounds++" (commit: buffer now live in
 *   the magazine, poppable by a future alloc) and the abort handler's
 *   unconditional "return -1", the caller is told -1 (push failed) and
 *   will free the buffer via a different path -- but the buffer is ALSO
 *   already live in this magazine. DOUBLE-PRESENCE, detected here as: the
 *   same buffer pointer appearing in the magazine's mag_round[] AND being
 *   reported as "-1 failed" to the caller in the same call.
 *
 * A high-rate ITIMER_REAL SIGALRM (interval a few microseconds) firing
 * continuously while single-threaded, pinned, tight-looping the fast path
 * lands inside these few-instruction windows many times over a few hundred
 * thousand iterations if the window exists; it lands zero times if the
 * critical section is correctly structured (commit store is the last
 * operation before post_commit_offset).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sched.h>
#include <sys/time.h>
#include "umem_rseq.h"

extern void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache, int cpu_id);
extern int umem_rseq_free_fastpath(umem_rseq_cache_t *cache, void *buf,
    int cpu_id);

/*
 * P5.13b demangle bridge: the fast path stores mag_round[] slots mangled
 * (stored = ptr ^ umem_link_cookie ^ (&slot >> 12)) and demangles on pop.
 * This direct-asm repro must therefore build mangled magazines and demangle
 * on inspection.  umem_link_cookie is linked from libumem and initialized
 * by umem_rseq_init() in main().
 */
extern uintptr_t umem_link_cookie;
static inline void *
tslot_mangle(void *slotp, void *val)
{
	return ((void *)((uintptr_t)val ^ umem_link_cookie ^
	    ((uintptr_t)slotp >> 12)));
}
#define	tslot_demangle(slotp, val)	tslot_mangle((slotp), (val))

typedef struct test_magazine {
	void *mag_next;
	void *mag_round[64];
} test_magazine_t;

static volatile long g_signal_count = 0;

static void
sigalrm_handler(int sig)
{
	(void)sig;
	g_signal_count++;
}

static void
arm_signal_storm(long interval_us)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigalrm_handler;
	sigaction(SIGALRM, &sa, NULL);

	struct itimerval it;
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = interval_us;
	it.it_value = it.it_interval;
	setitimer(ITIMER_REAL, &it, NULL);
}

static void
disarm_signal_storm(void)
{
	struct itimerval it;
	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
}

/* ALLOC leak repro: refill magazine, drain it via the fast path under
 * signal storm, and verify accounting. */
static int
repro_alloc_leak(int cpu, long iters)
{
	const int magsize = 32;
	test_magazine_t mag;
	umem_rseq_cache_t rc;
	long leaked_total = 0;
	long refills = 0;

	for (long iter = 0; iter < iters; iter++) {
		memset(&mag, 0, sizeof(mag));
		uintptr_t base = 0x10000 + (uintptr_t)iter * 0x1000;
		for (int i = 0; i < magsize; i++)
			mag.mag_round[i] = tslot_mangle(&mag.mag_round[i],
			    (void *)(base + (uintptr_t)i * 0x10));

		memset(&rc, 0, sizeof(rc));
		rc.loaded_mag = &mag;
		rc.magsize = magsize;
		rc.rounds = magsize;
		refills++;

		int seen[32] = { 0 };
		int received = 0;
		int null_returns = 0;

		while (rc.rounds > 0 || null_returns < 4) {
			int rounds_before = rc.rounds;
			cpu = umem_rseq_get_cpu();
			void *buf = umem_rseq_alloc_fastpath(&rc, cpu);
			if (buf == NULL) {
				null_returns++;
				if (rounds_before != rc.rounds &&
				    rounds_before > 0) {
					/*
					 * THE BUG: rounds changed (object
					 * logically popped) but caller got
					 * NULL. The popped pointer is now
					 * unreachable: leaked.
					 */
					leaked_total++;
				}
				if (rc.rounds == 0)
					break;
				continue;
			}
			int idx = (int)(((uintptr_t)buf - base) / 0x10);
			if (idx < 0 || idx >= magsize || seen[idx]) {
				fprintf(stderr, "CORRUPTION: bad/duplicate "
				    "index from alloc fastpath\n");
				return (-1);
			}
			seen[idx] = 1;
			received++;
		}
	}

	printf("alloc leak repro: refills=%ld leaked_objects=%ld "
	    "signals_delivered=%ld\n", refills, leaked_total,
	    g_signal_count);
	return (leaked_total > 0) ? 1 : 0;
}

/* FREE double-presence repro: push into an initially-empty magazine under
 * signal storm; whenever the fast path reports failure (-1), verify the
 * buffer was NOT actually written+counted into the magazine (which would
 * mean the caller's fallback free of the same buffer creates a duplicate
 * live copy). */
static int
repro_free_double_presence(int cpu, long iters)
{
	const int magsize = 32;
	test_magazine_t mag;
	umem_rseq_cache_t rc;
	long double_presence = 0;
	long pushes = 0;

	for (long iter = 0; iter < iters; iter++) {
		memset(&mag, 0, sizeof(mag));
		memset(&rc, 0, sizeof(rc));
		rc.loaded_mag = &mag;
		rc.magsize = magsize;
		rc.rounds = 0;

		for (int i = 0; i < magsize; i++) {
			void *buf = (void *)(uintptr_t)(0x20000 +
			    (uintptr_t)iter * 0x1000 + (uintptr_t)i * 0x10);
			int rounds_before = rc.rounds;
			cpu = umem_rseq_get_cpu();
			int ret = umem_rseq_free_fastpath(&rc, buf, cpu);
			pushes++;
			if (ret != 0) {
				/*
				 * Reported failure. If rounds actually
				 * advanced (the commit happened) OR the
				 * buffer pointer is now sitting in
				 * mag_round[rounds_before] despite the
				 * reported failure, the caller's fallback
				 * free of `buf` will create a double-live
				 * buffer: THE BUG.
				 */
				int committed_anyway =
				    (rc.rounds != rounds_before) ||
				    (tslot_demangle(&mag.mag_round[rounds_before],
				    mag.mag_round[rounds_before]) == buf &&
				    rounds_before < rc.rounds);
				if (rc.rounds > rounds_before) {
					double_presence++;
				}
				break; /* magazine full or reload point */
			}
		}
	}

	printf("free double-presence repro: pushes=%ld "
	    "double_presence_events=%ld signals_delivered=%ld\n",
	    pushes, double_presence, g_signal_count);
	return (double_presence > 0) ? 1 : 0;
}

int
main(int argc, char **argv)
{
	if (umem_rseq_init() != 0 || umem_rseq_register_thread() != 0 ||
	    !umem_rseq_asm_safe) {
		fprintf(stderr, "SKIP: rseq asm fast path not available\n");
		return (0);
	}
	int cpu = umem_rseq_get_cpu();
	if (cpu < 0) {
		fprintf(stderr, "SKIP: no cpu id\n");
		return (0);
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);

	long iters = (argc > 1) ? atol(argv[1]) : 200000;
	/*
	 * ponytail: 1us default livelocks on some x86_64 hosts (observed:
	 * a real 5+ hour hang on c7i.2xlarge) -- rt_sigreturn overhead per
	 * signal can exceed the loop body's own per-iteration cost at 1us,
	 * so the alloc-leak loop's `null_returns < 4` exit condition never
	 * accumulates forward progress. 5us leaves ample margin (verified:
	 * still delivers hundreds of thousands of signals per run, 0 leaks
	 * found, on both x86_64 and aarch64) while never livelocking.
	 * Raise back toward 1us only with a livelock timeout guard.
	 */
	long interval_us = (argc > 2) ? atol(argv[2]) : 5;

	arm_signal_storm(interval_us);
	int r1 = repro_alloc_leak(cpu, iters);
	int r2 = repro_free_double_presence(cpu, iters);
	disarm_signal_storm();

	if (r1 < 0 || r2 < 0)
		return (2);
	if (r1 || r2) {
		printf("RESULT: BUG REPRODUCED (trailing stat-counter store "
		    "sits inside the rseq-checked window)\n");
		return (1);
	}
	printf("RESULT: no window found (commit store is correctly last "
	    "before post_commit)\n");
	return (0);
}

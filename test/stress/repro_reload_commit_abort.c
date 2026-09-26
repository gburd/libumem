/*
 * P8.5b aliasing regression: an rseq reload commit must be ALL-OR-NOTHING
 * under an abort landing anywhere in its critical section.
 *
 * An rseq abort does not roll back stores that already retired.  So a
 * critical section is atomic only if every store before the last one leaves
 * the slot in a state equivalent to "not yet committed", and nothing after
 * the last store can still turn a commit into a reported abort.
 *
 * This test drives umem_rseq_reload_{alloc,free}_commit on a private slot,
 * pinned to one CPU, under a microsecond SIGALRM storm (every delivery inside
 * the window forces an abort), and checks the contract its callers rely on:
 *
 *   returned 1: slot == (new_mag, new_rounds) and the out-params == (old, r)
 *   returned 0: slot == (old, r), unchanged
 *
 * Anything else means one magazine has two owners or none:
 *   TORN            slot is neither state (e.g. (new, 0) or (new, old_r))
 *   COMMIT-REPORTED-ABORT  slot == (new, new_rounds) but 0 was returned; the
 *                   C slowpath then re-publishes or depot-frees a magazine
 *                   the slot still holds.
 *
 * Exit 0 PASS, 1 FAIL, 77 SKIP (no rseq asm).
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "umem_rseq.h"

extern int umem_rseq_reload_alloc_commit(umem_rseq_cache_t *cache,
    int cpu_id, void *new_mag, int new_rounds, void **old_mag_out,
    int *old_rounds_out);
extern int umem_rseq_reload_free_commit(umem_rseq_cache_t *cache,
    int cpu_id, void *new_mag, void **old_mag_out, int *old_rounds_out);

#define	OLD_MAG	((void *)0x1000)
#define	NEW_MAG	((void *)0x2000)
#define	OLD_R	3
#define	NEW_R	7
#define	MAGSZ	7

static volatile long g_sig;
static void on_alrm(int s) { (void)s; g_sig++; }

static void
storm(long us)
{
	struct sigaction sa;
	struct itimerval it;

	memset(&sa, 0, sizeof (sa));
	sa.sa_handler = on_alrm;
	(void) sigaction(SIGALRM, &sa, NULL);
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = us;
	it.it_value = it.it_interval;
	(void) setitimer(ITIMER_REAL, &it, NULL);
}

/* side 0: alloc commit, side 1: free commit.  Returns # of violations. */
static long
run(int side, long iters, long *commits, long *aborts)
{
	umem_rseq_cache_t rc;
	long torn = 0, cra = 0;
	int want_r = side ? 0 : NEW_R;

	for (long i = 0; i < iters; i++) {
		void *om = (void *)-1;
		int orr = -1, r;
		int cpu = umem_rseq_get_cpu();

		memset(&rc, 0, sizeof (rc));
		rc.loaded_mag = OLD_MAG;
		rc.rounds = OLD_R;
		rc.magsize = MAGSZ;
		r = side ?
		    umem_rseq_reload_free_commit(&rc, cpu, NEW_MAG, &om, &orr) :
		    umem_rseq_reload_alloc_commit(&rc, cpu, NEW_MAG, NEW_R,
		    &om, &orr);
		int is_old = rc.loaded_mag == OLD_MAG && rc.rounds == OLD_R;
		int is_new = rc.loaded_mag == NEW_MAG && rc.rounds == want_r;
		int v = 0;
		if (r == 1) {
			(*commits)++;
			if (!is_new || om != OLD_MAG || orr != OLD_R)
				v = 1, torn++;
		} else {
			(*aborts)++;
			if (is_new)
				v = 1, cra++;
			else if (!is_old)
				v = 1, torn++;
		}
		if (v && torn + cra <= 3)
			fprintf(stderr, "  %s iter %ld: ret=%d slot=(%p,%d)\n",
			    side ? "free" : "alloc", i, r, rc.loaded_mag,
			    rc.rounds);
	}
	printf("%s commit: torn=%ld commit_reported_abort=%ld\n",
	    side ? "free " : "alloc", torn, cra);
	return (torn + cra);
}

int
main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 20000000;
	long us = argc > 2 ? atol(argv[2]) : 5;
	long commits = 0, aborts = 0, bad;
	cpu_set_t set;
	int cpu;

	if (umem_rseq_init() != 0 || umem_rseq_register_thread() != 0 ||
	    !umem_rseq_asm_safe) {
		fprintf(stderr, "SKIP: rseq asm fast path not available\n");
		return (77);
	}
	cpu = umem_rseq_get_cpu();
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (cpu < 0 || sched_setaffinity(0, sizeof (set), &set) != 0) {
		fprintf(stderr, "SKIP: could not pin\n");
		return (77);
	}

	storm(us);
	bad = run(0, iters, &commits, &aborts);
	bad += run(1, iters, &commits, &aborts);
	storm(0);

	printf("commits=%ld aborts=%ld signals=%ld\n", commits, aborts, g_sig);
	if (aborts == 0) {
		/* No abort ever landed: the test proved nothing. */
		printf("RESULT: INCONCLUSIVE (0 aborts)\n");
		return (1);
	}
	printf("RESULT: %s\n", bad ? "FAIL (reload commit not atomic)" :
	    "PASS");
	return (bad ? 1 : 0);
}

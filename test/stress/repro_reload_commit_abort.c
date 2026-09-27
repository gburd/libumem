/*
 * P8.5b aliasing regression: an rseq reload commit must be ALL-OR-NOTHING
 * under an abort landing anywhere in its critical section.
 *
 * An rseq abort does not roll back stores that already retired.  So a
 * critical section is atomic only if every store before the last one leaves
 * the slot in a state equivalent to "not yet committed", and nothing after
 * the last store can still turn a commit into a reported abort.
 *
 * This test drives the reload's rseq sections on a private slot, pinned to
 * one CPU, under a microsecond SIGALRM storm (every delivery inside a window
 * forces an abort), and checks the contract its callers rely on: returned 1
 * means the slot is exactly the committed state, and returned 0 means it is
 * exactly the prior state.
 *
 * Anything else means one magazine has two owners or none:
 *   TORN            slot is neither state
 *   COMMIT-REPORTED-ABORT  the slot committed but 0 was returned, so the C
 *                   slowpath re-publishes or depot-frees a magazine the slot
 *                   still holds.
 *
 * PRE-FIX (017520f) this drove the old single swap commits,
 * umem_rseq_reload_{alloc,free}_commit, and FAILED every run on both arches
 * with thousands of torn and commit-reported-abort states
 * (docs/results/r5fix/prefix-*.log).  Those commits were replaced by
 * detach/attach, and this file was ported to them.
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

/*
 * Post-fix ABI (the swap commit this file first proved non-atomic was split
 * into these two single-store sections).  The contract is the same one:
 *   detach 1: out == (old, r), slot loaded_mag == NULL
 *   detach 0: slot unchanged
 *   attach 1: slot == (new, rounds, magsize)
 *   attach 0: slot loaded_mag unchanged (rounds/magsize don't-care iff NULL)
 * Pre-fix result (swap commit, 017520f): docs/results/r5fix/prefix-*.log.
 */
extern int umem_rseq_reload_detach(umem_rseq_cache_t *cache, int cpu_id,
    void **old_mag_out, int *old_rounds_out);
extern int umem_rseq_reload_attach(umem_rseq_cache_t *cache, int cpu_id,
    void *new_mag, int rounds, int magsize);

#define	OLD_MAG	((void *)0x1000)
#define	NEW_MAG	((void *)0x2000)
#define	OLD_R	3
#define	NEW_R	7
#define	MAGSZ	7
#define	OLD_MS	5

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

/* side 0: detach, side 1: attach.  Returns # of violations. */
static long
run(int side, long iters, long *commits, long *aborts)
{
	umem_rseq_cache_t rc;
	long torn = 0, cra = 0;

	for (long i = 0; i < iters; i++) {
		void *om = (void *)-1;
		int orr = -1, r, v = 0;
		int cpu = umem_rseq_get_cpu();

		memset(&rc, 0, sizeof (rc));
		rc.loaded_mag = side ? NULL : OLD_MAG;
		rc.rounds = OLD_R;
		rc.magsize = OLD_MS;
		r = side ?
		    umem_rseq_reload_attach(&rc, cpu, NEW_MAG, NEW_R, MAGSZ) :
		    umem_rseq_reload_detach(&rc, cpu, &om, &orr);
		int is_old = side ? rc.loaded_mag == NULL :
		    (rc.loaded_mag == OLD_MAG && rc.rounds == OLD_R &&
		    rc.magsize == OLD_MS);
		int is_new = side ?
		    (rc.loaded_mag == NEW_MAG && rc.rounds == NEW_R &&
		    rc.magsize == MAGSZ) : rc.loaded_mag == NULL;
		if (r == 1) {
			(*commits)++;
			if (!is_new || (!side && (om != OLD_MAG ||
			    orr != OLD_R)))
				v = 1, torn++;
		} else {
			(*aborts)++;
			if (rc.loaded_mag != (side ? NULL : OLD_MAG))
				v = 1, cra += is_new, torn += !is_new;
			else if (!is_old)
				v = 1, torn++;
		}
		if (v && torn + cra <= 3)
			fprintf(stderr, "  %s iter %ld: ret=%d slot=(%p,%d,%d)\n",
			    side ? "attach" : "detach", i, r, rc.loaded_mag,
			    rc.rounds, rc.magsize);
	}
	printf("%s: torn=%ld commit_reported_abort=%ld\n",
	    side ? "attach" : "detach", torn, cra);
	return (torn + cra);
}

/* attach must refuse an occupied slot and write nothing. */
static long
refuse_occupied(int cpu)
{
	umem_rseq_cache_t rc;

	memset(&rc, 0, sizeof (rc));
	rc.loaded_mag = OLD_MAG;
	rc.rounds = OLD_R;
	rc.magsize = OLD_MS;
	while (umem_rseq_reload_attach(&rc, cpu, NEW_MAG, NEW_R, MAGSZ) == 0 &&
	    rc.loaded_mag == OLD_MAG && rc.rounds == OLD_R &&
	    rc.magsize == OLD_MS && rc.restart_count > 0) {
		/* aborted before the occupancy test: retry */
		rc.restart_count = 0;
	}
	if (rc.loaded_mag != OLD_MAG || rc.rounds != OLD_R ||
	    rc.magsize != OLD_MS) {
		printf("attach on an occupied slot WROTE it: (%p,%d,%d)\n",
		    rc.loaded_mag, rc.rounds, rc.magsize);
		return (1);
	}
	return (0);
}

int
main(int argc, char **argv)
{
	/*
	 * Default 2,000,000 iterations per side: what scripts/ec2/p85b_fix_*.sh
	 * gates with, ~1 s on an 8-vCPU box at the 10 us storm below.  The
	 * earlier default (20,000,000 at 5 us) hit a release script's per-test
	 * timeout; the storm interval, not the count, was the cause (see
	 * below).  Pass a larger count explicitly for a longer soak.
	 */
	long iters = argc > 1 ? atol(argv[1]) : 2000000;
	/*
	 * Signal interval.  At 5 us the storm livelocked this test on Debian 13
	 * c7i.2xlarge: 7 of 12 runs made no progress in 300 s.  At 10 us all 8
	 * runs finished in ~1 s on the fixed code, and the pre-fix code
	 * (017520f) still FAILED 3 of 3 with 58-257 violations per run, so
	 * 10 us keeps the test's teeth without the hang.
	 */
	long us = argc > 2 ? atol(argv[2]) : 10;
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
	bad += refuse_occupied(cpu);
	storm(0);

	printf("commits=%ld aborts=%ld signals=%ld\n", commits, aborts, g_sig);
	/*
	 * Too few aborts means the storm never landed where it matters and the
	 * run tested nothing.  The fixed code saw >= 2,397 aborts per run at
	 * 10 us and >= 440 at 20 us; 100 is a floor, not a target.
	 */
	if (aborts < 100) {
		printf("RESULT: INCONCLUSIVE (only %ld aborts)\n", aborts);
		return (1);
	}
	printf("RESULT: %s\n", bad ? "FAIL (reload commit not atomic)" :
	    "PASS");
	return (bad ? 1 : 0);
}

/*
 * Safety counterpart to repro_naive_reload_race.c (design checklist #2).
 *
 * The naive repro proves a plain-C reload double-issues under contention.
 * The armed asm commit (umem_rseq_reload_detach / _attach) is
 * safe for a different reason than "run it under contention and hope": its
 * publish of cache_rseq[cpu] is a registered rseq critical section, so it
 * commits ONLY while the thread is provably still on the slot's CPU, and
 * ABORTS (writing nothing) otherwise.  Since the kernel guarantees at most
 * one thread runs on a CPU at a time, "commit only while on this CPU" is
 * exactly "at most one writer per slot" -- the invariant the fast path
 * needs.  A userspace test cannot force two threads onto one CPU at once,
 * so it cannot reproduce a "safe" race the way the naive repro reproduces
 * an unsafe one; what it CAN test deterministically is the load-bearing
 * mechanism: the commit's cpu_id gate.
 *
 * (Ported 2026-09-26 from the single swap commit to the detach/attach pair
 * that replaced it.  NOTE what this test does NOT cover: it only aborts
 * BEFORE the first store (cpu mismatch), so it passed while the swap commit
 * was non-atomic against an abort BETWEEN its stores.  That is covered by
 * repro_reload_commit_abort; see docs/results/2026-09-26-p85b-aliasing-
 * mechanism-v2.md.)
 *
 * This test verifies, deterministically and single-threaded:
 *   (A) COMMIT-ON-MATCH: called with the thread's real current cpu, the
 *       commit publishes new_mag/new_rounds and returns 1, and returns the
 *       prior loaded_mag + its rounds via the out-params (so the caller can
 *       classify the old magazine for the depot -- the old_rounds ABI).
 *   (B) ABORT-ON-MISMATCH: called with a DELIBERATELY WRONG cpu_id (a value
 *       the thread is not on), the commit must ABORT: return 0 and leave
 *       cache_rseq[cpu] COMPLETELY UNCHANGED (loaded_mag and rounds both as
 *       before).  This is the property that makes a migration between the
 *       caller reading cpu_id and the commit safe: a stale cpu never gets
 *       its slot clobbered.  If the commit wrote the slot anyway on a cpu
 *       mismatch, THAT is the migration-safety hole, and it fails here.
 *
 * PASS => both properties hold on this arch.  FAIL (esp. (B)) => the commit
 * does not honor its rseq cpu gate; arming is unsound, leave it inert.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include "umem_rseq.h"

extern int umem_rseq_reload_detach(umem_rseq_cache_t *cache, int cpu_id,
    void **old_mag_out, int *old_rounds_out);
extern int umem_rseq_reload_attach(umem_rseq_cache_t *cache, int cpu_id,
    void *new_mag, int rounds, int magsize);

typedef struct test_magazine {
	void *mag_next;
	void *mag_round[64];
} test_magazine_t;

#define MAGSIZE 15

static int
pin_current(int *cpu_out)
{
	int cpu = umem_rseq_get_cpu();
	if (cpu < 0)
		return (-1);
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
		return (-1);
	*cpu_out = umem_rseq_get_cpu();
	return (*cpu_out == cpu) ? 0 : -1;
}

int
main(void)
{
	if (umem_rseq_init() != 0 || umem_rseq_register_thread() != 0 ||
	    !umem_rseq_asm_safe) {
		fprintf(stderr, "SKIP: rseq asm fast path not available\n");
		return (0);
	}
	int cpu;
	if (pin_current(&cpu) != 0) {
		fprintf(stderr, "SKIP: could not pin to a CPU\n");
		return (0);
	}

	int fail = 0;
	test_magazine_t old_mag, new_mag;
	umem_rseq_cache_t rc;

	void *out_mag;
	int out_rounds, r;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	int wrong = (ncpu >= 2) ? (cpu ^ 1) : 999999;

	/* --- (A) detach on match: slot -> NULL, old (mag, rounds) out --- */
	memset(&old_mag, 0, sizeof(old_mag));
	memset(&rc, 0, sizeof(rc));
	rc.magsize = MAGSIZE;
	rc.loaded_mag = &old_mag;
	rc.rounds = 3;			/* a partial old magazine */
	out_mag = NULL;
	out_rounds = -1;
	r = umem_rseq_reload_detach(&rc, cpu, &out_mag, &out_rounds);
	if (r != 1 || rc.loaded_mag != NULL || out_mag != &old_mag ||
	    out_rounds != 3) {
		fprintf(stderr, "FAIL(A): detach on match wrong (r=%d "
		    "loaded_mag=%p old_mag=%p old_rounds=%d)\n", r,
		    rc.loaded_mag, out_mag, out_rounds);
		fail = 1;
	}

	/* --- (A') attach on match: NULL -> (new, rounds, magsize) --- */
	memset(&new_mag, 0, sizeof(new_mag));
	r = umem_rseq_reload_attach(&rc, cpu, &new_mag, MAGSIZE, MAGSIZE);
	if (r != 1 || rc.loaded_mag != &new_mag || rc.rounds != MAGSIZE ||
	    rc.magsize != MAGSIZE) {
		fprintf(stderr, "FAIL(A'): attach on match wrong (r=%d "
		    "loaded_mag=%p rounds=%d magsize=%d)\n", r, rc.loaded_mag,
		    rc.rounds, rc.magsize);
		fail = 1;
	}

	/* --- (B) detach with a WRONG cpu: abort, slot untouched --- */
	r = umem_rseq_reload_detach(&rc, wrong, &out_mag, &out_rounds);
	if (r != 0 || rc.loaded_mag != &new_mag || rc.rounds != MAGSIZE) {
		fprintf(stderr, "FAIL(B): detach with WRONG cpu_id=%d (thread "
		    "on %d) r=%d loaded_mag=%p rounds=%d -- migration-safety "
		    "hole\n", wrong, cpu, r, rc.loaded_mag, rc.rounds);
		fail = 1;
	}

	/* --- (B') attach with a WRONG cpu: abort, slot untouched --- */
	memset(&rc, 0, sizeof(rc));
	r = umem_rseq_reload_attach(&rc, wrong, &new_mag, MAGSIZE, MAGSIZE);
	if (r != 0 || rc.loaded_mag != NULL) {
		fprintf(stderr, "FAIL(B'): attach with WRONG cpu published "
		    "(r=%d loaded_mag=%p)\n", r, rc.loaded_mag);
		fail = 1;
	}

	/* --- (C) attach on an OCCUPIED slot: refuse, write nothing --- */
	memset(&rc, 0, sizeof(rc));
	rc.loaded_mag = &old_mag;
	rc.rounds = 2;
	rc.magsize = 4;
	r = umem_rseq_reload_attach(&rc, cpu, &new_mag, MAGSIZE, MAGSIZE);
	if (r != 0 || rc.loaded_mag != &old_mag || rc.rounds != 2 ||
	    rc.magsize != 4) {
		fprintf(stderr, "FAIL(C): attach on an occupied slot wrote it "
		    "(r=%d loaded_mag=%p rounds=%d magsize=%d) -- two "
		    "magazines, one slot\n", r, rc.loaded_mag, rc.rounds,
		    rc.magsize);
		fail = 1;
	}

	if (fail) {
		printf("RESULT: FAIL -- the armed reload commit does not "
		    "honor its rseq cpu gate; arming is unsound.\n");
		return (1);
	}
	printf("RESULT: PASS -- commit publishes on cpu match (with correct "
	    "old-magazine rounds ABI) and ABORTS leaving the slot untouched "
	    "on cpu mismatch (migration-safety gate holds).\n");
	return (0);
}

/*
 * Safety counterpart to repro_naive_reload_race.c (design checklist #2).
 *
 * The naive repro proves a plain-C reload double-issues under contention.
 * The armed asm commit (umem_rseq_reload_alloc_commit / _free_commit) is
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

extern int umem_rseq_reload_alloc_commit(umem_rseq_cache_t *cache,
    int cpu_id, void *new_mag, int new_rounds, void **old_mag_out,
    int *old_rounds_out);
extern int umem_rseq_reload_free_commit(umem_rseq_cache_t *cache,
    int cpu_id, void *new_mag, void **old_mag_out, int *old_rounds_out);

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

	/* --- (A) COMMIT-ON-MATCH (alloc side) --- */
	memset(&old_mag, 0, sizeof(old_mag));
	memset(&new_mag, 0, sizeof(new_mag));
	memset(&rc, 0, sizeof(rc));
	rc.magsize = MAGSIZE;
	rc.loaded_mag = &old_mag;
	rc.rounds = 3;			/* a partial old magazine */
	void *out_mag = NULL;
	int out_rounds = -1;
	int r = umem_rseq_reload_alloc_commit(&rc, cpu, &new_mag, MAGSIZE,
	    &out_mag, &out_rounds);
	if (r != 1) {
		fprintf(stderr, "FAIL(A): commit-on-match returned %d, "
		    "want 1\n", r);
		fail = 1;
	}
	if (rc.loaded_mag != &new_mag || rc.rounds != MAGSIZE) {
		fprintf(stderr, "FAIL(A): slot not published (loaded_mag=%p "
		    "want %p, rounds=%d want %d)\n", rc.loaded_mag,
		    (void *)&new_mag, rc.rounds, MAGSIZE);
		fail = 1;
	}
	if (out_mag != &old_mag || out_rounds != 3) {
		fprintf(stderr, "FAIL(A): old-magazine ABI wrong (old_mag=%p "
		    "want %p, old_rounds=%d want 3) -- depot would be "
		    "misclassified\n", out_mag, (void *)&old_mag, out_rounds);
		fail = 1;
	}

	/* --- (B) ABORT-ON-MISMATCH (alloc side): the migration-safety gate.
	 * Use a cpu_id the thread is NOT on.  On a >=2-cpu box use (cpu ^ 1);
	 * if only 1 cpu is online, use a large invalid id.  The kernel's rseq
	 * check must abort the commit: return 0, slot untouched. --- */
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	int wrong = (ncpu >= 2) ? (cpu ^ 1) : 999999;
	memset(&old_mag, 0, sizeof(old_mag));
	memset(&new_mag, 0, sizeof(new_mag));
	memset(&rc, 0, sizeof(rc));
	rc.magsize = MAGSIZE;
	rc.loaded_mag = &old_mag;
	rc.rounds = 7;
	out_mag = (void *)0xdead;
	out_rounds = -42;
	r = umem_rseq_reload_alloc_commit(&rc, wrong, &new_mag, MAGSIZE,
	    &out_mag, &out_rounds);
	if (r != 0) {
		fprintf(stderr, "FAIL(B): commit with WRONG cpu_id=%d "
		    "(thread on %d) returned %d, want 0 (abort) -- the rseq "
		    "cpu gate did not fire; migration-safety hole\n",
		    wrong, cpu, r);
		fail = 1;
	}
	if (rc.loaded_mag != &old_mag || rc.rounds != 7) {
		fprintf(stderr, "FAIL(B): CPU-mismatched commit CLOBBERED the "
		    "slot (loaded_mag=%p want %p, rounds=%d want 7) -- this is "
		    "exactly the migration corruption arming must never do\n",
		    rc.loaded_mag, (void *)&old_mag, rc.rounds);
		fail = 1;
	}

	/* --- (A') COMMIT-ON-MATCH (free side), old_rounds ABI --- */
	memset(&old_mag, 0, sizeof(old_mag));
	memset(&new_mag, 0, sizeof(new_mag));
	memset(&rc, 0, sizeof(rc));
	rc.magsize = MAGSIZE;
	rc.loaded_mag = &old_mag;
	rc.rounds = MAGSIZE;		/* a full old magazine */
	out_mag = NULL;
	out_rounds = -1;
	r = umem_rseq_reload_free_commit(&rc, cpu, &new_mag, &out_mag,
	    &out_rounds);
	if (r != 1 || rc.loaded_mag != &new_mag || rc.rounds != 0 ||
	    out_mag != &old_mag || out_rounds != MAGSIZE) {
		fprintf(stderr, "FAIL(A'): free commit-on-match wrong "
		    "(r=%d loaded_mag=%p rounds=%d old_mag=%p old_rounds=%d)\n",
		    r, rc.loaded_mag, rc.rounds, out_mag, out_rounds);
		fail = 1;
	}

	/* --- (B') ABORT-ON-MISMATCH (free side) --- */
	memset(&old_mag, 0, sizeof(old_mag));
	memset(&new_mag, 0, sizeof(new_mag));
	memset(&rc, 0, sizeof(rc));
	rc.magsize = MAGSIZE;
	rc.loaded_mag = &old_mag;
	rc.rounds = MAGSIZE;
	r = umem_rseq_reload_free_commit(&rc, wrong, &new_mag, &out_mag,
	    &out_rounds);
	if (r != 0 || rc.loaded_mag != &old_mag || rc.rounds != MAGSIZE) {
		fprintf(stderr, "FAIL(B'): free commit with WRONG cpu did not "
		    "abort cleanly (r=%d loaded_mag=%p rounds=%d)\n",
		    r, rc.loaded_mag, rc.rounds);
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

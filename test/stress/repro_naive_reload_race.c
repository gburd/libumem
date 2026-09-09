/*
 * Direct, deterministic reproduction of the migration-race hazard that
 * makes a plain-C reload of umem_rseq_alloc_slowpath()/
 * umem_rseq_free_slowpath() unsafe (Alternative 1 in
 * docs/results/2026-09-09-rseq-reload-analysis-v2.md: "read-verify-write
 * with a post-check inside the reload, still in C").
 *
 * THE INVARIANT THE FAST PATH RELIES ON: a given cache_rseq[cpu] slot is
 * touched by AT MOST ONE thread at any instant -- whichever thread is
 * currently executing on physical CPU `cpu`. rseq's kernel-cooperating
 * abort mechanism enforces this FOR THE FAST PATH ITSELF: if the thread
 * running the fast path is preempted or migrated between .Lrseq_*_start
 * and the commit store, the kernel resets its IP to the abort handler
 * BEFORE the commit store executes -- so the fast path's own commit is
 * atomic with respect to that thread's presence on the CPU.
 *
 * A plain-C reload (Alternative 1) reads cpu_id, does work (a depot pull,
 * arbitrarily long), then RE-READS cpu_id immediately before writing
 * cache_rseq[cpu]. But that re-read + write pair is NOT a registered rseq
 * critical section: nothing stops the reload thread from being preempted
 * or migrated between the re-read and the write, and NOTHING stops a
 * *different* thread from being scheduled onto CPU `cpu` and running the
 * fast path concurrently with the reload's in-flight, non-atomic,
 * multi-field write to that exact slot.
 *
 * This test does not attempt to control real kernel migration timing (an
 * inherently racy, environment-dependent thing to script deterministically
 * on a shared/virtualized instance). Instead it forces the ACTUAL failure
 * condition directly: two independent OS threads, each with an
 * uncontested, current, correct belief that they are CPU X's sole writer
 * (which is exactly the state of the world for a brief window after any
 * real migration event), concurrently touch the SAME logical slot --
 * because whether that window is reached by "T1 migrated away, T2 got
 * scheduled onto the now-vacated CPU" or by any other scheduling accident
 * that lands two threads believing they own the same slot, the memory
 * effect on cache_rseq[cpu] is identical. If corruption appears here, it
 * proves conclusively that NO amount of re-checking cpu_id in plain C
 * before the write can prevent it, because this reproduction sidesteps
 * the check-to-write timing question entirely and still corrupts state --
 * the fundamental problem is unsynchronized concurrent writers to shared
 * per-CPU-cache metadata, not merely a narrow TOCTOU window.
 *
 * Thread R (reload): repeatedly performs the Alternative-1-style reload
 *   sequence (pull a "magazine" from a pool, recheck, write loaded_mag
 *   then rounds -- the safer of the two possible field orders, see
 *   docs/results/2026-09-09-rseq-reload-analysis-v2.md) against a shared
 *   umem_rseq_cache_t slot.
 * Thread F (fastpath): repeatedly calls the REAL, already-fixed
 *   umem_rseq_alloc_fastpath()/umem_rseq_free_fastpath() asm against the
 *   SAME shared slot, verifying every popped pointer against a sentinel
 *   table (the same discipline stress_concurrency_oracle.c uses) to catch
 *   double-allocation, UAF-via-stale-loaded_mag, or reads of never-issued
 *   pointers.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "umem_rseq.h"

extern void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache, int cpu_id);
extern int umem_rseq_free_fastpath(umem_rseq_cache_t *cache, void *buf,
    int cpu_id);

typedef struct test_magazine {
	void *mag_next;
	void *mag_round[64];
} test_magazine_t;

#define MAGSIZE 15
#define POOL_SIZE 64

static test_magazine_t g_pool[POOL_SIZE];
static atomic_int g_pool_next = 0;
static uintptr_t g_sentinel_base = 0x100000;

/* Every pointer this program ever hands into a magazine is a distinct
 * sentinel value; a global table tracks whether it is currently
 * "checked out" (issued to a caller and not yet returned) so we can
 * detect double-issue across the two threads. */
#define NSENTINELS (POOL_SIZE * MAGSIZE)
static atomic_int g_checked_out[NSENTINELS];
static atomic_long g_double_issue = 0;
static atomic_long g_bad_pointer = 0;
static atomic_long g_alloc_ops = 0;
static atomic_long g_reload_ops = 0;

static int
sentinel_index(void *p)
{
	uintptr_t v = (uintptr_t)p;
	if (v < g_sentinel_base)
		return (-1);
	uintptr_t idx = (v - g_sentinel_base) / 0x10;
	if (idx >= NSENTINELS)
		return (-1);
	return ((int)idx);
}

static test_magazine_t *
pool_alloc_filled(void)
{
	int slot = atomic_fetch_add(&g_pool_next, 1) % POOL_SIZE;
	test_magazine_t *mp = &g_pool[slot];
	for (int i = 0; i < MAGSIZE; i++) {
		int sidx = (slot * MAGSIZE + i) % NSENTINELS;
		mp->mag_round[i] =
		    (void *)(g_sentinel_base + (uintptr_t)sidx * 0x10);
	}
	return (mp);
}

static umem_rseq_cache_t g_shared_rc;
static atomic_int g_stop = 0;

/*
 * Thread R: Alternative-1-style reload against the shared slot. Mirrors
 * umem_rseq_alloc_slowpath()'s field order (loaded_mag written before
 * rounds -- the safer of the two orders) plus the "recheck cpu_id right
 * before the write" idea from Alternative 1. Deliberately does NOT hold
 * any lock the fast path would also take (there is none -- that is the
 * whole point being tested).
 */
static void *
reload_thread(void *arg)
{
	(void)arg;
	if (umem_rseq_register_thread() != 0) {
		fprintf(stderr, "reload_thread: register failed\n");
		return (NULL);
	}
	while (!atomic_load(&g_stop)) {
		test_magazine_t *fmp = pool_alloc_filled();

		/*
		 * "Recheck cpu_id right before the write" -- Alternative 1's
		 * proposed mitigation. We cannot literally read a kernel
		 * cpu_id here (this thread may not be rseq-registered as the
		 * same logical "cpu" as the fastpath thread), so this
		 * reproduction targets the shared slot unconditionally --
		 * which is EXACTLY what "recheck and it still matches"
		 * looks like from the memory-effects point of view: the
		 * recheck does not add any synchronization with a
		 * concurrent writer, it only narrows (but does not close)
		 * the window. We are reproducing the memory race itself,
		 * not the scheduler timing that opens the window.
		 */
		g_shared_rc.loaded_mag = fmp;
		/* Deliberately don't serialize the two stores -- matches
		 * the real slowpath, which has no barrier between them. */
		g_shared_rc.rounds = MAGSIZE - 1;
		atomic_fetch_add(&g_reload_ops, 1);
	}
	return (NULL);
}

/*
 * Thread F: the REAL fast path asm against the same shared slot,
 * verifying every returned pointer with the sentinel discipline.
 */
static void *
fastpath_thread(void *arg)
{
	(void)arg;
	/* Each new pthread needs its own registration: umem_rseq_registered
	 * and umem_rseq_cpu_idp are __thread, so the main thread's earlier
	 * umem_rseq_register_thread() call does not cover these threads. */
	if (umem_rseq_register_thread() != 0) {
		fprintf(stderr, "fastpath_thread: register failed\n");
		return (NULL);
	}
	while (!atomic_load(&g_stop)) {
		/*
		 * Re-read this thread's OWN current cpu_id before every call,
		 * exactly like _umem_cache_alloc()/_umem_cache_free() in
		 * umem.c do (`int cpu = (int)*umem_rseq_cpu_idp;` immediately
		 * before each fast path call, never a value captured earlier
		 * or borrowed from another thread).
		 */
		int cpu = umem_rseq_get_cpu();
		if (cpu < 0)
			continue;
		void *buf = umem_rseq_alloc_fastpath(&g_shared_rc, cpu);
		if (buf == NULL)
			continue;
		atomic_fetch_add(&g_alloc_ops, 1);
		int idx = sentinel_index(buf);
		if (idx < 0) {
			atomic_fetch_add(&g_bad_pointer, 1);
			continue;
		}
		int prev = atomic_fetch_add(&g_checked_out[idx], 1);
		if (prev != 0) {
			atomic_fetch_add(&g_double_issue, 1);
		}
		/* Simulate "using" the buffer, then return it via the free
		 * fast path against the SAME shared slot -- mirrors real
		 * alloc/free churn. Re-read cpu_id again: this thread may
		 * have migrated between the alloc and free calls. */
		cpu = umem_rseq_get_cpu();
		if (cpu >= 0)
			(void)umem_rseq_free_fastpath(&g_shared_rc, buf, cpu);
		atomic_fetch_sub(&g_checked_out[idx], 1);
	}
	return (NULL);
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

	int nfastpath = (argc > 1) ? atoi(argv[1]) : 4;
	int duration_s = (argc > 2) ? atoi(argv[2]) : 5;

	memset(&g_shared_rc, 0, sizeof(g_shared_rc));
	g_shared_rc.magsize = MAGSIZE;

	pthread_t reloader;
	pthread_create(&reloader, NULL, reload_thread, NULL);

	pthread_t *fastpaths = calloc(nfastpath, sizeof(pthread_t));
	for (int i = 0; i < nfastpath; i++)
		pthread_create(&fastpaths[i], NULL, fastpath_thread, NULL);

	sleep(duration_s);
	atomic_store(&g_stop, 1);

	pthread_join(reloader, NULL);
	for (int i = 0; i < nfastpath; i++)
		pthread_join(fastpaths[i], NULL);

	long reload_ops = atomic_load(&g_reload_ops);
	long alloc_ops = atomic_load(&g_alloc_ops);
	long double_issue = atomic_load(&g_double_issue);
	long bad_pointer = atomic_load(&g_bad_pointer);

	printf("reload_ops=%ld alloc_ops=%ld double_issue=%ld "
	    "bad_pointer=%ld\n", reload_ops, alloc_ops, double_issue,
	    bad_pointer);

	if (double_issue > 0 || bad_pointer > 0) {
		printf("RESULT: RACE CONFIRMED -- a plain-C reload racing "
		    "the lock-free fast path against the same slot causes "
		    "double-issue and/or invalid-pointer corruption, exactly "
		    "as the migration-safety analysis predicts. No amount "
		    "of cpu_id rechecking in the reload closes this: the "
		    "corruption is caused by unsynchronized concurrent "
		    "writers to shared per-CPU-cache fields, which a TOCTOU "
		    "check narrows but cannot eliminate.\n");
		return (1);
	}
	printf("RESULT: no corruption observed in this run (does not prove "
	    "safety -- see the written analysis for why this design is "
	    "unsafe regardless)\n");
	return (0);
}

/*
 * Safety counterpart to repro_naive_reload_race.c (design validation
 * checklist #2, docs/results/2026-09-09-rseq-reload-asm-design.md): the
 * naive repro proves a plain-C reload double-issues ~42-47% under
 * contention; THIS one proves the armed asm reload commit
 * (umem_rseq_reload_alloc_commit / _free_commit) does NOT, because its
 * publish of cache_rseq[cpu] is itself a registered rseq critical section,
 * atomic w.r.t. the thread's CPU occupancy -- the same kernel guarantee the
 * fast path has.
 *
 * Structure: N fastpath threads run the REAL alloc/free fast path against a
 * shared slot (sentinel double-issue detector, same discipline as
 * stress_concurrency_oracle.c), while a reload thread hammers the REAL
 * commit functions against that same slot.  Every magazine handed to a
 * commit is filled by PUSHING sentinels through umem_rseq_free_fastpath,
 * which mangles each round exactly as the depot path does, so the fast
 * path's demangle-on-pop yields the true sentinel (no private knowledge of
 * the hidden umem_link_cookie needed).
 *
 * PASS: double_issue == 0 && bad_pointer == 0 over a multi-second run on
 * both x86_64 and aarch64.  A nonzero count is a real corruption and blocks
 * arming.
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
#define POOL_SIZE 128

static test_magazine_t g_pool[POOL_SIZE];
static atomic_int g_pool_next = 0;
static uintptr_t g_sentinel_base = 0x100000;

#define NSENTINELS (POOL_SIZE * MAGSIZE)
static atomic_int g_checked_out[NSENTINELS];
static atomic_long g_double_issue = 0;
static atomic_long g_bad_pointer = 0;
static atomic_long g_alloc_ops = 0;
static atomic_long g_commit_ops = 0;
static atomic_long g_commit_abort = 0;

static umem_rseq_cache_t g_shared_rc;
static atomic_int g_stop = 0;

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

/*
 * Build a magazine holding MAGSIZE mangled sentinels by pushing them onto a
 * scratch slot through the real free fast path (which mangles).  Returns a
 * magazine whose mag_round[0..MAGSIZE-1] are correctly mangled so the alloc
 * fast path demangles them back to the sentinels.
 */
static test_magazine_t *
pool_alloc_filled(int cpu)
{
	int slot = atomic_fetch_add(&g_pool_next, 1) % POOL_SIZE;
	test_magazine_t *mp = &g_pool[slot];
	umem_rseq_cache_t scratch;

	memset(mp, 0, sizeof(*mp));
	memset(&scratch, 0, sizeof(scratch));
	scratch.magsize = MAGSIZE;
	scratch.loaded_mag = mp;
	scratch.rounds = 0;
	for (int i = 0; i < MAGSIZE; i++) {
		int sidx = (slot * MAGSIZE + i) % NSENTINELS;
		void *s = (void *)(g_sentinel_base + (uintptr_t)sidx * 0x10);
		/* Push onto the scratch slot; free_fastpath mangles the round.
		 * Retry until it commits (aborts on this thread's migration). */
		int c = cpu;
		while (c >= 0 && umem_rseq_free_fastpath(&scratch, s, c) != 0)
			c = umem_rseq_get_cpu();
	}
	return (mp);
}

static void *
reload_thread(void *arg)
{
	(void)arg;
	if (umem_rseq_register_thread() != 0)
		return (NULL);
	while (!atomic_load(&g_stop)) {
		int cpu = umem_rseq_get_cpu();
		if (cpu < 0)
			continue;
		test_magazine_t *fmp = pool_alloc_filled(cpu);
		void *old_mag = NULL;
		int old_rounds = 0;
		cpu = umem_rseq_get_cpu();
		if (cpu < 0)
			continue;
		if (umem_rseq_reload_alloc_commit(&g_shared_rc, cpu, fmp,
		    MAGSIZE, &old_mag, &old_rounds))
			atomic_fetch_add(&g_commit_ops, 1);
		else
			atomic_fetch_add(&g_commit_abort, 1);
	}
	return (NULL);
}

static void *
fastpath_thread(void *arg)
{
	(void)arg;
	if (umem_rseq_register_thread() != 0)
		return (NULL);
	while (!atomic_load(&g_stop)) {
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
		if (prev != 0)
			atomic_fetch_add(&g_double_issue, 1);
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
	if (umem_rseq_get_cpu() < 0) {
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

	long commit_ops = atomic_load(&g_commit_ops);
	long commit_abort = atomic_load(&g_commit_abort);
	long alloc_ops = atomic_load(&g_alloc_ops);
	long double_issue = atomic_load(&g_double_issue);
	long bad_pointer = atomic_load(&g_bad_pointer);

	printf("commit_ops=%ld commit_abort=%ld alloc_ops=%ld "
	    "double_issue=%ld bad_pointer=%ld\n", commit_ops, commit_abort,
	    alloc_ops, double_issue, bad_pointer);

	if (double_issue > 0 || bad_pointer > 0) {
		printf("RESULT: FAIL -- the armed reload commit races the "
		    "fast path (double_issue/bad_pointer > 0). Arming is "
		    "unsound; leave the reload inert.\n");
		return (1);
	}
	printf("RESULT: PASS -- no double-issue or bad pointer with the "
	    "armed rseq commit hammering the same slot as the fast path.\n");
	return (0);
}

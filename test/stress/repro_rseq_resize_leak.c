/*
 * P8.5b H1: a magazine resize must not strand buffers in the rseq layer.
 *
 * DEFECT (pre-fix): cache_rseq[cpu].magsize was set once, at cache creation,
 * and never refreshed.  After umem_cache_magazine_resize() the depot hands
 * the rseq reload magazines of the NEW, larger capacity, but the alloc reload
 * committed only the stale magsize rounds.  The rounds above that were live
 * free buffers the slot did not count.  On swap-out the magazine went to the
 * empty list with them still inside, and every consumer of an empty magazine
 * overwrites its slots.  They were never freed or issued again: a leak.
 *
 * SETUP: one thread pinned to one cpu (one rseq slot), one >64 KiB class
 * (magsize 1, can grow), alloc/free churn in batches, then everything freed.
 * umem_walk_allocated() then reports as ALLOCATED any buffer of this cache
 * that sits in no magazine the walker knows about.  At quiescence that must
 * be 0.  It is measured before a forced resize (control) and after it.
 *
 * Usage: repro_rseq_resize_leak [batch=16]
 * Exit 0 PASS, 1 FAIL or INCONCLUSIVE (no resize happened), 77 SKIP.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "umem.h"
#include "umem_inspect.h"
#include "../../umem_impl.h"
#include "umem_rseq.h"

#define	SZ	100000
#define	MAXB	256

static int batch = 16;

static umem_cache_t *cp;
static size_t g_n;

static int
cb(const umem_buffer_info_t *info, void *arg)
{
	(void)arg;
	if (info != NULL && info->cache == (void *)cp)
		g_n++;
	return (0);
}

static size_t
outstanding(void)
{
	g_n = 0;
	(void) umem_walk_allocated(cb, NULL);
	return (g_n);
}

static void
churn(int rounds)
{
	void *b[MAXB];

	for (int r = 0; r < rounds; r++) {
		for (int k = 0; k < batch; k++)
			b[k] = umem_alloc(SZ, UMEM_DEFAULT);
		for (int k = 0; k < batch; k++)
			if (b[k] != NULL)
				umem_free(b[k], SZ);
	}
}

int
main(int argc, char **argv)
{
	extern uint32_t umem_reap_interval;
	extern uint_t umem_depot_contention;
	cpu_set_t set;
	size_t before, after;
	int from, to = 0;

	if (argc > 1 && atoi(argv[1]) > 0 && atoi(argv[1]) <= MAXB)
		batch = atoi(argv[1]);

	CPU_ZERO(&set);
	CPU_SET(0, &set);
	(void) sched_setaffinity(0, sizeof (set), &set);
	umem_depot_contention = UINT_MAX;	/* no resize until we ask */
	umem_reap_interval = 1;

	umem_free(umem_alloc(SZ, UMEM_DEFAULT), SZ);	/* init */
	cp = umem_alloc_table[(SZ - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		printf("SKIP: no cache for size %d\n", SZ);
		return (77);
	}
	/* norseq runs too: it is the control arm (rseq layer absent). */
	printf("rseq_enabled=%d asm_safe=%d batch=%d\n", umem_rseq_enabled,
	    umem_rseq_asm_safe, batch);

	churn(2000);
	before = outstanding();
	from = cp->cache_magtype->mt_magsize;

	/*
	 * Force the resize through umem_cache_update()'s own decision: open
	 * the gate and supply the contention input (the counter a failed depot
	 * trylock increments), as test_ptc_resize_no_loss does.
	 */
	umem_depot_contention = 0;
	for (int sec = 0; sec < 20 && to == 0; sec++) {
		(void) atomic_fetch_add((_Atomic(uint64_t) *)
		    &cp->cache_depot_contention, 4096);
		umem_reap();
		(void) sleep(1);
		if (cp->cache_magtype->mt_magsize != from)
			to = cp->cache_magtype->mt_magsize;
	}
	umem_depot_contention = UINT_MAX;

	churn(2000);
	after = outstanding();

	printf("cache=%s magsize %d -> %d  stranded before=%zu after=%zu\n",
	    cp->cache_name, from, to, before, after);
	if (to == 0) {
		printf("RESULT: INCONCLUSIVE (no resize)\n");
		return (1);
	}
	if (before != 0 || after != 0) {
		printf("RESULT: FAIL (buffers stranded in rseq-returned "
		    "magazines)\n");
		return (1);
	}
	printf("RESULT: PASS\n");
	return (0);
}

/*
 * P8.5b arming probe: prove the armed rseq reload is actually EXERCISED,
 * not silently inert.  Threads bounce across a small CPU set (forcing
 * scheduler migrations, which are what drive the reload's phase-2 commit
 * and its aborts), churn real umem allocations, then dump the per-cache
 * contention counters.  rseq_alloc/rseq_free going from 0 to nonzero is the
 * signal that the fast path served hits from a reloaded magazine; a nonzero
 * rseq_restart shows the migration-safe commit aborted and retried as
 * designed (validation checklist #5).
 *
 * Not a correctness gate (the oracle is) -- a liveness/coverage probe so a
 * "0 corruption" oracle cannot be dismissed as "the path never ran".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "umem.h"

static atomic_int g_stop = 0;
static int g_ncpu_set = 2;

static void *
worker(void *arg)
{
	long id = (long)arg;
	/* Pin to a rotating pair of CPUs so the scheduler bounces us. */
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET((int)(id % g_ncpu_set), &set);
	CPU_SET((int)((id + 1) % g_ncpu_set), &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

	void *bufs[16];
	size_t szs[16];
	memset(bufs, 0, sizeof(bufs));
	memset(szs, 0, sizeof(szs));
	unsigned r = (unsigned)(id * 2654435761u) | 1u;
	while (!atomic_load(&g_stop)) {
		for (int i = 0; i < 16; i++) {
			r = r * 1103515245u + 12345u;
			size_t sz = 8 + (r % 512);
			if (bufs[i])
				umem_free(bufs[i], szs[i]);
			bufs[i] = umem_alloc(sz, UMEM_DEFAULT);
			szs[i] = sz;
			/* migrate hint: yield so the scheduler can move us */
			if ((r & 0x3f) == 0)
				sched_yield();
		}
	}
	for (int i = 0; i < 16; i++)
		if (bufs[i])
			umem_free(bufs[i], szs[i]);
	(void)id;
	return (NULL);
}

int
main(int argc, char **argv)
{
	int nthreads = (argc > 1) ? atoi(argv[1]) : 8;
	int dur = (argc > 2) ? atoi(argv[2]) : 8;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	g_ncpu_set = (ncpu >= 2) ? 2 : 1;

	pthread_t *th = calloc(nthreads, sizeof(*th));
	for (long i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, worker, (void *)i);
	sleep(dur);
	atomic_store(&g_stop, 1);
	for (int i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);

	printf("=== contention counters after migration-churn ===\n");
	umem_dump_contention(stdout);
	{
		extern unsigned long umem_dbg_rseq_enter, umem_dbg_rseq_no_full,
		    umem_dbg_rseq_armed;
		printf("DBG entered=%lu armed=%lu no_full_mag=%lu\n",
		    umem_dbg_rseq_enter, umem_dbg_rseq_armed,
		    umem_dbg_rseq_no_full);
	}
	return (0);
}

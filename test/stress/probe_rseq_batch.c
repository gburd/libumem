/*
 * P8.5b-arch batch arming probe.  Each thread allocates a batch LARGER than
 * a PTC magazine, holds it, then frees it -- this drains the PTC per-thread
 * magazine (both loaded+previous) on the alloc side and fills it on the free
 * side, so both PTC-miss paths (_umem_alloc refill / _umem_free flush) are
 * reached and the P8.5b routing (umem_rseq_ptc_alloc / umem_rseq_ptc_free)
 * fires.  No cross-thread buffer sharing -> no double-free hazard; each
 * thread owns its own buffers start to finish.  Prints umem_dump_contention
 * (rseq_alloc/free go nonzero when the layer actually serves) and, under
 * -DUMEM_RSEQ_ARM_DEBUG with UMEM_DBG_RSEQ_PROBE=1, the arming counters.
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
#define BATCH 1024	/* > any magazine (max magsize 255) */
#define SZ    64

static void *
worker(void *arg)
{
	(void)arg;
	void **buf = malloc(BATCH * sizeof(*buf));
	unsigned long iters = 0;
	while (!atomic_load(&g_stop)) {
		for (int i = 0; i < BATCH; i++)
			buf[i] = umem_alloc(SZ, UMEM_DEFAULT);
		/* touch to defeat any dead-store elision */
		for (int i = 0; i < BATCH; i++)
			if (buf[i]) *(char *)buf[i] = (char)i;
		for (int i = 0; i < BATCH; i++)
			if (buf[i]) umem_free(buf[i], SZ);
		iters++;
		/* occasional yield to encourage migration across CPUs */
		if ((iters & 0x3f) == 0)
			sched_yield();
	}
	free(buf);
	return NULL;
}

int
main(int argc, char **argv)
{
	int nt = (argc > 1) ? atoi(argv[1]) : 8;
	int dur = (argc > 2) ? atoi(argv[2]) : 8;
	pthread_t *t = calloc(nt, sizeof(*t));
	for (int i = 0; i < nt; i++) pthread_create(&t[i], NULL, worker, NULL);
	sleep(dur);
	atomic_store(&g_stop, 1);
	for (int i = 0; i < nt; i++) pthread_join(t[i], NULL);

	printf("=== contention counters (batch alloc/free) ===\n");
	umem_dump_contention(stdout);
#ifdef UMEM_RSEQ_ARM_DEBUG
	{
		extern unsigned long umem_dbg_rseq_enter, umem_dbg_rseq_no_full,
		    umem_dbg_rseq_armed, umem_dbg_rseq_slow_called,
		    umem_dbg_rseq_commit_abort, umem_dbg_rseq_break_cpu;
		printf("DBG entered=%lu slow_called=%lu armed=%lu abort=%lu no_full=%lu break_cpu=%lu\n",
		    umem_dbg_rseq_enter, umem_dbg_rseq_slow_called,
		    umem_dbg_rseq_armed, umem_dbg_rseq_commit_abort,
		    umem_dbg_rseq_no_full, umem_dbg_rseq_break_cpu);
	}
#endif
	return 0;
}

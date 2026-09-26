/*
 * P8.5b-arch producer/consumer arming probe.  Unlike probe_rseq_armed
 * (each thread recycles its own buffers, so the depot's cache_full list
 * stays empty and the rseq slowpath sees no_full=100%), this drives a
 * CROSS-THREAD flow: producers alloc and hand buffers to consumers via a
 * shared ring, consumers free.  Cross-thread free is exactly what forces
 * full magazines into the depot's cache_full list -- the traffic the rseq
 * reload must intercept.  Prints umem_dump_contention (rseq_alloc/free go
 * nonzero when the layer serves) and, under -DUMEM_RSEQ_ARM_DEBUG with
 * UMEM_DBG_RSEQ_PROBE=1, the arming counters.
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

#define RING 4096
#define SZ   64

static atomic_int g_stop = 0;
static void *ring[RING];
static size_t ringsz[RING];
static atomic_uint head = 0, tail = 0;

static void *
producer(void *arg)
{
	(void)arg;
	unsigned r = 0x1234567u;
	while (!atomic_load(&g_stop)) {
		unsigned h = atomic_load(&head);
		if (h - atomic_load(&tail) >= RING) { sched_yield(); continue; }
		r = r * 1103515245u + 12345u;
		size_t sz = 8 + (r % 512);
		void *b = umem_alloc(sz, UMEM_DEFAULT);
		if (!b) continue;
		ring[h % RING] = b;
		ringsz[h % RING] = sz;
		atomic_store(&head, h + 1);
	}
	return NULL;
}

static void *
consumer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_stop) || atomic_load(&tail) != atomic_load(&head)) {
		unsigned t = atomic_load(&tail);
		if (t == atomic_load(&head)) { sched_yield(); continue; }
		void *b = ring[t % RING];
		size_t sz = ringsz[t % RING];
		if (!atomic_compare_exchange_weak(&tail, &t, t + 1)) continue;
		umem_free(b, sz);
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	int np = (argc > 1) ? atoi(argv[1]) : 8;
	int nc = (argc > 2) ? atoi(argv[2]) : 8;
	int dur = (argc > 3) ? atoi(argv[3]) : 8;
	pthread_t *pt = calloc(np, sizeof(*pt));
	pthread_t *ct = calloc(nc, sizeof(*ct));
	for (int i = 0; i < np; i++) pthread_create(&pt[i], NULL, producer, NULL);
	for (int i = 0; i < nc; i++) pthread_create(&ct[i], NULL, consumer, NULL);
	sleep(dur);
	atomic_store(&g_stop, 1);
	for (int i = 0; i < np; i++) pthread_join(pt[i], NULL);
	for (int i = 0; i < nc; i++) pthread_join(ct[i], NULL);

	printf("=== contention counters (producer/consumer) ===\n");
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

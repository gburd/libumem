/*
 * P8.5b aliasing, end to end: under rseq aborts, the armed reload must never
 * hand one buffer to two owners.
 *
 * DEFECT (pre-fix, docs/results/2026-09-26-p85b-aliasing-mechanism-v2.md):
 * umem_rseq_reload_{alloc,free}_commit were not all-or-nothing.  An abort
 * landing between their stores, or after the committing store but before the
 * success value was set, left a magazine published in cache_rseq[cpu] while
 * the C slowpath believed it was not and gave it back to the depot too.
 *
 * SETUP: plain umem_alloc/umem_free of one >64 KiB size class (magsize 1, so
 * nearly every operation is a reload), NTHR workers pinned to ONE cpu so they
 * share one cache_rseq slot, and a signaller thread off that cpu that
 * pthread_kill()s them continuously, forcing aborts inside the windows.
 * Each worker keeps a ring of held buffers stamped (tid, seq) at both ends and
 * checks the stamp before freeing.  A buffer re-issued to anyone while held
 * is re-stamped and caught here, the same oracle as stress_concurrency_oracle.
 *
 * Exit 0 PASS, 1 FAIL (aliasing, or the layer never fired), 77 SKIP.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "umem.h"
#include "../../umem_impl.h"
#include "umem_rseq.h"

#define	SZ	100000
#define	NTHR	4
#define	RING	4

static atomic_int stop, bad;
static int pin_cpu;
static pthread_t wt[NTHR];

static void on_sig(int s) { (void)s; }

static void
pin(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	(void) pthread_setaffinity_np(pthread_self(), sizeof (s), &s);
}

static void *
worker(void *arg)
{
	uint64_t tid = (uint64_t)(uintptr_t)arg, seq = 0;
	uint64_t *ring[RING] = { 0 }, stamp[RING];

	pin(pin_cpu);
	while (!atomic_load(&stop) && !atomic_load(&bad)) {
		int k = (int)(seq % RING);
		uint64_t *p = ring[k];
		if (p != NULL) {
			if (p[0] != stamp[k] || p[SZ / 8 - 1] != stamp[k]) {
				fprintf(stderr, "ALIAS: tid=%llu buf=%p want "
				    "%#llx found %#llx/%#llx\n",
				    (unsigned long long)tid, (void *)p,
				    (unsigned long long)stamp[k],
				    (unsigned long long)p[0],
				    (unsigned long long)p[SZ / 8 - 1]);
				atomic_store(&bad, 1);
				break;
			}
			umem_free(p, SZ);
		}
		p = umem_alloc(SZ, UMEM_DEFAULT);
		if (p == NULL)
			continue;
		stamp[k] = (tid << 48) | ++seq;
		p[0] = p[SZ / 8 - 1] = stamp[k];
		ring[k] = p;
	}
	for (int k = 0; k < RING; k++)
		if (ring[k] != NULL)
			umem_free(ring[k], SZ);
	return (NULL);
}

int
main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	struct sigaction sa;
	struct timespec t0, t;
	umem_cache_t *cp;
	uint64_t served = 0, restarts = 0, kills = 0;

	umem_free(umem_alloc(SZ, UMEM_DEFAULT), SZ);	/* init + warm */
	cp = umem_alloc_table[(SZ - 1) >> UMEM_ALIGN_SHIFT];
	if (!umem_rseq_enabled || !umem_rseq_asm_safe || ncpu < 2 ||
	    cp == NULL || cp->cache_rseq == NULL) {
		printf("SKIP: rseq asm layer not armed for this cache\n");
		return (77);
	}
	pin_cpu = (int)(ncpu - 1);
	pin(0);
	memset(&sa, 0, sizeof (sa));
	sa.sa_handler = on_sig;
	(void) sigaction(SIGUSR1, &sa, NULL);

	for (uintptr_t i = 0; i < NTHR; i++)
		(void) pthread_create(&wt[i], NULL, worker, (void *)(i + 1));
	(void) clock_gettime(CLOCK_MONOTONIC, &t0);
	do {
		for (int i = 0; i < NTHR; i++, kills++)
			(void) pthread_kill(wt[i], SIGUSR1);
		(void) clock_gettime(CLOCK_MONOTONIC, &t);
	} while (!atomic_load(&bad) && t.tv_sec - t0.tv_sec < secs);
	atomic_store(&stop, 1);
	for (int i = 0; i < NTHR; i++)
		(void) pthread_join(wt[i], NULL);

	for (int i = 0; i < umem_rseq_get_ncpus(); i++) {
		served += cp->cache_rseq[i].alloc_count;
		restarts += cp->cache_rseq[i].restart_count;
	}
	printf("cache=%s magsize=%d rseq_alloc=%llu rseq_restarts=%llu "
	    "signals=%llu\n", cp->cache_name, cp->cache_magtype->mt_magsize,
	    (unsigned long long)served, (unsigned long long)restarts,
	    (unsigned long long)kills);
	if (atomic_load(&bad)) {
		printf("RESULT: FAIL (buffer issued to two owners)\n");
		return (1);
	}
	if (served == 0 || restarts == 0) {
		printf("RESULT: FAIL (inconclusive: rseq layer %s)\n",
		    served == 0 ? "never served" : "never aborted");
		return (1);
	}
	printf("RESULT: PASS\n");
	return (0);
}

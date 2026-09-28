/*
 * O6 exit-storm bench (docs/results/r8exit/).  Built on the box against the
 * post tree's umem.h and run under each arm's libumem.so via
 * LD_LIBRARY_PATH, so every arm runs the SAME binary.
 *
 * Each round: T threads each allocate K 64 B objects and free them all,
 * park on a barrier, then exit when main releases them.  K = 128 + 2 * 255
 * (the 64 B PTC bin plus two 255-round magazines), so each thread exits
 * holding one EXACTLY FULL loaded magazine -- the shape the fix changes
 * (pre-fix: 255 _umem_cache_free calls per thread at exit; post: one push).
 * This is the path's best case by construction, not a typical workload.
 *
 * exit_us = release -> all joined, summed over rounds (the exit drain).
 * total_s = the whole loop.  Usage: exit_storm <threads> <rounds>
 */
#include <umem.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define	SZ	64
#define	K	(128 + 2 * 255)

static pthread_barrier_t filled, go;

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec * 1e-9);
}

static void *
worker(void *arg)
{
	void *p[K];
	int i;

	(void) arg;
	for (i = 0; i < K; i++)
		p[i] = umem_alloc(SZ, UMEM_DEFAULT);
	for (i = 0; i < K; i++)
		if (p[i] != NULL)
			umem_free(p[i], SZ);
	pthread_barrier_wait(&filled);
	pthread_barrier_wait(&go);
	return (NULL);
}

int
main(int argc, char **argv)
{
	int t = argc > 1 ? atoi(argv[1]) : 8, rounds = argc > 2 ? atoi(argv[2]) : 200;
	pthread_t *th = calloc(t, sizeof (*th));
	double exit_s = 0, t0 = now(), a;
	int r, i;

	umem_free(umem_alloc(SZ, UMEM_DEFAULT), SZ);
	for (r = 0; r < rounds; r++) {
		pthread_barrier_init(&filled, NULL, t + 1);
		pthread_barrier_init(&go, NULL, t + 1);
		for (i = 0; i < t; i++)
			if (pthread_create(&th[i], NULL, worker, NULL) != 0)
				return (2);
		pthread_barrier_wait(&filled);
		a = now();
		pthread_barrier_wait(&go);
		for (i = 0; i < t; i++)
			pthread_join(th[i], NULL);
		exit_s += now() - a;
		pthread_barrier_destroy(&filled);
		pthread_barrier_destroy(&go);
	}
	printf("threads=%d rounds=%d exit_us_per_round=%.2f total_s=%.4f\n",
	    t, rounds, exit_s * 1e6 / rounds, now() - t0);
	return (0);
}

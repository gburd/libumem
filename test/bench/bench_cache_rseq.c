/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * Benchmark umem_cache_alloc/free latency with RSEQ enabled vs disabled.
 * Also compares named cache path against umem_alloc (PTC path).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#include "../../umem.h"
#include "../../umem_rseq.h"
#include "../tdigest.h"

#define SAMPLES 10000
#define WARMUP_OPS 1000

typedef struct {
	int thread_id;
	td_histogram_t *latencies;
	umem_cache_t *cache;
	int use_cache;	/* 1 = umem_cache_alloc, 0 = umem_alloc */
	uint64_t ops_completed;
} thread_ctx_t;

static inline uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *
cache_worker(void *arg)
{
	thread_ctx_t *ctx = (thread_ctx_t *)arg;
	int i;

	ctx->ops_completed = 0;

	for (i = 0; i < SAMPLES; i++) {
		void *p;
		uint64_t t0, t1;

		if (ctx->use_cache) {
			t0 = now_ns();
			p = umem_cache_alloc(ctx->cache, UMEM_DEFAULT);
			t1 = now_ns();
			if (p != NULL) {
				td_add(ctx->latencies, (double)(t1 - t0), 1);
				ctx->ops_completed++;
				umem_cache_free(ctx->cache, p);
			}
		} else {
			t0 = now_ns();
			p = umem_alloc(64, UMEM_DEFAULT);
			t1 = now_ns();
			if (p != NULL) {
				td_add(ctx->latencies, (double)(t1 - t0), 1);
				ctx->ops_completed++;
				umem_free(p, 64);
			}
		}
	}

	return NULL;
}

static int
run_test(const char *label, int nthreads, umem_cache_t *cache,
    int use_cache)
{
	pthread_t *threads;
	thread_ctx_t *ctxs;
	td_histogram_t *merged;
	double p50, p99, p999;
	uint64_t total_ops = 0;
	int i;

	threads = malloc(nthreads * sizeof(pthread_t));
	ctxs = malloc(nthreads * sizeof(thread_ctx_t));
	if (threads == NULL || ctxs == NULL) {
		free(threads);
		free(ctxs);
		return -1;
	}

	for (i = 0; i < nthreads; i++) {
		ctxs[i].thread_id = i;
		ctxs[i].cache = cache;
		ctxs[i].use_cache = use_cache;
		ctxs[i].ops_completed = 0;
		ctxs[i].latencies = td_new(200.0);
		if (ctxs[i].latencies == NULL) {
			while (--i >= 0)
				td_free(ctxs[i].latencies);
			free(threads);
			free(ctxs);
			return -1;
		}
	}

	for (i = 0; i < nthreads; i++) {
		if (pthread_create(&threads[i], NULL, cache_worker,
		    &ctxs[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d\n", i);
			for (int j = 0; j < nthreads; j++)
				td_free(ctxs[j].latencies);
			free(threads);
			free(ctxs);
			return -1;
		}
	}

	for (i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	merged = td_new(200.0);
	if (merged == NULL) {
		for (i = 0; i < nthreads; i++)
			td_free(ctxs[i].latencies);
		free(threads);
		free(ctxs);
		return -1;
	}

	for (i = 0; i < nthreads; i++) {
		total_ops += ctxs[i].ops_completed;
		td_merge(merged, ctxs[i].latencies);
		td_free(ctxs[i].latencies);
	}
	td_compress(merged);

	p50 = td_quantile(merged, 0.50);
	p99 = td_quantile(merged, 0.99);
	p999 = td_quantile(merged, 0.999);

	printf("  %-28s %2dT: p50=%6.0f ns  p99=%7.0f ns  "
	    "p999=%8.0f ns  (%llu ops)\n",
	    label, nthreads, p50, p99, p999,
	    (unsigned long long)total_ops);

	td_free(merged);
	free(threads);
	free(ctxs);
	return 0;
}

static void
warmup_cache(umem_cache_t *cache)
{
	void *ptrs[WARMUP_OPS];
	int i;

	for (i = 0; i < WARMUP_OPS; i++) {
		ptrs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		if (ptrs[i] == NULL)
			break;
	}
	for (int j = 0; j < i; j++)
		umem_cache_free(cache, ptrs[j]);
}

static void
warmup_alloc(void)
{
	void *ptrs[WARMUP_OPS];
	int i;

	for (i = 0; i < WARMUP_OPS; i++) {
		ptrs[i] = umem_alloc(64, UMEM_DEFAULT);
		if (ptrs[i] == NULL)
			break;
	}
	for (int j = 0; j < i; j++)
		umem_free(ptrs[j], 64);
}

int
main(void)
{
	umem_cache_t *cache;
	int nt;
	int rseq_active = 0;

	cache = umem_cache_create("bench64", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	if (cache == NULL) {
		fprintf(stderr, "Failed to create cache\n");
		return 1;
	}

	warmup_cache(cache);
	warmup_alloc();

	printf("Named Cache RSEQ Benchmark\n");
	printf("==========================\n");
	printf("Samples per thread: %d\n", SAMPLES);

#ifdef UMEM_RSEQ_AVAILABLE
	if (umem_rseq_enabled && umem_rseq_asm_safe) {
		rseq_active = 1;
		printf("RSEQ: ENABLED (asm fast path active)\n");
	} else if (umem_rseq_enabled) {
		rseq_active = 1;
		printf("RSEQ: ENABLED (C slow path)\n");
	} else {
		printf("RSEQ: DISABLED (not available)\n");
	}
#else
	printf("RSEQ: NOT COMPILED IN\n");
#endif

	printf("\n--- umem_cache_alloc (named cache) ---\n");
	for (nt = 1; nt <= 8; nt *= 2)
		run_test("cache_alloc", nt, cache, 1);

	printf("\n--- umem_alloc (PTC path, 64 bytes) ---\n");
	for (nt = 1; nt <= 8; nt *= 2)
		run_test("umem_alloc(64)", nt, NULL, 0);

	printf("\n--- Comparison ---\n");
	printf("Named cache bypasses size-class lookup, PTC uses "
	    "per-thread cache.\n");
	if (rseq_active) {
		printf("RSEQ provides per-CPU cache for named caches, "
		    "reducing contention.\n");
	}

#ifdef UMEM_RSEQ_AVAILABLE
	if (rseq_active) {
		printf("\nRSEQ stats:\n");
		int ncpus = umem_rseq_get_ncpus();
		printf("  CPUs tracked: %d\n", ncpus);
		umem_rseq_dump();
	}
#endif

	umem_cache_destroy(cache);

	printf("\nDone.\n");
	return 0;
}

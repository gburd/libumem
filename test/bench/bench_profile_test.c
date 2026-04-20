/*
 * Phased profile benchmark: exercises the umem_profile record/replay
 * feature over 3 distinct phases in ~30 seconds.
 *
 * Phase 0 (Startup, 0-5s): 4 threads burst-allocate 2500 objects each
 *   from 5 size classes (50,000 total). High alloc rate, near-zero frees.
 *
 * Phase 1 (Steady state, 5-25s): same 4 threads do alloc-then-free
 *   cycles at ~1000 ops/sec per thread per cache. Working set constant.
 *
 * Phase 2 (Shutdown, 25-30s): all threads free remaining objects.
 *   High free rate, near-zero allocs. Threads join.
 *
 * Usage:
 *   UMEM_PROFILE=record:/tmp/prof.ump ./bench_profile_test
 *   UMEM_PROFILE=use:/tmp/prof.ump   ./bench_profile_test
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "../../umem.h"
#include "../../umem_profile.h"

#define NUM_THREADS      4
#define NUM_SIZE_CLASSES  5
#define BUFS_PER_CLASS   2500
#define TOTAL_PER_THREAD (NUM_SIZE_CLASSES * BUFS_PER_CLASS)

#define PHASE0_SECS  5
#define PHASE1_SECS  20
#define PHASE2_SECS  5
#define TOTAL_SECS   (PHASE0_SECS + PHASE1_SECS + PHASE2_SECS)

/* Steady-state target: ~1000 alloc+free pairs / sec / thread / cache */
#define STEADY_OPS_PER_SEC 1000

static const size_t size_classes[NUM_SIZE_CLASSES] = {
	32, 64, 128, 256, 512
};

static const char *cache_names[NUM_SIZE_CLASSES] = {
	"prof_32", "prof_64", "prof_128", "prof_256", "prof_512"
};

static umem_cache_t *caches[NUM_SIZE_CLASSES];

/* Shared phase indicator: 0, 1, 2, or 3 (done) */
static atomic_int current_phase;

/* Per-thread storage for the burst-allocated buffers */
typedef struct thread_ctx {
	int id;
	void *bufs[NUM_SIZE_CLASSES][BUFS_PER_CLASS];
	int held[NUM_SIZE_CLASSES];
} thread_ctx_t;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL +
	    (uint64_t)ts.tv_nsec;
}

static void
sleep_us(unsigned us)
{
	struct timespec ts;
	ts.tv_sec = us / 1000000;
	ts.tv_nsec = (us % 1000000) * 1000L;
	nanosleep(&ts, NULL);
}

/*
 * Phase 0: burst-allocate BUFS_PER_CLASS from each cache.
 */
static void
phase0_startup(thread_ctx_t *ctx)
{
	for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
		for (int i = 0; i < BUFS_PER_CLASS; i++) {
			ctx->bufs[c][i] =
			    umem_cache_alloc(caches[c], UMEM_DEFAULT);
			if (ctx->bufs[c][i] == NULL) {
				fprintf(stderr,
				    "T%d: alloc failed class=%zu i=%d\n",
				    ctx->id, size_classes[c], i);
				ctx->held[c] = i;
				return;
			}
		}
		ctx->held[c] = BUFS_PER_CLASS;
	}
}

/*
 * Phase 1: alloc-then-free at a steady rate. We hold the working set
 * constant by freeing the oldest buffer for each new allocation.
 */
static void
phase1_steady(thread_ctx_t *ctx)
{
	unsigned interval_us = 1000000 / STEADY_OPS_PER_SEC;
	int cursor[NUM_SIZE_CLASSES];

	for (int c = 0; c < NUM_SIZE_CLASSES; c++)
		cursor[c] = 0;

	while (atomic_load(&current_phase) == 1) {
		for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
			int idx = cursor[c];
			if (idx >= ctx->held[c])
				idx = 0;

			/* Free old, alloc new into same slot */
			umem_cache_free(caches[c], ctx->bufs[c][idx]);
			ctx->bufs[c][idx] =
			    umem_cache_alloc(caches[c], UMEM_DEFAULT);
			if (ctx->bufs[c][idx] == NULL) {
				fprintf(stderr,
				    "T%d: steady alloc failed c=%d\n",
				    ctx->id, c);
				ctx->held[c] = idx;
			}
			cursor[c] = idx + 1;
		}
		sleep_us(interval_us);
	}
}

/*
 * Phase 2: free all remaining objects.
 */
static void
phase2_shutdown(thread_ctx_t *ctx)
{
	for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
		for (int i = 0; i < ctx->held[c]; i++) {
			if (ctx->bufs[c][i] != NULL)
				umem_cache_free(caches[c], ctx->bufs[c][i]);
		}
		ctx->held[c] = 0;
	}
}

static void *
worker(void *arg)
{
	thread_ctx_t *ctx = (thread_ctx_t *)arg;

	/* Wait for phase 0 */
	while (atomic_load(&current_phase) < 0)
		sleep_us(100);

	phase0_startup(ctx);

	/* Wait for phase 1 */
	while (atomic_load(&current_phase) == 0)
		sleep_us(1000);

	phase1_steady(ctx);

	/* Phase 2 triggered by main thread setting phase=2 */
	phase2_shutdown(ctx);

	return NULL;
}

/*
 * Periodically call umem_profile_sample() to drive phase detection.
 * Runs from the main thread alongside the phase timer.
 *
 * Sample every 200ms to ensure we capture rate differences between
 * consecutive samples (phase detection requires a non-zero prev_rate).
 */
static void
run_sampler(int total_seconds)
{
	uint64_t start = now_ns();
	uint64_t deadline = start + (uint64_t)total_seconds * 1000000000ULL;
	uint64_t phase1_ns = (uint64_t)PHASE0_SECS * 1000000000ULL;
	uint64_t phase2_ns = phase1_ns +
	    (uint64_t)PHASE1_SECS * 1000000000ULL;
	int prev_phase = -1;

	while (now_ns() < deadline) {
		uint64_t elapsed = now_ns() - start;
		int phase;

		if (elapsed < phase1_ns) {
			phase = 0;
		} else if (elapsed < phase2_ns) {
			phase = 1;
		} else {
			phase = 2;
		}

		if (phase != prev_phase) {
			if (phase == 1)
				printf("=== Phase 1: Steady state "
				    "(%d seconds) ===\n", PHASE1_SECS);
			else if (phase == 2)
				printf("=== Phase 2: Shutdown "
				    "(%d seconds) ===\n", PHASE2_SECS);
			atomic_store(&current_phase, phase);
			prev_phase = phase;
		}

		umem_profile_sample();
		sleep_us(200000); /* sample every 200ms */
	}

	/* Ensure phase 2 is set so workers can finish */
	atomic_store(&current_phase, 2);
}

static int
is_record_mode(void)
{
	const char *env = getenv("UMEM_PROFILE");
	if (env == NULL)
		return 0;
	return (strncmp(env, "record:", 7) == 0);
}

static const char *
profile_path(void)
{
	const char *env = getenv("UMEM_PROFILE");
	if (env == NULL)
		return NULL;
	const char *colon = strchr(env, ':');
	if (colon == NULL)
		return NULL;
	return colon + 1;
}

int
main(void)
{
	pthread_t threads[NUM_THREADS];
	thread_ctx_t ctxs[NUM_THREADS];
	int record = is_record_mode();

	printf("bench_profile_test: %d threads, %d size classes, "
	    "%d seconds\n", NUM_THREADS, NUM_SIZE_CLASSES, TOTAL_SECS);
	printf("mode: %s\n",
	    record ? "record" : (getenv("UMEM_PROFILE") ? "use" : "none"));

	/* Create caches */
	for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
		caches[c] = umem_cache_create(
		    (char *)cache_names[c], size_classes[c], 0,
		    NULL, NULL, NULL, NULL, NULL, 0);
		if (caches[c] == NULL) {
			fprintf(stderr, "Failed to create cache %s\n",
			    cache_names[c]);
			return 1;
		}
	}

	/* Initialize thread contexts */
	atomic_store(&current_phase, -1);
	for (int t = 0; t < NUM_THREADS; t++) {
		memset(&ctxs[t], 0, sizeof(ctxs[t]));
		ctxs[t].id = t;
	}

	/* Launch workers */
	for (int t = 0; t < NUM_THREADS; t++) {
		if (pthread_create(&threads[t], NULL, worker, &ctxs[t]) != 0) {
			fprintf(stderr, "pthread_create failed for T%d\n", t);
			return 1;
		}
	}

	printf("=== Phase 0: Startup (%d seconds) ===\n", PHASE0_SECS);
	atomic_store(&current_phase, 0);

	/* Run the phase timer + sampler */
	run_sampler(TOTAL_SECS);

	printf("=== Waiting for workers ===\n");
	for (int t = 0; t < NUM_THREADS; t++)
		pthread_join(threads[t], NULL);

	printf("=== Workload complete ===\n");

	/* If recording, finalize and then reload to dump */
	if (record) {
		const char *path = profile_path();

		umem_profile_fini();

		if (path != NULL && umem_profile_load(path) == 0) {
			printf("\n");
			umem_profile_dump_text(stdout);
		} else {
			fprintf(stderr,
			    "Failed to reload profile for dump\n");
		}
	}

	/* Destroy caches */
	for (int c = 0; c < NUM_SIZE_CLASSES; c++)
		umem_cache_destroy(caches[c]);

	printf("bench_profile_test: done\n");
	return 0;
}

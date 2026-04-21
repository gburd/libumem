/*
 * Phased profile benchmark: exercises the umem_profile record/replay
 * feature over 4 distinct phases in ~30 seconds.
 *
 * Phase 0 (0-3s): Startup/initialization
 *   Create caches, allocate initial working set (100K objects across
 *   5 size classes). High alloc rate, low free rate.
 *
 * Phase 1 (3-20s): Steady state (request processing)
 *   Each "request": allocate 10-50 objects (mixed sizes), process, free.
 *   Multiple threads handling requests concurrently.
 *   Working set stable around 100K objects. Alloc rate ~ free rate.
 *
 * Phase 2 (20-25s): Load spike
 *   Double the request rate for 5 seconds. Working set spikes to 200K.
 *   Tests whether profile predicts and pre-allocates.
 *
 * Phase 3 (25-30s): Return to steady state
 *   Back to normal rate. Working set drops back.
 *
 * Usage:
 *   UMEM_PROFILE=record:/tmp/prof.ump ./bench_profile_test
 *   UMEM_PROFILE=use:/tmp/prof.ump   ./bench_profile_test
 *
 * Prints comparison metrics at end.
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

#define UMEM_ENABLE_EXPERIMENTAL
#include "../../umem.h"
#include "../../umem_profile.h"

#define NUM_THREADS       4
#define NUM_SIZE_CLASSES  5
#define WORKING_SET_SIZE  25000  /* per thread, across all size classes */
#define SPIKE_EXTRA       25000  /* additional per thread during spike */

#define PHASE0_SECS  3
#define PHASE1_SECS  17
#define PHASE2_SECS  5
#define PHASE3_SECS  5
#define TOTAL_SECS   (PHASE0_SECS + PHASE1_SECS + PHASE2_SECS + PHASE3_SECS)

/* Request sizes: 10-50 objects per request */
#define MIN_REQUEST_OBJS 10
#define MAX_REQUEST_OBJS 50

/* Steady-state: ~500 requests/sec per thread */
#define STEADY_REQ_INTERVAL_US 2000
/* Spike: ~1000 requests/sec per thread */
#define SPIKE_REQ_INTERVAL_US  1000

static const size_t size_classes[NUM_SIZE_CLASSES] = {
	32, 64, 128, 256, 512
};

static const char *cache_names[NUM_SIZE_CLASSES] = {
	"prof_32", "prof_64", "prof_128", "prof_256", "prof_512"
};

static umem_cache_t *caches[NUM_SIZE_CLASSES];

/* Shared phase indicator: 0-3, or 4 (done) */
static atomic_int current_phase;

/* Latency tracking (nanoseconds) */
static atomic_uint_least64_t phase0_alloc_ns;
static atomic_uint_least64_t phase0_alloc_count;
static atomic_uint_least64_t phase1_alloc_ns;
static atomic_uint_least64_t phase1_alloc_count;
static atomic_uint_least64_t phase2_alloc_ns;
static atomic_uint_least64_t phase2_alloc_count;
static atomic_uint_least64_t phase3_alloc_ns;
static atomic_uint_least64_t phase3_alloc_count;

typedef struct thread_ctx {
	int id;
	/* Ring buffer of allocated objects for steady-state cycling */
	void *ring[WORKING_SET_SIZE + SPIKE_EXTRA];
	int ring_classes[WORKING_SET_SIZE + SPIKE_EXTRA];
	int ring_head;
	int ring_count;
	int ring_cap;
	unsigned int rng_state;
} thread_ctx_t;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
sleep_us(unsigned us)
{
	struct timespec ts;
	ts.tv_sec = us / 1000000;
	ts.tv_nsec = (us % 1000000) * 1000L;
	nanosleep(&ts, NULL);
}

static unsigned int
fast_rand(unsigned int *state)
{
	*state = *state * 1103515245 + 12345;
	return (*state >> 16) & 0x7fff;
}

static void
track_alloc_latency(int phase, uint64_t ns)
{
	switch (phase) {
	case 0:
		atomic_fetch_add(&phase0_alloc_ns, ns);
		atomic_fetch_add(&phase0_alloc_count, 1);
		break;
	case 1:
		atomic_fetch_add(&phase1_alloc_ns, ns);
		atomic_fetch_add(&phase1_alloc_count, 1);
		break;
	case 2:
		atomic_fetch_add(&phase2_alloc_ns, ns);
		atomic_fetch_add(&phase2_alloc_count, 1);
		break;
	case 3:
		atomic_fetch_add(&phase3_alloc_ns, ns);
		atomic_fetch_add(&phase3_alloc_count, 1);
		break;
	}
}

/*
 * Phase 0: Build up initial working set.
 */
static void
phase0_startup(thread_ctx_t *ctx)
{
	int phase = atomic_load(&current_phase);

	for (int i = 0; i < WORKING_SET_SIZE && phase == 0; i++) {
		int c = fast_rand(&ctx->rng_state) % NUM_SIZE_CLASSES;
		uint64_t t0 = now_ns();
		void *p = umem_cache_alloc(caches[c], UMEM_DEFAULT);
		uint64_t t1 = now_ns();

		if (p == NULL)
			break;

		track_alloc_latency(0, t1 - t0);
		ctx->ring[ctx->ring_count] = p;
		ctx->ring_classes[ctx->ring_count] = c;
		ctx->ring_count++;
		phase = atomic_load(&current_phase);
	}
	ctx->ring_cap = ctx->ring_count;
}

/*
 * Process one "request": allocate N objects, then free N oldest.
 * This keeps the working set roughly constant.
 */
static void
process_request(thread_ctx_t *ctx, int phase, int extra_allocs)
{
	int nobjs = MIN_REQUEST_OBJS +
	    (int)(fast_rand(&ctx->rng_state) %
	    (MAX_REQUEST_OBJS - MIN_REQUEST_OBJS + 1));
	nobjs += extra_allocs;

	for (int i = 0; i < nobjs; i++) {
		int c = fast_rand(&ctx->rng_state) % NUM_SIZE_CLASSES;
		uint64_t t0 = now_ns();
		void *p = umem_cache_alloc(caches[c], UMEM_DEFAULT);
		uint64_t t1 = now_ns();

		if (p == NULL)
			continue;

		track_alloc_latency(phase, t1 - t0);

		/* If ring is full, free the oldest to make room */
		if (ctx->ring_count >= ctx->ring_cap) {
			int idx = ctx->ring_head;
			int fc = ctx->ring_classes[idx];
			umem_cache_free(caches[fc], ctx->ring[idx]);
			ctx->ring[idx] = p;
			ctx->ring_classes[idx] = c;
			ctx->ring_head = (idx + 1) % ctx->ring_cap;
		} else {
			ctx->ring[ctx->ring_count] = p;
			ctx->ring_classes[ctx->ring_count] = c;
			ctx->ring_count++;
		}
	}
}

/*
 * Phase 1/3: Steady state request processing.
 */
static void
phase_steady(thread_ctx_t *ctx, int target_phase)
{
	while (atomic_load(&current_phase) == target_phase) {
		process_request(ctx, target_phase, 0);
		sleep_us(STEADY_REQ_INTERVAL_US);
	}
}

/*
 * Phase 2: Load spike -- double the rate and grow working set.
 */
static void
phase2_spike(thread_ctx_t *ctx)
{
	/* Grow ring capacity to accommodate spike */
	ctx->ring_cap = WORKING_SET_SIZE + SPIKE_EXTRA;

	while (atomic_load(&current_phase) == 2) {
		process_request(ctx, 2, MAX_REQUEST_OBJS);
		sleep_us(SPIKE_REQ_INTERVAL_US);
	}

	/* Shrink back: free excess objects */
	while (ctx->ring_count > WORKING_SET_SIZE) {
		ctx->ring_count--;
		int idx = ctx->ring_count;
		int c = ctx->ring_classes[idx];
		if (ctx->ring[idx] != NULL)
			umem_cache_free(caches[c], ctx->ring[idx]);
	}
	ctx->ring_cap = WORKING_SET_SIZE;
}

/*
 * Free all remaining objects.
 */
static void
phase_cleanup(thread_ctx_t *ctx)
{
	for (int i = 0; i < ctx->ring_count; i++) {
		if (ctx->ring[i] != NULL) {
			int c = ctx->ring_classes[i];
			umem_cache_free(caches[c], ctx->ring[i]);
		}
	}
	ctx->ring_count = 0;
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

	phase_steady(ctx, 1);
	phase2_spike(ctx);
	phase_steady(ctx, 3);
	phase_cleanup(ctx);

	return NULL;
}

/*
 * Drive the phase timer and call umem_profile_sample() periodically.
 */
static void
run_phases(void)
{
	uint64_t start = now_ns();
	uint64_t phase_boundaries[4];

	phase_boundaries[0] = start +
	    (uint64_t)PHASE0_SECS * 1000000000ULL;
	phase_boundaries[1] = phase_boundaries[0] +
	    (uint64_t)PHASE1_SECS * 1000000000ULL;
	phase_boundaries[2] = phase_boundaries[1] +
	    (uint64_t)PHASE2_SECS * 1000000000ULL;
	phase_boundaries[3] = phase_boundaries[2] +
	    (uint64_t)PHASE3_SECS * 1000000000ULL;

	int prev_phase = -1;
	uint64_t deadline = phase_boundaries[3];

	while (now_ns() < deadline) {
		uint64_t t = now_ns();
		int phase;

		if (t < phase_boundaries[0])
			phase = 0;
		else if (t < phase_boundaries[1])
			phase = 1;
		else if (t < phase_boundaries[2])
			phase = 2;
		else
			phase = 3;

		if (phase != prev_phase) {
			const char *names[] = {
			    "Startup", "Steady state",
			    "Load spike", "Return to steady"
			};
			printf("=== Phase %d: %s ===\n", phase, names[phase]);
			atomic_store(&current_phase, phase);
			prev_phase = phase;
		}

		if (umem_profile_active)
			umem_profile_sample();
		sleep_us(200000);
	}

	atomic_store(&current_phase, 4);
}

static void
print_results(void)
{
	uint64_t p0_ns = atomic_load(&phase0_alloc_ns);
	uint64_t p0_cnt = atomic_load(&phase0_alloc_count);
	uint64_t p1_ns = atomic_load(&phase1_alloc_ns);
	uint64_t p1_cnt = atomic_load(&phase1_alloc_count);
	uint64_t p2_ns = atomic_load(&phase2_alloc_ns);
	uint64_t p2_cnt = atomic_load(&phase2_alloc_count);
	uint64_t p3_ns = atomic_load(&phase3_alloc_ns);
	uint64_t p3_cnt = atomic_load(&phase3_alloc_count);

	printf("\n=== Results ===\n");
	printf("%-20s %12s %12s %12s\n",
	    "Phase", "Allocs", "Total ns", "Avg ns/alloc");
	printf("%-20s %12llu %12llu %12.1f\n", "0 (startup)",
	    (unsigned long long)p0_cnt, (unsigned long long)p0_ns,
	    p0_cnt ? (double)p0_ns / (double)p0_cnt : 0.0);
	printf("%-20s %12llu %12llu %12.1f\n", "1 (steady)",
	    (unsigned long long)p1_cnt, (unsigned long long)p1_ns,
	    p1_cnt ? (double)p1_ns / (double)p1_cnt : 0.0);
	printf("%-20s %12llu %12llu %12.1f\n", "2 (spike)",
	    (unsigned long long)p2_cnt, (unsigned long long)p2_ns,
	    p2_cnt ? (double)p2_ns / (double)p2_cnt : 0.0);
	printf("%-20s %12llu %12llu %12.1f\n", "3 (return steady)",
	    (unsigned long long)p3_cnt, (unsigned long long)p3_ns,
	    p3_cnt ? (double)p3_ns / (double)p3_cnt : 0.0);

	printf("\nTo compare: run once with UMEM_PROFILE=record:/tmp/prof.ump\n"
	    "then again with UMEM_PROFILE=use:/tmp/prof.ump\n"
	    "Lower avg ns/alloc in phases 0 and 2 indicates profile benefit.\n");
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
		ctxs[t].rng_state = (unsigned int)(t + 1) * 7919;
		ctxs[t].ring_cap = WORKING_SET_SIZE;
	}

	/* Launch workers */
	for (int t = 0; t < NUM_THREADS; t++) {
		if (pthread_create(&threads[t], NULL, worker, &ctxs[t]) != 0) {
			fprintf(stderr, "pthread_create failed for T%d\n", t);
			return 1;
		}
	}

	printf("=== Starting workload ===\n");
	atomic_store(&current_phase, 0);

	run_phases();

	printf("=== Waiting for workers ===\n");
	for (int t = 0; t < NUM_THREADS; t++)
		pthread_join(threads[t], NULL);

	printf("=== Workload complete ===\n");

	print_results();

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

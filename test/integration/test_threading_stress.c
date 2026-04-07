/*
 * Comprehensive threading stress test for libumem
 *
 * This test is designed to find threading bugs through aggressive stress testing:
 * - Many threads (up to 128)
 * - Heavy allocation/free/realloc workloads
 * - Mixed malloc() and umem_alloc() paths
 * - Different allocation sizes and patterns
 * - Memory corruption detection
 * - Race condition detection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include "../munit.h"
#include "../../umem.h"

#define MAX_THREADS 128
#define STRESS_ITERATIONS 50000
#define PATTERN_SEED 0x42

/* Global error counter */
static atomic_int error_count = 0;

/* Thread-safe random number generator state */
typedef struct {
	unsigned int seed;
} thread_rng_t;

static unsigned int
thread_rand(thread_rng_t *rng)
{
	rng->seed = rng->seed * 1103515245 + 12345;
	return (rng->seed / 65536) % 32768;
}

/*
 * Test 1: Aggressive realloc() stress test
 * Tests the recently fixed realloc() bug under heavy threading
 */
typedef struct {
	int thread_id;
	int iterations;
} stress_args_t;

static void *
worker_realloc_stress(void *arg)
{
	stress_args_t *args = (stress_args_t *)arg;
	thread_rng_t rng = { .seed = (unsigned int)args->thread_id };

	for (int i = 0; i < args->iterations; i++) {
		/* Start with small allocation */
		size_t size = 64 + (thread_rand(&rng) % 128);
		char *ptr = malloc(size);
		if (!ptr) {
			atomic_fetch_add(&error_count, 1);
			continue;
		}

		/* Fill with pattern */
		unsigned char pattern = (unsigned char)(args->thread_id + i);
		memset(ptr, pattern, size);

		/* Grow several times */
		for (int j = 0; j < 5; j++) {
			size_t old_size = size;
			size = old_size + (thread_rand(&rng) % 1000) + 100;

			char *new_ptr = realloc(ptr, size);
			if (!new_ptr) {
				free(ptr);
				atomic_fetch_add(&error_count, 1);
				goto next_iteration;
			}
			ptr = new_ptr;

			/* Verify old data preserved */
			for (size_t k = 0; k < old_size; k++) {
				if (ptr[k] != pattern) {
					atomic_fetch_add(&error_count, 1);
					free(ptr);
					goto next_iteration;
				}
			}

			/* Fill new space */
			memset(ptr + old_size, pattern, size - old_size);
		}

		/* Shrink back down */
		for (int j = 0; j < 3; j++) {
			size_t old_size = size;
			size = size / 2;
			if (size < 32) size = 32;

			char *new_ptr = realloc(ptr, size);
			if (!new_ptr) {
				free(ptr);
				atomic_fetch_add(&error_count, 1);
				goto next_iteration;
			}
			ptr = new_ptr;

			/* Verify data still intact */
			for (size_t k = 0; k < size; k++) {
				if (ptr[k] != pattern) {
					atomic_fetch_add(&error_count, 1);
					free(ptr);
					goto next_iteration;
				}
			}
		}

		free(ptr);
next_iteration:
		/* Yield occasionally to increase contention */
		if (i % 100 == 0) {
			sched_yield();
		}
	}

	return NULL;
}

static MunitResult
test_realloc_stress(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	const int NUM_THREADS = 32;
	const int ITERATIONS = 1000;
	pthread_t threads[NUM_THREADS];
	stress_args_t args[NUM_THREADS];

	atomic_store(&error_count, 0);

	printf("  Starting realloc stress test: %d threads, %d iterations each\n",
	       NUM_THREADS, ITERATIONS);

	for (int i = 0; i < NUM_THREADS; i++) {
		args[i].thread_id = i;
		args[i].iterations = ITERATIONS;
		munit_assert_int(pthread_create(&threads[i], NULL,
		    worker_realloc_stress, &args[i]), ==, 0);
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
	}

	int errors = atomic_load(&error_count);
	munit_assert_int(errors, ==, 0);

	return MUNIT_OK;
}

/*
 * Test 2: Mixed malloc/umem_alloc with heavy load
 */
static void *
worker_mixed_heavy(void *arg)
{
	stress_args_t *args = (stress_args_t *)arg;
	thread_rng_t rng = { .seed = (unsigned int)(args->thread_id + 1000) };

	for (int i = 0; i < args->iterations; i++) {
		int choice = thread_rand(&rng) % 4;
		size_t size = 16 + (thread_rand(&rng) % 512);
		void *ptr;

		switch (choice) {
		case 0:
			/* malloc path */
			ptr = malloc(size);
			if (ptr) {
				memset(ptr, 'M', size);
				free(ptr);
			} else {
				atomic_fetch_add(&error_count, 1);
			}
			break;

		case 1:
			/* umem_alloc path */
			ptr = umem_alloc(size, UMEM_DEFAULT);
			if (ptr) {
				memset(ptr, 'U', size);
				umem_free(ptr, size);
			} else {
				atomic_fetch_add(&error_count, 1);
			}
			break;

		case 2:
			/* calloc path */
			ptr = calloc(1, size);
			if (ptr) {
				/* Verify it's zeroed */
				unsigned char *bytes = (unsigned char *)ptr;
				for (size_t j = 0; j < size; j++) {
					if (bytes[j] != 0) {
						atomic_fetch_add(&error_count, 1);
						break;
					}
				}
				free(ptr);
			} else {
				atomic_fetch_add(&error_count, 1);
			}
			break;

		case 3:
			/* umem_zalloc path */
			ptr = umem_zalloc(size, UMEM_DEFAULT);
			if (ptr) {
				/* Verify it's zeroed */
				unsigned char *bytes = (unsigned char *)ptr;
				for (size_t j = 0; j < size; j++) {
					if (bytes[j] != 0) {
						atomic_fetch_add(&error_count, 1);
						break;
					}
				}
				umem_free(ptr, size);
			} else {
				atomic_fetch_add(&error_count, 1);
			}
			break;
		}
	}

	return NULL;
}

static MunitResult
test_mixed_heavy(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	const int NUM_THREADS = 64;
	const int ITERATIONS = 5000;
	pthread_t threads[NUM_THREADS];
	stress_args_t args[NUM_THREADS];

	atomic_store(&error_count, 0);

	printf("  Starting mixed malloc/umem test: %d threads, %d iterations each\n",
	       NUM_THREADS, ITERATIONS);

	for (int i = 0; i < NUM_THREADS; i++) {
		args[i].thread_id = i;
		args[i].iterations = ITERATIONS;
		munit_assert_int(pthread_create(&threads[i], NULL,
		    worker_mixed_heavy, &args[i]), ==, 0);
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
	}

	int errors = atomic_load(&error_count);
	munit_assert_int(errors, ==, 0);

	return MUNIT_OK;
}

/*
 * Test 3: Rapid thread create/destroy with allocations
 * Tests thread-local cache cleanup
 */
static void *
worker_rapid_alloc(void *arg)
{
	int thread_id = *(int *)arg;
	thread_rng_t rng = { .seed = (unsigned int)(thread_id + 2000) };

	/* Do some quick allocations */
	for (int i = 0; i < 50; i++) {
		size_t size = 32 + (thread_rand(&rng) % 128);
		void *ptr = umem_alloc(size, UMEM_DEFAULT);
		if (!ptr) {
			atomic_fetch_add(&error_count, 1);
			return NULL;
		}
		memset(ptr, (unsigned char)thread_id, size);
		umem_free(ptr, size);
	}

	return NULL;
}

static MunitResult
test_rapid_thread_churn(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	const int NUM_WAVES = 20;
	const int THREADS_PER_WAVE = 32;

	atomic_store(&error_count, 0);

	printf("  Starting rapid thread churn: %d waves of %d threads\n",
	       NUM_WAVES, THREADS_PER_WAVE);

	for (int wave = 0; wave < NUM_WAVES; wave++) {
		pthread_t threads[THREADS_PER_WAVE];
		int thread_ids[THREADS_PER_WAVE];

		for (int i = 0; i < THREADS_PER_WAVE; i++) {
			thread_ids[i] = wave * THREADS_PER_WAVE + i;
			munit_assert_int(pthread_create(&threads[i], NULL,
			    worker_rapid_alloc, &thread_ids[i]), ==, 0);
		}

		for (int i = 0; i < THREADS_PER_WAVE; i++) {
			munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
		}
	}

	int errors = atomic_load(&error_count);
	munit_assert_int(errors, ==, 0);

	return MUNIT_OK;
}

/*
 * Test 4: Size variation stress - tests different size classes
 */
static void *
worker_size_variation(void *arg)
{
	stress_args_t *args = (stress_args_t *)arg;
	thread_rng_t rng = { .seed = (unsigned int)(args->thread_id + 3000) };

	/* Sizes from tiny to large */
	size_t sizes[] = {
		8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512,
		768, 1024, 1536, 2048, 4096, 8192, 16384, 32768
	};
	int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

	for (int i = 0; i < args->iterations; i++) {
		int size_idx = thread_rand(&rng) % num_sizes;
		size_t size = sizes[size_idx];

		void *ptr = umem_alloc(size, UMEM_DEFAULT);
		if (!ptr) {
			atomic_fetch_add(&error_count, 1);
			continue;
		}

		/* Fill with pattern */
		unsigned char pattern = (unsigned char)(args->thread_id ^ i);
		memset(ptr, pattern, size);

		/* Verify pattern */
		unsigned char *bytes = (unsigned char *)ptr;
		for (size_t j = 0; j < size; j++) {
			if (bytes[j] != pattern) {
				atomic_fetch_add(&error_count, 1);
				break;
			}
		}

		umem_free(ptr, size);
	}

	return NULL;
}

static MunitResult
test_size_variation(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	const int NUM_THREADS = 48;
	const int ITERATIONS = 2000;
	pthread_t threads[NUM_THREADS];
	stress_args_t args[NUM_THREADS];

	atomic_store(&error_count, 0);

	printf("  Starting size variation test: %d threads, %d iterations each\n",
	       NUM_THREADS, ITERATIONS);

	for (int i = 0; i < NUM_THREADS; i++) {
		args[i].thread_id = i;
		args[i].iterations = ITERATIONS;
		munit_assert_int(pthread_create(&threads[i], NULL,
		    worker_size_variation, &args[i]), ==, 0);
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
	}

	int errors = atomic_load(&error_count);
	munit_assert_int(errors, ==, 0);

	return MUNIT_OK;
}

/*
 * Test 5: Maximum thread stress
 * Push to 128 threads to stress depot and magazine layer
 */
static void *
worker_max_threads(void *arg)
{
	int thread_id = *(int *)arg;
	thread_rng_t rng = { .seed = (unsigned int)(thread_id + 4000) };

	void *ptrs[100];

	/* Allocate */
	for (int i = 0; i < 100; i++) {
		size_t size = 64 + (thread_rand(&rng) % 128);
		ptrs[i] = malloc(size);
		if (!ptrs[i]) {
			atomic_fetch_add(&error_count, 1);
			/* Free what we have */
			for (int j = 0; j < i; j++) {
				free(ptrs[j]);
			}
			return NULL;
		}
		memset(ptrs[i], (unsigned char)thread_id, size);
	}

	/* Some realloc operations */
	for (int i = 0; i < 50; i++) {
		int idx = thread_rand(&rng) % 100;
		size_t new_size = 128 + (thread_rand(&rng) % 256);
		void *new_ptr = realloc(ptrs[idx], new_size);
		if (new_ptr) {
			ptrs[idx] = new_ptr;
		}
	}

	/* Free all */
	for (int i = 0; i < 100; i++) {
		free(ptrs[i]);
	}

	return NULL;
}

static MunitResult
test_maximum_threads(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	const int NUM_THREADS = 128;
	pthread_t threads[NUM_THREADS];
	int thread_ids[NUM_THREADS];

	atomic_store(&error_count, 0);

	printf("  Starting maximum thread test: %d threads\n", NUM_THREADS);

	for (int i = 0; i < NUM_THREADS; i++) {
		thread_ids[i] = i;
		munit_assert_int(pthread_create(&threads[i], NULL,
		    worker_max_threads, &thread_ids[i]), ==, 0);
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
	}

	int errors = atomic_load(&error_count);
	munit_assert_int(errors, ==, 0);

	return MUNIT_OK;
}

/*
 * Test 6: Cross-thread free stress
 * Thread A allocates, thread B frees
 */
typedef struct {
	pthread_mutex_t lock;
	void *ptrs[1000];
	int count;
	int done;
} shared_queue_t;

static void *
worker_allocator(void *arg)
{
	shared_queue_t *queue = (shared_queue_t *)arg;
	thread_rng_t rng = { .seed = 5000 };

	for (int i = 0; i < 1000; i++) {
		size_t size = 64 + (thread_rand(&rng) % 256);
		void *ptr = malloc(size);
		if (!ptr) {
			atomic_fetch_add(&error_count, 1);
			continue;
		}

		memset(ptr, 'A', size);

		pthread_mutex_lock(&queue->lock);
		while (queue->count >= 1000) {
			pthread_mutex_unlock(&queue->lock);
			sched_yield();
			pthread_mutex_lock(&queue->lock);
		}
		queue->ptrs[queue->count++] = ptr;
		pthread_mutex_unlock(&queue->lock);
	}

	pthread_mutex_lock(&queue->lock);
	queue->done = 1;
	pthread_mutex_unlock(&queue->lock);

	return NULL;
}

static void *
worker_freer(void *arg)
{
	shared_queue_t *queue = (shared_queue_t *)arg;

	while (1) {
		pthread_mutex_lock(&queue->lock);

		if (queue->count > 0) {
			void *ptr = queue->ptrs[--queue->count];
			pthread_mutex_unlock(&queue->lock);
			free(ptr);
		} else if (queue->done) {
			pthread_mutex_unlock(&queue->lock);
			break;
		} else {
			pthread_mutex_unlock(&queue->lock);
			sched_yield();
		}
	}

	return NULL;
}

static MunitResult
test_cross_thread_free(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	shared_queue_t queue = {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.count = 0,
		.done = 0
	};

	pthread_t allocator, freer;

	atomic_store(&error_count, 0);

	printf("  Starting cross-thread free test\n");

	munit_assert_int(pthread_create(&freer, NULL, worker_freer, &queue), ==, 0);
	munit_assert_int(pthread_create(&allocator, NULL, worker_allocator, &queue), ==, 0);

	munit_assert_int(pthread_join(allocator, NULL), ==, 0);
	munit_assert_int(pthread_join(freer, NULL), ==, 0);

	pthread_mutex_destroy(&queue.lock);

	int errors = atomic_load(&error_count);
	munit_assert_int(errors, ==, 0);

	return MUNIT_OK;
}

static MunitTest test_suite_tests[] = {
	{
		"/realloc_stress",
		test_realloc_stress,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/mixed_heavy",
		test_mixed_heavy,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/rapid_thread_churn",
		test_rapid_thread_churn,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/size_variation",
		test_size_variation,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/maximum_threads",
		test_maximum_threads,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/cross_thread_free",
		test_cross_thread_free,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite test_suite = {
	"/threading_stress",
	test_suite_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	printf("\n");
	printf("═══════════════════════════════════════════════════════════════\n");
	printf("  Threading Stress Test - Bug Hunter\n");
	printf("═══════════════════════════════════════════════════════════════\n");
	printf("\n");
	printf("This test suite aggressively stresses threading:\n");
	printf("  • Up to 128 concurrent threads\n");
	printf("  • Heavy realloc() workloads\n");
	printf("  • Mixed malloc/umem_alloc paths\n");
	printf("  • Rapid thread create/destroy\n");
	printf("  • Cross-thread frees\n");
	printf("  • Various allocation sizes\n");
	printf("\n");
	printf("═══════════════════════════════════════════════════════════════\n");
	printf("\n");

	return munit_suite_main(&test_suite, NULL, argc, argv);
}

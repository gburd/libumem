/*
 * Test magazine size auto-tuning feature
 */

#include "../munit.h"
#include "umem.h"
#include <pthread.h>
#include <string.h>

extern uint32_t umem_magazine_tuning;

/*
 * Test that tuning can be disabled
 */
static MunitResult
test_tuning_disabled(const MunitParameter params[], void* data)
{
	umem_cache_t *cache;
	void *bufs[100];
	int i;

	/* Ensure magazine tuning is disabled */
	umem_magazine_tuning = 0;

	/* Create a cache */
	cache = umem_cache_create("test_no_tune_cache", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Perform allocations */
	for (i = 0; i < 100; i++) {
		bufs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Free all buffers */
	for (i = 0; i < 100; i++) {
		umem_cache_free(cache, bufs[i]);
	}

	umem_cache_destroy(cache);

	return MUNIT_OK;
}

/*
 * Test basic magazine tuning functionality
 */
static MunitResult
test_basic_tuning(const MunitParameter params[], void* data)
{
	umem_cache_t *cache;
	void *bufs[1000];
	int i;

	/* Enable magazine tuning */
	umem_magazine_tuning = 1;

	/* Create a cache */
	cache = umem_cache_create("test_tune_cache", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Perform many allocations to trigger reloads */
	for (i = 0; i < 1000; i++) {
		bufs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Free all buffers */
	for (i = 0; i < 1000; i++) {
		umem_cache_free(cache, bufs[i]);
	}

	/* Allocate and free in a pattern that causes magazine reloads */
	for (i = 0; i < 10; i++) {
		int j;
		for (j = 0; j < 100; j++) {
			bufs[j] = umem_cache_alloc(cache, UMEM_DEFAULT);
			munit_assert_not_null(bufs[j]);
		}
		for (j = 0; j < 100; j++) {
			umem_cache_free(cache, bufs[j]);
		}
	}

	umem_cache_destroy(cache);

	/* Disable magazine tuning */
	umem_magazine_tuning = 0;

	return MUNIT_OK;
}

/*
 * Worker thread for multithreaded test
 */
static void *
worker_thread(void *arg)
{
	umem_cache_t *cache = (umem_cache_t *)arg;
	void *bufs[100];
	int i, iter;

	for (iter = 0; iter < 50; iter++) {
		/* Allocate */
		for (i = 0; i < 100; i++) {
			bufs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
			if (bufs[i] == NULL) {
				return (void *)1;
			}
		}

		/* Free */
		for (i = 0; i < 100; i++) {
			umem_cache_free(cache, bufs[i]);
		}
	}

	return NULL;
}

/*
 * Test tuning with multiple threads
 */
static MunitResult
test_threaded_tuning(const MunitParameter params[], void* data)
{
	umem_cache_t *cache;
	pthread_t threads[4];
	int i, rc;
	void *result;

	/* Enable magazine tuning */
	umem_magazine_tuning = 1;

	/* Create a cache */
	cache = umem_cache_create("test_tune_mt_cache", 128, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Launch worker threads */
	for (i = 0; i < 4; i++) {
		rc = pthread_create(&threads[i], NULL, worker_thread, cache);
		munit_assert_int(rc, ==, 0);
	}

	/* Wait for threads to complete */
	for (i = 0; i < 4; i++) {
		pthread_join(threads[i], &result);
		munit_assert_null(result);
	}

	umem_cache_destroy(cache);

	/* Disable magazine tuning */
	umem_magazine_tuning = 0;

	return MUNIT_OK;
}

/*
 * Test tuning with UMEM_OPTIONS
 */
static MunitResult
test_tuning_via_options(const MunitParameter params[], void* data)
{
	umem_cache_t *cache;
	void *bufs[100];
	int i;
	uint32_t orig_tuning = umem_magazine_tuning;

	/* Manually set tuning (normally done via UMEM_OPTIONS) */
	umem_magazine_tuning = 1;

	/* Create a cache */
	cache = umem_cache_create("test_opt_tune_cache", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Perform allocations */
	for (i = 0; i < 100; i++) {
		bufs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Free all buffers */
	for (i = 0; i < 100; i++) {
		umem_cache_free(cache, bufs[i]);
	}

	umem_cache_destroy(cache);

	/* Restore original value */
	umem_magazine_tuning = orig_tuning;

	return MUNIT_OK;
}

static MunitTest magazine_tune_tests[] = {
	{ "/disabled", test_tuning_disabled, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/basic", test_basic_tuning, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/threaded", test_threaded_tuning, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/options", test_tuning_via_options, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_magazine_tune = {
	"/magazine_tune",
	magazine_tune_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

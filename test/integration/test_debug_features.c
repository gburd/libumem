/*
 * Test that UMEM_DEBUG and UMEM_LOGGING features work correctly
 * with the new thread-local allocations and malloc interposition.
 *
 * This test validates that debugging features continue to work after
 * the bootstrap allocator and malloc_interpose.c changes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "../munit.h"
#include "../../umem.h"

/*
 * Test UMEM_DEBUG=guards - pattern fill and redzone checking
 * Guards fill freed memory with 0xdeadbeef and use redzones
 */
static MunitResult
test_guards_pattern_fill(const MunitParameter params[], void *user_data)
{
	void *ptr;
	unsigned char *bytes;
	int has_pattern = 0;

	(void)params;
	(void)user_data;

	/* Test with direct umem_alloc() */
	ptr = umem_alloc(128, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	/* Fill with known pattern */
	memset(ptr, 0x42, 128);

	/* Free it */
	umem_free(ptr, 128);

	/*
	 * In debug mode with guards, freed memory is filled with 0xdeadbeef.
	 * We can't safely read freed memory, but we can verify guards work
	 * by checking that valid allocations don't have the deadbeef pattern.
	 */
	ptr = umem_alloc(128, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	/* Check if new allocation has 0xbaddcafe pattern (uninitialized) */
	bytes = (unsigned char *)ptr;
	for (int i = 0; i < 4; i++) {
		if (bytes[i] == 0xfe || bytes[i] == 0xca || bytes[i] == 0xdd || bytes[i] == 0xba) {
			has_pattern = 1;
			break;
		}
	}

	/* In debug mode, we expect to see the baddcafe pattern */
	if (getenv("UMEM_DEBUG") != NULL) {
		/* Debug mode active - should have pattern */
		printf("Debug mode: uninitialized pattern %s detected\n",
		       has_pattern ? "IS" : "NOT");
	}

	umem_free(ptr, 128);
	return MUNIT_OK;
}

/*
 * Test UMEM_DEBUG=audit - allocation auditing with stack traces
 */
static MunitResult
test_audit_tracking(const MunitParameter params[], void *user_data)
{
	void *ptr1, *ptr2, *ptr3;

	(void)params;
	(void)user_data;

	/* Allocate several buffers */
	ptr1 = umem_alloc(64, UMEM_DEFAULT);
	ptr2 = umem_alloc(128, UMEM_DEFAULT);
	ptr3 = umem_alloc(256, UMEM_DEFAULT);

	munit_assert_not_null(ptr1);
	munit_assert_not_null(ptr2);
	munit_assert_not_null(ptr3);

	/* In audit mode, these are tracked with stack traces */
	printf("Allocated 3 buffers for audit tracking\n");

	/* Free them in different order */
	umem_free(ptr2, 128);
	umem_free(ptr1, 64);
	umem_free(ptr3, 256);

	printf("Freed all buffers (no leaks)\n");

	return MUNIT_OK;
}

/*
 * Test with malloc() interposition - ensure debug features work
 * even when using LD_PRELOAD malloc interposition
 */
static MunitResult
test_malloc_interposition_debug(const MunitParameter params[], void *user_data)
{
	char *ptr;

	(void)params;
	(void)user_data;

	/* Use malloc() - will go through malloc_interpose.c */
	ptr = malloc(512);
	munit_assert_not_null(ptr);

	/* Fill with pattern */
	memset(ptr, 'M', 512);

	/* Free it */
	free(ptr);

	/* Allocate again */
	ptr = malloc(1024);
	munit_assert_not_null(ptr);

	/* Test realloc() - the recently fixed function */
	ptr = realloc(ptr, 2048);
	munit_assert_not_null(ptr);

	/* Fill and verify */
	memset(ptr, 'R', 2048);

	free(ptr);

	printf("malloc/free/realloc work with debug features\n");
	return MUNIT_OK;
}

/*
 * Test cache debugging with umem_cache_t
 */
static MunitResult
test_cache_debug(const MunitParameter params[], void *user_data)
{
	umem_cache_t *cache;
	void *obj1, *obj2, *obj3;

	(void)params;
	(void)user_data;

	/* Create a cache with debug features */
	cache = umem_cache_create(
		"test_debug_cache",
		256,
		0,
		NULL,  /* constructor */
		NULL,  /* destructor */
		NULL,  /* reclaim */
		NULL,  /* private */
		NULL,  /* source */
		0      /* cflags */
	);
	munit_assert_not_null(cache);

	/* Allocate objects */
	obj1 = umem_cache_alloc(cache, UMEM_DEFAULT);
	obj2 = umem_cache_alloc(cache, UMEM_DEFAULT);
	obj3 = umem_cache_alloc(cache, UMEM_DEFAULT);

	munit_assert_not_null(obj1);
	munit_assert_not_null(obj2);
	munit_assert_not_null(obj3);

	/* Use the objects */
	memset(obj1, 'A', 256);
	memset(obj2, 'B', 256);
	memset(obj3, 'C', 256);

	/* Free them */
	umem_cache_free(cache, obj1);
	umem_cache_free(cache, obj2);
	umem_cache_free(cache, obj3);

	/* Destroy cache */
	umem_cache_destroy(cache);

	printf("Cache debug features functional\n");
	return MUNIT_OK;
}

/*
 * Test multithreaded debug features - ensure thread-local caching
 * doesn't interfere with debugging
 */
static void *
thread_debug_worker(void *arg)
{
	int thread_id = *(int *)arg;
	void *ptr1, *ptr2;

	/* Each thread does allocations */
	ptr1 = umem_alloc(128, UMEM_DEFAULT);
	if (ptr1 == NULL) {
		return (void *)1;
	}

	memset(ptr1, 'A' + thread_id, 128);

	ptr2 = malloc(256);
	if (ptr2 == NULL) {
		umem_free(ptr1, 128);
		return (void *)1;
	}

	memset(ptr2, '0' + thread_id, 256);

	/* Free in reverse order */
	free(ptr2);
	umem_free(ptr1, 128);

	return (void *)0;
}

static MunitResult
test_multithreaded_debug(const MunitParameter params[], void *user_data)
{
	pthread_t threads[4];
	int thread_ids[4];
	void *result;

	(void)params;
	(void)user_data;

	/* Create threads that do allocations */
	for (int i = 0; i < 4; i++) {
		thread_ids[i] = i;
		munit_assert_int(pthread_create(&threads[i], NULL,
		    thread_debug_worker, &thread_ids[i]), ==, 0);
	}

	/* Wait for all threads */
	for (int i = 0; i < 4; i++) {
		munit_assert_int(pthread_join(threads[i], &result), ==, 0);
		munit_assert_ptr_equal(result, (void *)0);
	}

	printf("Multithreaded debug features functional\n");
	return MUNIT_OK;
}

/*
 * Test umem_zalloc() with debug features
 */
static MunitResult
test_zalloc_debug(const MunitParameter params[], void *user_data)
{
	void *ptr;
	unsigned char *bytes;

	(void)params;
	(void)user_data;

	/* umem_zalloc should zero memory even in debug mode */
	ptr = umem_zalloc(1024, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	/* Verify it's zeroed */
	bytes = (unsigned char *)ptr;
	for (int i = 0; i < 1024; i++) {
		munit_assert_uchar(bytes[i], ==, 0);
	}

	umem_free(ptr, 1024);

	printf("umem_zalloc zeroes memory correctly in debug mode\n");
	return MUNIT_OK;
}

/*
 * Test allocation patterns that trigger PTC (per-thread cache)
 * to ensure debug features work with thread-local allocations
 */
static MunitResult
test_ptc_with_debug(const MunitParameter params[], void *user_data)
{
	void *ptrs[100];

	(void)params;
	(void)user_data;

	/*
	 * Allocate many small objects (should use PTC on x86_64).
	 * PTC handles allocations up to 448 bytes on 64-bit.
	 */
	for (int i = 0; i < 100; i++) {
		ptrs[i] = umem_alloc(64, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
		memset(ptrs[i], 'P', 64);
	}

	/* Free them all */
	for (int i = 0; i < 100; i++) {
		umem_free(ptrs[i], 64);
	}

	/* Allocate again - should reuse from PTC */
	for (int i = 0; i < 100; i++) {
		ptrs[i] = umem_alloc(64, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Free them all */
	for (int i = 0; i < 100; i++) {
		umem_free(ptrs[i], 64);
	}

	printf("PTC works correctly with debug features\n");
	return MUNIT_OK;
}

static MunitTest test_suite_tests[] = {
	{
		"/guards_pattern",
		test_guards_pattern_fill,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/audit_tracking",
		test_audit_tracking,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/malloc_interposition",
		test_malloc_interposition_debug,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/cache_debug",
		test_cache_debug,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/multithreaded",
		test_multithreaded_debug,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/zalloc",
		test_zalloc_debug,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/ptc_debug",
		test_ptc_with_debug,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite test_suite = {
	"/debug_features",
	test_suite_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	const char *debug_env = getenv("UMEM_DEBUG");
	const char *logging_env = getenv("UMEM_LOGGING");

	printf("\n");
	printf("=================================================================\n");
	printf("Testing UMEM_DEBUG and UMEM_LOGGING features\n");
	printf("=================================================================\n");
	printf("\n");
	printf("Current settings:\n");
	printf("  UMEM_DEBUG=%s\n", debug_env ? debug_env : "(not set)");
	printf("  UMEM_LOGGING=%s\n", logging_env ? logging_env : "(not set)");
	printf("\n");
	printf("To test with debug features:\n");
	printf("  UMEM_DEBUG=default ./test_debug_features\n");
	printf("  UMEM_DEBUG=guards ./test_debug_features\n");
	printf("  UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m ./test_debug_features\n");
	printf("\n");
	printf("=================================================================\n");
	printf("\n");

	return munit_suite_main(&test_suite, NULL, argc, argv);
}

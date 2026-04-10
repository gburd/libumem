/*
 * Advanced umem.c tests for improving coverage
 * Focuses on magazine layer, depot operations, and edge cases
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "munit.h"
#include "umem.h"

/* Test: Magazine layer operations */
static MunitResult
test_umem_magazine_operations(const MunitParameter params[], void* data)
{
	umem_cache_t *cache = umem_cache_create("magazine_test",
	    sizeof(int), 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Allocate enough to fill magazines */
	void *ptrs[100];
	for (int i = 0; i < 100; i++) {
		ptrs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Free to populate magazines */
	for (int i = 0; i < 100; i++) {
		umem_cache_free(cache, ptrs[i]);
	}

	/* Allocate again - should reuse from magazines */
	for (int i = 0; i < 50; i++) {
		ptrs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	for (int i = 0; i < 50; i++) {
		umem_cache_free(cache, ptrs[i]);
	}

	umem_cache_destroy(cache);
	return MUNIT_OK;
}

/* Test: Cache reaping */
static MunitResult
test_umem_cache_reap_detailed(const MunitParameter params[], void* data)
{
	umem_cache_t *cache = umem_cache_create("reap_test",
	    128, 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Allocate and free many objects to build up magazines */
	void *ptrs[200];
	for (int i = 0; i < 200; i++) {
		ptrs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
	}

	for (int i = 0; i < 200; i++) {
		umem_cache_free(cache, ptrs[i]);
	}

	/* Trigger reaping */
	umem_reap();

	/* Verify cache still works */
	void *ptr = umem_cache_alloc(cache, UMEM_DEFAULT);
	munit_assert_not_null(ptr);
	umem_cache_free(cache, ptr);

	umem_cache_destroy(cache);
	return MUNIT_OK;
}

/* Test: Multiple cache sizes */
static MunitResult
test_umem_multiple_cache_sizes(const MunitParameter params[], void* data)
{
	size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
	umem_cache_t *caches[9];

	/* Create caches of different sizes */
	for (int i = 0; i < 9; i++) {
		char name[32];
		snprintf(name, sizeof(name), "size_%zu", sizes[i]);
		caches[i] = umem_cache_create(name, sizes[i], 0,
		    NULL, NULL, NULL, NULL, NULL, 0);
		munit_assert_not_null(caches[i]);
	}

	/* Allocate from each */
	void *ptrs[9];
	for (int i = 0; i < 9; i++) {
		ptrs[i] = umem_cache_alloc(caches[i], UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Free and destroy */
	for (int i = 0; i < 9; i++) {
		umem_cache_free(caches[i], ptrs[i]);
		umem_cache_destroy(caches[i]);
	}

	return MUNIT_OK;
}

static int stress_ctor_calls = 0;
static int stress_dtor_calls = 0;

static int
stress_ctor(void *buf, void *arg, int flags)
{
	(void)arg;
	(void)flags;
	__atomic_add_fetch(&stress_ctor_calls, 1, __ATOMIC_SEQ_CST);
	memset(buf, 0xAA, 64);
	return 0;
}

static void
stress_dtor(void *buf, void *arg)
{
	(void)arg;
	__atomic_add_fetch(&stress_dtor_calls, 1, __ATOMIC_SEQ_CST);
	memset(buf, 0xBB, 64);
}

/* Test: Cache with constructors and destructors */
static MunitResult
test_umem_cache_ctor_dtor_stress(const MunitParameter params[], void* data)
{
	stress_ctor_calls = 0;
	stress_dtor_calls = 0;

	umem_cache_t *cache = umem_cache_create("ctor_dtor_stress",
	    64, 0, stress_ctor, stress_dtor, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Allocate many objects */
	void *ptrs[50];
	for (int i = 0; i < 50; i++) {
		ptrs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Verify constructor was called */
	munit_assert_int(stress_ctor_calls, >, 0);

	/* Free all */
	for (int i = 0; i < 50; i++) {
		umem_cache_free(cache, ptrs[i]);
	}

	/* Destroy cache - destructors should be called */
	umem_cache_destroy(cache);
	munit_assert_int(stress_dtor_calls, >, 0);

	return MUNIT_OK;
}

/* Test: Aligned allocations of various alignments */
static MunitResult
test_umem_aligned_alloc_comprehensive(const MunitParameter params[], void* data)
{
	size_t alignments[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

	for (int i = 0; i < 10; i++) {
		void *ptr = umem_alloc_align(1024, alignments[i], UMEM_DEFAULT);
		munit_assert_not_null(ptr);
		munit_assert_uint64((uintptr_t)ptr % alignments[i], ==, 0);
		umem_free_align(ptr, 1024);
	}

	return MUNIT_OK;
}

/* Test: Mixed aligned and non-aligned allocations */
static MunitResult
test_umem_mixed_aligned(const MunitParameter params[], void* data)
{
	void *ptrs[20];

	/* Alternate between aligned and non-aligned */
	for (int i = 0; i < 20; i++) {
		if (i % 2 == 0) {
			ptrs[i] = umem_alloc(512, UMEM_DEFAULT);
		} else {
			ptrs[i] = umem_alloc_align(512, 64, UMEM_DEFAULT);
		}
		munit_assert_not_null(ptrs[i]);
	}

	/* Free in reverse */
	for (int i = 19; i >= 0; i--) {
		if (i % 2 == 0) {
			umem_free(ptrs[i], 512);
		} else {
			umem_free_align(ptrs[i], 512);
		}
	}

	return MUNIT_OK;
}

/* Test: Zero-initialized allocations */
static MunitResult
test_umem_zalloc_verification(const MunitParameter params[], void* data)
{
	size_t sizes[] = {16, 64, 256, 1024, 4096};

	for (int i = 0; i < 5; i++) {
		void *ptr = umem_zalloc(sizes[i], UMEM_DEFAULT);
		munit_assert_not_null(ptr);

		/* Verify memory is zeroed */
		unsigned char *bytes = (unsigned char *)ptr;
		for (size_t j = 0; j < sizes[i]; j++) {
			munit_assert_uchar(bytes[j], ==, 0);
		}

		umem_free(ptr, sizes[i]);
	}

	return MUNIT_OK;
}

/* Test: Large allocations that go to vmem */
static MunitResult
test_umem_large_vmem_allocations(const MunitParameter params[], void* data)
{
	/* Allocate sizes larger than typical cache sizes */
	size_t sizes[] = {16*1024, 32*1024, 64*1024, 128*1024, 256*1024};
	void *ptrs[5];

	for (int i = 0; i < 5; i++) {
		ptrs[i] = umem_alloc(sizes[i], UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	for (int i = 0; i < 5; i++) {
		umem_free(ptrs[i], sizes[i]);
	}

	return MUNIT_OK;
}

/* Test: Allocation stress under memory pressure */
static MunitResult
test_umem_memory_pressure(const MunitParameter params[], void* data)
{
	/* Allocate many objects to create memory pressure */
	void *ptrs[500];
	int allocated = 0;

	for (int i = 0; i < 500; i++) {
		ptrs[i] = umem_alloc(4096, UMEM_NOFAIL);
		if (ptrs[i] == NULL) {
			break;
		}
		allocated++;
	}

	/* Free half */
	for (int i = 0; i < allocated / 2; i++) {
		umem_free(ptrs[i], 4096);
		ptrs[i] = NULL;
	}

	/* Trigger reaping */
	umem_reap();

	/* Allocate again */
	for (int i = 0; i < allocated / 2; i++) {
		if (ptrs[i] == NULL) {
			ptrs[i] = umem_alloc(4096, UMEM_DEFAULT);
			munit_assert_not_null(ptrs[i]);
		}
	}

	/* Clean up */
	for (int i = 0; i < allocated; i++) {
		if (ptrs[i] != NULL) {
			umem_free(ptrs[i], 4096);
		}
	}

	return MUNIT_OK;
}

/* Test: Cache-to-cache migration pattern */
static MunitResult
test_umem_cache_migration(const MunitParameter params[], void* data)
{
	umem_cache_t *cache1 = umem_cache_create("migrate1",
	    128, 0, NULL, NULL, NULL, NULL, NULL, 0);
	umem_cache_t *cache2 = umem_cache_create("migrate2",
	    128, 0, NULL, NULL, NULL, NULL, NULL, 0);

	munit_assert_not_null(cache1);
	munit_assert_not_null(cache2);

	/* Allocate from cache1 */
	void *ptrs[20];
	for (int i = 0; i < 20; i++) {
		ptrs[i] = umem_cache_alloc(cache1, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Free to cache1 */
	for (int i = 0; i < 20; i++) {
		umem_cache_free(cache1, ptrs[i]);
	}

	/* Allocate from cache2 */
	for (int i = 0; i < 20; i++) {
		ptrs[i] = umem_cache_alloc(cache2, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Free to cache2 */
	for (int i = 0; i < 20; i++) {
		umem_cache_free(cache2, ptrs[i]);
	}

	umem_cache_destroy(cache1);
	umem_cache_destroy(cache2);

	return MUNIT_OK;
}

/* Test: Empty cache behavior */
static MunitResult
test_umem_empty_cache(const MunitParameter params[], void* data)
{
	umem_cache_t *cache = umem_cache_create("empty_cache",
	    64, 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Immediately reap without any allocations */
	umem_reap();

	/* Allocate after reaping empty cache */
	void *ptr = umem_cache_alloc(cache, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	umem_cache_free(cache, ptr);
	umem_cache_destroy(cache);

	return MUNIT_OK;
}

/* Test: Burst allocation/deallocation pattern */
static MunitResult
test_umem_burst_pattern(const MunitParameter params[], void* data)
{
	umem_cache_t *cache = umem_cache_create("burst_cache",
	    256, 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cache);

	/* Perform multiple bursts */
	for (int burst = 0; burst < 5; burst++) {
		void *ptrs[50];

		/* Burst allocate */
		for (int i = 0; i < 50; i++) {
			ptrs[i] = umem_cache_alloc(cache, UMEM_DEFAULT);
			munit_assert_not_null(ptrs[i]);
		}

		/* Burst free */
		for (int i = 0; i < 50; i++) {
			umem_cache_free(cache, ptrs[i]);
		}
	}

	umem_cache_destroy(cache);
	return MUNIT_OK;
}

/* Test: Allocation size boundaries */
static MunitResult
test_umem_size_boundaries(const MunitParameter params[], void* data)
{
	/* Test allocation sizes around common boundaries */
	size_t sizes[] = {
		1, 7, 8, 9, 15, 16, 17,
		31, 32, 33, 63, 64, 65,
		127, 128, 129, 255, 256, 257,
		511, 512, 513, 1023, 1024, 1025
	};

	for (int i = 0; i < 25; i++) {
		void *ptr = umem_alloc(sizes[i], UMEM_DEFAULT);
		munit_assert_not_null(ptr);
		umem_free(ptr, sizes[i]);
	}

	return MUNIT_OK;
}

/* Test: Interleaved operations on multiple caches */
static MunitResult
test_umem_interleaved_caches(const MunitParameter params[], void* data)
{
	umem_cache_t *caches[5];

	/* Create multiple caches */
	for (int i = 0; i < 5; i++) {
		char name[32];
		snprintf(name, sizeof(name), "interleave_%d", i);
		caches[i] = umem_cache_create(name, 64 * (i + 1), 0,
		    NULL, NULL, NULL, NULL, NULL, 0);
		munit_assert_not_null(caches[i]);
	}

	/* Interleaved allocations */
	void *ptrs[5][10];
	for (int round = 0; round < 10; round++) {
		for (int cache_idx = 0; cache_idx < 5; cache_idx++) {
			ptrs[cache_idx][round] = umem_cache_alloc(
			    caches[cache_idx], UMEM_DEFAULT);
			munit_assert_not_null(ptrs[cache_idx][round]);
		}
	}

	/* Interleaved frees */
	for (int round = 0; round < 10; round++) {
		for (int cache_idx = 0; cache_idx < 5; cache_idx++) {
			umem_cache_free(caches[cache_idx],
			    ptrs[cache_idx][round]);
		}
	}

	/* Destroy caches */
	for (int i = 0; i < 5; i++) {
		umem_cache_destroy(caches[i]);
	}

	return MUNIT_OK;
}

/* Test suite */
static MunitTest umem_advanced_tests[] = {
	{ "/magazine_operations", test_umem_magazine_operations, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cache_reap_detailed", test_umem_cache_reap_detailed, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/multiple_cache_sizes", test_umem_multiple_cache_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ctor_dtor_stress", test_umem_cache_ctor_dtor_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aligned_alloc_comprehensive", test_umem_aligned_alloc_comprehensive, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mixed_aligned", test_umem_mixed_aligned, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/zalloc_verification", test_umem_zalloc_verification, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/large_vmem_allocations", test_umem_large_vmem_allocations, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/memory_pressure", test_umem_memory_pressure, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cache_migration", test_umem_cache_migration, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/empty_cache", test_umem_empty_cache, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/burst_pattern", test_umem_burst_pattern, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/size_boundaries", test_umem_size_boundaries, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/interleaved_caches", test_umem_interleaved_caches, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_advanced = {
	"/umem_advanced", umem_advanced_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

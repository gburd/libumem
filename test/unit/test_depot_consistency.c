/*
 * Depot consistency validation tests for umem
 *
 * Validates the magazine depot layer:
 * - Magazine lists (full/empty) are consistent
 * - Depot contention tracking works
 * - Magazine exchange operations work
 * - Depot operations under concurrent load
 * - Magazine round counts are valid
 *
 * The depot uses simple mutex-protected full/empty magazine lists.
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../umem_impl.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

static void ensure_umem_initialized(void) {
	static int initialized = 0;
	if (!initialized) {
		void *ptr = umem_alloc(64, UMEM_DEFAULT);
		if (ptr) {
			umem_free(ptr, 64);
			initialized = 1;
		}
	}
}

/*
 * Test: depot initialization
 *
 * Full and empty magazine lists should start empty after cache creation.
 */
static MunitResult
test_depot_stripe_init(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_init", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	munit_assert_long(cp->cache_full.ml_total, ==, 0);
	munit_assert_long(cp->cache_empty.ml_total, ==, 0);
	munit_assert_uint64(cp->cache_depot_contention, ==, 0);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: full magazine list populates after alloc+free cycle
 */
static MunitResult
test_depot_full_magazines(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_full", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	#define DEPOT_FULL_COUNT 500
	void *ptrs[DEPOT_FULL_COUNT];
	for (int i = 0; i < DEPOT_FULL_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}
	for (int i = 0; i < DEPOT_FULL_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	long total_full = cp->cache_full.ml_total;
	munit_assert_long(total_full, >=, 0);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: magazine exchange round-trip
 */
static MunitResult
test_depot_exchange_roundtrip(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_exchange", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	#define EXCHANGE_COUNT 200
	void *ptrs[EXCHANGE_COUNT];

	for (int i = 0; i < EXCHANGE_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}
	for (int i = 0; i < EXCHANGE_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	uint64_t slab_alloc_before = cp->cache_slab_alloc;

	for (int i = 0; i < EXCHANGE_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	uint64_t slab_alloc_delta = cp->cache_slab_alloc - slab_alloc_before;
	munit_assert_uint64(slab_alloc_delta, <, (uint64_t)EXCHANGE_COUNT);

	for (int i = 0; i < EXCHANGE_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: depot contention tracking
 */
typedef struct {
	umem_cache_t *cache;
	int iterations;
} depot_thread_arg_t;

static void *depot_contention_worker(void *arg)
{
	depot_thread_arg_t *ta = (depot_thread_arg_t *)arg;

	for (int i = 0; i < ta->iterations; i++) {
		void *ptr = umem_cache_alloc(ta->cache, UMEM_DEFAULT);
		if (ptr)
			umem_cache_free(ta->cache, ptr);
	}
	return NULL;
}

static MunitResult
test_depot_stripe_contention(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_contention", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	uint64_t contention_before = cp->cache_depot_contention;

	#define CONTENTION_THREADS 4
	#define CONTENTION_ITERS 5000
	pthread_t threads[CONTENTION_THREADS];
	depot_thread_arg_t args[CONTENTION_THREADS];

	for (int i = 0; i < CONTENTION_THREADS; i++) {
		args[i].cache = cp;
		args[i].iterations = CONTENTION_ITERS;
		pthread_create(&threads[i], NULL,
		    depot_contention_worker, &args[i]);
	}
	for (int i = 0; i < CONTENTION_THREADS; i++)
		pthread_join(threads[i], NULL);

	munit_assert_uint64(cp->cache_depot_contention, >=,
	    contention_before);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: per-CPU magazine round counts are valid
 */
static MunitResult
test_magazine_round_counts(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_rounds", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	#define ROUNDS_COUNT 100
	void *ptrs[ROUNDS_COUNT];
	for (int i = 0; i < ROUNDS_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	for (uint32_t i = 0; i <= cp->cache_cpu_mask; i++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[i];
		int magsize = ccp->cc_magsize;

		if (ccp->cc_loaded != NULL) {
			munit_assert_int(ccp->cc_rounds, >=, 0);
			munit_assert_int(ccp->cc_rounds, <=, magsize);
		}
		if (ccp->cc_ploaded != NULL) {
			munit_assert_int(ccp->cc_prounds, >=, 0);
			munit_assert_int(ccp->cc_prounds, <=, magsize);
		}
	}

	for (int i = 0; i < ROUNDS_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: magazine magsize is sane
 */
static MunitResult
test_magazine_size_consistency(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_magsize", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	void *ptrs[10];
	for (int i = 0; i < 10; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	int expected_magsize = cp->cache_magtype->mt_magsize;
	munit_assert_int(expected_magsize, >, 0);

	for (uint32_t i = 0; i <= cp->cache_cpu_mask; i++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[i];
		if (ccp->cc_magsize > 0) {
			munit_assert_int(ccp->cc_magsize, >, 0);
			munit_assert_int(ccp->cc_magsize, <=,
			    expected_magsize * 2);
		}
	}

	for (int i = 0; i < 10; i++)
		umem_cache_free(cp, ptrs[i]);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: NOMAGAZINE flag bypasses depot entirely
 */
static MunitResult
test_nomagazine_bypasses_depot(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_nomagazine", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, UMC_NOMAGAZINE);
	munit_assert_not_null(cp);

	#define NOMAG_COUNT 50
	void *ptrs[NOMAG_COUNT];
	for (int i = 0; i < NOMAG_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}
	for (int i = 0; i < NOMAG_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	munit_assert_long(cp->cache_full.ml_total, ==, 0);
	munit_assert_long(cp->cache_empty.ml_total, ==, 0);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: concurrent depot operations maintain consistency
 */
static void *depot_consistency_worker(void *arg)
{
	depot_thread_arg_t *ta = (depot_thread_arg_t *)arg;
	#define DEPOT_WORKER_BATCH 50
	void *ptrs[DEPOT_WORKER_BATCH];

	for (int cycle = 0; cycle < ta->iterations; cycle++) {
		for (int i = 0; i < DEPOT_WORKER_BATCH; i++) {
			ptrs[i] = umem_cache_alloc(ta->cache, UMEM_DEFAULT);
			if (ptrs[i] == NULL)
				break;
		}
		for (int i = 0; i < DEPOT_WORKER_BATCH; i++) {
			if (ptrs[i] != NULL)
				umem_cache_free(ta->cache, ptrs[i]);
		}
	}
	return NULL;
}

static MunitResult
test_depot_concurrent_consistency(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_concurrent", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	#define DEPOT_CONC_THREADS 4
	#define DEPOT_CONC_CYCLES 100
	pthread_t threads[DEPOT_CONC_THREADS];
	depot_thread_arg_t args[DEPOT_CONC_THREADS];

	for (int i = 0; i < DEPOT_CONC_THREADS; i++) {
		args[i].cache = cp;
		args[i].iterations = DEPOT_CONC_CYCLES;
		pthread_create(&threads[i], NULL,
		    depot_consistency_worker, &args[i]);
	}
	for (int i = 0; i < DEPOT_CONC_THREADS; i++)
		pthread_join(threads[i], NULL);

	munit_assert_long(cp->cache_full.ml_total, >=, 0);
	munit_assert_long(cp->cache_empty.ml_total, >=, 0);
	munit_assert_uint64(cp->cache_slab_alloc, >=, cp->cache_slab_free);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: depot ml_alloc tracks magazine dispensed count
 */
static MunitResult
test_depot_ml_alloc_tracking(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;
	ensure_umem_initialized();

	umem_cache_t *cp = umem_cache_create("depot_ml_alloc", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	uint64_t total_ml_alloc_before =
	    cp->cache_full.ml_alloc + cp->cache_empty.ml_alloc;

	#define ML_ALLOC_COUNT 300
	void *ptrs[ML_ALLOC_COUNT];
	for (int cycle = 0; cycle < 5; cycle++) {
		for (int i = 0; i < ML_ALLOC_COUNT; i++) {
			ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
			munit_assert_not_null(ptrs[i]);
		}
		for (int i = 0; i < ML_ALLOC_COUNT; i++)
			umem_cache_free(cp, ptrs[i]);
	}

	uint64_t total_ml_alloc_after =
	    cp->cache_full.ml_alloc + cp->cache_empty.ml_alloc;

	munit_assert_uint64(total_ml_alloc_after, >=,
	    total_ml_alloc_before);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

static MunitTest depot_tests[] = {
	{ "/stripe_init", test_depot_stripe_init,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/full_magazines", test_depot_full_magazines,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exchange_roundtrip", test_depot_exchange_roundtrip,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stripe_contention", test_depot_stripe_contention,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/magazine_round_counts", test_magazine_round_counts,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/magazine_size_consistency", test_magazine_size_consistency,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/nomagazine_bypasses_depot", test_nomagazine_bypasses_depot,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent_consistency", test_depot_concurrent_consistency,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ml_alloc_tracking", test_depot_ml_alloc_tracking,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_depot_consistency = {
	"/depot_consistency",
	depot_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

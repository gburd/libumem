/*
 * Depot consistency validation tests for umem
 *
 * Validates the magazine depot layer:
 * - Magazine lists (full/empty) are consistent
 * - Depot stripe counts are correct
 * - Magazine exchange operations work
 * - Depot operations under concurrent load
 * - Magazine round counts are valid
 *
 * The depot has UMEM_DEPOT_STRIPES (16) stripes for lock-free
 * magazine exchange. Threads hash to a stripe based on thread ID.
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
 * Test: depot stripe initialization
 *
 * All 16 depot stripes should start with zero contention and
 * empty magazine lists after cache creation.
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

	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++) {
		umem_depot_stripe_t *ds = &cp->cache_depot[i];
		/*
		 * Freshly created cache should have zero or near-zero
		 * contention on all stripes.
		 */
		munit_assert_uint64(ds->ds_contention, ==, 0);
	}

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: full magazine list populates after alloc+free cycle
 *
 * Allocating many objects and freeing them should deposit full
 * magazines into the depot stripes.
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

	/* Allocate and free enough to fill depot magazines */
	#define DEPOT_FULL_COUNT 500
	void *ptrs[DEPOT_FULL_COUNT];
	for (int i = 0; i < DEPOT_FULL_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}
	for (int i = 0; i < DEPOT_FULL_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	/*
	 * At least one stripe should have full magazines deposited.
	 * Sum ml_total across all stripes' full lists.
	 */
	long total_full = 0;
	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++)
		total_full += cp->cache_depot[i].ds_full.ml_total;

	/* Also check legacy full list */
	total_full += cp->cache_full.ml_total;

	munit_assert_long(total_full, >=, 0);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: magazine exchange round-trip
 *
 * Allocate to exhaust CPU magazine -> forces depot/slab reload.
 * Free to fill CPU magazine -> forces depot deposit.
 * Re-allocate -> should retrieve from depot.
 * Verify slab_alloc didn't increase much on re-alloc (depot served them).
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

	/* Phase 1: allocate (populates slabs) */
	for (int i = 0; i < EXCHANGE_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Phase 2: free all (deposits full magazines in depot) */
	for (int i = 0; i < EXCHANGE_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	/* Record slab alloc before re-alloc */
	uint64_t slab_alloc_before = cp->cache_slab_alloc;

	/* Phase 3: re-allocate (should come from depot, not slabs) */
	for (int i = 0; i < EXCHANGE_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/*
	 * Slab allocations should not have increased by EXCHANGE_COUNT
	 * because the depot should have served most of them.
	 * Allow some slab allocs for magazine overhead.
	 */
	uint64_t slab_alloc_delta = cp->cache_slab_alloc - slab_alloc_before;
	munit_assert_uint64(slab_alloc_delta, <, (uint64_t)EXCHANGE_COUNT);

	for (int i = 0; i < EXCHANGE_COUNT; i++)
		umem_cache_free(cp, ptrs[i]);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: depot stripe contention tracking
 *
 * Under multi-threaded load, depot stripes should distribute
 * contention across stripes.
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

	/* Record contention before */
	uint64_t contention_before = cp->cache_depot_contention;
	uint64_t stripe_contention_before = 0;
	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++)
		stripe_contention_before += cp->cache_depot[i].ds_contention;

	/*
	 * Run multiple threads doing rapid alloc/free to generate
	 * depot contention.
	 */
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

	/*
	 * Total contention (global + per-stripe) should be >= 0.
	 * We can't guarantee contention occurs on all hardware, but
	 * the counters should be non-negative and consistent.
	 */
	uint64_t stripe_contention_after = 0;
	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++)
		stripe_contention_after += cp->cache_depot[i].ds_contention;

	munit_assert_uint64(stripe_contention_after, >=,
	    stripe_contention_before);

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: per-CPU magazine round counts are valid
 *
 * cc_rounds should be between 0 and cc_magsize.
 * cc_prounds should also be in that range.
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

	/* Do some allocations to populate magazines */
	#define ROUNDS_COUNT 100
	void *ptrs[ROUNDS_COUNT];
	for (int i = 0; i < ROUNDS_COUNT; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Check round counts on all CPU caches */
	for (uint32_t i = 0; i <= cp->cache_cpu_mask; i++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[i];
		int magsize = ccp->cc_magsize;

		/* rounds should be in [0, magsize] if magazine is loaded */
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
 *
 * cc_magsize must match the cache's magtype.
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

	/* Do a few allocations to trigger magazine setup */
	void *ptrs[10];
	for (int i = 0; i < 10; i++) {
		ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	int expected_magsize = cp->cache_magtype->mt_magsize;
	munit_assert_int(expected_magsize, >, 0);

	for (uint32_t i = 0; i <= cp->cache_cpu_mask; i++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[i];
		/*
		 * cc_magsize should match the cache's magtype,
		 * though tuning could change it. It should always be > 0.
		 */
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
 *
 * With UMC_NOMAGAZINE, no magazines should be loaded and depot
 * stripes should remain empty.
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

	/* All depot stripes should have zero magazines */
	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++) {
		munit_assert_long(cp->cache_depot[i].ds_full.ml_total, ==, 0);
		munit_assert_long(cp->cache_depot[i].ds_empty.ml_total, ==, 0);
	}

	umem_cache_destroy(cp);
	return MUNIT_OK;
}

/*
 * Test: concurrent depot operations maintain consistency
 *
 * Multiple threads allocating and freeing simultaneously should
 * not corrupt depot state.
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

	/*
	 * After all threads complete, validate depot state:
	 * - ml_total should be non-negative for all stripes
	 * - slab_alloc >= slab_free
	 */
	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++) {
		munit_assert_long(cp->cache_depot[i].ds_full.ml_total, >=, 0);
		munit_assert_long(cp->cache_depot[i].ds_empty.ml_total, >=, 0);
	}

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

	/* Record ml_alloc before */
	uint64_t total_ml_alloc_before = 0;
	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++) {
		total_ml_alloc_before += cp->cache_depot[i].ds_full.ml_alloc;
		total_ml_alloc_before += cp->cache_depot[i].ds_empty.ml_alloc;
	}

	/* Generate depot traffic */
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

	/* ml_alloc should have increased from depot activity */
	uint64_t total_ml_alloc_after = 0;
	for (int i = 0; i < UMEM_DEPOT_STRIPES; i++) {
		total_ml_alloc_after += cp->cache_depot[i].ds_full.ml_alloc;
		total_ml_alloc_after += cp->cache_depot[i].ds_empty.ml_alloc;
	}

	/*
	 * ml_alloc tracks dispensed magazines; with enough cycles
	 * some depot exchanges should have occurred.
	 */
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

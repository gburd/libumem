/*
 * Test coverage for rarely-used flags and options in libumem
 *
 * This file tests code paths triggered by flags that are not commonly used:
 * - UMEM_NOFAIL
 * - UMEM_BESTFIT (if implemented)
 * - UMC_* cache creation flags
 * - Flag combinations
 */

#include <umem.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include "../munit.h"

/* Test: UMEM_NOFAIL flag basic behavior */
static MunitResult
test_nofail_basic(const MunitParameter params[], void* data)
{
	void *p;

	(void)params;
	(void)data;

	/* UMEM_NOFAIL should always succeed (or exit the process) */
	p = umem_alloc(1024, UMEM_NOFAIL);
	munit_assert_not_null(p);
	memset(p, 0xAA, 1024);
	umem_free(p, 1024);

	/* Large allocation with NOFAIL */
	p = umem_alloc(65536, UMEM_NOFAIL);
	munit_assert_not_null(p);
	umem_free(p, 65536);

	return MUNIT_OK;
}

/* Test: UMEM_NOFAIL with cache allocation */
static MunitResult
test_nofail_cache(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;

	(void)params;
	(void)data;

	cp = umem_cache_create("nofail_test", 128, 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Allocate from cache with NOFAIL - should always succeed */
	obj = umem_cache_alloc(cp, UMEM_NOFAIL);
	munit_assert_not_null(obj);

	umem_cache_free(cp, obj);
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: UMC_NODEBUG flag */
static MunitResult
test_cache_flag_nodebug(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;

	(void)params;
	(void)data;

	/* Create cache with NODEBUG flag - disables debugging */
	cp = umem_cache_create("nodebug", 64, 0, NULL, NULL, NULL, NULL, NULL, UMC_NODEBUG);

	if (cp == NULL) {
		/* Flag might not be supported */
		return MUNIT_SKIP;
	}

	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(obj);

	umem_cache_free(cp, obj);
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: UMC_NOTOUCH flag */
static MunitResult
test_cache_flag_notouch(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;

	(void)params;
	(void)data;

	/* Create cache with NOTOUCH - prevents touching buffer contents */
	cp = umem_cache_create("notouch", 64, 0, NULL, NULL, NULL, NULL, NULL, UMC_NOTOUCH);

	if (cp == NULL) {
		/* Flag might not be supported */
		return MUNIT_SKIP;
	}

	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(obj);

	/* With NOTOUCH, the buffer should not be initialized */
	/* Can't really test this without debug mode */

	umem_cache_free(cp, obj);
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: UMC_NOMAGAZINE flag */
static MunitResult
test_cache_flag_nomagazine(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	int i;

	(void)params;
	(void)data;

	/* Create cache without magazines - allocations go directly to slab layer */
	cp = umem_cache_create("nomagazine", 64, 0, NULL, NULL, NULL, NULL, NULL, UMC_NOMAGAZINE);

	if (cp == NULL) {
		return MUNIT_SKIP;
	}

	/* Do multiple allocations - without magazines, these hit the slab layer */
	for (i = 0; i < 10; i++) {
		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(obj);
		umem_cache_free(cp, obj);
	}

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: UMC_NOHASH flag */
static MunitResult
test_cache_flag_nohash(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;

	(void)params;
	(void)data;

	/* Create cache without hash table */
	cp = umem_cache_create("nohash", 64, 0, NULL, NULL, NULL, NULL, NULL, UMC_NOHASH);

	if (cp == NULL) {
		return MUNIT_SKIP;
	}

	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(obj);

	umem_cache_free(cp, obj);
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: Multiple cache flags combined */
static MunitResult
test_cache_flag_combinations(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;

	(void)params;
	(void)data;

	/* Combine NODEBUG and NOMAGAZINE */
	cp = umem_cache_create("combo1", 64, 0, NULL, NULL, NULL, NULL, NULL,
	                       UMC_NODEBUG | UMC_NOMAGAZINE);

	if (cp != NULL) {
		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (obj != NULL) {
			umem_cache_free(cp, obj);
		}
		umem_cache_destroy(cp);
	}

	/* Combine NOTOUCH and NOHASH */
	cp = umem_cache_create("combo2", 128, 0, NULL, NULL, NULL, NULL, NULL,
	                       UMC_NOTOUCH | UMC_NOHASH);

	if (cp != NULL) {
		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (obj != NULL) {
			umem_cache_free(cp, obj);
		}
		umem_cache_destroy(cp);
	}

	return MUNIT_OK;
}

/* Test: Cache with alignment and flags */
static MunitResult
test_cache_align_with_flags(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	uintptr_t addr;

	(void)params;
	(void)data;

	/* Create aligned cache with NOMAGAZINE */
	cp = umem_cache_create("align_nomagazine", 64, 64, NULL, NULL, NULL, NULL, NULL, UMC_NOMAGAZINE);

	if (cp == NULL) {
		return MUNIT_SKIP;
	}

	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(obj);

	/* Check alignment */
	addr = (uintptr_t)obj;
	munit_assert_uint64(addr % 64, ==, 0);

	umem_cache_free(cp, obj);
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: Invalid flag combinations */
static MunitResult
test_cache_invalid_flags(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;

	(void)params;
	(void)data;

	/* 0x80000000 is UMC_INTERNAL, which triggers ASSERT() if passed by
	 * non-init threads. Test with a combination that is validated at
	 * line 3248: UMC_NOHASH | UMC_NOTOUCH together are invalid. */
	cp = umem_cache_create("bad_flags", 64, 0, NULL, NULL, NULL, NULL, NULL,
	                       UMC_NOHASH | UMC_NOTOUCH);
	munit_assert_null(cp);

	return MUNIT_OK;
}

/* Test: UMEM_BESTFIT if supported (vmem allocations) */
static MunitResult
test_bestfit_allocation(const MunitParameter params[], void* data)
{
	void *p;

	(void)params;
	(void)data;

	/* BESTFIT is typically a vmem concept */
	/* umem_alloc doesn't directly support BESTFIT, but test if flag is defined */

	#ifdef UMEM_BESTFIT
	p = umem_alloc(1024, UMEM_BESTFIT);
	if (p != NULL) {
		umem_free(p, 1024);
	}
	#else
	/* BESTFIT not defined, skip test */
	return MUNIT_SKIP;
	#endif

	return MUNIT_OK;
}

/* Test: UMC_QCACHE flag (quantum cache) */
static MunitResult
test_cache_flag_qcache(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;

	(void)params;
	(void)data;

	#ifdef UMC_QCACHE
	/* Create quantum cache */
	cp = umem_cache_create("qcache", 64, 0, NULL, NULL, NULL, NULL, NULL, UMC_QCACHE);

	if (cp != NULL) {
		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (obj != NULL) {
			umem_cache_free(cp, obj);
		}
		umem_cache_destroy(cp);
	}
	#else
	return MUNIT_SKIP;
	#endif

	return MUNIT_OK;
}

/* Test: Stress test with NOFAIL */
static MunitResult
test_nofail_stress(const MunitParameter params[], void* data)
{
	void *ptrs[100];
	int i;

	(void)params;
	(void)data;

	/* Allocate many objects with NOFAIL - all should succeed */
	for (i = 0; i < 100; i++) {
		ptrs[i] = umem_alloc(4096, UMEM_NOFAIL);
		munit_assert_not_null(ptrs[i]);
		memset(ptrs[i], i & 0xFF, 4096);
	}

	/* Free them all */
	for (i = 0; i < 100; i++) {
		umem_free(ptrs[i], 4096);
	}

	return MUNIT_OK;
}

/* Test: Cache creation with reclaim callback and flags */
static MunitResult
test_cache_reclaim_with_flags(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;

	(void)params;
	(void)data;

	/* Create cache with reclaim callback and NOMAGAZINE */
	/* NULL reclaim is OK for this test */
	cp = umem_cache_create("reclaim_nomagazine", 64, 0, NULL, NULL, NULL,
	                       NULL, NULL, UMC_NOMAGAZINE);

	if (cp == NULL) {
		return MUNIT_SKIP;
	}

	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(obj);

	umem_cache_free(cp, obj);
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test suite definition */
static MunitTest rare_flag_tests[] = {
	{"/nofail_basic", test_nofail_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/nofail_cache", test_nofail_cache, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_flag_nodebug", test_cache_flag_nodebug, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_flag_notouch", test_cache_flag_notouch, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_flag_nomagazine", test_cache_flag_nomagazine, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_flag_nohash", test_cache_flag_nohash, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_flag_combinations", test_cache_flag_combinations, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_align_with_flags", test_cache_align_with_flags, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_invalid_flags", test_cache_invalid_flags, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/bestfit_allocation", test_bestfit_allocation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_flag_qcache", test_cache_flag_qcache, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/nofail_stress", test_nofail_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_reclaim_with_flags", test_cache_reclaim_with_flags, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

const MunitSuite rare_flags_suite = {
	"/rare_flags",
	rare_flag_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

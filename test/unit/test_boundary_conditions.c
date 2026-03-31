/*
 * Test coverage for boundary conditions in libumem
 *
 * This file tests edge cases and boundary conditions:
 * - Minimum and maximum sizes
 * - Power-of-2 boundaries
 * - Alignment boundaries
 * - Slab size calculations
 * - Transitions between allocation strategies
 */

#include "../../umem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../munit.h"

#define UMEM_ALIGN 8
#define UMEM_MAXBUF 131072

/* Test: Minimum allocation size (1 byte) */
static MunitResult
test_alloc_min_size(const MunitParameter params[], void* data)
{
	void *p;

	(void)params;
	(void)data;

	/* 1 byte allocation */
	p = umem_alloc(1, UMEM_DEFAULT);
	munit_assert_not_null(p);

	/* Should be writable */
	*(uint8_t*)p = 0xAA;
	munit_assert_uint8(*(uint8_t*)p, ==, 0xAA);

	umem_free(p, 1);

	return MUNIT_OK;
}

/* Test: Maximum buffer allocation */
static MunitResult
test_alloc_max_size(const MunitParameter params[], void* data)
{
	void *p;

	(void)params;
	(void)data;

	/* UMEM_MAXBUF allocation - largest slab-based allocation */
	p = umem_alloc(UMEM_MAXBUF, UMEM_DEFAULT);
	munit_assert_not_null(p);

	/* Write to first and last byte */
	((uint8_t*)p)[0] = 0xAA;
	((uint8_t*)p)[UMEM_MAXBUF-1] = 0xBB;

	munit_assert_uint8(((uint8_t*)p)[0], ==, 0xAA);
	munit_assert_uint8(((uint8_t*)p)[UMEM_MAXBUF-1], ==, 0xBB);

	umem_free(p, UMEM_MAXBUF);

	/* One byte over MAXBUF - goes to vmem */
	p = umem_alloc(UMEM_MAXBUF + 1, UMEM_DEFAULT);
	if (p != NULL) {
		((uint8_t*)p)[0] = 0xCC;
		munit_assert_uint8(((uint8_t*)p)[0], ==, 0xCC);
		umem_free(p, UMEM_MAXBUF + 1);
	}

	return MUNIT_OK;
}

/* Test: Power-of-2 size boundaries */
static MunitResult
test_alloc_power_of_2_boundaries(const MunitParameter params[], void* data)
{
	size_t sizes[] = {
		4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
	};
	size_t i;
	void *p;

	(void)params;
	(void)data;

	/* Test each power-of-2 size */
	for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
		p = umem_alloc(sizes[i], UMEM_DEFAULT);
		munit_assert_not_null(p);

		/* Write pattern */
		memset(p, 0xAA, sizes[i]);

		/* Verify */
		munit_assert_uint8(((uint8_t*)p)[0], ==, 0xAA);
		munit_assert_uint8(((uint8_t*)p)[sizes[i]-1], ==, 0xAA);

		umem_free(p, sizes[i]);
	}

	return MUNIT_OK;
}

/* Test: Sizes just below and above power-of-2 */
static MunitResult
test_alloc_near_power_of_2(const MunitParameter params[], void* data)
{
	size_t bases[] = {64, 128, 256, 512, 1024, 2048, 4096};
	size_t i;
	void *p1, *p2, *p3;

	(void)params;
	(void)data;

	for (i = 0; i < sizeof(bases)/sizeof(bases[0]); i++) {
		/* One below */
		p1 = umem_alloc(bases[i] - 1, UMEM_DEFAULT);
		munit_assert_not_null(p1);

		/* Exact */
		p2 = umem_alloc(bases[i], UMEM_DEFAULT);
		munit_assert_not_null(p2);

		/* One above */
		p3 = umem_alloc(bases[i] + 1, UMEM_DEFAULT);
		munit_assert_not_null(p3);

		umem_free(p1, bases[i] - 1);
		umem_free(p2, bases[i]);
		umem_free(p3, bases[i] + 1);
	}

	return MUNIT_OK;
}

/* Test: Cache size boundaries */
static MunitResult
test_cache_size_boundaries(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	size_t sizes[] = {1, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128};
	size_t i;

	(void)params;
	(void)data;

	for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
		char name[32];
		snprintf(name, sizeof(name), "cache_%zu", sizes[i]);

		cp = umem_cache_create(name, sizes[i], 0, NULL, NULL, NULL, NULL, NULL, 0);
		munit_assert_not_null(cp);

		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(obj);

		/* Fill buffer */
		memset(obj, 0xBB, sizes[i]);

		umem_cache_free(cp, obj);
		umem_cache_destroy(cp);
	}

	return MUNIT_OK;
}

/* Test: Alignment equal to size */
static MunitResult
test_align_equals_size(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	uintptr_t addr;
	size_t sizes[] = {16, 32, 64, 128, 256};
	size_t i;

	(void)params;
	(void)data;

	for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
		char name[32];
		snprintf(name, sizeof(name), "align_eq_%zu", sizes[i]);

		/* Alignment == size */
		cp = umem_cache_create(name, sizes[i], sizes[i], NULL, NULL, NULL, NULL, NULL, 0);
		munit_assert_not_null(cp);

		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(obj);

		/* Verify alignment */
		addr = (uintptr_t)obj;
		munit_assert_uint64(addr % sizes[i], ==, 0);

		umem_cache_free(cp, obj);
		umem_cache_destroy(cp);
	}

	return MUNIT_OK;
}

/* Test: Alignment greater than size */
static MunitResult
test_align_greater_than_size(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	uintptr_t addr;

	(void)params;
	(void)data;

	/* Size 32, alignment 64 */
	cp = umem_cache_create("align_gt_size", 32, 64, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(obj);

	/* Should be 64-byte aligned */
	addr = (uintptr_t)obj;
	munit_assert_uint64(addr % 64, ==, 0);

	umem_cache_free(cp, obj);
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: Maximum reasonable alignment */
static MunitResult
test_align_maximum(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	uintptr_t addr;

	(void)params;
	(void)data;

	/* 4K alignment (page-aligned) */
	cp = umem_cache_create("align_4k", 64, 4096, NULL, NULL, NULL, NULL, NULL, 0);

	if (cp == NULL) {
		/* Might not be supported */
		return MUNIT_SKIP;
	}

	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	if (obj != NULL) {
		addr = (uintptr_t)obj;
		munit_assert_uint64(addr % 4096, ==, 0);
		umem_cache_free(cp, obj);
	}

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: Odd-sized allocations with alignment */
static MunitResult
test_align_with_odd_sizes(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	uintptr_t addr;
	size_t odd_sizes[] = {17, 33, 65, 129, 257};
	size_t i;

	(void)params;
	(void)data;

	for (i = 0; i < sizeof(odd_sizes)/sizeof(odd_sizes[0]); i++) {
		char name[32];
		snprintf(name, sizeof(name), "odd_%zu", odd_sizes[i]);

		/* Odd size with power-of-2 alignment */
		cp = umem_cache_create(name, odd_sizes[i], 32, NULL, NULL, NULL, NULL, NULL, 0);
		munit_assert_not_null(cp);

		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(obj);

		/* Check alignment */
		addr = (uintptr_t)obj;
		munit_assert_uint64(addr % 32, ==, 0);

		/* Fill entire buffer */
		memset(obj, 0xDD, odd_sizes[i]);

		umem_cache_free(cp, obj);
		umem_cache_destroy(cp);
	}

	return MUNIT_OK;
}

/* Test: Slab calculation boundaries */
static MunitResult
test_slab_size_boundaries(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *objs[10];
	int i;

	(void)params;
	(void)data;

	/* Test size that results in specific slab configurations */
	/* Small objects - many per slab */
	cp = umem_cache_create("small_slab", 16, 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Allocate multiple to potentially trigger slab allocation */
	for (i = 0; i < 10; i++) {
		objs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(objs[i]);
	}

	for (i = 0; i < 10; i++) {
		umem_cache_free(cp, objs[i]);
	}

	umem_cache_destroy(cp);

	/* Large objects - few per slab */
	cp = umem_cache_create("large_slab", 8192, 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	for (i = 0; i < 10; i++) {
		objs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(objs[i]);
	}

	for (i = 0; i < 10; i++) {
		umem_cache_free(cp, objs[i]);
	}

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/* Test: Transition from slab to vmem allocations */
static MunitResult
test_slab_to_vmem_transition(const MunitParameter params[], void* data)
{
	void *p1, *p2;

	(void)params;
	(void)data;

	/* Just below MAXBUF - slab allocation */
	p1 = umem_alloc(UMEM_MAXBUF, UMEM_DEFAULT);
	munit_assert_not_null(p1);

	/* Just above MAXBUF - vmem allocation */
	p2 = umem_alloc(UMEM_MAXBUF + 1, UMEM_DEFAULT);
	if (p2 != NULL) {
		/* Both should work */
		memset(p1, 0xAA, UMEM_MAXBUF);
		memset(p2, 0xBB, UMEM_MAXBUF + 1);

		umem_free(p2, UMEM_MAXBUF + 1);
	}

	umem_free(p1, UMEM_MAXBUF);

	return MUNIT_OK;
}

/* Test: Zero-size allocation behavior */
static MunitResult
test_zero_size_alloc(const MunitParameter params[], void* data)
{
	void *p;

	(void)params;
	(void)data;

	/* Zero-size allocation */
	p = umem_alloc(0, UMEM_DEFAULT);

	/* Behavior varies - might return NULL or a valid pointer */
	if (p != NULL) {
		/* If non-NULL, should be safe to free */
		umem_free(p, 0);
	}

	return MUNIT_OK;
}

/* Test: Cache with size at multiples of cache line size */
static MunitResult
test_cache_line_size_multiples(const MunitParameter params[], void* data)
{
	umem_cache_t *cp;
	void *obj;
	uintptr_t addr;
	size_t cacheline = 64; /* typical cache line size */
	size_t multiples[] = {64, 128, 192, 256, 320, 384, 448, 512};
	size_t i;

	(void)params;
	(void)data;

	for (i = 0; i < sizeof(multiples)/sizeof(multiples[0]); i++) {
		char name[32];
		snprintf(name, sizeof(name), "cl_%zu", multiples[i]);

		cp = umem_cache_create(name, multiples[i], cacheline, NULL, NULL, NULL, NULL, NULL, 0);
		munit_assert_not_null(cp);

		obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(obj);

		/* Should be cache-line aligned */
		addr = (uintptr_t)obj;
		munit_assert_uint64(addr % cacheline, ==, 0);

		umem_cache_free(cp, obj);
		umem_cache_destroy(cp);
	}

	return MUNIT_OK;
}

/* Test suite definition */
static MunitTest boundary_tests[] = {
	{"/alloc_min_size", test_alloc_min_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/alloc_max_size", test_alloc_max_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/alloc_power_of_2_boundaries", test_alloc_power_of_2_boundaries, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/alloc_near_power_of_2", test_alloc_near_power_of_2, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_size_boundaries", test_cache_size_boundaries, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/align_equals_size", test_align_equals_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/align_greater_than_size", test_align_greater_than_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/align_maximum", test_align_maximum, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/align_with_odd_sizes", test_align_with_odd_sizes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/slab_size_boundaries", test_slab_size_boundaries, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/slab_to_vmem_transition", test_slab_to_vmem_transition, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/zero_size_alloc", test_zero_size_alloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/cache_line_size_multiples", test_cache_line_size_multiples, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

const MunitSuite boundary_conditions_suite = {
	"/boundary_conditions",
	boundary_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

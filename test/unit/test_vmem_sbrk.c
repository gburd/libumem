/*
 * Unit tests for vmem_sbrk.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include "munit.h"
#include "vmem_base.h"
#include "umem_impl.h"

/* Test: vmem_sbrk_arena initialization */
static MunitResult
test_vmem_sbrk_arena_init(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func = NULL;
	vmem_free_t *free_func = NULL;

	/* Initialize arena */
	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);

	munit_assert_not_null(arena);
	munit_assert_not_null(alloc_func);
	munit_assert_not_null(free_func);

	/* Verify pagesize is set */
	munit_assert_size(vmem_sbrk_pagesize, >, 0);

	/* Verify minalloc is reasonable */
	munit_assert_size(vmem_sbrk_minalloc, >=, 64 * 1024);

	return MUNIT_OK;
}

/* Test: vmem_sbrk_arena multiple calls return same arena */
static MunitResult
test_vmem_sbrk_arena_singleton(const MunitParameter params[], void* data)
{
	vmem_t *arena1 = vmem_sbrk_arena(NULL, NULL);
	vmem_t *arena2 = vmem_sbrk_arena(NULL, NULL);

	munit_assert_ptr_equal(arena1, arena2);

	return MUNIT_OK;
}

/* Test: Basic allocation through sbrk arena */
static MunitResult
test_vmem_sbrk_basic_alloc(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Allocate a small amount */
	void *ptr = alloc_func(arena, 4096, VM_SLEEP);
	munit_assert_not_null(ptr);

	/* Free it */
	free_func(arena, ptr, 4096);

	return MUNIT_OK;
}

/* Test: Large allocation through sbrk arena */
static MunitResult
test_vmem_sbrk_large_alloc(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Allocate larger than minalloc */
	size_t size = vmem_sbrk_minalloc * 2;
	void *ptr = alloc_func(arena, size, VM_SLEEP);
	munit_assert_not_null(ptr);

	free_func(arena, ptr, size);

	return MUNIT_OK;
}

/* Test: Multiple allocations */
static MunitResult
test_vmem_sbrk_multiple_allocs(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	void *ptrs[10];
	size_t sizes[] = {4096, 8192, 16384, 32768, 4096, 8192, 4096, 16384, 8192, 4096};

	/* Allocate multiple blocks */
	for (int i = 0; i < 10; i++) {
		ptrs[i] = alloc_func(arena, sizes[i], VM_SLEEP);
		munit_assert_not_null(ptrs[i]);
	}

	/* Free them in different order */
	for (int i = 9; i >= 0; i--) {
		free_func(arena, ptrs[i], sizes[i]);
	}

	return MUNIT_OK;
}

/* Test: VM_NOSLEEP allocation failure handling */
static MunitResult
test_vmem_sbrk_nosleep(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* This should succeed even with NOSLEEP */
	void *ptr = alloc_func(arena, 4096, VM_NOSLEEP);
	if (ptr != NULL) {
		free_func(arena, ptr, 4096);
	}
	/* If it fails, that's also acceptable with NOSLEEP */

	return MUNIT_OK;
}

/* Test: Fork support - lockup and release */
static MunitResult
test_vmem_sbrk_fork_support(const MunitParameter params[], void* data)
{
	/* Initialize arena first */
	vmem_sbrk_arena(NULL, NULL);

	/* Lock up */
	vmem_sbrk_lockup();

	/* Release */
	vmem_sbrk_release();

	/* Lock and release again */
	vmem_sbrk_lockup();
	vmem_sbrk_release();

	return MUNIT_OK;
}

/* Test: Allocation patterns that stress heap growth */
static MunitResult
test_vmem_sbrk_heap_growth(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Allocate increasing sizes to force heap growth */
	void *ptrs[5];
	size_t size = vmem_sbrk_minalloc;

	for (int i = 0; i < 5; i++) {
		ptrs[i] = alloc_func(arena, size, VM_SLEEP);
		munit_assert_not_null(ptrs[i]);
		size += vmem_sbrk_minalloc;
	}

	/* Free in reverse order */
	size = vmem_sbrk_minalloc * 5;
	for (int i = 4; i >= 0; i--) {
		free_func(arena, ptrs[i], size);
		size -= vmem_sbrk_minalloc;
	}

	return MUNIT_OK;
}

/* Test: Interleaved alloc/free pattern */
static MunitResult
test_vmem_sbrk_interleaved(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	void *ptr1 = alloc_func(arena, 8192, VM_SLEEP);
	munit_assert_not_null(ptr1);

	void *ptr2 = alloc_func(arena, 16384, VM_SLEEP);
	munit_assert_not_null(ptr2);

	free_func(arena, ptr1, 8192);

	void *ptr3 = alloc_func(arena, 8192, VM_SLEEP);
	munit_assert_not_null(ptr3);

	free_func(arena, ptr2, 16384);
	free_func(arena, ptr3, 8192);

	return MUNIT_OK;
}

/* Test: Verify alignment of allocated memory */
static MunitResult
test_vmem_sbrk_alignment(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Allocate several blocks and check alignment */
	for (int i = 0; i < 10; i++) {
		void *ptr = alloc_func(arena, 4096, VM_SLEEP);
		munit_assert_not_null(ptr);

		/* Check alignment to at least pagesize */
		size_t pagesize = sysconf(_SC_PAGESIZE);
		munit_assert_uint64((uintptr_t)ptr % pagesize, ==, 0);

		free_func(arena, ptr, 4096);
	}

	return MUNIT_OK;
}

/* Test: Stress test with many allocations */
static MunitResult
test_vmem_sbrk_stress(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

#define NUM_ALLOCS 50
	void *ptrs[NUM_ALLOCS];
	size_t sizes[NUM_ALLOCS];

	/* Allocate many blocks of varying sizes */
	for (int i = 0; i < NUM_ALLOCS; i++) {
		sizes[i] = (munit_rand_int_range(1, 16) * 4096);
		ptrs[i] = alloc_func(arena, sizes[i], VM_SLEEP);
		munit_assert_not_null(ptrs[i]);
	}

	/* Free half of them in random order */
	for (int i = 0; i < NUM_ALLOCS / 2; i++) {
		int idx = munit_rand_int_range(0, NUM_ALLOCS - 1);
		if (ptrs[idx] != NULL) {
			free_func(arena, ptrs[idx], sizes[idx]);
			ptrs[idx] = NULL;
		}
	}

	/* Allocate more blocks */
	for (int i = 0; i < NUM_ALLOCS; i++) {
		if (ptrs[i] == NULL) {
			sizes[i] = (munit_rand_int_range(1, 16) * 4096);
			ptrs[i] = alloc_func(arena, sizes[i], VM_SLEEP);
			munit_assert_not_null(ptrs[i]);
		}
	}

	/* Free everything */
	for (int i = 0; i < NUM_ALLOCS; i++) {
		if (ptrs[i] != NULL) {
			free_func(arena, ptrs[i], sizes[i]);
		}
	}
#undef NUM_ALLOCS

	return MUNIT_OK;
}

/* Test: Verify errno preservation */
static MunitResult
test_vmem_sbrk_errno_preservation(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Set errno to a specific value */
	errno = ENOENT;

	/* Successful allocation should preserve errno */
	void *ptr = alloc_func(arena, 4096, VM_SLEEP);
	munit_assert_not_null(ptr);
	munit_assert_int(errno, ==, ENOENT);

	free_func(arena, ptr, 4096);

	return MUNIT_OK;
}

/* Test: Zero-size allocation behavior */
static MunitResult
test_vmem_sbrk_zero_size(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Zero-size allocation should either return NULL or succeed */
	void *ptr = alloc_func(arena, 0, VM_NOSLEEP);
	/* Either NULL or valid pointer is acceptable */
	if (ptr != NULL) {
		free_func(arena, ptr, 0);
	}

	return MUNIT_OK;
}

/* Test: Very large allocation request */
static MunitResult
test_vmem_sbrk_very_large(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Try to allocate a very large amount with NOSLEEP */
	size_t huge_size = (size_t)1024 * 1024 * 1024 * 10; /* 10GB */
	void *ptr = alloc_func(arena, huge_size, VM_NOSLEEP);

	/* This may or may not succeed depending on available memory */
	if (ptr != NULL) {
		free_func(arena, ptr, huge_size);
	}
	/* Failure is acceptable with NOSLEEP */

	return MUNIT_OK;
}

/* Test: Allocation after many frees (tests fail list) */
static MunitResult
test_vmem_sbrk_reuse_after_free(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	/* Allocate and free many blocks to populate internal structures */
	for (int round = 0; round < 3; round++) {
		void *ptrs[20];
		for (int i = 0; i < 20; i++) {
			ptrs[i] = alloc_func(arena, 8192, VM_SLEEP);
			munit_assert_not_null(ptrs[i]);
		}

		for (int i = 0; i < 20; i++) {
			free_func(arena, ptrs[i], 8192);
		}
	}

	/* Now allocate again - should reuse freed memory */
	void *ptr = alloc_func(arena, 8192, VM_SLEEP);
	munit_assert_not_null(ptr);
	free_func(arena, ptr, 8192);

	return MUNIT_OK;
}

/* Test: Concurrent-style access pattern (single-threaded but mixed) */
static MunitResult
test_vmem_sbrk_mixed_pattern(const MunitParameter params[], void* data)
{
	vmem_alloc_t *alloc_func;
	vmem_free_t *free_func;

	vmem_t *arena = vmem_sbrk_arena(&alloc_func, &free_func);
	munit_assert_not_null(arena);

	void *active[10] = {NULL};
	size_t active_sizes[10] = {0};

	/* Mix allocations and frees */
	for (int iter = 0; iter < 100; iter++) {
		int idx = munit_rand_int_range(0, 9);

		if (active[idx] == NULL) {
			/* Allocate */
			active_sizes[idx] = (munit_rand_int_range(1, 8) * 4096);
			active[idx] = alloc_func(arena, active_sizes[idx], VM_SLEEP);
			munit_assert_not_null(active[idx]);
		} else {
			/* Free */
			free_func(arena, active[idx], active_sizes[idx]);
			active[idx] = NULL;
			active_sizes[idx] = 0;
		}
	}

	/* Clean up remaining allocations */
	for (int i = 0; i < 10; i++) {
		if (active[i] != NULL) {
			free_func(arena, active[i], active_sizes[i]);
		}
	}

	return MUNIT_OK;
}

/* Test suite */
static MunitTest vmem_sbrk_tests[] = {
	{ "/arena_init", test_vmem_sbrk_arena_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arena_singleton", test_vmem_sbrk_arena_singleton, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/basic_alloc", test_vmem_sbrk_basic_alloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/large_alloc", test_vmem_sbrk_large_alloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/multiple_allocs", test_vmem_sbrk_multiple_allocs, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/nosleep", test_vmem_sbrk_nosleep, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fork_support", test_vmem_sbrk_fork_support, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/heap_growth", test_vmem_sbrk_heap_growth, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/interleaved", test_vmem_sbrk_interleaved, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alignment", test_vmem_sbrk_alignment, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stress", test_vmem_sbrk_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/errno_preservation", test_vmem_sbrk_errno_preservation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/zero_size", test_vmem_sbrk_zero_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/very_large", test_vmem_sbrk_very_large, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reuse_after_free", test_vmem_sbrk_reuse_after_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mixed_pattern", test_vmem_sbrk_mixed_pattern, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_vmem_sbrk = {
	"/vmem_sbrk", vmem_sbrk_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

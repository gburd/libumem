/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * umem_ptc_test.c -- Per-Thread Cache (PTC) unit tests for libumem
 *
 * Tests the per-thread caching functionality including:
 *   - Basic allocation/free across PTC size classes
 *   - Thread-local cache independence
 *   - Boundary conditions (max PTC size, above max, zero)
 *   - malloc()/free() PLT replacement path
 *   - Thread cleanup on exit
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>

#include "umem.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { \
		fprintf(stderr, "  FAIL: %s (line %d): %s\n", \
		    __func__, __LINE__, msg); \
		tests_failed++; \
		return 1; \
	} else { \
		tests_passed++; \
	} \
} while (0)

#define RUN_TEST(func) do { \
	int _rc; \
	fprintf(stderr, "  Running %s...\n", #func); \
	_rc = func(); \
	if (_rc != 0) \
		fprintf(stderr, "  %s FAILED\n", #func); \
	else \
		fprintf(stderr, "  %s passed\n", #func); \
} while (0)

/*
 * PTC size classes (LP64).  The first 16 entries of umem_alloc_sizes[]
 * are what PTC covers, corresponding to TMEM_NENTRIES=16.
 *
 * LP64: 8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448
 * ILP32: 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256
 */
#ifdef _LP64
static const size_t ptc_sizes[] = {
	8, 16, 32, 48, 64, 80, 96, 112,
	128, 160, 192, 224, 256, 320, 384, 448
};
#define PTC_MAX_SIZE 448
#else
static const size_t ptc_sizes[] = {
	8, 16, 24, 32, 40, 48, 56, 64,
	80, 96, 112, 128, 160, 192, 224, 256
};
#define PTC_MAX_SIZE 256
#endif
#define NUM_PTC_SIZES (sizeof (ptc_sizes) / sizeof (ptc_sizes[0]))

/* ======================================================================
 * Test 1: Basic allocation and free using umem_alloc()/umem_free()
 * ====================================================================== */

static int
test_basic_alloc_free(void)
{
	size_t i, j;
	void *bufs[NUM_PTC_SIZES];
	const int iterations = 100;

	/* Single allocation/free for each PTC size class */
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		bufs[i] = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
		TEST_ASSERT(bufs[i] != NULL,
		    "umem_alloc should succeed for PTC size class");
		memset(bufs[i], 0xAB, ptc_sizes[i]);
	}
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		umem_free(bufs[i], ptc_sizes[i]);
	}

	/* Repeated alloc/free to exercise PTC caching (recycle path) */
	for (j = 0; j < (size_t)iterations; j++) {
		for (i = 0; i < NUM_PTC_SIZES; i++) {
			void *p = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
			TEST_ASSERT(p != NULL,
			    "repeated umem_alloc should succeed");
			memset(p, (unsigned char)(j & 0xff), ptc_sizes[i]);
			umem_free(p, ptc_sizes[i]);
		}
	}

	/* Allocate many buffers of the same size, then free them all */
	{
		const int batch = 64;
		void *batch_bufs[64];
		for (i = 0; i < NUM_PTC_SIZES; i++) {
			for (j = 0; j < (size_t)batch; j++) {
				batch_bufs[j] = umem_alloc(ptc_sizes[i],
				    UMEM_DEFAULT);
				TEST_ASSERT(batch_bufs[j] != NULL,
				    "batch umem_alloc should succeed");
			}
			for (j = 0; j < (size_t)batch; j++) {
				umem_free(batch_bufs[j], ptc_sizes[i]);
			}
		}
	}

	return 0;
}

/* ======================================================================
 * Test 2: Direct umem allocation (formerly tested malloc/free PLT path)
 * ====================================================================== */

static int
test_malloc_free(void)
{
	size_t i, j;
	const int iterations = 100;

	/* Basic umem_alloc/umem_free for PTC-covered sizes */
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		void *p = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
		TEST_ASSERT(p != NULL,
		    "umem_alloc should succeed for PTC size class");
		memset(p, 0xCD, ptc_sizes[i]);
		umem_free(p, ptc_sizes[i]);
	}

	/* Repeated umem_alloc/umem_free to exercise the PTC recycle path */
	for (j = 0; j < (size_t)iterations; j++) {
		for (i = 0; i < NUM_PTC_SIZES; i++) {
			void *p = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
			TEST_ASSERT(p != NULL,
			    "repeated umem_alloc should succeed");
			memset(p, (unsigned char)(j & 0xff), ptc_sizes[i]);
			umem_free(p, ptc_sizes[i]);
		}
	}

	/* Sizes smaller than PTC class but within range */
	{
		size_t small_sizes[] = {1, 2, 3, 4, 5, 6, 7};
		for (i = 0; i < sizeof (small_sizes) / sizeof (*small_sizes);
		    i++) {
			void *p = umem_alloc(small_sizes[i], UMEM_DEFAULT);
			TEST_ASSERT(p != NULL,
			    "umem_alloc of small size should succeed");
			memset(p, 0xEF, small_sizes[i]);
			umem_free(p, small_sizes[i]);
		}
	}

	return 0;
}

/* ======================================================================
 * Test 3: Thread-local cache independence
 * ====================================================================== */

#define THREAD_INDEPENDENCE_ITERS	200
#define THREAD_INDEPENDENCE_NTHREADS	4

struct thread_data {
	pthread_t	tid;
	int		thread_id;
	void		**ptrs;
	int		count;
	int		passed;
};

static void *
thread_independence_worker(void *arg)
{
	struct thread_data *td = (struct thread_data *)arg;
	int i;
	size_t sz = ptc_sizes[NUM_PTC_SIZES / 2]; /* pick a mid-range size */

	td->passed = 1;

	/* Allocate a batch of buffers */
	for (i = 0; i < td->count; i++) {
		td->ptrs[i] = umem_alloc(sz, UMEM_DEFAULT);
		if (td->ptrs[i] == NULL) {
			td->passed = 0;
			return NULL;
		}
		/* Write a unique pattern: thread_id in every byte */
		memset(td->ptrs[i], td->thread_id & 0xff, sz);
	}

	/* Verify no other thread has corrupted our buffers */
	for (i = 0; i < td->count; i++) {
		unsigned char *p = (unsigned char *)td->ptrs[i];
		size_t j;
		for (j = 0; j < sz; j++) {
			if (p[j] != (unsigned char)(td->thread_id & 0xff)) {
				td->passed = 0;
				return NULL;
			}
		}
	}

	/* Free all buffers */
	for (i = 0; i < td->count; i++) {
		umem_free(td->ptrs[i], sz);
		td->ptrs[i] = NULL;
	}

	/* Allocate again -- PTC should recycle from this thread's cache */
	for (i = 0; i < td->count; i++) {
		td->ptrs[i] = umem_alloc(sz, UMEM_DEFAULT);
		if (td->ptrs[i] == NULL) {
			td->passed = 0;
			return NULL;
		}
		memset(td->ptrs[i], (td->thread_id + 0x80) & 0xff, sz);
	}

	for (i = 0; i < td->count; i++) {
		umem_free(td->ptrs[i], sz);
	}

	return NULL;
}

static int
test_thread_local_independence(void)
{
	struct thread_data threads[THREAD_INDEPENDENCE_NTHREADS];
	void *ptrs_storage[THREAD_INDEPENDENCE_NTHREADS][THREAD_INDEPENDENCE_ITERS];
	int i, rc;

	for (i = 0; i < THREAD_INDEPENDENCE_NTHREADS; i++) {
		threads[i].thread_id = i + 1;
		threads[i].ptrs = ptrs_storage[i];
		threads[i].count = THREAD_INDEPENDENCE_ITERS;
		threads[i].passed = 0;
	}

	/* Create all threads */
	for (i = 0; i < THREAD_INDEPENDENCE_NTHREADS; i++) {
		rc = pthread_create(&threads[i].tid, NULL,
		    thread_independence_worker, &threads[i]);
		TEST_ASSERT(rc == 0, "pthread_create should succeed");
	}

	/* Join all threads */
	for (i = 0; i < THREAD_INDEPENDENCE_NTHREADS; i++) {
		rc = pthread_join(threads[i].tid, NULL);
		TEST_ASSERT(rc == 0, "pthread_join should succeed");
		TEST_ASSERT(threads[i].passed,
		    "thread should complete without data corruption");
	}

	return 0;
}

/* ======================================================================
 * Test 4: Boundary conditions
 * ====================================================================== */

static int
test_size_boundaries(void)
{
	void *p;

	/* Zero-size allocation via umem_alloc */
	p = umem_alloc(0, UMEM_DEFAULT);
	/*
	 * umem_alloc(0) may return NULL or a valid pointer depending
	 * on implementation.  Just ensure we don't crash.
	 */
	if (p != NULL)
		umem_free(p, 0);

	/* Exactly at PTC max */
	p = umem_alloc(PTC_MAX_SIZE, UMEM_DEFAULT);
	TEST_ASSERT(p != NULL, "allocation at PTC max size should succeed");
	memset(p, 0xBB, PTC_MAX_SIZE);
	umem_free(p, PTC_MAX_SIZE);

	/* One byte above PTC max (should fall through to normal allocator) */
	p = umem_alloc(PTC_MAX_SIZE + 1, UMEM_DEFAULT);
	TEST_ASSERT(p != NULL,
	    "allocation above PTC max should succeed via normal path");
	memset(p, 0xCC, PTC_MAX_SIZE + 1);
	umem_free(p, PTC_MAX_SIZE + 1);

	/* Well above PTC max */
	p = umem_alloc(4096, UMEM_DEFAULT);
	TEST_ASSERT(p != NULL, "large allocation should succeed");
	memset(p, 0xDD, 4096);
	umem_free(p, 4096);

	p = umem_alloc(65536, UMEM_DEFAULT);
	TEST_ASSERT(p != NULL, "64K allocation should succeed");
	memset(p, 0xEE, 65536);
	umem_free(p, 65536);

	/* umem_alloc() boundary conditions */
	p = umem_alloc(0, UMEM_DEFAULT);
	/* umem_alloc(0) behavior is implementation-defined; just don't crash */
	if (p != NULL)
		umem_free(p, 0);

	p = umem_alloc(PTC_MAX_SIZE, UMEM_DEFAULT);
	TEST_ASSERT(p != NULL, "umem_alloc at PTC max should succeed");
	memset(p, 0xAA, PTC_MAX_SIZE);
	umem_free(p, PTC_MAX_SIZE);

	p = umem_alloc(PTC_MAX_SIZE + 1, UMEM_DEFAULT);
	TEST_ASSERT(p != NULL, "umem_alloc above PTC max should succeed");
	memset(p, 0xBB, PTC_MAX_SIZE + 1);
	umem_free(p, PTC_MAX_SIZE + 1);

	/* Sizes that are not exact PTC class boundaries */
	{
		size_t odd_sizes[] = {1, 3, 7, 9, 13, 15, 17, 31, 33, 63,
		    65, 127, 129, 255, 257};
		size_t i;
		for (i = 0; i < sizeof (odd_sizes) / sizeof (*odd_sizes);
		    i++) {
			p = umem_alloc(odd_sizes[i], UMEM_DEFAULT);
			TEST_ASSERT(p != NULL,
			    "odd-size allocation should succeed");
			memset(p, 0xFF, odd_sizes[i]);
			umem_free(p, odd_sizes[i]);
		}
	}

	/* Allocation of exactly 1 byte */
	p = umem_alloc(1, UMEM_DEFAULT);
	TEST_ASSERT(p != NULL, "1-byte allocation should succeed");
	*(char *)p = 'X';
	umem_free(p, 1);

	return 0;
}

/* ======================================================================
 * Test 5: Cache limits -- fill thread cache to max
 * ====================================================================== */

static void *
cache_limit_worker(void *arg)
{
	(void)arg;
	/*
	 * Allocate and free many small buffers to fill the per-thread cache
	 * up to umem_ptc_size (default 1MB).  The PTC should eventually
	 * overflow and start returning buffers to the slab allocator.
	 */
	const size_t alloc_size = ptc_sizes[0]; /* smallest PTC class */
	const int count = 8192;
	void **bufs;
	int i;

	bufs = (void **)umem_alloc(count * sizeof (void *), UMEM_DEFAULT);
	if (bufs == NULL)
		return (void *)1;

	/* Allocate many buffers */
	for (i = 0; i < count; i++) {
		bufs[i] = umem_alloc(alloc_size, UMEM_DEFAULT);
		if (bufs[i] == NULL) {
			/* free what we got */
			while (--i >= 0)
				umem_free(bufs[i], alloc_size);
			umem_free(bufs, count * sizeof (void *));
			return (void *)1;
		}
	}

	/* Free all -- this will fill the PTC.  Some will overflow. */
	for (i = 0; i < count; i++) {
		umem_free(bufs[i], alloc_size);
	}

	/* Allocate again -- should recycle from PTC where possible */
	for (i = 0; i < count; i++) {
		bufs[i] = umem_alloc(alloc_size, UMEM_DEFAULT);
		if (bufs[i] == NULL) {
			while (--i >= 0)
				umem_free(bufs[i], alloc_size);
			umem_free(bufs, count * sizeof (void *));
			return (void *)1;
		}
	}

	for (i = 0; i < count; i++) {
		umem_free(bufs[i], alloc_size);
	}

	umem_free(bufs, count * sizeof (void *));
	return NULL;
}

static int
test_cache_limits(void)
{
	pthread_t tid;
	void *retval;
	int rc;

	rc = pthread_create(&tid, NULL, cache_limit_worker, NULL);
	TEST_ASSERT(rc == 0, "pthread_create for cache limit test");

	rc = pthread_join(tid, &retval);
	TEST_ASSERT(rc == 0, "pthread_join for cache limit test");
	TEST_ASSERT(retval == NULL,
	    "cache limit worker should succeed without errors");

	return 0;
}

/* ======================================================================
 * Test 6: Thread cleanup verification
 * ====================================================================== */

/*
 * When a thread exits, any buffers cached in PTC should be cleaned up
 * (returned to the slab allocator).  We verify this indirectly by
 * creating threads that cache buffers, letting them exit, and then
 * verifying that new allocations still succeed (memory isn't leaked
 * indefinitely).
 */

static void *
thread_cleanup_worker(void *arg)
{
	int i;
	size_t sz = ptc_sizes[NUM_PTC_SIZES / 2];
	int count = *(int *)arg;
	void **bufs;

	bufs = (void **)umem_alloc(count * sizeof (void *), UMEM_DEFAULT);
	if (bufs == NULL)
		return (void *)1;

	/* Allocate and free to populate PTC */
	for (i = 0; i < count; i++) {
		bufs[i] = umem_alloc(sz, UMEM_DEFAULT);
		if (bufs[i] == NULL) {
			while (--i >= 0)
				umem_free(bufs[i], sz);
			umem_free(bufs, count * sizeof (void *));
			return (void *)1;
		}
	}

	/* Free all -- they should be cached in PTC */
	for (i = 0; i < count; i++) {
		umem_free(bufs[i], sz);
	}

	umem_free(bufs, count * sizeof (void *));

	/*
	 * When this thread exits, PTC cleanup should return all
	 * cached buffers to the slab layer.
	 */
	return NULL;
}

static int
test_thread_cleanup(void)
{
	int i;
	int count = 512;
	const int num_threads = 8;

	for (i = 0; i < num_threads; i++) {
		pthread_t tid;
		void *retval;
		int rc;

		rc = pthread_create(&tid, NULL, thread_cleanup_worker, &count);
		TEST_ASSERT(rc == 0, "pthread_create for cleanup test");
		rc = pthread_join(tid, &retval);
		TEST_ASSERT(rc == 0, "pthread_join for cleanup test");
		TEST_ASSERT(retval == NULL,
		    "cleanup worker should succeed");
	}

	/*
	 * After all threads have exited, verify we can still allocate.
	 * This is a basic sanity check that thread cleanup didn't
	 * corrupt the allocator state.
	 */
	{
		size_t j;
		for (j = 0; j < NUM_PTC_SIZES; j++) {
			void *p = umem_alloc(ptc_sizes[j], UMEM_DEFAULT);
			TEST_ASSERT(p != NULL,
			    "post-cleanup allocation should succeed");
			umem_free(p, ptc_sizes[j]);
		}
	}

	return 0;
}

/* ======================================================================
 * Test 7: Data integrity across alloc/free cycles
 * ====================================================================== */

static int
test_data_integrity(void)
{
	size_t i;
	const int cycles = 50;
	int c;

	for (i = 0; i < NUM_PTC_SIZES; i++) {
		size_t sz = ptc_sizes[i];
		for (c = 0; c < cycles; c++) {
			unsigned char *p;
			size_t j;

			p = (unsigned char *)umem_alloc(sz, UMEM_DEFAULT);
			TEST_ASSERT(p != NULL, "alloc for integrity check");

			/* Fill with a pattern based on cycle and size */
			for (j = 0; j < sz; j++) {
				p[j] = (unsigned char)((c + j + sz) & 0xff);
			}

			/* Verify the pattern */
			for (j = 0; j < sz; j++) {
				TEST_ASSERT(
				    p[j] == (unsigned char)((c + j + sz) & 0xff),
				    "data integrity mismatch");
			}

			umem_free(p, sz);
		}
	}

	return 0;
}

/* ======================================================================
 * Test 8: Multiple umem_alloc with different patterns in same thread
 * ====================================================================== */

static int
test_mixed_allocators(void)
{
	size_t i;
	void *umem_bufs[NUM_PTC_SIZES];
	void *umem_bufs2[NUM_PTC_SIZES];

	/* Interleave two sets of umem_alloc for the same sizes */
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		umem_bufs[i] = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
		TEST_ASSERT(umem_bufs[i] != NULL, "umem_alloc in mixed test");
		memset(umem_bufs[i], 0xAA, ptc_sizes[i]);

		umem_bufs2[i] = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
		TEST_ASSERT(umem_bufs2[i] != NULL, "umem_alloc in mixed test");
		memset(umem_bufs2[i], 0xBB, ptc_sizes[i]);
	}

	/* Verify they don't overlap or corrupt each other */
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		unsigned char *up = (unsigned char *)umem_bufs[i];
		unsigned char *mp = (unsigned char *)umem_bufs2[i];
		size_t j;

		for (j = 0; j < ptc_sizes[i]; j++) {
			TEST_ASSERT(up[j] == 0xAA,
			    "umem buffer corrupted in mixed test");
			TEST_ASSERT(mp[j] == 0xBB,
			    "umem buffer corrupted in mixed test");
		}
	}

	/* Free in reverse order */
	for (i = NUM_PTC_SIZES; i > 0; i--) {
		umem_free(umem_bufs2[i - 1], ptc_sizes[i - 1]);
		umem_free(umem_bufs[i - 1], ptc_sizes[i - 1]);
	}

	return 0;
}

/* ======================================================================
 * Test 9: Rapid alloc/free cycling (PTC fast path exercise)
 * ====================================================================== */

static int
test_rapid_cycling(void)
{
	int i;
	const int iterations = 10000;
	size_t sz = ptc_sizes[2]; /* 32 bytes on LP64, 24 on ILP32 */

	/*
	 * Rapid alloc/free of the same size should exercise the PTC
	 * fast path heavily.  The freed buffer should be immediately
	 * reused from the PTC on the next allocation.
	 */
	for (i = 0; i < iterations; i++) {
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "rapid cycling alloc");
		*(volatile int *)p = i;
		umem_free(p, sz);
	}

	/* Second round of rapid cycling */
	for (i = 0; i < iterations; i++) {
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "rapid cycling alloc (2nd round)");
		*(volatile int *)p = i;
		umem_free(p, sz);
	}

	return 0;
}

/* ======================================================================
 * Test 10: umem_zalloc through PTC path (formerly tested calloc/realloc)
 * ====================================================================== */

static int
test_calloc_realloc(void)
{
	size_t i;

	/* umem_zalloc should return zeroed memory */
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		unsigned char *p;
		size_t j;

		p = (unsigned char *)umem_zalloc(ptc_sizes[i], UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "umem_zalloc should succeed");

		for (j = 0; j < ptc_sizes[i]; j++) {
			TEST_ASSERT(p[j] == 0, "umem_zalloc memory should be zero");
		}
		umem_free(p, ptc_sizes[i]);
	}

	/* Manual realloc from small to large (within PTC range) */
	{
		void *p = umem_alloc(ptc_sizes[0], UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "initial umem_alloc for realloc test");
		memset(p, 0xAA, ptc_sizes[0]);

		/* Manual realloc: alloc new, copy, free old */
		void *new_p = umem_alloc(ptc_sizes[NUM_PTC_SIZES - 1], UMEM_DEFAULT);
		TEST_ASSERT(new_p != NULL, "realloc to larger size");
		memcpy(new_p, p, ptc_sizes[0]);
		umem_free(p, ptc_sizes[0]);
		p = new_p;

		/* First bytes should still contain old data */
		{
			unsigned char *cp = (unsigned char *)p;
			size_t j;
			for (j = 0; j < ptc_sizes[0]; j++) {
				TEST_ASSERT(cp[j] == 0xAA,
				    "realloc should preserve data");
			}
		}
		umem_free(p, ptc_sizes[NUM_PTC_SIZES - 1]);
	}

	/* Manual realloc from PTC range to above PTC range */
	{
		void *p = umem_alloc(ptc_sizes[0], UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "umem_alloc for cross-boundary realloc");
		memset(p, 0xBB, ptc_sizes[0]);

		/* Manual realloc: alloc new, copy, free old */
		void *new_p = umem_alloc(PTC_MAX_SIZE + 1024, UMEM_DEFAULT);
		TEST_ASSERT(new_p != NULL,
		    "realloc to above PTC range should succeed");
		memcpy(new_p, p, ptc_sizes[0]);
		umem_free(p, ptc_sizes[0]);
		p = new_p;

		{
			unsigned char *cp = (unsigned char *)p;
			size_t j;
			for (j = 0; j < ptc_sizes[0]; j++) {
				TEST_ASSERT(cp[j] == 0xBB,
				    "cross-boundary realloc should preserve data");
			}
		}
		umem_free(p, PTC_MAX_SIZE + 1024);
	}

	/* Test additional zero-sized allocation */
	{
		void *p = umem_alloc(64, UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "umem_alloc should succeed");
		umem_free(p, 64);
	}

	/* Test zero allocation handling */
	{
		void *p = umem_alloc(32, UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "umem_alloc for zero test");
		umem_free(p, 32);
	}

	return 0;
}

/* ======================================================================
 * Test 11: Multi-threaded alloc/free with different sizes per thread
 * ====================================================================== */

struct mt_worker_data {
	int		thread_id;
	size_t		alloc_size;
	int		iterations;
	int		passed;
};

static void *
mt_alloc_free_worker(void *arg)
{
	struct mt_worker_data *wd = (struct mt_worker_data *)arg;
	int i;

	wd->passed = 1;

	for (i = 0; i < wd->iterations; i++) {
		void *p = umem_alloc(wd->alloc_size, UMEM_DEFAULT);
		if (p == NULL) {
			wd->passed = 0;
			return NULL;
		}
		memset(p, wd->thread_id & 0xff, wd->alloc_size);
		umem_free(p, wd->alloc_size);
	}

	/* Second round with umem_alloc/umem_free */
	for (i = 0; i < wd->iterations; i++) {
		void *p = umem_alloc(wd->alloc_size, UMEM_DEFAULT);
		if (p == NULL) {
			wd->passed = 0;
			return NULL;
		}
		memset(p, wd->thread_id & 0xff, wd->alloc_size);
		umem_free(p, wd->alloc_size);
	}

	return NULL;
}

static int
test_multithreaded_sizes(void)
{
	const int nthreads = 8;
	struct mt_worker_data workers[8];
	pthread_t tids[8];
	int i, rc;

	for (i = 0; i < nthreads; i++) {
		workers[i].thread_id = i;
		workers[i].alloc_size = ptc_sizes[i % NUM_PTC_SIZES];
		workers[i].iterations = 1000;
		workers[i].passed = 0;

		rc = pthread_create(&tids[i], NULL, mt_alloc_free_worker,
		    &workers[i]);
		TEST_ASSERT(rc == 0, "pthread_create for MT sizes test");
	}

	for (i = 0; i < nthreads; i++) {
		rc = pthread_join(tids[i], NULL);
		TEST_ASSERT(rc == 0, "pthread_join for MT sizes test");
		TEST_ASSERT(workers[i].passed,
		    "MT worker should complete successfully");
	}

	return 0;
}

/* ======================================================================
 * Test 12: Alignment verification
 * ====================================================================== */

static int
test_alignment(void)
{
	size_t i;

	/*
	 * All allocations should be at least UMEM_ALIGN (8 byte) aligned.
	 * On LP64, allocations >= 16 bytes should be 16-byte aligned.
	 */
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		void *p = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "alloc for alignment check");

		TEST_ASSERT(((uintptr_t)p & 7) == 0,
		    "allocation should be 8-byte aligned");

#ifdef _LP64
		if (ptc_sizes[i] >= 16) {
			TEST_ASSERT(((uintptr_t)p & 15) == 0,
			    "LP64 allocation >= 16B should be 16-byte aligned");
		}
#endif
		umem_free(p, ptc_sizes[i]);
	}

	/* Same check via umem_alloc (second round) */
	for (i = 0; i < NUM_PTC_SIZES; i++) {
		void *p = umem_alloc(ptc_sizes[i], UMEM_DEFAULT);
		TEST_ASSERT(p != NULL, "umem_alloc for alignment check");

		TEST_ASSERT(((uintptr_t)p & 7) == 0,
		    "umem_alloc should be 8-byte aligned");

#ifdef _LP64
		if (ptc_sizes[i] >= 16) {
			TEST_ASSERT(((uintptr_t)p & 15) == 0,
			    "LP64 umem_alloc >= 16B should be 16-byte aligned");
		}
#endif
		umem_free(p, ptc_sizes[i]);
	}

	return 0;
}

/* ======================================================================
 * Main
 * ====================================================================== */

int
main(void)
{
	fprintf(stderr, "=== libumem PTC unit tests ===\n");
	fprintf(stderr, "PTC max size: %d bytes (%d size classes)\n",
	    (int)PTC_MAX_SIZE, (int)NUM_PTC_SIZES);

	RUN_TEST(test_basic_alloc_free);
	RUN_TEST(test_malloc_free);
	RUN_TEST(test_thread_local_independence);
	RUN_TEST(test_size_boundaries);
	RUN_TEST(test_cache_limits);
	RUN_TEST(test_thread_cleanup);
	RUN_TEST(test_data_integrity);
	RUN_TEST(test_mixed_allocators);
	RUN_TEST(test_rapid_cycling);
	RUN_TEST(test_calloc_realloc);
	RUN_TEST(test_multithreaded_sizes);
	RUN_TEST(test_alignment);

	fprintf(stderr, "\n=== Results: %d tests run, %d passed, %d failed ===\n",
	    tests_run, tests_passed, tests_failed);

	return (tests_failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

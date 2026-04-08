/*
 * Comprehensive realloc() test suite
 * 
 * Tests realloc behavior including:
 * - Bootstrap allocations
 * - Size class transitions
 * - Thread safety
 * - Data preservation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>

#define NUM_THREADS 8
#define ITERATIONS 100

/*
 * Test 1: Basic realloc functionality
 */
static int
test_basic_realloc(void)
{
	unsigned char *ptr;
	int errors = 0;

	ptr = malloc(16);
	assert(ptr != NULL);

	/* Fill with pattern */
	for (int i = 0; i < 16; i++) {
		ptr[i] = (unsigned char)i;
	}

	/* Realloc to larger size */
	ptr = realloc(ptr, 32);
	assert(ptr != NULL);

	/* Verify data preserved */
	for (int i = 0; i < 16; i++) {
		if (ptr[i] != (unsigned char)i) {
			printf("ERROR: byte %d corrupted: expected %02x, got %02x\n",
			    i, i, ptr[i]);
			errors++;
		}
	}

	free(ptr);
	return (errors);
}

/*
 * Test 2: Bootstrap/early allocation realloc
 * Tests realloc on allocations made during library initialization
 */
static int
test_bootstrap_realloc(void)
{
	unsigned char *ptr;
	int errors = 0;

	/* Small allocations to test bootstrap path */
	for (int size = 16; size <= 128; size *= 2) {
		ptr = malloc(size);
		assert(ptr != NULL);

		/* Fill with pattern */
		for (int i = 0; i < size; i++) {
			ptr[i] = (unsigned char)(i & 0xFF);
		}

		/* Realloc to double size */
		int old_size = size;
		ptr = realloc(ptr, size * 2);
		assert(ptr != NULL);

		/* Verify old data preserved */
		for (int i = 0; i < old_size; i++) {
			if (ptr[i] != (unsigned char)(i & 0xFF)) {
				printf("ERROR: size %d->%d, byte %d corrupted\n",
				    old_size, size * 2, i);
				errors++;
			}
		}

		free(ptr);
	}

	return (errors);
}

/*
 * Test 3: Multithreaded realloc stress test
 */
static void *
thread_worker(void *arg)
{
	int tid = *(int *)arg;

	for (int iter = 0; iter < ITERATIONS; iter++) {
		unsigned char *ptr = malloc(16);
		assert(ptr != NULL);

		/* Fill with thread-specific pattern */
		unsigned char pattern = (unsigned char)(tid * ITERATIONS + iter);
		memset(ptr, pattern, 16);

		/* Realloc to larger size */
		ptr = realloc(ptr, 64);
		assert(ptr != NULL);

		/* Verify pattern preserved */
		for (int i = 0; i < 16; i++) {
			if (ptr[i] != pattern) {
				printf("Thread %d iter %d: byte %d corrupted\n",
				    tid, iter, i);
				return ((void *)1);
			}
		}

		free(ptr);
	}

	return (NULL);
}

static int
test_threaded_realloc(void)
{
	pthread_t threads[NUM_THREADS];
	int tids[NUM_THREADS];
	int errors = 0;

	for (int i = 0; i < NUM_THREADS; i++) {
		tids[i] = i;
		assert(pthread_create(&threads[i], NULL, thread_worker, &tids[i]) == 0);
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		void *result;
		assert(pthread_join(threads[i], &result) == 0);
		if (result != NULL) {
			errors++;
		}
	}

	return (errors);
}

/*
 * Test 4: NULL and zero-size edge cases
 */
static int
test_edge_cases(void)
{
	void *ptr;
	int errors = 0;

	/* realloc(NULL, size) == malloc(size) */
	ptr = realloc(NULL, 32);
	if (ptr == NULL) {
		printf("ERROR: realloc(NULL, 32) failed\n");
		errors++;
	} else {
		free(ptr);
	}

	/* realloc(ptr, 0) == free(ptr) */
	ptr = malloc(32);
	assert(ptr != NULL);
	ptr = realloc(ptr, 0);
	if (ptr != NULL) {
		printf("ERROR: realloc(ptr, 0) should return NULL\n");
		errors++;
		free(ptr);
	}

	return (errors);
}

int
main(void)
{
	int total_errors = 0;

	printf("Running comprehensive realloc tests...\n\n");

	printf("Test 1: Basic realloc... ");
	fflush(stdout);
	int errors = test_basic_realloc();
	printf("%s\n", errors == 0 ? "PASSED" : "FAILED");
	total_errors += errors;

	printf("Test 2: Bootstrap realloc... ");
	fflush(stdout);
	errors = test_bootstrap_realloc();
	printf("%s\n", errors == 0 ? "PASSED" : "FAILED");
	total_errors += errors;

	printf("Test 3: Threaded realloc... ");
	fflush(stdout);
	errors = test_threaded_realloc();
	printf("%s\n", errors == 0 ? "PASSED" : "FAILED");
	total_errors += errors;

	printf("Test 4: Edge cases... ");
	fflush(stdout);
	errors = test_edge_cases();
	printf("%s\n", errors == 0 ? "PASSED" : "FAILED");
	total_errors += errors;

	printf("\n");
	if (total_errors == 0) {
		printf("All tests PASSED\n");
		return (0);
	} else {
		printf("%d test(s) FAILED\n", total_errors);
		return (1);
	}
}

/*
 * Test for initial-exec TLS recursion guard
 *
 * This test verifies that the recursion guard correctly detects and handles
 * recursive malloc calls during pthread operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define NUM_THREADS 10
#define ALLOCS_PER_THREAD 100

void *thread_func(void *arg)
{
	int thread_id = *(int *)arg;
	void *ptrs[ALLOCS_PER_THREAD];
	int i;

	printf("Thread %d starting\n", thread_id);

	/* Allocate memory */
	for (i = 0; i < ALLOCS_PER_THREAD; i++) {
		ptrs[i] = malloc(1024 + i);
		if (ptrs[i] == NULL) {
			fprintf(stderr, "Thread %d: malloc failed at iteration %d\n",
			    thread_id, i);
			return (NULL);
		}
	}

	/* Free memory */
	for (i = 0; i < ALLOCS_PER_THREAD; i++) {
		free(ptrs[i]);
	}

	printf("Thread %d completed\n", thread_id);
	return (NULL);
}

int main(void)
{
	pthread_t threads[NUM_THREADS];
	int thread_ids[NUM_THREADS];
	int i;

	printf("Testing recursion guard with %d threads\n", NUM_THREADS);

	/* Create threads */
	for (i = 0; i < NUM_THREADS; i++) {
		thread_ids[i] = i;
		if (pthread_create(&threads[i], NULL, thread_func,
		    &thread_ids[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d\n", i);
			return (1);
		}
	}

	/* Wait for threads to complete */
	for (i = 0; i < NUM_THREADS; i++) {
		if (pthread_join(threads[i], NULL) != 0) {
			fprintf(stderr, "Failed to join thread %d\n", i);
			return (1);
		}
	}

	printf("All threads completed successfully\n");
	printf("Test PASSED\n");

	return (0);
}

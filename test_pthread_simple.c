/*
 * Minimal pthread_create test to verify malloc interposition doesn't deadlock
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *thread_func(void *arg) {
    printf("Thread %d: allocating memory\n", *(int*)arg);

    /* These malloc calls will go through interposition */
    char *buf = malloc(1024);
    if (buf) {
        memset(buf, 0, 1024);
        free(buf);
    }

    printf("Thread %d: done\n", *(int*)arg);
    return NULL;
}

int main(void) {
    pthread_t threads[4];
    int thread_ids[4];

    printf("Main: starting test\n");

    /* This malloc should work fine */
    char *main_buf = malloc(512);
    if (main_buf) {
        memset(main_buf, 0, 512);
        printf("Main: malloc works\n");
    }

    /* Create threads - this internally calls malloc */
    printf("Main: creating threads\n");
    for (int i = 0; i < 4; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, thread_func, &thread_ids[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
        printf("Main: created thread %d\n", i);
    }

    /* Wait for threads */
    printf("Main: waiting for threads\n");
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
        printf("Main: joined thread %d\n", i);
    }

    free(main_buf);
    printf("Main: test completed successfully\n");
    return 0;
}

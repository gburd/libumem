/*
 * Multithreaded integration tests for libumem
 *
 * Tests concurrent allocation behavior with direct umem API and pthread library.
 * Focuses on PTC (Per-Thread Cache) behavior under various threading scenarios.
 *
 * Test scenarios:
 * 1. test_concurrent_alloc_free - Basic concurrent allocation stress test
 * 2. test_producer_consumer - Producer/consumer pattern with shared queue
 * 3. test_cache_contention - Multiple threads hitting same cache
 * 4. test_mixed_operations - Different umem APIs concurrently
 * 5. test_thread_exit_cleanup - Thread cleanup with outstanding allocations
 * 6. test_high_thread_count - Scalability test with many threads
 */

#include "../munit.h"
#include "../../umem.h"
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>

#define BUFFER_PATTERN 0xAA
#define VERIFY_PATTERN 0xBB

typedef struct {
    int thread_id;
    int iterations;
    umem_cache_t *cache;
    atomic_int *shared_counter;
    void **shared_queue;
    int *queue_head;
    int *queue_tail;
    pthread_mutex_t *queue_lock;
    int queue_capacity;
} thread_args_t;

/*
 * Test 1: test_concurrent_alloc_free()
 *
 * Basic concurrent allocation stress test.
 * 8 threads, each doing 10,000 alloc/free cycles.
 * Each thread allocates 128-byte buffers, writes pattern, frees immediately.
 * No sharing between threads - tests PTC behavior under contention.
 */
static void* worker_alloc_free(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        void *ptr = umem_alloc(128, UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        memset(ptr, BUFFER_PATTERN, 128);

        unsigned char *bytes = (unsigned char *)ptr;
        for (int j = 0; j < 128; j++) {
            munit_assert_uint8(bytes[j], ==, BUFFER_PATTERN);
        }

        umem_free(ptr, 128);
    }

    return NULL;
}

static MunitResult test_concurrent_alloc_free(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    const int NUM_THREADS = 8;
    const int ITERATIONS = 10000;
    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].cache = NULL;
        args[i].shared_counter = NULL;
        args[i].shared_queue = NULL;
        args[i].queue_lock = NULL;

        munit_assert_int(pthread_create(&threads[i], NULL, worker_alloc_free, &args[i]), ==, 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
    }

    return MUNIT_OK;
}

/*
 * Test 2: test_producer_consumer()
 *
 * Producer/consumer pattern with shared queue.
 * 4 producers allocate buffers and push to queue.
 * 4 consumers pop from queue and free buffers.
 * 5000 operations total, queue protected by mutex.
 * Verifies cross-thread free behavior.
 */
static void* worker_producer(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    int operations = args->iterations;

    for (int i = 0; i < operations; i++) {
        void *ptr = umem_alloc(256, UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        memset(ptr, (unsigned char)(args->thread_id), 256);

        pthread_mutex_lock(args->queue_lock);

        int next_tail = (*args->queue_tail + 1) % args->queue_capacity;
        while (next_tail == *args->queue_head) {
            pthread_mutex_unlock(args->queue_lock);
            sched_yield();
            pthread_mutex_lock(args->queue_lock);
            next_tail = (*args->queue_tail + 1) % args->queue_capacity;
        }

        args->shared_queue[*args->queue_tail] = ptr;
        *args->queue_tail = next_tail;

        pthread_mutex_unlock(args->queue_lock);
    }

    return NULL;
}

static void* worker_consumer(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    int operations = args->iterations;

    for (int i = 0; i < operations; i++) {
        void *ptr = NULL;

        while (ptr == NULL) {
            pthread_mutex_lock(args->queue_lock);

            if (*args->queue_head != *args->queue_tail) {
                ptr = args->shared_queue[*args->queue_head];
                *args->queue_head = (*args->queue_head + 1) % args->queue_capacity;
            }

            pthread_mutex_unlock(args->queue_lock);

            if (ptr == NULL) {
                sched_yield();
            }
        }

        umem_free(ptr, 256);
    }

    return NULL;
}

static MunitResult test_producer_consumer(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    const int NUM_PRODUCERS = 4;
    const int NUM_CONSUMERS = 4;
    const int OPERATIONS_PER_THREAD = 1250;
    const int QUEUE_CAPACITY = 1024;

    pthread_t producers[NUM_PRODUCERS];
    pthread_t consumers[NUM_CONSUMERS];
    thread_args_t producer_args[NUM_PRODUCERS];
    thread_args_t consumer_args[NUM_CONSUMERS];

    void *queue[QUEUE_CAPACITY];
    int queue_head = 0;
    int queue_tail = 0;
    pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_args[i].thread_id = i;
        producer_args[i].iterations = OPERATIONS_PER_THREAD;
        producer_args[i].shared_queue = queue;
        producer_args[i].queue_head = &queue_head;
        producer_args[i].queue_tail = &queue_tail;
        producer_args[i].queue_lock = &queue_lock;
        producer_args[i].queue_capacity = QUEUE_CAPACITY;

        munit_assert_int(pthread_create(&producers[i], NULL, worker_producer, &producer_args[i]), ==, 0);
    }

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        consumer_args[i].thread_id = i + NUM_PRODUCERS;
        consumer_args[i].iterations = OPERATIONS_PER_THREAD;
        consumer_args[i].shared_queue = queue;
        consumer_args[i].queue_head = &queue_head;
        consumer_args[i].queue_tail = &queue_tail;
        consumer_args[i].queue_lock = &queue_lock;
        consumer_args[i].queue_capacity = QUEUE_CAPACITY;

        munit_assert_int(pthread_create(&consumers[i], NULL, worker_consumer, &consumer_args[i]), ==, 0);
    }

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        munit_assert_int(pthread_join(producers[i], NULL), ==, 0);
    }

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        munit_assert_int(pthread_join(consumers[i], NULL), ==, 0);
    }

    munit_assert_int(queue_head, ==, queue_tail);

    pthread_mutex_destroy(&queue_lock);

    return MUNIT_OK;
}

/*
 * Test 3: test_cache_contention()
 *
 * Multiple threads hitting same cache.
 * Creates single umem_cache, 16 threads allocate/free from it (1000 ops each).
 * Tests cache locking and magazine behavior under heavy contention.
 */
static void* worker_cache_contention(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        void *ptr = umem_cache_alloc(args->cache, UMEM_DEFAULT);
        munit_assert_not_null(ptr);

        memset(ptr, VERIFY_PATTERN, 64);

        umem_cache_free(args->cache, ptr);
    }

    return NULL;
}

static MunitResult test_cache_contention(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    const int NUM_THREADS = 16;
    const int ITERATIONS = 1000;
    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];

    umem_cache_t *cache = umem_cache_create("test_cache", 64, 8, NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cache);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].cache = cache;

        munit_assert_int(pthread_create(&threads[i], NULL, worker_cache_contention, &args[i]), ==, 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
    }

    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/*
 * Test 4: test_mixed_operations()
 *
 * Threads doing different operations concurrently.
 * Thread groups use different umem APIs:
 *   - Threads 0-3: umem_alloc()
 *   - Threads 4-7: umem_cache_alloc()
 *   - Threads 8-11: umem_alloc_align()
 * 2000 ops per thread. Verifies all APIs work together.
 */
static void* worker_mixed_alloc(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        void *ptr = umem_alloc(128, UMEM_DEFAULT);
        munit_assert_not_null(ptr);
        memset(ptr, 0xCC, 128);
        umem_free(ptr, 128);
    }

    return NULL;
}

static void* worker_mixed_cache(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        void *ptr = umem_cache_alloc(args->cache, UMEM_DEFAULT);
        munit_assert_not_null(ptr);
        memset(ptr, 0xDD, 128);
        umem_cache_free(args->cache, ptr);
    }

    return NULL;
}

static void* worker_mixed_align(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        void *ptr = umem_alloc_align(128, 64, UMEM_DEFAULT);
        munit_assert_not_null(ptr);
        munit_assert_uint64((uintptr_t)ptr % 64, ==, 0);
        memset(ptr, 0xEE, 128);
        umem_free_align(ptr, 128);
    }

    return NULL;
}

static MunitResult test_mixed_operations(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    const int THREADS_PER_TYPE = 4;
    const int TOTAL_THREADS = THREADS_PER_TYPE * 3;
    const int ITERATIONS = 2000;
    pthread_t threads[TOTAL_THREADS];
    thread_args_t args[TOTAL_THREADS];

    umem_cache_t *cache = umem_cache_create("mixed_cache", 128, 8, NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cache);

    for (int i = 0; i < THREADS_PER_TYPE; i++) {
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].cache = NULL;
        munit_assert_int(pthread_create(&threads[i], NULL, worker_mixed_alloc, &args[i]), ==, 0);
    }

    for (int i = THREADS_PER_TYPE; i < THREADS_PER_TYPE * 2; i++) {
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].cache = cache;
        munit_assert_int(pthread_create(&threads[i], NULL, worker_mixed_cache, &args[i]), ==, 0);
    }

    for (int i = THREADS_PER_TYPE * 2; i < TOTAL_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].cache = NULL;
        munit_assert_int(pthread_create(&threads[i], NULL, worker_mixed_align, &args[i]), ==, 0);
    }

    for (int i = 0; i < TOTAL_THREADS; i++) {
        munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
    }

    umem_cache_destroy(cache);

    return MUNIT_OK;
}

/*
 * Test 5: test_thread_exit_cleanup()
 *
 * Threads exit with outstanding allocations.
 * 8 threads each allocate 100 buffers, then exit WITHOUT freeing.
 * Tests PTC cleanup behavior when threads terminate.
 * Main thread frees remaining allocations after join.
 */
static void* worker_exit_with_allocs(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    void **buffers = (void **)malloc(sizeof(void*) * 100);

    for (int i = 0; i < 100; i++) {
        buffers[i] = umem_alloc(64, UMEM_DEFAULT);
        munit_assert_not_null(buffers[i]);
        memset(buffers[i], (unsigned char)(args->thread_id), 64);
    }

    void **result = malloc(sizeof(void*));
    *result = buffers;

    return result;
}

static MunitResult test_thread_exit_cleanup(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    const int NUM_THREADS = 8;
    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];
    void **thread_buffers[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = 100;

        munit_assert_int(pthread_create(&threads[i], NULL, worker_exit_with_allocs, &args[i]), ==, 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        void *result;
        munit_assert_int(pthread_join(threads[i], &result), ==, 0);
        thread_buffers[i] = (void **)result;
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        void **buffers = (void **)(*thread_buffers[i]);
        for (int j = 0; j < 100; j++) {
            umem_free(buffers[j], 64);
        }
        free(buffers);
        free(thread_buffers[i]);
    }

    return MUNIT_OK;
}

/*
 * Test 6: test_high_thread_count()
 *
 * Scalability test with many threads.
 * Creates 64 threads, each allocates 100 small buffers (32 bytes).
 * Tests PTC scalability and performance under high thread count.
 * Each thread frees all buffers before exit.
 */
static void* worker_high_thread_count(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    void *buffers[100];

    for (int i = 0; i < 100; i++) {
        buffers[i] = umem_alloc(32, UMEM_DEFAULT);
        munit_assert_not_null(buffers[i]);
        memset(buffers[i], (unsigned char)(args->thread_id & 0xFF), 32);
    }

    for (int i = 0; i < 100; i++) {
        umem_free(buffers[i], 32);
    }

    return NULL;
}

static MunitResult test_high_thread_count(const MunitParameter params[], void* data) {
    (void)params;
    (void)data;

    const int NUM_THREADS = 64;
    pthread_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = 100;

        munit_assert_int(pthread_create(&threads[i], NULL, worker_high_thread_count, &args[i]), ==, 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        munit_assert_int(pthread_join(threads[i], NULL), ==, 0);
    }

    return MUNIT_OK;
}

static MunitTest tests[] = {
    { "/concurrent_alloc_free", test_concurrent_alloc_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/producer_consumer", test_producer_consumer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/cache_contention", test_cache_contention, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/mixed_operations", test_mixed_operations, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/thread_exit_cleanup", test_thread_exit_cleanup, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { "/high_thread_count", test_high_thread_count, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/multithreaded", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[]) {
    return munit_suite_main(&suite, NULL, argc, argv);
}

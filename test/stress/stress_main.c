/*
 * Long-running stress test for libumem 2.0
 *
 * Exercises multiple allocation patterns under sustained load.
 * Default duration: 60 seconds (-d flag to override).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

#include "../../umem.h"

/* --- Time helpers --- */

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static size_t
get_rss_bytes(void)
{
#ifdef __linux__
	FILE *f = fopen("/proc/self/status", "r");
	if (!f)
		return 0;
	char line[256];
	size_t rss = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "VmRSS:", 6) == 0) {
			char *p = line + 6;
			while (*p == ' ' || *p == '\t')
				p++;
			rss = (size_t)strtoull(p, NULL, 10) * 1024;
			break;
		}
	}
	fclose(f);
	return rss;
#else
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__APPLE__)
		return (size_t)ru.ru_maxrss;
#else
		return (size_t)ru.ru_maxrss * 1024;
#endif
	}
	return 0;
#endif
}

/* --- Per-thread RNG --- */

typedef struct {
	uint32_t state;
} rng_t;

static uint32_t
rng_next(rng_t *r)
{
	r->state ^= r->state << 13;
	r->state ^= r->state >> 17;
	r->state ^= r->state << 5;
	return r->state;
}

static size_t
rng_range(rng_t *r, size_t lo, size_t hi)
{
	if (hi <= lo)
		return lo;
	return lo + (rng_next(r) % (hi - lo + 1));
}

/* --- Latency tracking (simple reservoir sampling) --- */

#define LATENCY_SAMPLES 10000

typedef struct {
	uint64_t samples[LATENCY_SAMPLES];
	size_t count;
	size_t total;
	rng_t rng;
} latency_tracker_t;

static void
lat_init(latency_tracker_t *lt, uint32_t seed)
{
	lt->count = 0;
	lt->total = 0;
	lt->rng.state = seed ? seed : 1;
}

static void
lat_record(latency_tracker_t *lt, uint64_t ns)
{
	lt->total++;
	if (lt->count < LATENCY_SAMPLES) {
		lt->samples[lt->count++] = ns;
	} else {
		size_t idx = rng_next(&lt->rng) % lt->total;
		if (idx < LATENCY_SAMPLES)
			lt->samples[idx] = ns;
	}
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	if (va < vb) return -1;
	if (va > vb) return 1;
	return 0;
}

static uint64_t
lat_percentile(latency_tracker_t *lt, double p)
{
	if (lt->count == 0)
		return 0;
	qsort(lt->samples, lt->count, sizeof(uint64_t), cmp_u64);
	size_t idx = (size_t)(p * (double)(lt->count - 1));
	if (idx >= lt->count)
		idx = lt->count - 1;
	return lt->samples[idx];
}

/* --- Scenario result --- */

typedef struct {
	const char *name;
	uint64_t ops;
	double elapsed_sec;
	uint64_t p99_ns;
	size_t peak_rss;
	int errors;
} scenario_result_t;

static void
print_result(const scenario_result_t *r)
{
	double ops_sec = r->elapsed_sec > 0 ? (double)r->ops / r->elapsed_sec : 0;
	double rss_mb = r->peak_rss / (1024.0 * 1024.0);
	printf("  %-16s %10.0f ops/s  p99 %6lu ns  RSS %6.1f MB  errors %d\n",
	    r->name, ops_sec, (unsigned long)r->p99_ns, rss_mb, r->errors);
}

/* --- Scenario 1: Thread churn --- */

#define CHURN_THREADS   8
#define CHURN_OBJECTS   10000

typedef struct {
	int duration_sec;
	atomic_int errors;
	atomic_ulong total_ops;
	latency_tracker_t lat;
	pthread_mutex_t lat_lock;
} churn_shared_t;

static void *
churn_worker(void *arg)
{
	churn_shared_t *shared = (churn_shared_t *)arg;
	rng_t rng = { .state = (uint32_t)(uintptr_t)pthread_self() };

	void *ptrs[CHURN_OBJECTS];
	size_t sizes[CHURN_OBJECTS];

	for (int i = 0; i < CHURN_OBJECTS; i++) {
		sizes[i] = rng_range(&rng, 32, 4096);
		uint64_t t0 = now_ns();
		ptrs[i] = umem_alloc(sizes[i], UMEM_DEFAULT);
		uint64_t t1 = now_ns();

		if (!ptrs[i]) {
			atomic_fetch_add(&shared->errors, 1);
			sizes[i] = 0;
			continue;
		}
		memset(ptrs[i], 0xAA, sizes[i]);

		pthread_mutex_lock(&shared->lat_lock);
		lat_record(&shared->lat, t1 - t0);
		pthread_mutex_unlock(&shared->lat_lock);
	}

	atomic_fetch_add(&shared->total_ops, CHURN_OBJECTS);

	for (int i = 0; i < CHURN_OBJECTS; i++) {
		if (ptrs[i])
			umem_free(ptrs[i], sizes[i]);
	}

	return NULL;
}

static void
scenario_thread_churn(int duration_sec, scenario_result_t *result)
{
	churn_shared_t shared = {
		.duration_sec = duration_sec,
		.errors = 0,
		.total_ops = 0,
		.lat_lock = PTHREAD_MUTEX_INITIALIZER,
	};
	lat_init(&shared.lat, 12345);

	size_t peak_rss = 0;
	uint64_t start = now_ns();
	uint64_t deadline = start + (uint64_t)duration_sec * 1000000000ULL;

	while (now_ns() < deadline) {
		pthread_t threads[CHURN_THREADS];
		for (int i = 0; i < CHURN_THREADS; i++)
			pthread_create(&threads[i], NULL, churn_worker, &shared);
		for (int i = 0; i < CHURN_THREADS; i++)
			pthread_join(threads[i], NULL);

		size_t rss = get_rss_bytes();
		if (rss > peak_rss)
			peak_rss = rss;
	}

	uint64_t elapsed = now_ns() - start;

	result->name = "thread_churn";
	result->ops = atomic_load(&shared.total_ops);
	result->elapsed_sec = elapsed / 1e9;
	result->p99_ns = lat_percentile(&shared.lat, 0.99);
	result->peak_rss = peak_rss;
	result->errors = atomic_load(&shared.errors);

	pthread_mutex_destroy(&shared.lat_lock);
}

/* --- Scenario 2: Mixed sizes --- */

#define MIXED_THREADS 4

typedef struct {
	int duration_sec;
	atomic_int errors;
	atomic_ulong total_ops;
	latency_tracker_t lat;
	pthread_mutex_t lat_lock;
} mixed_shared_t;

static void *
mixed_worker(void *arg)
{
	mixed_shared_t *shared = (mixed_shared_t *)arg;
	rng_t rng = { .state = (uint32_t)(uintptr_t)pthread_self() };

	uint64_t deadline = now_ns() +
	    (uint64_t)shared->duration_sec * 1000000000ULL;
	uint64_t ops = 0;

	while (now_ns() < deadline) {
		size_t sz;
		uint32_t r = rng_next(&rng) % 100;
		if (r < 50)
			sz = rng_range(&rng, 8, 64);
		else if (r < 80)
			sz = rng_range(&rng, 256, 4096);
		else
			sz = rng_range(&rng, 16384, 131072);

		uint64_t t0 = now_ns();
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		uint64_t t1 = now_ns();

		if (!p) {
			atomic_fetch_add(&shared->errors, 1);
			continue;
		}
		memset(p, 0xBB, sz < 256 ? sz : 256);

		pthread_mutex_lock(&shared->lat_lock);
		lat_record(&shared->lat, t1 - t0);
		pthread_mutex_unlock(&shared->lat_lock);

		umem_free(p, sz);
		ops++;
	}

	atomic_fetch_add(&shared->total_ops, ops);
	return NULL;
}

static void
scenario_mixed_sizes(int duration_sec, scenario_result_t *result)
{
	mixed_shared_t shared = {
		.duration_sec = duration_sec,
		.errors = 0,
		.total_ops = 0,
		.lat_lock = PTHREAD_MUTEX_INITIALIZER,
	};
	lat_init(&shared.lat, 54321);

	pthread_t threads[MIXED_THREADS];
	uint64_t start = now_ns();

	for (int i = 0; i < MIXED_THREADS; i++)
		pthread_create(&threads[i], NULL, mixed_worker, &shared);
	for (int i = 0; i < MIXED_THREADS; i++)
		pthread_join(threads[i], NULL);

	uint64_t elapsed = now_ns() - start;

	result->name = "mixed_sizes";
	result->ops = atomic_load(&shared.total_ops);
	result->elapsed_sec = elapsed / 1e9;
	result->p99_ns = lat_percentile(&shared.lat, 0.99);
	result->peak_rss = get_rss_bytes();
	result->errors = atomic_load(&shared.errors);

	pthread_mutex_destroy(&shared.lat_lock);
}

/* --- Scenario 3: Memory pressure --- */

#define PRESSURE_TARGET_MB  500
#define PRESSURE_HOLD_MB    400
#define PRESSURE_OBJ_SIZE   4096

static void
scenario_memory_pressure(int duration_sec, scenario_result_t *result)
{
	rng_t rng = { .state = 99999 };
	latency_tracker_t lat;
	lat_init(&lat, 77777);

	size_t cap = (PRESSURE_TARGET_MB * 1024UL * 1024UL) / PRESSURE_OBJ_SIZE;
	void **pool = calloc(cap, sizeof(void *));
	if (!pool) {
		result->name = "pressure";
		result->errors = 1;
		return;
	}

	size_t count = 0;
	size_t peak_rss = 0;
	uint64_t ops = 0;
	int errors = 0;

	uint64_t start = now_ns();
	uint64_t deadline = start + (uint64_t)duration_sec * 1000000000ULL;

	/* Phase 1: fill to target */
	while (count < cap && now_ns() < deadline) {
		uint64_t t0 = now_ns();
		pool[count] = umem_alloc(PRESSURE_OBJ_SIZE, UMEM_DEFAULT);
		uint64_t t1 = now_ns();
		if (!pool[count]) {
			errors++;
			break;
		}
		memset(pool[count], 0xCC, PRESSURE_OBJ_SIZE);
		lat_record(&lat, t1 - t0);
		count++;
		ops++;
	}

	/* Phase 2: maintain pressure with free/realloc cycles */
	size_t hold = (PRESSURE_HOLD_MB * 1024UL * 1024UL) / PRESSURE_OBJ_SIZE;
	if (hold > count)
		hold = count;

	while (now_ns() < deadline) {
		/* Free some to get down to hold level */
		while (count > hold && count > 0) {
			count--;
			umem_free(pool[count], PRESSURE_OBJ_SIZE);
			pool[count] = NULL;
			ops++;
		}

		/* Reallocate back up with varied sizes */
		size_t target = hold + rng_range(&rng, 0, cap - hold);
		if (target > cap)
			target = cap;
		while (count < target && now_ns() < deadline) {
			size_t sz = rng_range(&rng, 1024, 8192);
			uint64_t t0 = now_ns();
			pool[count] = umem_alloc(sz, UMEM_DEFAULT);
			uint64_t t1 = now_ns();
			if (!pool[count]) {
				errors++;
				break;
			}
			memset(pool[count], 0xDD, sz < 256 ? sz : 256);
			lat_record(&lat, t1 - t0);
			count++;
			ops++;
		}

		size_t rss = get_rss_bytes();
		if (rss > peak_rss)
			peak_rss = rss;
	}

	/* Cleanup - free with original size since we mixed sizes */
	for (size_t i = 0; i < count; i++) {
		if (pool[i])
			umem_free(pool[i], PRESSURE_OBJ_SIZE);
	}
	free(pool);

	uint64_t elapsed = now_ns() - start;

	result->name = "pressure";
	result->ops = ops;
	result->elapsed_sec = elapsed / 1e9;
	result->p99_ns = lat_percentile(&lat, 0.99);
	result->peak_rss = peak_rss;
	result->errors = errors;
}

/* --- Scenario 4: Producer-consumer (cross-thread free) --- */

#define PC_PRODUCERS  4
#define PC_CONSUMERS  4
#define PC_QUEUE_SIZE 8192

typedef struct {
	void *ptr;
	size_t size;
} pc_item_t;

typedef struct {
	pc_item_t items[PC_QUEUE_SIZE];
	_Alignas(64) atomic_ulong head;
	_Alignas(64) atomic_ulong tail;
	atomic_int producers_done;
} pc_queue_t;

static void
pc_queue_init(pc_queue_t *q)
{
	atomic_init(&q->head, 0);
	atomic_init(&q->tail, 0);
	atomic_init(&q->producers_done, 0);
	memset(q->items, 0, sizeof(q->items));
}

static bool
pc_push(pc_queue_t *q, void *ptr, size_t sz)
{
	unsigned long h = atomic_load_explicit(&q->head, memory_order_relaxed);
	unsigned long t = atomic_load_explicit(&q->tail, memory_order_acquire);
	if (h - t >= PC_QUEUE_SIZE)
		return false;
	if (!atomic_compare_exchange_weak_explicit(&q->head, &h, h + 1,
	    memory_order_acq_rel, memory_order_relaxed))
		return false;
	q->items[h % PC_QUEUE_SIZE].ptr = ptr;
	q->items[h % PC_QUEUE_SIZE].size = sz;
	return true;
}

static bool
pc_pop(pc_queue_t *q, void **ptr, size_t *sz)
{
	unsigned long t = atomic_load_explicit(&q->tail, memory_order_relaxed);
	unsigned long h = atomic_load_explicit(&q->head, memory_order_acquire);
	if (t >= h)
		return false;
	if (!atomic_compare_exchange_weak_explicit(&q->tail, &t, t + 1,
	    memory_order_acq_rel, memory_order_relaxed))
		return false;
	*ptr = q->items[t % PC_QUEUE_SIZE].ptr;
	*sz = q->items[t % PC_QUEUE_SIZE].size;
	return true;
}

typedef struct {
	pc_queue_t *queue;
	int duration_sec;
	atomic_ulong prod_ops;
	atomic_ulong cons_ops;
	atomic_int errors;
	latency_tracker_t lat;
	pthread_mutex_t lat_lock;
} pc_shared_t;

static void *
pc_producer(void *arg)
{
	pc_shared_t *shared = (pc_shared_t *)arg;
	rng_t rng = { .state = (uint32_t)(uintptr_t)pthread_self() };
	uint64_t deadline = now_ns() +
	    (uint64_t)shared->duration_sec * 1000000000ULL;
	uint64_t ops = 0;

	while (now_ns() < deadline) {
		size_t sz = rng_range(&rng, 64, 2048);
		uint64_t t0 = now_ns();
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		uint64_t t1 = now_ns();
		if (!p) {
			atomic_fetch_add(&shared->errors, 1);
			continue;
		}
		memset(p, 0xEE, sz < 128 ? sz : 128);

		pthread_mutex_lock(&shared->lat_lock);
		lat_record(&shared->lat, t1 - t0);
		pthread_mutex_unlock(&shared->lat_lock);

		while (!pc_push(shared->queue, p, sz)) {
			sched_yield();
			if (now_ns() >= deadline) {
				umem_free(p, sz);
				goto done;
			}
		}
		ops++;
	}
done:
	atomic_fetch_add(&shared->prod_ops, ops);
	return NULL;
}

static void *
pc_consumer(void *arg)
{
	pc_shared_t *shared = (pc_shared_t *)arg;
	uint64_t ops = 0;

	for (;;) {
		void *ptr;
		size_t sz;
		if (pc_pop(shared->queue, &ptr, &sz)) {
			if (ptr) {
				umem_free(ptr, sz);
				ops++;
			}
		} else if (atomic_load(&shared->queue->producers_done) ==
		    PC_PRODUCERS) {
			/* Drain remaining */
			while (pc_pop(shared->queue, &ptr, &sz)) {
				if (ptr) {
					umem_free(ptr, sz);
					ops++;
				}
			}
			break;
		} else {
			sched_yield();
		}
	}

	atomic_fetch_add(&shared->cons_ops, ops);
	return NULL;
}

static void
scenario_producer_consumer(int duration_sec, scenario_result_t *result)
{
	pc_queue_t *queue = calloc(1, sizeof(pc_queue_t));
	if (!queue) {
		result->name = "prodcons";
		result->errors = 1;
		return;
	}
	pc_queue_init(queue);

	pc_shared_t shared = {
		.queue = queue,
		.duration_sec = duration_sec,
		.prod_ops = 0,
		.cons_ops = 0,
		.errors = 0,
		.lat_lock = PTHREAD_MUTEX_INITIALIZER,
	};
	lat_init(&shared.lat, 11111);

	pthread_t producers[PC_PRODUCERS];
	pthread_t consumers[PC_CONSUMERS];

	uint64_t start = now_ns();

	for (int i = 0; i < PC_CONSUMERS; i++)
		pthread_create(&consumers[i], NULL, pc_consumer, &shared);
	for (int i = 0; i < PC_PRODUCERS; i++)
		pthread_create(&producers[i], NULL, pc_producer, &shared);

	for (int i = 0; i < PC_PRODUCERS; i++)
		pthread_join(producers[i], NULL);
	atomic_store(&queue->producers_done, PC_PRODUCERS);

	for (int i = 0; i < PC_CONSUMERS; i++)
		pthread_join(consumers[i], NULL);

	uint64_t elapsed = now_ns() - start;

	result->name = "prodcons";
	result->ops = atomic_load(&shared.prod_ops) +
	    atomic_load(&shared.cons_ops);
	result->elapsed_sec = elapsed / 1e9;
	result->p99_ns = lat_percentile(&shared.lat, 0.99);
	result->peak_rss = get_rss_bytes();
	result->errors = atomic_load(&shared.errors);

	pthread_mutex_destroy(&shared.lat_lock);
	free(queue);
}

/* --- Scenario 5: Cache thrash --- */

#define CACHE_THRASH_COUNT   50
#define CACHE_THRASH_THREADS 4
#define CACHE_OBJ_PER_CACHE  100

typedef struct {
	umem_cache_t *caches[CACHE_THRASH_COUNT];
	pthread_mutex_t cache_locks[CACHE_THRASH_COUNT];
	int duration_sec;
	atomic_int active_caches;
	atomic_ulong total_ops;
	atomic_int errors;
	latency_tracker_t lat;
	pthread_mutex_t lat_lock;
} thrash_shared_t;

static void *
thrash_worker(void *arg)
{
	thrash_shared_t *shared = (thrash_shared_t *)arg;
	rng_t rng = { .state = (uint32_t)(uintptr_t)pthread_self() };
	uint64_t deadline = now_ns() +
	    (uint64_t)shared->duration_sec * 1000000000ULL;
	uint64_t ops = 0;

	while (now_ns() < deadline) {
		int idx = (int)rng_range(&rng, 0, CACHE_THRASH_COUNT - 1);

		pthread_mutex_lock(&shared->cache_locks[idx]);
		umem_cache_t *c = shared->caches[idx];
		pthread_mutex_unlock(&shared->cache_locks[idx]);

		if (!c)
			continue;

		/* Alloc batch from this cache */
		void *objs[CACHE_OBJ_PER_CACHE];
		int alloc_count = 0;

		for (int i = 0; i < CACHE_OBJ_PER_CACHE; i++) {
			uint64_t t0 = now_ns();
			objs[i] = umem_cache_alloc(c, UMEM_DEFAULT);
			uint64_t t1 = now_ns();
			if (!objs[i])
				break;
			alloc_count++;

			pthread_mutex_lock(&shared->lat_lock);
			lat_record(&shared->lat, t1 - t0);
			pthread_mutex_unlock(&shared->lat_lock);
		}

		/* Free them all */
		for (int i = 0; i < alloc_count; i++)
			umem_cache_free(c, objs[i]);

		ops += (uint64_t)alloc_count * 2;
	}

	atomic_fetch_add(&shared->total_ops, ops);
	return NULL;
}

static void
scenario_cache_thrash(int duration_sec, scenario_result_t *result)
{
	thrash_shared_t shared = {
		.duration_sec = duration_sec,
		.active_caches = 0,
		.total_ops = 0,
		.errors = 0,
		.lat_lock = PTHREAD_MUTEX_INITIALIZER,
	};
	lat_init(&shared.lat, 33333);

	for (int i = 0; i < CACHE_THRASH_COUNT; i++) {
		pthread_mutex_init(&shared.cache_locks[i], NULL);
		char name[64];
		snprintf(name, sizeof(name), "stress_cache_%d", i);
		size_t sz = 64 + (size_t)(i * 32);
		shared.caches[i] = umem_cache_create(name, sz, 0,
		    NULL, NULL, NULL, NULL, NULL, 0);
		if (shared.caches[i])
			atomic_fetch_add(&shared.active_caches, 1);
	}

	pthread_t threads[CACHE_THRASH_THREADS];
	uint64_t start = now_ns();

	for (int i = 0; i < CACHE_THRASH_THREADS; i++)
		pthread_create(&threads[i], NULL, thrash_worker, &shared);

	/* Periodically destroy and recreate some caches */
	rng_t rng = { .state = 55555 };
	uint64_t deadline = start + (uint64_t)duration_sec * 1000000000ULL;
	while (now_ns() < deadline) {
		usleep(500000); /* every 500ms */
		int idx = (int)rng_range(&rng, 0, CACHE_THRASH_COUNT - 1);

		pthread_mutex_lock(&shared.cache_locks[idx]);
		if (shared.caches[idx]) {
			umem_cache_destroy(shared.caches[idx]);
			shared.caches[idx] = NULL;
			atomic_fetch_sub(&shared.active_caches, 1);
		}

		char name[64];
		snprintf(name, sizeof(name), "stress_cache_%d_v2", idx);
		size_t sz = 64 + (size_t)(idx * 32);
		shared.caches[idx] = umem_cache_create(name, sz, 0,
		    NULL, NULL, NULL, NULL, NULL, 0);
		if (shared.caches[idx])
			atomic_fetch_add(&shared.active_caches, 1);
		pthread_mutex_unlock(&shared.cache_locks[idx]);
	}

	for (int i = 0; i < CACHE_THRASH_THREADS; i++)
		pthread_join(threads[i], NULL);

	uint64_t elapsed = now_ns() - start;

	/* Cleanup remaining caches */
	for (int i = 0; i < CACHE_THRASH_COUNT; i++) {
		if (shared.caches[i])
			umem_cache_destroy(shared.caches[i]);
		pthread_mutex_destroy(&shared.cache_locks[i]);
	}

	result->name = "cache_thrash";
	result->ops = atomic_load(&shared.total_ops);
	result->elapsed_sec = elapsed / 1e9;
	result->p99_ns = lat_percentile(&shared.lat, 0.99);
	result->peak_rss = get_rss_bytes();
	result->errors = atomic_load(&shared.errors);

	pthread_mutex_destroy(&shared.lat_lock);
}

/* --- Scenario 6: Fork under load --- */

#define FORK_THREADS     4
#define FORK_CHILD_ALLOCS 1000

typedef struct {
	int duration_sec;
	atomic_int keep_running;
	atomic_ulong total_ops;
	atomic_int errors;
} fork_shared_t;

static void *
fork_bg_worker(void *arg)
{
	fork_shared_t *shared = (fork_shared_t *)arg;
	rng_t rng = { .state = (uint32_t)(uintptr_t)pthread_self() };
	uint64_t ops = 0;

	while (atomic_load(&shared->keep_running)) {
		size_t sz = rng_range(&rng, 64, 1024);
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (p) {
			memset(p, 0xFF, sz);
			umem_free(p, sz);
			ops++;
		}
	}

	atomic_fetch_add(&shared->total_ops, ops);
	return NULL;
}

static void
scenario_fork_under_load(int duration_sec, scenario_result_t *result)
{
	fork_shared_t shared = {
		.duration_sec = duration_sec,
		.keep_running = 1,
		.total_ops = 0,
		.errors = 0,
	};
	latency_tracker_t lat;
	lat_init(&lat, 44444);

	pthread_t threads[FORK_THREADS];
	uint64_t start = now_ns();

	for (int i = 0; i < FORK_THREADS; i++)
		pthread_create(&threads[i], NULL, fork_bg_worker, &shared);

	uint64_t deadline = start + (uint64_t)duration_sec * 1000000000ULL;
	int fork_count = 0;
	int fork_errors = 0;

	while (now_ns() < deadline) {
		uint64_t t0 = now_ns();
		pid_t pid = fork();
		uint64_t t1 = now_ns();

		if (pid < 0) {
			fork_errors++;
			usleep(100000);
			continue;
		}

		if (pid == 0) {
			/* Child: allocate some objects, verify, exit */
			int child_ok = 1;
			for (int i = 0; i < FORK_CHILD_ALLOCS; i++) {
				void *p = umem_alloc(256, UMEM_DEFAULT);
				if (!p) {
					child_ok = 0;
					break;
				}
				memset(p, 0x42, 256);
				/* Verify pattern */
				unsigned char *bp = (unsigned char *)p;
				for (int j = 0; j < 256; j++) {
					if (bp[j] != 0x42) {
						child_ok = 0;
						break;
					}
				}
				umem_free(p, 256);
				if (!child_ok)
					break;
			}
			_exit(child_ok ? 0 : 1);
		}

		/* Parent: wait for child */
		int status;
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			fork_errors++;

		lat_record(&lat, t1 - t0);
		fork_count++;

		usleep(50000); /* 50ms between forks */
	}

	atomic_store(&shared.keep_running, 0);
	for (int i = 0; i < FORK_THREADS; i++)
		pthread_join(threads[i], NULL);

	uint64_t elapsed = now_ns() - start;

	result->name = "fork_load";
	result->ops = atomic_load(&shared.total_ops) + (uint64_t)fork_count;
	result->elapsed_sec = elapsed / 1e9;
	result->p99_ns = lat_percentile(&lat, 0.99);
	result->peak_rss = get_rss_bytes();
	result->errors = fork_errors + atomic_load(&shared.errors);
}

/* --- Scenario runner --- */

typedef void (*scenario_fn)(int duration_sec, scenario_result_t *result);

static void
run_scenario(const char *name, int duration_sec, scenario_fn fn,
    int *total_errors)
{
	printf("[%s] running for %d seconds...\n", name, duration_sec);
	fflush(stdout);

	scenario_result_t result;
	memset(&result, 0, sizeof(result));
	result.name = name;

	fn(duration_sec, &result);
	print_result(&result);

	*total_errors += result.errors;
}

/* --- Main --- */

static void
print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n", prog);
	printf("  -d SECONDS   Total duration (default: 60)\n");
	printf("  -h           Show help\n");
}

int
main(int argc, char *argv[])
{
	int duration = 60;
	int opt;

	while ((opt = getopt(argc, argv, "d:h")) != -1) {
		switch (opt) {
		case 'd':
			duration = atoi(optarg);
			if (duration < 6)
				duration = 6;
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	int per_scenario = duration / 6;
	if (per_scenario < 1)
		per_scenario = 1;

	printf("=== libumem stress test ===\n");
	printf("Total duration: %d seconds (%d per scenario)\n\n",
	    duration, per_scenario);

	int total_errors = 0;

	run_scenario("thread_churn", per_scenario,
	    scenario_thread_churn, &total_errors);
	run_scenario("mixed_sizes", per_scenario,
	    scenario_mixed_sizes, &total_errors);
	run_scenario("pressure", per_scenario,
	    scenario_memory_pressure, &total_errors);
	run_scenario("prodcons", per_scenario,
	    scenario_producer_consumer, &total_errors);
	run_scenario("cache_thrash", per_scenario,
	    scenario_cache_thrash, &total_errors);
	run_scenario("fork_load", per_scenario,
	    scenario_fork_under_load, &total_errors);

	printf("\n=== Summary ===\n");
	printf("Total errors: %d\n", total_errors);
	printf("Result: %s\n", total_errors == 0 ? "PASS" : "FAIL");

	return total_errors == 0 ? 0 : 1;
}

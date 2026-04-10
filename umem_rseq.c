/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

#include "config.h"
#include "umem_rseq.h"

#ifdef UMEM_RSEQ_AVAILABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <pthread.h>
#include "umem_impl.h"
#include "umem_base.h"

#ifndef __NR_rseq
#if defined(__x86_64__)
#define __NR_rseq 334
#elif defined(__aarch64__)
#define __NR_rseq 293
#elif defined(__i386__)
#define __NR_rseq 381
#else
#define __NR_rseq 0
#endif
#endif

#ifndef RSEQ_FLAG_UNREGISTER
#define RSEQ_FLAG_UNREGISTER 1
#endif

#ifndef RSEQ_CPU_ID_UNINITIALIZED
#define RSEQ_CPU_ID_UNINITIALIZED (~0U)
#endif

/*
 * glibc 2.35+ registers rseq for every thread automatically.
 * It exports __rseq_offset (offset into TLS) and __rseq_size
 * (size of the rseq area, 0 if not registered).
 *
 * We use weak symbols to detect glibc's rseq at runtime.
 */
extern __attribute__((weak)) ptrdiff_t __rseq_offset;
extern __attribute__((weak)) unsigned int __rseq_size;

/* Global state */
int umem_rseq_enabled = 0;
umem_rseq_cache_t *umem_rseq_caches = NULL;
static int umem_rseq_ncpus = 0;
static int umem_rseq_use_glibc = 0;
static pthread_key_t umem_rseq_key;

/* Per-thread rseq area (used only when glibc hasn't registered rseq) */
__thread struct umem_rseq umem_rseq_area __attribute__((aligned(32))) = {
	.cpu_id_start = RSEQ_CPU_ID_UNINITIALIZED,
	.cpu_id = RSEQ_CPU_ID_UNINITIALIZED,
	.rseq_cs = 0,
	.flags = 0,
	.pad = 0,
};

__thread int umem_rseq_registered = 0;
__thread volatile uint32_t *umem_rseq_cpu_idp = NULL;

static long
sys_rseq(struct umem_rseq *rseq_abi, uint32_t rseq_len,
    int flags, uint32_t sig)
{
	return syscall(__NR_rseq, rseq_abi, rseq_len, flags, sig);
}

/*
 * Get pointer to the thread's rseq area.
 * When glibc manages rseq, the area is at a TLS offset.
 * Otherwise, we use our own __thread variable.
 */
static inline struct umem_rseq *
umem_rseq_get_area(void)
{
	if (umem_rseq_use_glibc) {
		char *tp;
#if defined(__x86_64__)
		__asm__ volatile ("movq %%fs:0, %0" : "=r"(tp));
#elif defined(__aarch64__)
		__asm__ volatile ("mrs %0, tpidr_el0" : "=r"(tp));
#else
		return &umem_rseq_area;
#endif
		return (struct umem_rseq *)(tp + __rseq_offset);
	}
	return &umem_rseq_area;
}

static void
umem_rseq_thread_cleanup(void *arg)
{
	(void)arg;
	if (umem_rseq_registered && !umem_rseq_use_glibc) {
		umem_rseq_unregister_thread();
	}
}

int
umem_rseq_available(void)
{
	/*
	 * Check if glibc has already registered rseq (glibc 2.35+).
	 * If __rseq_size > 0, glibc manages rseq for all threads.
	 */
	if (&__rseq_size != NULL && __rseq_size > 0) {
		return 1;
	}

#ifdef __NR_rseq
	struct umem_rseq test_area __attribute__((aligned(32))) = {
		.cpu_id_start = RSEQ_CPU_ID_UNINITIALIZED,
		.cpu_id = RSEQ_CPU_ID_UNINITIALIZED,
		.rseq_cs = 0,
		.flags = 0,
	};

	long ret = sys_rseq(&test_area, sizeof(test_area), 0, 0);
	if (ret == 0) {
		sys_rseq(&test_area, sizeof(test_area),
		    RSEQ_FLAG_UNREGISTER, 0);
		return 1;
	}

	if (errno == EBUSY || errno == EINVAL) {
		return 1;
	}
#endif
	return 0;
}

int
umem_rseq_init(void)
{
	long ncpus;

	if (!umem_rseq_available()) {
		return -1;
	}

	/*
	 * Detect glibc rseq management.
	 * When glibc manages rseq, we don't need to register/unregister
	 * per-thread; glibc does it automatically.
	 */
	if (&__rseq_size != NULL && __rseq_size > 0) {
		umem_rseq_use_glibc = 1;
	}

	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	if (ncpus <= 0) {
		ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	}
	if (ncpus <= 0) {
		ncpus = 1;
	}

	umem_rseq_ncpus = (int)ncpus;

	umem_rseq_caches = calloc(umem_rseq_ncpus,
	    sizeof (umem_rseq_cache_t));
	if (umem_rseq_caches == NULL) {
		return -1;
	}

	if (pthread_key_create(&umem_rseq_key,
	    umem_rseq_thread_cleanup) != 0) {
		free(umem_rseq_caches);
		umem_rseq_caches = NULL;
		return -1;
	}

	umem_rseq_enabled = 1;
	return 0;
}

void
umem_rseq_fini(void)
{
	if (umem_rseq_caches != NULL) {
		free(umem_rseq_caches);
		umem_rseq_caches = NULL;
	}
	umem_rseq_enabled = 0;
	pthread_key_delete(umem_rseq_key);
}

int
umem_rseq_register_thread(void)
{
	if (umem_rseq_registered) {
		return 0;
	}

	/*
	 * When glibc manages rseq, the area is already registered.
	 * We just need to copy cpu_id into our thread-local area
	 * so umem_rseq_get_cpu() works.
	 */
	if (umem_rseq_use_glibc) {
		struct umem_rseq *glibc_area = umem_rseq_get_area();
		umem_rseq_cpu_idp = &glibc_area->cpu_id;
		umem_rseq_registered = 1;
		pthread_setspecific(umem_rseq_key, (void *)1);
		return 0;
	}

#ifdef __NR_rseq
	umem_rseq_area.cpu_id_start = RSEQ_CPU_ID_UNINITIALIZED;
	umem_rseq_area.cpu_id = RSEQ_CPU_ID_UNINITIALIZED;
	umem_rseq_area.rseq_cs = 0;
	umem_rseq_area.flags = 0;

	long ret = sys_rseq(&umem_rseq_area,
	    sizeof (umem_rseq_area), 0, 0);
	if (ret != 0) {
		if (errno == EBUSY) {
			/*
			 * Already registered (probably by glibc).
			 * Read glibc's cpu_id.
			 */
			umem_rseq_use_glibc = 1;
			return umem_rseq_register_thread();
		}
		if (errno != EINVAL) {
			return -1;
		}
	}

	umem_rseq_cpu_idp = &umem_rseq_area.cpu_id;
	umem_rseq_registered = 1;
	pthread_setspecific(umem_rseq_key, (void *)1);
	return 0;
#else
	return -1;
#endif
}

int
umem_rseq_unregister_thread(void)
{
	if (!umem_rseq_registered) {
		return 0;
	}

	if (umem_rseq_use_glibc) {
		umem_rseq_cpu_idp = NULL;
		umem_rseq_registered = 0;
		return 0;
	}

#ifdef __NR_rseq
	sys_rseq(&umem_rseq_area, sizeof (umem_rseq_area),
	    RSEQ_FLAG_UNREGISTER, 0);
	umem_rseq_cpu_idp = NULL;
	umem_rseq_registered = 0;
	return 0;
#else
	return -1;
#endif
}

/*
 * Assembly fastpath prototypes (architecture-specific)
 */
#if defined(__x86_64__) || defined(__aarch64__)
extern void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache,
    int cpu_id);
extern int umem_rseq_free_fastpath(umem_rseq_cache_t *cache,
    void *buf, int cpu_id);
#endif

void *
umem_cache_alloc_rseq(void *cp, int umflag)
{
	umem_rseq_cache_t *rseq_cache;
	int cpu_id;
	void *obj;

	if (!umem_rseq_registered) {
		if (umem_rseq_register_thread() != 0) {
			return NULL;
		}
	}

	cpu_id = umem_rseq_get_cpu();
	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus) {
		return umem_cache_alloc_rseq_slowpath(cp, -1, umflag);
	}

	rseq_cache = &umem_rseq_caches[cpu_id];

#if defined(__x86_64__) || defined(__aarch64__)
	obj = umem_rseq_alloc_fastpath(rseq_cache, cpu_id);
	if (obj != NULL) {
		return obj;
	}
	return umem_cache_alloc_rseq_slowpath(cp, cpu_id, umflag);
#else
	return umem_cache_alloc_rseq_slowpath(cp, cpu_id, umflag);
#endif
}

/*
 * Slow path for rseq allocation.
 *
 * Falls through to the main umem allocation path which handles
 * depot refill and slab allocation.
 */
void *
umem_cache_alloc_rseq_slowpath(void *cp, int cpu_id, int umflag)
{
	(void)cpu_id;
	return _umem_cache_alloc((umem_cache_t *)cp, umflag);
}

void
umem_cache_free_rseq(void *cp, void *buf)
{
	umem_rseq_cache_t *rseq_cache;
	int cpu_id;

	if (buf == NULL) {
		return;
	}

	if (!umem_rseq_registered) {
		if (umem_rseq_register_thread() != 0) {
			_umem_cache_free((umem_cache_t *)cp, buf);
			return;
		}
	}

	cpu_id = umem_rseq_get_cpu();
	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus) {
		umem_cache_free_rseq_slowpath(cp, -1, buf);
		return;
	}

	rseq_cache = &umem_rseq_caches[cpu_id];

#if defined(__x86_64__) || defined(__aarch64__)
	if (umem_rseq_free_fastpath(rseq_cache, buf, cpu_id) == 0) {
		return;
	}
	umem_cache_free_rseq_slowpath(cp, cpu_id, buf);
#else
	umem_cache_free_rseq_slowpath(cp, cpu_id, buf);
#endif
}

/*
 * Slow path for rseq free.
 *
 * Falls through to the main umem free path which handles
 * depot return and slab deallocation.
 */
void
umem_cache_free_rseq_slowpath(void *cp, int cpu_id, void *buf)
{
	(void)cpu_id;
	_umem_cache_free((umem_cache_t *)cp, buf);
}

void
umem_rseq_stats(int cpu_id, umem_rseq_cache_t *stats)
{
	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus || stats == NULL) {
		return;
	}
	memcpy(stats, &umem_rseq_caches[cpu_id],
	    sizeof (umem_rseq_cache_t));
}

void
umem_rseq_dump(void)
{
	int i;

	fprintf(stderr, "RSEQ State:\n");
	fprintf(stderr, "  Enabled: %d\n", umem_rseq_enabled);
	fprintf(stderr, "  Registered: %d\n", umem_rseq_registered);
	fprintf(stderr, "  CPUs: %d\n", umem_rseq_ncpus);
	fprintf(stderr, "  Using glibc rseq: %d\n", umem_rseq_use_glibc);

	if (umem_rseq_registered && umem_rseq_cpu_idp != NULL) {
		fprintf(stderr, "  Current CPU: %u\n",
		    *umem_rseq_cpu_idp);
	}

	if (umem_rseq_caches != NULL) {
		fprintf(stderr, "\nPer-CPU Statistics:\n");
		for (i = 0; i < umem_rseq_ncpus; i++) {
			umem_rseq_cache_t *cache = &umem_rseq_caches[i];
			if (cache->alloc_count == 0 &&
			    cache->free_count == 0) {
				continue;
			}
			fprintf(stderr, "  CPU %d:\n", i);
			fprintf(stderr, "    Allocs: %lu\n",
			    (unsigned long)cache->alloc_count);
			fprintf(stderr, "    Frees: %lu\n",
			    (unsigned long)cache->free_count);
			fprintf(stderr, "    Restarts: %lu\n",
			    (unsigned long)cache->restart_count);
			fprintf(stderr, "    Migrations: %lu\n",
			    (unsigned long)cache->migration_count);
		}
	}
}

#endif /* UMEM_RSEQ_AVAILABLE */

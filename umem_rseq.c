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

/* Syscall numbers for rseq */
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

/* RSEQ flags */
#ifndef RSEQ_FLAG_UNREGISTER
#define RSEQ_FLAG_UNREGISTER 1
#endif

#ifndef RSEQ_CPU_ID_UNINITIALIZED
#define RSEQ_CPU_ID_UNINITIALIZED (~0U)
#endif

/* Global state */
int umem_rseq_enabled = 0;
umem_rseq_cache_t *umem_rseq_caches = NULL;
static int umem_rseq_ncpus = 0;
static pthread_key_t umem_rseq_key;

/* Per-thread rseq area */
__thread struct umem_rseq umem_rseq_area __attribute__((aligned(32))) = {
	.cpu_id_start = RSEQ_CPU_ID_UNINITIALIZED,
	.cpu_id = RSEQ_CPU_ID_UNINITIALIZED,
	.rseq_cs = 0,
	.flags = 0,
	.pad = 0,
};

__thread int umem_rseq_registered = 0;

/*
 * Wrapper for rseq syscall
 */
static long
sys_rseq(struct umem_rseq *rseq_abi, uint32_t rseq_len, int flags, uint32_t sig)
{
	return syscall(__NR_rseq, rseq_abi, rseq_len, flags, sig);
}

/*
 * Thread cleanup callback
 */
static void
umem_rseq_thread_cleanup(void *arg)
{
	(void)arg;
	if (umem_rseq_registered) {
		umem_rseq_unregister_thread();
	}
}

int
umem_rseq_available(void)
{
#ifdef __NR_rseq
	struct umem_rseq test_area __attribute__((aligned(32))) = {
		.cpu_id_start = RSEQ_CPU_ID_UNINITIALIZED,
		.cpu_id = RSEQ_CPU_ID_UNINITIALIZED,
		.rseq_cs = 0,
		.flags = 0,
	};

	/* Try to register a test rseq area */
	long ret = sys_rseq(&test_area, sizeof(test_area), 0, 0);
	if (ret == 0) {
		/* Success, unregister it */
		sys_rseq(&test_area, sizeof(test_area), RSEQ_FLAG_UNREGISTER, 0);
		return 1;
	}

	/* EINVAL might mean already registered or not supported */
	if (errno == EINVAL) {
		/* Try to check if it's supported via sysfs */
		if (access("/sys/kernel/rseq", F_OK) == 0) {
			return 1;
		}
	}
#endif
	return 0;
}

int
umem_rseq_init(void)
{
	long ncpus;

	/* Check if rseq is available */
	if (!umem_rseq_available()) {
		return -1;
	}

	/* Get CPU count */
	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	if (ncpus <= 0) {
		ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	}
	if (ncpus <= 0) {
		ncpus = 1;
	}

	umem_rseq_ncpus = (int)ncpus;

	/* Allocate per-CPU caches */
	umem_rseq_caches = calloc(umem_rseq_ncpus, sizeof(umem_rseq_cache_t));
	if (umem_rseq_caches == NULL) {
		return -1;
	}

	/* Create thread-local cleanup key */
	if (pthread_key_create(&umem_rseq_key, umem_rseq_thread_cleanup) != 0) {
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
	long ret;

	if (umem_rseq_registered) {
		return 0;
	}

#ifdef __NR_rseq
	/* Initialize rseq area */
	umem_rseq_area.cpu_id_start = RSEQ_CPU_ID_UNINITIALIZED;
	umem_rseq_area.cpu_id = RSEQ_CPU_ID_UNINITIALIZED;
	umem_rseq_area.rseq_cs = 0;
	umem_rseq_area.flags = 0;

	/* Register with kernel */
	ret = sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area), 0, 0);
	if (ret != 0) {
		/* EINVAL might mean already registered */
		if (errno != EINVAL) {
			return -1;
		}
	}

	umem_rseq_registered = 1;

	/* Set cleanup handler */
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

#ifdef __NR_rseq
	sys_rseq(&umem_rseq_area, sizeof(umem_rseq_area),
	    RSEQ_FLAG_UNREGISTER, 0);
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
extern void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache, int cpu_id);
extern int umem_rseq_free_fastpath(umem_rseq_cache_t *cache, void *buf, int cpu_id);
#endif

/*
 * C implementation of rseq allocation
 *
 * Uses architecture-specific assembly for true restartable sequences on
 * x86_64 and aarch64. Falls back to regular allocation on other platforms.
 */
void *
umem_cache_alloc_rseq(void *cp, int umflag)
{
	umem_cache_t *cache = (umem_cache_t *)cp;
	umem_rseq_cache_t *rseq_cache;
	int cpu_id;
	void *obj;

	/* Register thread if not already done */
	if (!umem_rseq_registered) {
		if (umem_rseq_register_thread() != 0) {
			/* Fall back to regular allocation */
			return NULL;
		}
	}

	/* Get current CPU from rseq area */
	cpu_id = umem_rseq_get_cpu();
	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus) {
		return umem_cache_alloc_rseq_slowpath(cp, -1, umflag);
	}

	rseq_cache = &umem_rseq_caches[cpu_id];

#if defined(__x86_64__) || defined(__aarch64__)
	/*
	 * Fast path: use assembly implementation with true rseq critical section.
	 * The kernel will automatically restart if CPU migration occurs.
	 */
	obj = umem_rseq_alloc_fastpath(rseq_cache, cpu_id);
	if (obj != NULL) {
		return obj;
	}

	/* Magazine empty or rseq restarted - go to slow path */
	return umem_cache_alloc_rseq_slowpath(cp, cpu_id, umflag);
#else
	/*
	 * Fallback for other architectures: use regular magazine access
	 * with paranoid CPU checks. This is not true rseq but provides
	 * some protection against migration.
	 */
	umem_magazine_t *mag = (umem_magazine_t *)rseq_cache->loaded_mag;
	if (mag != NULL && rseq_cache->rounds > 0) {
		/* Check CPU didn't change */
		if (umem_rseq_get_cpu() != cpu_id) {
			rseq_cache->migration_count++;
			rseq_cache->restart_count++;
			return umem_cache_alloc_rseq_slowpath(cp, cpu_id, umflag);
		}

		obj = mag->mag_round[--rseq_cache->rounds];
		rseq_cache->alloc_count++;
		return obj;
	}

	return umem_cache_alloc_rseq_slowpath(cp, cpu_id, umflag);
#endif
}

void *
umem_cache_alloc_rseq_slowpath(void *cp, int cpu_id, int umflag)
{
	umem_cache_t *cache = (umem_cache_t *)cp;
	umem_rseq_cache_t *rseq_cache;
	umem_magazine_t *mag, *prev_mag;

	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus) {
		cpu_id = umem_rseq_get_cpu();
		if (cpu_id < 0) {
			/* Fall back to cache's regular allocation */
			return NULL;
		}
	}

	rseq_cache = &umem_rseq_caches[cpu_id];

	/* Try previous magazine */
	prev_mag = (umem_magazine_t *)rseq_cache->previous_mag;
	if (prev_mag != NULL && rseq_cache->prounds > 0) {
		/* Swap magazines */
		rseq_cache->previous_mag = rseq_cache->loaded_mag;
		rseq_cache->loaded_mag = prev_mag;
		int tmp = rseq_cache->prounds;
		rseq_cache->prounds = rseq_cache->rounds;
		rseq_cache->rounds = tmp;

		mag = prev_mag;
		void *obj = mag->mag_round[--rseq_cache->rounds];
		rseq_cache->alloc_count++;
		return obj;
	}

	/*
	 * Need to get a magazine from depot or allocate from slab.
	 * This would require integrating with the main umem depot logic.
	 * For now, return NULL to indicate slow path needed.
	 */
	return NULL;
}

void
umem_cache_free_rseq(void *cp, void *buf)
{
	umem_cache_t *cache = (umem_cache_t *)cp;
	umem_rseq_cache_t *rseq_cache;
	int cpu_id;
	int result;

	if (buf == NULL) {
		return;
	}

	/* Register thread if not already done */
	if (!umem_rseq_registered) {
		if (umem_rseq_register_thread() != 0) {
			return;
		}
	}

	/* Get current CPU */
	cpu_id = umem_rseq_get_cpu();
	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus) {
		umem_cache_free_rseq_slowpath(cp, -1, buf);
		return;
	}

	rseq_cache = &umem_rseq_caches[cpu_id];

#if defined(__x86_64__) || defined(__aarch64__)
	/*
	 * Fast path: use assembly implementation with true rseq critical section.
	 */
	result = umem_rseq_free_fastpath(rseq_cache, buf, cpu_id);
	if (result == 0) {
		return;  /* Success */
	}

	/* Magazine full or rseq restarted - go to slow path */
	umem_cache_free_rseq_slowpath(cp, cpu_id, buf);
#else
	/*
	 * Fallback for other architectures
	 */
	umem_magazine_t *mag = (umem_magazine_t *)rseq_cache->loaded_mag;
	if (mag != NULL && rseq_cache->rounds < cache->cache_magtype->mt_magsize) {
		/* Check CPU didn't change */
		if (umem_rseq_get_cpu() != cpu_id) {
			rseq_cache->migration_count++;
			rseq_cache->restart_count++;
			umem_cache_free_rseq_slowpath(cp, cpu_id, buf);
			return;
		}

		mag->mag_round[rseq_cache->rounds++] = buf;
		rseq_cache->free_count++;
		return;
	}

	umem_cache_free_rseq_slowpath(cp, cpu_id, buf);
#endif
}

void
umem_cache_free_rseq_slowpath(void *cp, int cpu_id, void *buf)
{
	umem_cache_t *cache = (umem_cache_t *)cp;
	umem_rseq_cache_t *rseq_cache;
	umem_magazine_t *mag, *prev_mag;

	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus) {
		cpu_id = umem_rseq_get_cpu();
		if (cpu_id < 0) {
			return;
		}
	}

	rseq_cache = &umem_rseq_caches[cpu_id];

	/* Try previous magazine */
	prev_mag = (umem_magazine_t *)rseq_cache->previous_mag;
	if (prev_mag != NULL && rseq_cache->prounds <
	    cache->cache_magtype->mt_magsize) {
		/* Swap magazines */
		rseq_cache->previous_mag = rseq_cache->loaded_mag;
		rseq_cache->loaded_mag = prev_mag;
		int tmp = rseq_cache->prounds;
		rseq_cache->prounds = rseq_cache->rounds;
		rseq_cache->rounds = tmp;

		mag = prev_mag;
		mag->mag_round[rseq_cache->rounds++] = buf;
		rseq_cache->free_count++;
		return;
	}

	/* Need to return magazine to depot - would integrate with main depot */
}

void
umem_rseq_stats(int cpu_id, umem_rseq_cache_t *stats)
{
	if (cpu_id < 0 || cpu_id >= umem_rseq_ncpus || stats == NULL) {
		return;
	}

	memcpy(stats, &umem_rseq_caches[cpu_id], sizeof(umem_rseq_cache_t));
}

void
umem_rseq_dump(void)
{
	int i;

	fprintf(stderr, "RSEQ State:\n");
	fprintf(stderr, "  Enabled: %d\n", umem_rseq_enabled);
	fprintf(stderr, "  Registered: %d\n", umem_rseq_registered);
	fprintf(stderr, "  CPUs: %d\n", umem_rseq_ncpus);

	if (umem_rseq_registered) {
		fprintf(stderr, "  Current CPU: %u\n", umem_rseq_area.cpu_id);
	}

	if (umem_rseq_caches != NULL) {
		fprintf(stderr, "\nPer-CPU Statistics:\n");
		for (i = 0; i < umem_rseq_ncpus; i++) {
			umem_rseq_cache_t *cache = &umem_rseq_caches[i];
			if (cache->alloc_count > 0 || cache->free_count > 0) {
				fprintf(stderr, "  CPU %d:\n", i);
				fprintf(stderr, "    Allocs: %lu\n", cache->alloc_count);
				fprintf(stderr, "    Frees: %lu\n", cache->free_count);
				fprintf(stderr, "    Restarts: %lu\n", cache->restart_count);
				fprintf(stderr, "    Migrations: %lu\n", cache->migration_count);
			}
		}
	}
}

/*
 * Assembly implementations would go here for x86_64 and aarch64.
 * These would use proper rseq critical sections with abort handlers.
 */

#endif /* UMEM_RSEQ_AVAILABLE */

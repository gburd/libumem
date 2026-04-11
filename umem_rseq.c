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
int umem_rseq_asm_safe = 0;
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

	if (pthread_key_create(&umem_rseq_key,
	    umem_rseq_thread_cleanup) != 0) {
		return -1;
	}

	umem_rseq_enabled = 1;
	umem_rseq_asm_safe = !umem_rseq_use_glibc;
	return 0;
}

void
umem_rseq_fini(void)
{
	umem_rseq_enabled = 0;
	pthread_key_delete(umem_rseq_key);
}

int
umem_rseq_get_ncpus(void)
{
	return umem_rseq_ncpus;
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

void
umem_rseq_stats(int cpu_id, umem_rseq_cache_t *stats)
{
	(void)cpu_id;
	(void)stats;
}

void
umem_rseq_dump(void)
{
	fprintf(stderr, "RSEQ State:\n");
	fprintf(stderr, "  Enabled: %d\n", umem_rseq_enabled);
	fprintf(stderr, "  Registered: %d\n", umem_rseq_registered);
	fprintf(stderr, "  CPUs: %d\n", umem_rseq_ncpus);
	fprintf(stderr, "  Using glibc rseq: %d\n", umem_rseq_use_glibc);

	if (umem_rseq_registered && umem_rseq_cpu_idp != NULL) {
		fprintf(stderr, "  Current CPU: %u\n",
		    *umem_rseq_cpu_idp);
	}
}

#endif /* UMEM_RSEQ_AVAILABLE */

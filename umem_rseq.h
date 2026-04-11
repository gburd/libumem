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

/*
 * Per-CPU Caching with Linux Restartable Sequences (rseq)
 *
 * This provides true lock-free per-CPU caching using the Linux kernel's
 * rseq system call (Linux 4.18+). Unlike sched_getcpu() which can race,
 * rseq provides atomic CPU-pinned operations that restart on migration.
 *
 * Performance: 50-200% improvement over per-thread caching at high thread
 * counts, zero synchronization overhead in the fast path.
 *
 * Requires: Linux 4.18+, glibc 2.35+ (or manual syscall)
 * Enable with: UMEM_OPTIONS=percpu=rseq
 */

#ifndef _UMEM_RSEQ_H
#define _UMEM_RSEQ_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Check for rseq support at compile time */
#ifdef __linux__
#ifdef HAVE_LINUX_RSEQ_H
#define UMEM_RSEQ_AVAILABLE 1
#endif
#endif

#ifdef UMEM_RSEQ_AVAILABLE

#include <stdint.h>
#include <stddef.h>
#include <linux/rseq.h>

/*
 * Restartable sequence area structure
 *
 * This structure is registered with the kernel via the rseq() syscall.
 * The kernel updates cpu_id atomically when the thread migrates to a
 * different CPU, and will abort any critical section in progress.
 */
struct umem_rseq {
	uint32_t cpu_id_start;	/* CPU at start of critical section */
	uint32_t cpu_id;	/* Current CPU ID (updated by kernel) */
	uint64_t rseq_cs;	/* Pointer to critical section descriptor */
	uint32_t flags;		/* RSEQ_FLAG_* flags */
	uint32_t pad;		/* Padding for alignment */
} __attribute__((aligned(32)));

/*
 * Critical section descriptor
 *
 * Describes a restartable sequence critical section to the kernel.
 * If the thread migrates during execution from start_ip to post_commit_offset,
 * the kernel will jump to abort_ip instead of post_commit_offset.
 */
struct umem_rseq_cs {
	uint32_t version;		/* Version (always 0) */
	uint32_t flags;			/* RSEQ_CS_FLAG_* flags */
	uint64_t start_ip;		/* Start of critical section */
	uint64_t post_commit_offset;	/* Offset from start_ip to commit */
	uint64_t abort_ip;		/* Abort handler address */
} __attribute__((aligned(32)));

/*
 * Per-CPU cache structure for rseq
 *
 * Each CPU has its own cache with magazines. Access is protected by
 * rseq critical sections instead of locks.
 */
typedef struct umem_rseq_cache {
	void *loaded_mag;	/* Currently loaded magazine */
	void *previous_mag;	/* Previously loaded magazine */
	int rounds;		/* Rounds in loaded magazine */
	int prounds;		/* Rounds in previous magazine */
	uint64_t alloc_count;	/* Allocation counter */
	uint64_t free_count;	/* Free counter */
	uint64_t restart_count;	/* Number of rseq restarts */
	uint64_t migration_count; /* CPU migrations detected */
	int magsize;		/* Max rounds (magazine capacity) */
	char pad[64 - (2 * sizeof(void*) + 2 * sizeof(int) +
	    4 * sizeof(uint64_t) + sizeof(int))];
} umem_rseq_cache_t __attribute__((aligned(64)));

/*
 * Global rseq state
 */
extern int umem_rseq_enabled;		/* rseq available and enabled */
extern int umem_rseq_asm_safe;		/* safe to use assembly fast path */
extern __thread int umem_rseq_registered;	/* Thread has registered rseq */
extern __thread struct umem_rseq umem_rseq_area; /* Per-thread rseq area */
extern __thread volatile uint32_t *umem_rseq_cpu_idp; /* Ptr to active cpu_id */

/*
 * umem_rseq_available - Check if rseq is available on this system
 *
 * Checks kernel support by looking for /sys/kernel/rseq/supported or
 * attempting a test registration. Called during umem initialization.
 *
 * Returns 1 if rseq is available, 0 otherwise.
 */
int umem_rseq_available(void);

/*
 * umem_rseq_init - Initialize rseq subsystem
 *
 * Detects CPU count, allocates per-CPU cache structures, and prepares
 * for thread registration. Called once during umem_init().
 *
 * Returns 0 on success, -1 on failure.
 */
int umem_rseq_init(void);

/*
 * umem_rseq_fini - Clean up rseq subsystem
 *
 * Releases per-CPU resources. Called during umem shutdown.
 */
void umem_rseq_fini(void);

/*
 * umem_rseq_register_thread - Register current thread with rseq
 *
 * Called once per thread (lazily on first allocation). Registers the
 * thread's rseq area with the kernel so cpu_id is kept up to date.
 *
 * Returns 0 on success, -1 on failure (falls back to regular caching).
 */
int umem_rseq_register_thread(void);

/*
 * umem_rseq_unregister_thread - Unregister current thread from rseq
 *
 * Called during thread cleanup. Unregisters the rseq area.
 *
 * Returns 0 on success, -1 on failure.
 */
int umem_rseq_unregister_thread(void);

/*
 * umem_rseq_get_cpu - Get current CPU ID via rseq
 *
 * Fast inline read of the kernel-maintained cpu_id field. This is much
 * faster than sched_getcpu() because it's a simple memory read.
 *
 * Returns CPU ID (0 to ncpus-1) or -1 if not registered.
 */
static inline int umem_rseq_get_cpu(void)
{
	if (!umem_rseq_registered || umem_rseq_cpu_idp == NULL) {
		return -1;
	}
	return (int)*umem_rseq_cpu_idp;
}

/*
 * Allocation and free with rseq critical sections
 *
 * These functions use rseq to guarantee that the CPU doesn't change
 * during cache access. If migration occurs, the operation restarts.
 */

/*
 * The rseq fast path (assembly) and slow path (depot reload) are
 * wired directly into _umem_cache_alloc() and _umem_cache_free()
 * in umem.c. Each umem_cache has a per-CPU array of umem_rseq_cache_t.
 */

/*
 * Statistics and debugging
 */

/*
 * umem_rseq_stats - Get rseq statistics for a CPU
 *
 * @param cpu_id CPU to query
 * @param stats  Output structure (caller-allocated)
 */
void umem_rseq_stats(int cpu_id, umem_rseq_cache_t *stats);

/*
 * umem_rseq_dump - Dump rseq state for debugging
 *
 * Prints rseq information to stderr.
 */
int umem_rseq_get_ncpus(void);

void umem_rseq_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* UMEM_RSEQ_AVAILABLE */

#endif /* _UMEM_RSEQ_H */

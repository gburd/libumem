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
 * Per-CPU Magazine Caching with NUMA Awareness
 *
 * This is an experimental feature that eliminates lock contention by using
 * true per-CPU magazines instead of per-thread-hash magazines.
 *
 * Enable with: ./configure --enable-per-cpu-cache
 */

#ifndef _UMEM_PERCPU_H
#define _UMEM_PERCPU_H

#include "config.h"
#include "umem_impl.h"

#ifdef UMEM_PER_CPU_CACHE

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

/*
 * Per-CPU cache structure
 *
 * Each CPU has its own cache structure with loaded/previous magazines.
 * This structure is cache-line aligned to prevent false sharing.
 *
 * Unlike the current umem_cpu_cache_t which uses locks, this structure
 * is lock-free in the fast path because only one thread runs on a CPU
 * at any given time (guaranteed by the OS).
 */
typedef struct umem_percpu_cache {
	umem_magazine_t	*pc_loaded;	/* currently loaded magazine */
	umem_magazine_t	*pc_previous;	/* previously loaded magazine */
	int		pc_rounds;	/* rounds in loaded magazine */
	int		pc_prounds;	/* rounds in previous magazine */
	int		pc_cpu_id;	/* actual CPU ID */
	int		pc_numa_node;	/* NUMA node this CPU belongs to */
	uint64_t	pc_alloc;	/* allocation counter */
	uint64_t	pc_free;	/* free counter */
	uint64_t	pc_depot_refill; /* depot refill counter */
	uint64_t	pc_slab_alloc;	/* slab allocation counter */

	/* Pad to cache line size to prevent false sharing */
	char		pc_pad[CACHE_LINE_SIZE -
			    (2 * sizeof(void*) +	/* magazines */
			     4 * sizeof(int) +		/* rounds, cpu_id, numa */
			     4 * sizeof(uint64_t))];	/* counters */
} umem_percpu_cache_t __attribute__((aligned(CACHE_LINE_SIZE)));

/*
 * NUMA statistics per cache
 */
typedef struct umem_numa_stats {
	uint64_t	ns_local_allocs;	/* allocations from local node */
	uint64_t	ns_remote_allocs;	/* allocations from remote node */
	uint64_t	ns_cross_node_misses;	/* magazine misses needing remote */
	uint64_t	ns_local_bytes;		/* bytes allocated locally */
	uint64_t	ns_remote_bytes;	/* bytes allocated remotely */
} umem_numa_stats_t;

/*
 * Global per-CPU state
 */
extern int umem_percpu_enabled;		/* per-CPU caching enabled */
extern int umem_numa_enabled;		/* NUMA awareness enabled */
extern int umem_num_nodes;		/* number of NUMA nodes */
extern umem_percpu_cache_t *umem_percpu_caches; /* per-CPU cache array */

/*
 * CPU and NUMA detection functions
 */

/*
 * get_current_cpu - Get the actual CPU this thread is running on
 *
 * Returns the CPU ID (0 to umem_max_ncpus-1) or -1 on error.
 * Uses sched_getcpu() on Linux, cpuset functions on FreeBSD.
 */
int get_current_cpu(void);

/*
 * get_numa_node - Get the NUMA node for a given CPU
 *
 * Returns the NUMA node ID or 0 if NUMA is not available.
 * Uses libnuma on Linux.
 */
int get_numa_node(int cpu_id);

/*
 * umem_init_percpu - Initialize per-CPU caching subsystem
 *
 * Called during umem initialization. Detects CPU count, NUMA topology,
 * and sets up per-CPU cache structures.
 *
 * Returns 0 on success, -1 on failure (falls back to current implementation).
 */
int umem_init_percpu(void);

/*
 * umem_fini_percpu - Clean up per-CPU caching subsystem
 *
 * Called during umem shutdown. Releases per-CPU resources.
 */
void umem_fini_percpu(void);

/*
 * Per-CPU allocation and free functions
 */

/*
 * umem_cache_alloc_percpu - Allocate from cache using per-CPU magazines
 *
 * This is the lock-free fast path. Gets current CPU, checks its loaded
 * magazine, and returns an object if available. Falls back to slowpath
 * if magazine is empty.
 *
 * @param cp     Cache to allocate from
 * @param umflag Allocation flags (UMEM_DEFAULT, UMEM_NOFAIL, etc.)
 *
 * @return Allocated object or NULL on failure
 */
void *umem_cache_alloc_percpu(umem_cache_t *cp, int umflag);

/*
 * umem_cache_alloc_percpu_slowpath - Handle magazine refill and slab allocation
 *
 * Called when the per-CPU magazine is empty. Swaps with previous magazine,
 * gets a full magazine from depot, or allocates from slab layer.
 *
 * @param cp     Cache to allocate from
 * @param cpu_id CPU ID (from get_current_cpu)
 * @param umflag Allocation flags
 *
 * @return Allocated object or NULL on failure
 */
void *umem_cache_alloc_percpu_slowpath(umem_cache_t *cp, int cpu_id, int umflag);

/*
 * umem_cache_free_percpu - Free to cache using per-CPU magazines
 *
 * Lock-free fast path. Gets current CPU, puts object in loaded magazine
 * if there's space. Falls back to slowpath if magazine is full.
 *
 * @param cp  Cache to free to
 * @param buf Object to free
 */
void umem_cache_free_percpu(umem_cache_t *cp, void *buf);

/*
 * umem_cache_free_percpu_slowpath - Handle magazine exchange and depot return
 *
 * Called when the per-CPU magazine is full. Swaps with previous magazine,
 * gets an empty magazine from depot, or returns to slab layer.
 *
 * @param cp     Cache to free to
 * @param cpu_id CPU ID (from get_current_cpu)
 * @param buf    Object to free
 */
void umem_cache_free_percpu_slowpath(umem_cache_t *cp, int cpu_id, void *buf);

/*
 * NUMA-aware allocation functions
 */

/*
 * umem_alloc_numa - Allocate memory from specific NUMA node
 *
 * Attempts to allocate from the specified NUMA node. Falls back to
 * regular allocation if NUMA is not available or allocation fails.
 *
 * @param size      Allocation size in bytes
 * @param numa_node NUMA node to allocate from (0-based)
 * @param umflag    Allocation flags
 *
 * @return Allocated memory or NULL on failure
 */
void *umem_alloc_numa(size_t size, int numa_node, int umflag);

/*
 * umem_slab_alloc_numa - Allocate slab from specific NUMA node
 *
 * Used by umem_slab_create to allocate slab memory from the local
 * NUMA node of the current CPU.
 *
 * @param size      Slab size in bytes
 * @param numa_node NUMA node to allocate from
 * @param umflag    Allocation flags
 *
 * @return Allocated slab memory or NULL on failure
 */
void *umem_slab_alloc_numa(size_t size, int numa_node, int umflag);

/*
 * Statistics and debugging
 */

/*
 * umem_percpu_stats - Get per-CPU cache statistics
 *
 * Returns statistics for a specific cache's per-CPU layer.
 *
 * @param cp      Cache to query
 * @param cpu_id  CPU ID to query
 * @param stats   Output structure (caller-allocated)
 */
void umem_percpu_stats(umem_cache_t *cp, int cpu_id,
    umem_percpu_cache_t *stats);

/*
 * umem_numa_stats - Get NUMA statistics for a cache
 *
 * Returns NUMA allocation statistics for a cache.
 *
 * @param cp    Cache to query
 * @param stats Output structure (caller-allocated)
 */
void umem_numa_stats(umem_cache_t *cp, umem_numa_stats_t *stats);

/*
 * umem_percpu_dump - Dump per-CPU cache state for debugging
 *
 * Prints per-CPU cache information to stderr.
 *
 * @param cp Cache to dump
 */
void umem_percpu_dump(umem_cache_t *cp);

/*
 * Internal helper macros
 */

/*
 * UMEM_PERCPU_CACHE - Get per-CPU cache structure for a cache
 *
 * @param cp     Cache pointer
 * @param cpu_id CPU ID
 *
 * @return Pointer to umem_percpu_cache_t for this CPU
 */
#define UMEM_PERCPU_CACHE(cp, cpu_id) \
	(&(cp)->cache_percpu[(cpu_id)])

/*
 * UMEM_PERCPU_ENABLED - Check if per-CPU caching is enabled
 *
 * Returns non-zero if per-CPU caching is active.
 */
#define UMEM_PERCPU_ENABLED() (umem_percpu_enabled)

/*
 * UMEM_NUMA_ENABLED - Check if NUMA awareness is enabled
 *
 * Returns non-zero if NUMA detection and allocation is active.
 */
#define UMEM_NUMA_ENABLED() (umem_numa_enabled)

#ifdef __cplusplus
}
#endif

#endif /* UMEM_PER_CPU_CACHE */

#endif /* _UMEM_PERCPU_H */

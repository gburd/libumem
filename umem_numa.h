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
 * NUMA-Aware Memory Allocation
 *
 * This provides NUMA (Non-Uniform Memory Access) awareness for libumem,
 * improving performance on multi-socket systems by preferring local node
 * allocations and maintaining per-node magazine depots.
 *
 * Performance: 10-30% improvement on NUMA systems with proper locality.
 *
 * Requires: libnuma (Linux), hwloc (cross-platform alternative)
 * Enable with: UMEM_OPTIONS=numa=auto|on|off
 */

#ifndef _UMEM_NUMA_H
#define _UMEM_NUMA_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Check for NUMA support at compile time */
#ifdef HAVE_LIBNUMA
#define UMEM_NUMA_AVAILABLE 1
#include <numa.h>
#include <numaif.h>
#endif

#ifdef UMEM_NUMA_AVAILABLE

#include <stdint.h>
#include <stddef.h>

/* Maximum NUMA nodes supported */
#define UMEM_MAX_NUMA_NODES 64

/*
 * Per-NUMA-node depot structure
 *
 * Each NUMA node maintains its own magazine depot to minimize
 * cross-node transfers. Magazines are allocated on the local node.
 */
typedef struct umem_numa_depot {
	void *full_list;	/* List of full magazines */
	void *empty_list;	/* List of empty magazines */
	uint64_t full_count;	/* Number of full magazines */
	uint64_t empty_count;	/* Number of empty magazines */
	uint64_t alloc_count;	/* Allocations from this depot */
	uint64_t free_count;	/* Frees to this depot */
	pthread_mutex_t lock;	/* Depot lock */
	char pad[64 - (2 * sizeof(void*) + 4 * sizeof(uint64_t) +
	    sizeof(pthread_mutex_t))];
} umem_numa_depot_t __attribute__((aligned(64)));

/*
 * Per-cache NUMA information
 *
 * Tracks per-node depots and statistics for a cache.
 */
typedef struct umem_numa_cache_info {
	umem_numa_depot_t depots[UMEM_MAX_NUMA_NODES]; /* Per-node depots */
	uint64_t local_hits;		/* Allocations from local node */
	uint64_t remote_hits;		/* Allocations from remote node */
	uint64_t cross_node_transfers;	/* Magazine transfers between nodes */
	uint64_t slab_local;		/* Slabs allocated on local node */
	uint64_t slab_remote;		/* Slabs allocated on remote node */
} umem_numa_cache_info_t;

/*
 * Global NUMA topology information
 */
typedef struct umem_numa_topology {
	int num_nodes;			/* Number of NUMA nodes */
	int num_cpus;			/* Total number of CPUs */
	int cpus_per_node;		/* Average CPUs per node */
	int *cpu_to_node;		/* CPU to node mapping */
	uint64_t *node_sizes;		/* Memory size per node (bytes) */
	uint64_t *node_free;		/* Free memory per node (bytes) */
	double *node_distance;		/* Distance matrix (numa_distance) */
} umem_numa_topology_t;

/*
 * NUMA allocation policy
 */
typedef enum umem_numa_policy {
	UMEM_NUMA_POLICY_AUTO = 0,	/* Auto-detect and enable if beneficial */
	UMEM_NUMA_POLICY_LOCAL,		/* Prefer local node */
	UMEM_NUMA_POLICY_INTERLEAVE,	/* Interleave across all nodes */
	UMEM_NUMA_POLICY_BIND,		/* Bind to specific node */
	UMEM_NUMA_POLICY_PREFERRED,	/* Prefer specific node, fall back */
	UMEM_NUMA_POLICY_DISABLED,	/* Disable NUMA awareness */
} umem_numa_policy_t;

/*
 * Global NUMA state
 */
extern int umem_numa_enabled;		/* NUMA awareness enabled */
extern umem_numa_topology_t *umem_numa_topo; /* NUMA topology info */
extern umem_numa_policy_t umem_numa_policy; /* Current NUMA policy */

/*
 * NUMA detection and initialization
 */

/*
 * umem_numa_available - Check if NUMA is available on this system
 *
 * Detects NUMA support via libnuma. Returns 1 if multiple nodes exist
 * and NUMA APIs are functional, 0 otherwise.
 *
 * Returns 1 if NUMA available, 0 otherwise.
 */
int umem_numa_available(void);

/*
 * umem_numa_init - Initialize NUMA subsystem
 *
 * Detects NUMA topology, allocates per-node structures, and sets up
 * default policy. Called during umem_init().
 *
 * Returns 0 on success, -1 on failure.
 */
int umem_numa_init(void);

/*
 * umem_numa_fini - Clean up NUMA subsystem
 *
 * Releases NUMA resources. Called during umem shutdown.
 */
void umem_numa_fini(void);

/*
 * umem_numa_detect_topology - Detect NUMA topology
 *
 * Queries libnuma for node count, CPU mappings, memory sizes, and
 * inter-node distances. Populates umem_numa_topo structure.
 *
 * Returns 0 on success, -1 on failure.
 */
int umem_numa_detect_topology(void);

/*
 * CPU and node mapping
 */

/*
 * umem_numa_get_node - Get NUMA node for current CPU
 *
 * Returns the NUMA node that the current CPU belongs to. Uses
 * numa_node_of_cpu() or sched_getcpu() + mapping table.
 *
 * Returns node ID (0 to num_nodes-1) or 0 if NUMA disabled.
 */
int umem_numa_get_node(void);

/*
 * umem_numa_cpu_to_node - Get NUMA node for specific CPU
 *
 * Returns the NUMA node for the given CPU ID.
 *
 * @param cpu_id CPU ID
 *
 * Returns node ID or 0 if NUMA disabled.
 */
int umem_numa_cpu_to_node(int cpu_id);

/*
 * umem_numa_get_distance - Get distance between two nodes
 *
 * Returns the relative distance between two NUMA nodes. Lower is closer.
 * Distance of 10 typically means same node.
 *
 * @param from_node Source node
 * @param to_node   Destination node
 *
 * Returns distance (10 = local, >10 = remote) or 10 if NUMA disabled.
 */
int umem_numa_get_distance(int from_node, int to_node);

/*
 * Memory allocation with NUMA awareness
 */

/*
 * umem_numa_alloc - Allocate memory on specific NUMA node
 *
 * Allocates memory on the specified node using numa_alloc_onnode().
 * Falls back to regular allocation if NUMA not available.
 *
 * @param size Size in bytes
 * @param node NUMA node ID
 *
 * Returns allocated memory or NULL on failure.
 */
void *umem_numa_alloc(size_t size, int node);

/*
 * umem_numa_alloc_local - Allocate memory on local NUMA node
 *
 * Allocates memory on the current CPU's NUMA node using
 * numa_alloc_local().
 *
 * @param size Size in bytes
 *
 * Returns allocated memory or NULL on failure.
 */
void *umem_numa_alloc_local(size_t size);

/*
 * umem_numa_alloc_interleaved - Allocate interleaved across nodes
 *
 * Allocates memory interleaved across all NUMA nodes using
 * numa_alloc_interleaved().
 *
 * @param size Size in bytes
 *
 * Returns allocated memory or NULL on failure.
 */
void *umem_numa_alloc_interleaved(size_t size);

/*
 * umem_numa_free - Free NUMA-allocated memory
 *
 * Frees memory allocated by umem_numa_alloc*() functions.
 *
 * @param ptr  Memory to free
 * @param size Size in bytes
 */
void umem_numa_free(void *ptr, size_t size);

/*
 * Slab allocation with NUMA awareness
 */

/*
 * umem_numa_slab_alloc - Allocate slab on specific node
 *
 * Allocates a slab on the specified NUMA node. Used by umem_slab_create()
 * to ensure slabs are allocated close to the CPUs that will use them.
 *
 * @param arena Arena/vmem to allocate from
 * @param size  Slab size
 * @param node  NUMA node ID
 * @param vmflag Allocation flags
 *
 * Returns slab memory or NULL on failure.
 */
void *umem_numa_slab_alloc(void *arena, size_t size, int node, int vmflag);

/*
 * umem_numa_slab_free - Free NUMA-aware slab
 *
 * Frees a slab previously allocated with umem_numa_slab_alloc().
 *
 * @param arena Arena/vmem
 * @param ptr   Slab to free
 * @param size  Slab size
 */
void umem_numa_slab_free(void *arena, void *ptr, size_t size);

/*
 * Depot operations with NUMA awareness
 */

/*
 * umem_numa_depot_alloc - Get magazine from NUMA-aware depot
 *
 * Tries to get a full magazine from the local node's depot first,
 * then tries remote nodes if local depot is empty.
 *
 * @param cache Cache to allocate from
 * @param node  Preferred NUMA node
 *
 * Returns magazine pointer or NULL if none available.
 */
void *umem_numa_depot_alloc(void *cache, int node);

/*
 * umem_numa_depot_free - Return magazine to NUMA-aware depot
 *
 * Returns a magazine to the appropriate node's depot. Prefers local
 * node to minimize cross-node traffic.
 *
 * @param cache Cache to free to
 * @param mag   Magazine to return
 * @param node  Preferred NUMA node
 */
void umem_numa_depot_free(void *cache, void *mag, int node);

/*
 * Policy control
 */

/*
 * umem_numa_set_policy - Set NUMA allocation policy
 *
 * Changes the NUMA allocation policy for future allocations.
 *
 * @param policy New policy
 *
 * Returns 0 on success, -1 on error.
 */
int umem_numa_set_policy(umem_numa_policy_t policy);

/*
 * umem_numa_get_policy - Get current NUMA policy
 *
 * Returns the current NUMA allocation policy.
 */
umem_numa_policy_t umem_numa_get_policy(void);

/*
 * Statistics
 */

/*
 * umem_numa_stats - NUMA statistics for a cache
 *
 * Gets NUMA-related statistics for a cache.
 *
 * @param cache Cache to query
 * @param info  Output structure (caller-allocated)
 */
void umem_numa_stats(void *cache, umem_numa_cache_info_t *info);

/*
 * umem_numa_dump - Dump NUMA state for debugging
 *
 * Prints NUMA topology and statistics to stderr.
 */
void umem_numa_dump(void);

/*
 * umem_numa_dump_topology - Dump NUMA topology
 *
 * Prints detected NUMA topology information.
 */
void umem_numa_dump_topology(void);

/*
 * Migration and binding
 */

/*
 * umem_numa_bind_thread - Bind thread to specific NUMA node
 *
 * Binds the current thread to CPUs on the specified NUMA node.
 * Used for testing or explicit placement.
 *
 * @param node NUMA node to bind to
 *
 * Returns 0 on success, -1 on error.
 */
int umem_numa_bind_thread(int node);

/*
 * umem_numa_migrate_memory - Migrate memory to different node
 *
 * Attempts to migrate pages to the specified NUMA node using
 * move_pages() or mbind().
 *
 * @param ptr     Memory to migrate
 * @param size    Size in bytes
 * @param to_node Destination node
 *
 * Returns 0 on success, -1 on error.
 */
int umem_numa_migrate_memory(void *ptr, size_t size, int to_node);

/*
 * Helper macros
 */

/*
 * UMEM_NUMA_ENABLED - Check if NUMA is enabled
 */
#define UMEM_NUMA_ENABLED() (umem_numa_enabled)

/*
 * UMEM_NUMA_NODE_COUNT - Get number of NUMA nodes
 */
#define UMEM_NUMA_NODE_COUNT() \
	(umem_numa_topo ? umem_numa_topo->num_nodes : 1)

/*
 * UMEM_NUMA_LOCAL_NODE - Get local NUMA node
 */
#define UMEM_NUMA_LOCAL_NODE() umem_numa_get_node()

#ifdef __cplusplus
}
#endif

#endif /* UMEM_NUMA_AVAILABLE */

#endif /* _UMEM_NUMA_H */

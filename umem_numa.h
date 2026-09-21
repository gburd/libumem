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
 * NUMA topology queries (libnuma).
 *
 * PRIVATE HEADER.  Not installed, not a public API.  It includes
 * "config.h", which is not installed either.
 *
 * SCOPE -- read this before using anything here.
 *
 * This file provides *topology information only*: node count, CPU-to-node
 * mapping, inter-node distance, and two explicit placement helpers
 * (bind-thread, migrate-pages) used by tests and benchmarks.  It does not
 * make libumem NUMA-aware, and nothing in the allocator's allocation or
 * depot paths calls into it.
 *
 * What used to be here and was removed on 2026-09-21, because it was
 * advertised but did not work:
 *
 *   - umem_numa_alloc()/umem_numa_free(): with NUMA enabled but the
 *     per-node allocation failing, alloc fell back to malloc() while free
 *     unconditionally called numa_free().  That is an allocator mismatch,
 *     i.e. heap corruption, on a path a caller could reach.
 *   - umem_numa_get_node(): documented as "the current CPU's NUMA node".
 *     It actually hashed pthread_self() into capacity-weighted partitions,
 *     which is a stable thread->node assignment unrelated to where the
 *     thread is running.  Callers relying on the documented meaning got
 *     silently wrong locality.
 *   - umem_numa_depot_alloc()/umem_numa_depot_free(): both began with a
 *     `numa_info = NULL; if (numa_info == NULL) return;` placeholder, so
 *     the per-node depots could never be reached.  The depot struct's
 *     padding expression (64 - (16 + 32 + sizeof(pthread_mutex_t))) is
 *     also negative on x86-64 glibc, i.e. an invalid array bound.
 *   - umem_numa_set_policy()/get_policy(): stored an enum that no code
 *     path ever dispatched on.
 *   - umem_numa_stats(): memset the caller's struct to zero and returned.
 *
 * Those are recoverable from git history; none of them should come back
 * as-is.
 *
 * KNOWN GAP (in umem.c, not here): umem.c tests #ifdef
 * UMEM_NUMA_AVAILABLE *before* including this header, and that macro is
 * defined by this header -- so the include never happens and
 * umem_numa_init() is never called.  Consequently umem_numa_enabled is
 * always 0 at runtime today and every function below is reachable only
 * from a direct caller (tests, benchmarks).  The live NUMA topology code
 * in the allocator is the separate HAVE_LIBNUMA block in umem.c that
 * fills umem_cpu_node[]; it does not go through this file.
 */

#ifndef _UMEM_NUMA_H
#define _UMEM_NUMA_H

#include "config.h"

/* Availability is decided by configure's libnuma check. */
#ifdef HAVE_LIBNUMA
#define UMEM_NUMA_AVAILABLE 1
#endif

#ifdef UMEM_NUMA_AVAILABLE

#include <numa.h>
#include <numaif.h>
#include <stdint.h>
#include <stddef.h>

/*
 * extern "C" opens and closes INSIDE the availability guard.  It used to
 * open above it and close below its #endif, so a C++ translation unit on a
 * host without libnuma saw an unbalanced brace.
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Detected NUMA topology.  Read-only after umem_numa_init().
 */
typedef struct umem_numa_topology {
	int num_nodes;			/* Number of NUMA nodes */
	int num_cpus;			/* Total number of CPUs */
	int cpus_per_node;		/* Average CPUs per node */
	int *cpu_to_node;		/* CPU to node mapping, num_cpus long */
	uint64_t *node_sizes;		/* Memory size per node (bytes) */
	double *node_distance;		/* num_nodes x num_nodes, row-major */
} umem_numa_topology_t;

/*
 * Set to 1 by umem_numa_init() only after the topology is fully
 * populated; 0 otherwise.  Every function below returns a neutral value
 * when it is 0.
 */
extern int umem_numa_enabled;
extern umem_numa_topology_t *umem_numa_topo;

/*
 * umem_numa_available - is there a usable multi-node topology here?
 *
 * Returns 1 only if libnuma reports itself functional AND more than one
 * node is configured (on a single-node box there is nothing to be aware
 * of).  Returns 0 otherwise.
 */
int umem_numa_available(void);

/*
 * umem_numa_init - detect and publish the topology.
 *
 * Returns 0 on success (umem_numa_enabled becomes 1), -1 on failure or if
 * the system has no multi-node topology (umem_numa_enabled stays 0).
 * Not called by the allocator today; see KNOWN GAP above.
 */
int umem_numa_init(void);

/*
 * umem_numa_fini - release the topology.  Sets umem_numa_enabled to 0.
 *
 * Not thread-safe against concurrent readers of umem_numa_topo; call it
 * only when no other thread is using the topology.
 */
void umem_numa_fini(void);

/*
 * umem_numa_detect_topology - populate umem_numa_topo.
 *
 * Idempotent: returns 0 immediately if the topology is already detected.
 * Returns 0 on success, -1 on failure (nothing is published on failure).
 */
int umem_numa_detect_topology(void);

/*
 * umem_numa_cpu_to_node - node that owns a given CPU.
 *
 * Returns 0 for an out-of-range CPU or when NUMA is not enabled.  Note
 * that 0 is also a valid node, so this is a hint, not an error channel.
 */
int umem_numa_cpu_to_node(int cpu_id);

/*
 * umem_numa_get_distance - relative distance between two nodes.
 *
 * Returns libnuma's numa_distance() value (10 = local by convention), or
 * 10 when NUMA is not enabled or either node is out of range.
 */
int umem_numa_get_distance(int from_node, int to_node);

/*
 * umem_numa_bind_thread - restrict the calling thread to one node's CPUs.
 *
 * Explicit placement, for tests and benchmarks.  Returns 0 on success,
 * -1 on error.
 */
int umem_numa_bind_thread(int node);

/*
 * umem_numa_migrate_memory - move pages of [ptr, ptr+size) to a node.
 *
 * Wraps mbind(MPOL_BIND, MPOL_MF_MOVE|MPOL_MF_STRICT).  The range must be
 * page-aligned for the kernel to accept it.  Returns 0 on success, -1 on
 * error.
 */
int umem_numa_migrate_memory(void *ptr, size_t size, int to_node);

/*
 * umem_numa_dump_topology / umem_numa_dump - print detected topology to
 * stderr.  Debugging aids; output format is not stable.
 */
void umem_numa_dump_topology(void);
void umem_numa_dump(void);

/*
 * UMEM_NUMA_NODE_COUNT - detected node count, or 1 before detection.
 */
#define UMEM_NUMA_NODE_COUNT() \
	(umem_numa_topo ? umem_numa_topo->num_nodes : 1)

#ifdef __cplusplus
}
#endif

#endif /* UMEM_NUMA_AVAILABLE */

#endif /* _UMEM_NUMA_H */

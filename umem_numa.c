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
#include "umem_numa.h"

#ifdef UMEM_NUMA_AVAILABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "umem_impl.h"
#include "umem_hash_partition.h"

/* Global state */
int umem_numa_enabled = 0;
umem_numa_topology_t *umem_numa_topo = NULL;
umem_numa_policy_t umem_numa_policy = UMEM_NUMA_POLICY_AUTO;

/* Hash partitions for fast NUMA node selection (no syscalls) */
static hash_partitions_t *numa_partitions = NULL;

int
umem_numa_available(void)
{
	/* Check if NUMA is available and has multiple nodes */
	if (numa_available() < 0) {
		return 0;
	}

	int num_nodes = numa_num_configured_nodes();
	if (num_nodes <= 1) {
		/* Single node system, NUMA not beneficial */
		return 0;
	}

	return 1;
}

int
umem_numa_detect_topology(void)
{
	int i, node;
	int num_nodes, num_cpus;

	if (umem_numa_topo != NULL) {
		return 0; /* Already detected */
	}

	num_nodes = numa_num_configured_nodes();
	if (num_nodes <= 0) {
		return -1;
	}

	num_cpus = numa_num_configured_cpus();
	if (num_cpus <= 0) {
		return -1;
	}

	/* Allocate topology structure */
	umem_numa_topo = calloc(1, sizeof(umem_numa_topology_t));
	if (umem_numa_topo == NULL) {
		return -1;
	}

	umem_numa_topo->num_nodes = num_nodes;
	umem_numa_topo->num_cpus = num_cpus;
	umem_numa_topo->cpus_per_node = num_cpus / num_nodes;

	/* Allocate CPU to node mapping */
	umem_numa_topo->cpu_to_node = calloc(num_cpus, sizeof(int));
	if (umem_numa_topo->cpu_to_node == NULL) {
		goto error;
	}

	/* Build CPU to node mapping */
	for (i = 0; i < num_cpus; i++) {
		node = numa_node_of_cpu(i);
		if (node < 0) {
			node = 0; /* Default to node 0 */
		}
		umem_numa_topo->cpu_to_node[i] = node;
	}

	/* Allocate node memory size arrays */
	umem_numa_topo->node_sizes = calloc(num_nodes, sizeof(uint64_t));
	umem_numa_topo->node_free = calloc(num_nodes, sizeof(uint64_t));
	if (umem_numa_topo->node_sizes == NULL ||
	    umem_numa_topo->node_free == NULL) {
		goto error;
	}

	/* Get memory sizes per node */
	for (i = 0; i < num_nodes; i++) {
		long long size = numa_node_size64(i, NULL);
		if (size > 0) {
			umem_numa_topo->node_sizes[i] = (uint64_t)size;
		}
	}

	/* Allocate distance matrix */
	umem_numa_topo->node_distance = calloc(num_nodes * num_nodes,
	    sizeof(double));
	if (umem_numa_topo->node_distance == NULL) {
		goto error;
	}

	/* Build distance matrix */
	for (i = 0; i < num_nodes; i++) {
		int j;
		for (j = 0; j < num_nodes; j++) {
			int distance = numa_distance(i, j);
			umem_numa_topo->node_distance[i * num_nodes + j] =
			    (double)distance;
		}
	}

	return 0;

error:
	if (umem_numa_topo != NULL) {
		free(umem_numa_topo->cpu_to_node);
		free(umem_numa_topo->node_sizes);
		free(umem_numa_topo->node_free);
		free(umem_numa_topo->node_distance);
		free(umem_numa_topo);
		umem_numa_topo = NULL;
	}
	return -1;
}

int
umem_numa_init(void)
{
	/* Check if NUMA is available */
	if (!umem_numa_available()) {
		return -1;
	}

	/* Detect topology */
	if (umem_numa_detect_topology() != 0) {
		return -1;
	}

	/* Set default policy based on detection */
	if (umem_numa_policy == UMEM_NUMA_POLICY_AUTO) {
		/* Use local policy by default */
		umem_numa_policy = UMEM_NUMA_POLICY_LOCAL;
	}

	/*
	 * Create hash partitions weighted by node memory capacity
	 * This allows fast thread-to-node mapping without syscalls
	 */
	int num_nodes = umem_numa_topo->num_nodes;
	claimant_weight_t *weights = malloc(num_nodes *
	    sizeof (claimant_weight_t));
	if (weights == NULL) {
		return -1;
	}

	for (int i = 0; i < num_nodes; i++) {
		snprintf(weights[i].name, MAX_NAME_LEN, "node_%d", i);
		/* Weight by memory capacity for optimal load balancing */
		weights[i].weight = (double)umem_numa_topo->node_sizes[i];
	}

	numa_partitions = hash_partitions_create_with_weights(weights,
	    num_nodes, 2);
	free(weights);

	if (numa_partitions == NULL) {
		return -1;
	}

	umem_numa_enabled = 1;
	return 0;
}

void
umem_numa_fini(void)
{
	if (numa_partitions != NULL) {
		hash_partitions_free(numa_partitions);
		numa_partitions = NULL;
	}

	if (umem_numa_topo != NULL) {
		free(umem_numa_topo->cpu_to_node);
		free(umem_numa_topo->node_sizes);
		free(umem_numa_topo->node_free);
		free(umem_numa_topo->node_distance);
		free(umem_numa_topo);
		umem_numa_topo = NULL;
	}
	umem_numa_enabled = 0;
}

int
umem_numa_get_node(void)
{
	if (!umem_numa_enabled || numa_partitions == NULL) {
		return 0;
	}

	/*
	 * Fast hash-based node selection (no syscalls)
	 * Uses thread ID for consistent mapping
	 */
	pthread_t tid = pthread_self();
	const char *node_name = hash_partitions_get_claimant_by_key(
	    numa_partitions, &tid, sizeof (tid));

	if (node_name == NULL) {
		return 0;
	}

	/* Parse "node_X" to extract node number */
	return atoi(node_name + 5);
}

int
umem_numa_cpu_to_node(int cpu_id)
{
	if (!umem_numa_enabled || umem_numa_topo == NULL) {
		return 0;
	}

	if (cpu_id < 0 || cpu_id >= umem_numa_topo->num_cpus) {
		return 0;
	}

	return umem_numa_topo->cpu_to_node[cpu_id];
}

int
umem_numa_get_distance(int from_node, int to_node)
{
	if (!umem_numa_enabled || umem_numa_topo == NULL) {
		return 10; /* Local distance */
	}

	int num_nodes = umem_numa_topo->num_nodes;
	if (from_node < 0 || from_node >= num_nodes ||
	    to_node < 0 || to_node >= num_nodes) {
		return 10;
	}

	return (int)umem_numa_topo->node_distance[from_node * num_nodes + to_node];
}

void *
umem_numa_alloc(size_t size, int node)
{
	if (!umem_numa_enabled) {
		return malloc(size);
	}

	if (node < 0 || node >= umem_numa_topo->num_nodes) {
		node = umem_numa_get_node();
	}

	void *ptr = numa_alloc_onnode(size, node);
	if (ptr == NULL) {
		/* Fall back to regular allocation */
		ptr = malloc(size);
	}

	return ptr;
}

void *
umem_numa_alloc_local(size_t size)
{
	if (!umem_numa_enabled) {
		return malloc(size);
	}

	void *ptr = numa_alloc_local(size);
	if (ptr == NULL) {
		ptr = malloc(size);
	}

	return ptr;
}

void *
umem_numa_alloc_interleaved(size_t size)
{
	if (!umem_numa_enabled) {
		return malloc(size);
	}

	void *ptr = numa_alloc_interleaved(size);
	if (ptr == NULL) {
		ptr = malloc(size);
	}

	return ptr;
}

void
umem_numa_free(void *ptr, size_t size)
{
	if (!umem_numa_enabled) {
		free(ptr);
		return;
	}

	numa_free(ptr, size);
}

void *
umem_numa_slab_alloc(void *arena, size_t size, int node, int vmflag)
{
	void *ptr;

	if (!umem_numa_enabled) {
		/* Fall back to regular vmem allocation */
		return NULL; /* Caller will use vmem_alloc */
	}

	if (node < 0) {
		node = umem_numa_get_node();
	}

	/* Allocate on specific node */
	ptr = numa_alloc_onnode(size, node);
	if (ptr == NULL) {
		return NULL;
	}

	return ptr;
}

void
umem_numa_slab_free(void *arena, void *ptr, size_t size)
{
	if (!umem_numa_enabled) {
		return; /* Caller will use vmem_free */
	}

	numa_free(ptr, size);
}

void *
umem_numa_depot_alloc(void *cache, int node)
{
	umem_cache_t *cp = (umem_cache_t *)cache;
	umem_numa_cache_info_t *numa_info;

	if (!umem_numa_enabled || cp == NULL) {
		return NULL;
	}

	/* Get NUMA info for cache - would need to be added to umem_cache_t */
	numa_info = NULL; /* Placeholder */
	if (numa_info == NULL) {
		return NULL;
	}

	if (node < 0) {
		node = umem_numa_get_node();
	}

	umem_numa_depot_t *depot = &numa_info->depots[node];

	/* Try to get from local depot */
	pthread_mutex_lock(&depot->lock);
	void *mag = depot->full_list;
	if (mag != NULL) {
		/* Remove from list - simplified */
		depot->full_list = NULL; /* Would be proper list removal */
		depot->full_count--;
		depot->alloc_count++;
		numa_info->local_hits++;
	}
	pthread_mutex_unlock(&depot->lock);

	if (mag != NULL) {
		return mag;
	}

	/* Try remote depots */
	int i;
	for (i = 0; i < umem_numa_topo->num_nodes; i++) {
		if (i == node) {
			continue; /* Already tried local */
		}

		depot = &numa_info->depots[i];
		pthread_mutex_lock(&depot->lock);
		mag = depot->full_list;
		if (mag != NULL) {
			depot->full_list = NULL;
			depot->full_count--;
			depot->alloc_count++;
			numa_info->remote_hits++;
			numa_info->cross_node_transfers++;
			pthread_mutex_unlock(&depot->lock);
			return mag;
		}
		pthread_mutex_unlock(&depot->lock);
	}

	return NULL; /* No magazines available */
}

void
umem_numa_depot_free(void *cache, void *mag, int node)
{
	umem_cache_t *cp = (umem_cache_t *)cache;
	umem_numa_cache_info_t *numa_info;

	if (!umem_numa_enabled || cp == NULL || mag == NULL) {
		return;
	}

	numa_info = NULL; /* Placeholder */
	if (numa_info == NULL) {
		return;
	}

	if (node < 0) {
		node = umem_numa_get_node();
	}

	umem_numa_depot_t *depot = &numa_info->depots[node];

	/* Return to local depot */
	pthread_mutex_lock(&depot->lock);
	/* Would add to list - simplified */
	depot->empty_list = mag; /* Would be proper list insertion */
	depot->empty_count++;
	depot->free_count++;
	pthread_mutex_unlock(&depot->lock);
}

int
umem_numa_set_policy(umem_numa_policy_t policy)
{
	if (!umem_numa_enabled) {
		return -1;
	}

	umem_numa_policy = policy;
	return 0;
}

umem_numa_policy_t
umem_numa_get_policy(void)
{
	return umem_numa_policy;
}

void
umem_numa_stats(void *cache, umem_numa_cache_info_t *info)
{
	if (info == NULL || !umem_numa_enabled) {
		return;
	}

	/* Would copy stats from cache's NUMA info */
	memset(info, 0, sizeof(umem_numa_cache_info_t));
}

void
umem_numa_dump_topology(void)
{
	int i, j;

	if (!umem_numa_enabled || umem_numa_topo == NULL) {
		fprintf(stderr, "NUMA not enabled\n");
		return;
	}

	fprintf(stderr, "NUMA Topology:\n");
	fprintf(stderr, "  Nodes: %d\n", umem_numa_topo->num_nodes);
	fprintf(stderr, "  CPUs: %d\n", umem_numa_topo->num_cpus);
	fprintf(stderr, "  CPUs per node: %d\n", umem_numa_topo->cpus_per_node);

	fprintf(stderr, "\nNode Memory Sizes:\n");
	for (i = 0; i < umem_numa_topo->num_nodes; i++) {
		fprintf(stderr, "  Node %d: %lu MB\n", i,
		    umem_numa_topo->node_sizes[i] / (1024 * 1024));
	}

	fprintf(stderr, "\nCPU to Node Mapping:\n");
	for (i = 0; i < umem_numa_topo->num_cpus; i++) {
		if (i % 8 == 0) {
			fprintf(stderr, "  CPUs %3d-%3d: ", i,
			    (i + 7 < umem_numa_topo->num_cpus) ?
			    i + 7 : umem_numa_topo->num_cpus - 1);
		}
		fprintf(stderr, "%d ", umem_numa_topo->cpu_to_node[i]);
		if ((i + 1) % 8 == 0 || i == umem_numa_topo->num_cpus - 1) {
			fprintf(stderr, "\n");
		}
	}

	fprintf(stderr, "\nNode Distance Matrix:\n");
	fprintf(stderr, "     ");
	for (i = 0; i < umem_numa_topo->num_nodes; i++) {
		fprintf(stderr, "%4d ", i);
	}
	fprintf(stderr, "\n");

	for (i = 0; i < umem_numa_topo->num_nodes; i++) {
		fprintf(stderr, "%4d ", i);
		for (j = 0; j < umem_numa_topo->num_nodes; j++) {
			int distance = (int)umem_numa_topo->node_distance[
			    i * umem_numa_topo->num_nodes + j];
			fprintf(stderr, "%4d ", distance);
		}
		fprintf(stderr, "\n");
	}
}

void
umem_numa_dump(void)
{
	fprintf(stderr, "NUMA State:\n");
	fprintf(stderr, "  Enabled: %d\n", umem_numa_enabled);
	fprintf(stderr, "  Policy: %d\n", (int)umem_numa_policy);

	if (umem_numa_enabled) {
		int current_node = umem_numa_get_node();
		fprintf(stderr, "  Current node: %d\n", current_node);
		fprintf(stderr, "\n");
		umem_numa_dump_topology();
	}
}

int
umem_numa_bind_thread(int node)
{
	if (!umem_numa_enabled) {
		return -1;
	}

	if (node < 0 || node >= umem_numa_topo->num_nodes) {
		return -1;
	}

	/* Bind thread to CPUs on the specified node */
	struct bitmask *cpus = numa_allocate_cpumask();
	if (cpus == NULL) {
		return -1;
	}

	numa_node_to_cpus(node, cpus);
	int ret = numa_sched_setaffinity(0, cpus);
	numa_free_cpumask(cpus);

	return ret;
}

int
umem_numa_migrate_memory(void *ptr, size_t size, int to_node)
{
	if (!umem_numa_enabled) {
		return -1;
	}

	if (to_node < 0 || to_node >= umem_numa_topo->num_nodes) {
		return -1;
	}

	/* Use mbind to migrate pages */
	struct bitmask *nodemask = numa_allocate_nodemask();
	if (nodemask == NULL) {
		return -1;
	}

	numa_bitmask_clearall(nodemask);
	numa_bitmask_setbit(nodemask, to_node);

	int ret = mbind(ptr, size, MPOL_BIND, nodemask->maskp,
	    nodemask->size + 1, MPOL_MF_MOVE | MPOL_MF_STRICT);

	numa_free_nodemask(nodemask);
	return ret;
}

#endif /* UMEM_NUMA_AVAILABLE */

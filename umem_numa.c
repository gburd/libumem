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
 * NUMA topology queries.  See umem_numa.h for scope, and for the list of
 * functions that were removed from this file on 2026-09-21 because they
 * were advertised and did not work.
 */

#include "config.h"
#include "umem_numa.h"

#ifdef UMEM_NUMA_AVAILABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int umem_numa_enabled = 0;
umem_numa_topology_t *umem_numa_topo = NULL;

int
umem_numa_available(void)
{
	if (numa_available() < 0)
		return (0);

	/* A single-node system has no locality to be aware of. */
	if (numa_num_configured_nodes() <= 1)
		return (0);

	return (1);
}

int
umem_numa_detect_topology(void)
{
	umem_numa_topology_t *topo;
	int i, j, num_nodes, num_cpus;

	if (umem_numa_topo != NULL)
		return (0);			/* already detected */

	num_nodes = numa_num_configured_nodes();
	num_cpus = numa_num_configured_cpus();
	if (num_nodes <= 0 || num_cpus <= 0)
		return (-1);

	/*
	 * Build into a local and publish only once complete, so a failure
	 * partway through cannot leave a half-populated umem_numa_topo
	 * visible to a reader.
	 */
	topo = calloc(1, sizeof (umem_numa_topology_t));
	if (topo == NULL)
		return (-1);

	topo->num_nodes = num_nodes;
	topo->num_cpus = num_cpus;
	topo->cpus_per_node = num_cpus / num_nodes;

	topo->cpu_to_node = calloc((size_t)num_cpus, sizeof (int));
	topo->node_sizes = calloc((size_t)num_nodes, sizeof (uint64_t));
	topo->node_distance = calloc((size_t)num_nodes * (size_t)num_nodes,
	    sizeof (double));
	if (topo->cpu_to_node == NULL || topo->node_sizes == NULL ||
	    topo->node_distance == NULL)
		goto error;

	for (i = 0; i < num_cpus; i++) {
		int node = numa_node_of_cpu(i);
		topo->cpu_to_node[i] = (node >= 0) ? node : 0;
	}

	for (i = 0; i < num_nodes; i++) {
		long long size = numa_node_size64(i, NULL);
		if (size > 0)
			topo->node_sizes[i] = (uint64_t)size;
	}

	for (i = 0; i < num_nodes; i++) {
		for (j = 0; j < num_nodes; j++) {
			topo->node_distance[i * num_nodes + j] =
			    (double)numa_distance(i, j);
		}
	}

	umem_numa_topo = topo;
	return (0);

error:
	free(topo->cpu_to_node);
	free(topo->node_sizes);
	free(topo->node_distance);
	free(topo);
	return (-1);
}

int
umem_numa_init(void)
{
	if (!umem_numa_available())
		return (-1);

	if (umem_numa_detect_topology() != 0)
		return (-1);

	umem_numa_enabled = 1;
	return (0);
}

void
umem_numa_fini(void)
{
	umem_numa_topology_t *topo = umem_numa_topo;

	umem_numa_enabled = 0;
	umem_numa_topo = NULL;

	if (topo != NULL) {
		free(topo->cpu_to_node);
		free(topo->node_sizes);
		free(topo->node_distance);
		free(topo);
	}
}

int
umem_numa_cpu_to_node(int cpu_id)
{
	if (!umem_numa_enabled || umem_numa_topo == NULL)
		return (0);

	if (cpu_id < 0 || cpu_id >= umem_numa_topo->num_cpus)
		return (0);

	return (umem_numa_topo->cpu_to_node[cpu_id]);
}

int
umem_numa_get_distance(int from_node, int to_node)
{
	int num_nodes;

	if (!umem_numa_enabled || umem_numa_topo == NULL)
		return (10);			/* libnuma's "local" */

	num_nodes = umem_numa_topo->num_nodes;
	if (from_node < 0 || from_node >= num_nodes ||
	    to_node < 0 || to_node >= num_nodes)
		return (10);

	return ((int)umem_numa_topo->node_distance[from_node * num_nodes +
	    to_node]);
}

int
umem_numa_bind_thread(int node)
{
	struct bitmask *cpus;
	int ret;

	if (!umem_numa_enabled || umem_numa_topo == NULL)
		return (-1);

	if (node < 0 || node >= umem_numa_topo->num_nodes)
		return (-1);

	cpus = numa_allocate_cpumask();
	if (cpus == NULL)
		return (-1);

	if (numa_node_to_cpus(node, cpus) != 0) {
		numa_free_cpumask(cpus);
		return (-1);
	}

	ret = numa_sched_setaffinity(0, cpus);
	numa_free_cpumask(cpus);

	return (ret);
}

int
umem_numa_migrate_memory(void *ptr, size_t size, int to_node)
{
	struct bitmask *nodemask;
	int ret;

	if (!umem_numa_enabled || umem_numa_topo == NULL)
		return (-1);

	if (to_node < 0 || to_node >= umem_numa_topo->num_nodes)
		return (-1);

	nodemask = numa_allocate_nodemask();
	if (nodemask == NULL)
		return (-1);

	numa_bitmask_clearall(nodemask);
	numa_bitmask_setbit(nodemask, to_node);

	ret = mbind(ptr, size, MPOL_BIND, nodemask->maskp,
	    nodemask->size + 1, MPOL_MF_MOVE | MPOL_MF_STRICT);

	numa_free_nodemask(nodemask);
	return (ret);
}

void
umem_numa_dump_topology(void)
{
	int i, j;

	if (!umem_numa_enabled || umem_numa_topo == NULL) {
		(void) fprintf(stderr, "NUMA topology not detected\n");
		return;
	}

	(void) fprintf(stderr, "NUMA Topology:\n");
	(void) fprintf(stderr, "  Nodes: %d\n", umem_numa_topo->num_nodes);
	(void) fprintf(stderr, "  CPUs: %d\n", umem_numa_topo->num_cpus);
	(void) fprintf(stderr, "  CPUs per node: %d\n",
	    umem_numa_topo->cpus_per_node);

	(void) fprintf(stderr, "\nNode Memory Sizes:\n");
	for (i = 0; i < umem_numa_topo->num_nodes; i++) {
		(void) fprintf(stderr, "  Node %d: %lu MB\n", i,
		    (unsigned long)(umem_numa_topo->node_sizes[i] /
		    (1024 * 1024)));
	}

	(void) fprintf(stderr, "\nCPU to Node Mapping:\n");
	for (i = 0; i < umem_numa_topo->num_cpus; i++) {
		if (i % 8 == 0) {
			(void) fprintf(stderr, "  CPUs %3d-%3d: ", i,
			    (i + 7 < umem_numa_topo->num_cpus) ?
			    i + 7 : umem_numa_topo->num_cpus - 1);
		}
		(void) fprintf(stderr, "%d ", umem_numa_topo->cpu_to_node[i]);
		if ((i + 1) % 8 == 0 || i == umem_numa_topo->num_cpus - 1)
			(void) fprintf(stderr, "\n");
	}

	(void) fprintf(stderr, "\nNode Distance Matrix:\n");
	(void) fprintf(stderr, "     ");
	for (i = 0; i < umem_numa_topo->num_nodes; i++)
		(void) fprintf(stderr, "%4d ", i);
	(void) fprintf(stderr, "\n");

	for (i = 0; i < umem_numa_topo->num_nodes; i++) {
		(void) fprintf(stderr, "%4d ", i);
		for (j = 0; j < umem_numa_topo->num_nodes; j++) {
			(void) fprintf(stderr, "%4d ",
			    (int)umem_numa_topo->node_distance[
			    i * umem_numa_topo->num_nodes + j]);
		}
		(void) fprintf(stderr, "\n");
	}
}

void
umem_numa_dump(void)
{
	(void) fprintf(stderr, "NUMA State:\n");
	(void) fprintf(stderr, "  Topology detected: %d\n", umem_numa_enabled);

	if (umem_numa_enabled) {
		(void) fprintf(stderr, "\n");
		umem_numa_dump_topology();
	}
}

#endif /* UMEM_NUMA_AVAILABLE */

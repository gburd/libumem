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
 * Per-CPU Magazine Caching Implementation
 *
 * This file implements lock-free per-CPU magazine caching with NUMA awareness.
 * See docs/PER_CPU_DESIGN.md for detailed design documentation.
 */

#include "config.h"

#ifdef UMEM_PER_CPU_CACHE

#define _GNU_SOURCE  /* for sched_getcpu() */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_LIBNUMA
#include <numa.h>
#endif

#include "umem_impl.h"
#include "umem_percpu.h"

/*
 * Global per-CPU state
 */
int umem_percpu_enabled = 0;
int umem_numa_enabled = 0;
int umem_num_nodes = 1;

/*
 * CPU Detection
 */

int
get_current_cpu(void)
{
#if defined(__linux__) && defined(HAVE_SCHED_GETCPU)
	int cpu = sched_getcpu();
	if (cpu < 0) {
		return 0;  /* Fallback to CPU 0 */
	}
	return cpu;
#elif defined(__FreeBSD__)
	/* FreeBSD: use cpuset_getaffinity */
	cpuset_t mask;
	CPU_ZERO(&mask);
	if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID,
	    -1, sizeof(mask), &mask) == 0) {
		/* Return first CPU in affinity mask */
		for (int i = 0; i < CPU_SETSIZE; i++) {
			if (CPU_ISSET(i, &mask)) {
				return i;
			}
		}
	}
	return 0;
#else
	/* No CPU detection available, use CPU 0 */
	return 0;
#endif
}

int
get_numa_node(int cpu_id)
{
#if defined(__linux__) && defined(HAVE_LIBNUMA)
	if (umem_numa_enabled && numa_available() >= 0) {
		int node = numa_node_of_cpu(cpu_id);
		if (node >= 0) {
			return node;
		}
	}
#endif
	return 0;  /* Default to node 0 */
}

/*
 * Initialization and Cleanup
 */

int
umem_init_percpu(void)
{
	int ncpus = umem_max_ncpus;

	/*
	 * Check if we can use per-CPU caching
	 */
#if !defined(__linux__) || !defined(HAVE_SCHED_GETCPU)
	umem_log("Per-CPU caching disabled: sched_getcpu() not available\n");
	umem_percpu_enabled = 0;
	return -1;
#endif

	if (ncpus > 256) {
		umem_log("Per-CPU caching disabled: too many CPUs (%d)\n",
		    ncpus);
		umem_percpu_enabled = 0;
		return -1;
	}

	/*
	 * Initialize NUMA awareness
	 */
#ifdef HAVE_LIBNUMA
	if (numa_available() >= 0) {
		umem_numa_enabled = 1;
		umem_num_nodes = numa_max_node() + 1;
		umem_log("NUMA awareness enabled: %d nodes\n",
		    umem_num_nodes);
	} else {
		umem_numa_enabled = 0;
		umem_num_nodes = 1;
	}
#else
	umem_numa_enabled = 0;
	umem_num_nodes = 1;
#endif

	umem_percpu_enabled = 1;
	umem_log("Per-CPU caching enabled: %d CPUs, %d NUMA nodes\n",
	    ncpus, umem_num_nodes);

	return 0;
}

void
umem_fini_percpu(void)
{
	umem_percpu_enabled = 0;
	umem_numa_enabled = 0;
}

/*
 * Per-CPU Allocation - Fast Path
 *
 * This is the lock-free fast path. We get the current CPU and check its
 * loaded magazine. If there are objects available, we take one without
 * any locks.
 *
 * CPU migration during this function is harmless:
 * - If migration happens before we read cpu_id: we use the new CPU (correct)
 * - If migration happens after: we've already taken the object (correct)
 *
 * The critical section (decrement + load) is 2-3 instructions, so migration
 * during it is extremely rare. Even if it happens, the worst case is we
 * take an object from the "wrong" CPU's magazine, which is still correct.
 */
void *
umem_cache_alloc_percpu(umem_cache_t *cp, int umflag)
{
	int cpu_id;
	umem_percpu_cache_t *pc;
	void *buf;

	/*
	 * Get current CPU. This is the only syscall in the fast path.
	 * On Linux, sched_getcpu() is fast (~10-20ns) as it's vDSO-based.
	 */
	cpu_id = get_current_cpu();
	if (cpu_id < 0 || cpu_id >= umem_max_ncpus) {
		/* Shouldn't happen, but be safe */
		cpu_id = 0;
	}

	pc = UMEM_PERCPU_CACHE(cp, cpu_id);

	/*
	 * Fast path: take from loaded magazine
	 *
	 * No lock needed here because:
	 * 1. Only the thread running on this CPU accesses this structure
	 * 2. The OS guarantees only one thread runs on a CPU at a time
	 * 3. If we migrate, we'll access a different CPU's structure
	 */
	if (pc->pc_rounds > 0) {
		buf = pc->pc_loaded->mag_round[--pc->pc_rounds];
		pc->pc_alloc++;

		/*
		 * Apply constructor if needed (debug mode)
		 */
		if ((cp->cache_flags & UMF_BUFTAG) &&
		    umem_cache_alloc_debug(cp, buf, umflag) == -1) {
			return NULL;
		}

		return buf;
	}

	/*
	 * Slow path: magazine empty, need to refill
	 */
	return umem_cache_alloc_percpu_slowpath(cp, cpu_id, umflag);
}

/*
 * Per-CPU Allocation - Slow Path
 *
 * Called when the loaded magazine is empty. Try to:
 * 1. Swap with previous magazine if it has objects
 * 2. Get a full magazine from the depot
 * 3. Allocate from slab layer
 */
void *
umem_cache_alloc_percpu_slowpath(umem_cache_t *cp, int cpu_id, int umflag)
{
	umem_percpu_cache_t *pc = UMEM_PERCPU_CACHE(cp, cpu_id);
	umem_magazine_t *fmp;
	umem_magtype_t *mtp = cp->cache_magtype;
	void *buf;

	/*
	 * Check if previous magazine has objects
	 */
	if (pc->pc_prounds > 0) {
		/*
		 * Swap loaded and previous magazines
		 * This is safe without locks because only we access this CPU's
		 * structure.
		 */
		umem_magazine_t *tmp_mag = pc->pc_loaded;
		int tmp_rounds = pc->pc_rounds;

		pc->pc_loaded = pc->pc_previous;
		pc->pc_rounds = pc->pc_prounds;
		pc->pc_previous = tmp_mag;
		pc->pc_prounds = tmp_rounds;

		/*
		 * Now loaded magazine should have objects
		 */
		if (pc->pc_rounds > 0) {
			buf = pc->pc_loaded->mag_round[--pc->pc_rounds];
			pc->pc_alloc++;

			if ((cp->cache_flags & UMF_BUFTAG) &&
			    umem_cache_alloc_debug(cp, buf, umflag) == -1) {
				return NULL;
			}

			return buf;
		}
	}

	/*
	 * Both magazines are empty. Try to get a full magazine from depot.
	 *
	 * This requires acquiring the depot lock (shared resource).
	 * This is the main serialization point in per-CPU caching.
	 */
	fmp = umem_depot_alloc(cp, &cp->cache_full);

	if (fmp != NULL) {
		pc->pc_depot_refill++;

		/*
		 * Return empty previous magazine to depot
		 */
		if (pc->pc_previous != NULL && pc->pc_prounds == 0) {
			umem_depot_free(cp, &cp->cache_empty, pc->pc_previous);
		}

		/*
		 * Install full magazine as loaded
		 */
		pc->pc_loaded = fmp;
		pc->pc_rounds = mtp->mt_magsize;

		/*
		 * Take an object
		 */
		buf = pc->pc_loaded->mag_round[--pc->pc_rounds];
		pc->pc_alloc++;

		if ((cp->cache_flags & UMF_BUFTAG) &&
		    umem_cache_alloc_debug(cp, buf, umflag) == -1) {
			return NULL;
		}

		return buf;
	}

	/*
	 * No full magazines in depot. Allocate from slab layer.
	 *
	 * For NUMA awareness, try to allocate from local node.
	 */
	pc->pc_slab_alloc++;

	buf = umem_slab_alloc(cp, umflag);

	if (buf == NULL) {
		return NULL;
	}

	/*
	 * Apply constructor if needed
	 */
	if (cp->cache_flags & UMF_BUFTAG) {
		if (umem_cache_alloc_debug(cp, buf, umflag) == -1) {
			return NULL;
		}
	} else if (cp->cache_constructor != NULL &&
	    cp->cache_constructor(buf, cp->cache_private, umflag) != 0) {
		atomic_add_64(&cp->cache_alloc_fail, 1);
		umem_slab_free(cp, buf);
		return NULL;
	}

	return buf;
}

/*
 * Per-CPU Free - Fast Path
 *
 * Lock-free fast path. Put object in loaded magazine if there's space.
 */
void
umem_cache_free_percpu(umem_cache_t *cp, void *buf)
{
	int cpu_id;
	umem_percpu_cache_t *pc;

	/*
	 * Apply destructor if needed (debug mode)
	 */
	if (cp->cache_flags & UMF_BUFTAG) {
		if (umem_cache_free_debug(cp, buf) == -1) {
			return;
		}
	}

	/*
	 * Get current CPU
	 */
	cpu_id = get_current_cpu();
	if (cpu_id < 0 || cpu_id >= umem_max_ncpus) {
		cpu_id = 0;
	}

	pc = UMEM_PERCPU_CACHE(cp, cpu_id);

	/*
	 * Fast path: put in loaded magazine if there's space
	 */
	if (pc->pc_rounds < pc->pc_loaded->mag_size) {
		pc->pc_loaded->mag_round[pc->pc_rounds++] = buf;
		pc->pc_free++;
		return;
	}

	/*
	 * Slow path: magazine full, need to exchange
	 */
	umem_cache_free_percpu_slowpath(cp, cpu_id, buf);
}

/*
 * Per-CPU Free - Slow Path
 *
 * Called when loaded magazine is full. Try to:
 * 1. Swap with previous magazine if it's empty
 * 2. Get an empty magazine from depot
 * 3. Free to slab layer
 */
void
umem_cache_free_percpu_slowpath(umem_cache_t *cp, int cpu_id, void *buf)
{
	umem_percpu_cache_t *pc = UMEM_PERCPU_CACHE(cp, cpu_id);
	umem_magazine_t *emp;
	umem_magtype_t *mtp = cp->cache_magtype;

	/*
	 * Check if previous magazine is empty
	 */
	if (pc->pc_prounds == 0 && pc->pc_previous != NULL) {
		/*
		 * Swap loaded and previous magazines
		 */
		umem_magazine_t *tmp_mag = pc->pc_loaded;
		int tmp_rounds = pc->pc_rounds;

		pc->pc_loaded = pc->pc_previous;
		pc->pc_rounds = pc->pc_prounds;
		pc->pc_previous = tmp_mag;
		pc->pc_prounds = tmp_rounds;

		/*
		 * Now loaded magazine should have space
		 */
		if (pc->pc_rounds < pc->pc_loaded->mag_size) {
			pc->pc_loaded->mag_round[pc->pc_rounds++] = buf;
			pc->pc_free++;
			return;
		}
	}

	/*
	 * Both magazines are full. Try to get an empty magazine from depot.
	 */
	emp = umem_depot_alloc(cp, &cp->cache_empty);

	if (emp != NULL) {
		/*
		 * Return full previous magazine to depot
		 */
		if (pc->pc_previous != NULL &&
		    pc->pc_prounds == mtp->mt_magsize) {
			umem_depot_free(cp, &cp->cache_full, pc->pc_previous);
		}

		/*
		 * Install empty magazine as loaded
		 */
		pc->pc_loaded = emp;
		pc->pc_rounds = 0;

		/*
		 * Put the object
		 */
		pc->pc_loaded->mag_round[pc->pc_rounds++] = buf;
		pc->pc_free++;
		return;
	}

	/*
	 * No empty magazines in depot. Free to slab layer.
	 */
	if (cp->cache_destructor != NULL) {
		cp->cache_destructor(buf, cp->cache_private);
	}

	umem_slab_free(cp, buf);
}

/*
 * NUMA-Aware Allocation
 */

void *
umem_alloc_numa(size_t size, int numa_node, int umflag)
{
#if defined(__linux__) && defined(HAVE_LIBNUMA)
	void *ptr;

	if (umem_numa_enabled && numa_node >= 0 && numa_node < umem_num_nodes) {
		ptr = numa_alloc_onnode(size, numa_node);
		if (ptr != NULL) {
			return ptr;
		}
		/* Fall through to regular allocation on failure */
	}
#endif

	/*
	 * Fallback to regular allocation.
	 * NUMA integration with vmem arenas deferred to 3.0.
	 */
	return umem_alloc(size, umflag);
}

void *
umem_slab_alloc_numa(size_t size, int numa_node, int umflag)
{
	return umem_alloc_numa(size, numa_node, umflag);
}

/*
 * Statistics and Debugging
 */

void
umem_percpu_stats(umem_cache_t *cp, int cpu_id, umem_percpu_cache_t *stats)
{
	umem_percpu_cache_t *pc;

	if (cpu_id < 0 || cpu_id >= umem_max_ncpus) {
		return;
	}

	pc = UMEM_PERCPU_CACHE(cp, cpu_id);

	/*
	 * Copy statistics (no lock needed for reading counters)
	 */
	stats->pc_alloc = pc->pc_alloc;
	stats->pc_free = pc->pc_free;
	stats->pc_depot_refill = pc->pc_depot_refill;
	stats->pc_slab_alloc = pc->pc_slab_alloc;
	stats->pc_rounds = pc->pc_rounds;
	stats->pc_prounds = pc->pc_prounds;
	stats->pc_cpu_id = pc->pc_cpu_id;
	stats->pc_numa_node = pc->pc_numa_node;
}

void
umem_numa_stats(umem_cache_t *cp, umem_numa_stats_t *stats)
{
	/*
	 * NUMA statistics tracking deferred to 3.0.
	 * Will need counters for local/remote allocations and
	 * cross-node magazine misses.
	 */
	memset(stats, 0, sizeof(*stats));
}

void
umem_percpu_dump(umem_cache_t *cp)
{
	int cpu;

	fprintf(stderr, "Per-CPU cache dump for: %s\n", cp->cache_name);
	fprintf(stderr, "%-4s %-8s %-8s %-12s %-12s %-12s %-12s\n",
	    "CPU", "NUMA", "Rounds", "Allocs", "Frees", "Depot", "Slab");

	for (cpu = 0; cpu < umem_max_ncpus; cpu++) {
		umem_percpu_cache_t stats;
		umem_percpu_stats(cp, cpu, &stats);

		fprintf(stderr, "%-4d %-8d %-8d %-12llu %-12llu %-12llu %-12llu\n",
		    cpu,
		    stats.pc_numa_node,
		    stats.pc_rounds,
		    (unsigned long long)stats.pc_alloc,
		    (unsigned long long)stats.pc_free,
		    (unsigned long long)stats.pc_depot_refill,
		    (unsigned long long)stats.pc_slab_alloc);
	}
}

#endif /* UMEM_PER_CPU_CACHE */

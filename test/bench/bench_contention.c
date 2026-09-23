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
 * bench_contention - Workstream D1 contention diagnosis harness.
 *
 * Runs the umem `multi` workload (all threads alloc+free the same size-class)
 * at a caller-chosen thread count, then dumps the already-tracked contention
 * counters via umem_dump_contention():
 *   - per-CPU rseq alloc/free/restart counts (rseq_rstrt is the migration-abort
 *     signal: high => threads keep migrating and restarting the lock-free CS)
 *   - depot full/empty magazine reload counts (magazine thrash signal)
 *   - depot local/remote hits + trylock-fail contention count (depot-lock
 *     serialization signal)
 *
 * This lets us attribute the `multi` scaling cliff (see
 * docs/results/2026-07-23-baseline.md) to rseq aborts vs. depot lock vs.
 * magazine thrash, with counter evidence rather than guesswork.
 *
 * Usage: bench_contention [-t threads] [-n ops] [-s min:max] [-w multi|prodcons]
 *   Run under the same pinning as matrix.sh, e.g.
 *     numactl --physcpubind=0-127 --localalloc -- bench_contention -t 128
 *
 * -w prodcons runs the same cross-thread producer/consumer workload as
 * scripts/ec2/sustained_load.sh's prodcons-sustained point (see
 * docs/results/2026-09-08-allocator-shootout.md sec 7 and
 * docs/results/2026-09-09-sustained-depot-contention-diagnosis.md), so the
 * depot/rseq counters below can be attributed to the *sustained cross-thread*
 * path specifically, not just the same-CPU `multi` path D1 diagnosed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench_framework.h"
#include "../../umem.h"

int main(int argc, char *argv[])
{
	int thread_count = 8;
	uint64_t operation_count = 10000000;
	size_t min_size = 64, max_size = 256;
	const char *workload_name = "multi";
	const char *alloc_name = "umem";

	int opt;
	while ((opt = getopt(argc, argv, "a:t:n:s:w:h")) != -1) {
		switch (opt) {
		case 'a':
			alloc_name = optarg;
			break;
		case 't':
			thread_count = atoi(optarg);
			if (thread_count < 1)
				thread_count = 1;
			break;
		case 'n':
			operation_count = strtoull(optarg, NULL, 10);
			break;
		case 's': {
			char *colon = strchr(optarg, ':');
			if (colon) {
				*colon = '\0';
				min_size = strtoull(optarg, NULL, 10);
				max_size = strtoull(colon + 1, NULL, 10);
			}
			break;
		}
		case 'w':
			workload_name = optarg;
			break;
		case 'h':
		default:
			printf("Usage: %s [-a umem|umem-preload] [-t threads] "
			    "[-n ops] [-s min:max] [-w multi|prodcons|frag]\n",
			    argv[0]);
			return (opt == 'h' ? 0 : 1);
		}
	}

	int is_prodcons = !strcmp(workload_name, "prodcons");
	int is_frag = !strcmp(workload_name, "frag");

	/*
	 * -a umem-preload: measure through libumem_malloc.so's malloc()/free()
	 * (requires LD_PRELOAD of that library; allocators.c detects it).  The
	 * contention counters live in libumem.so either way, so the dump below
	 * attributes the drop-in path too.
	 */
	allocator_ops_t *ops = &allocator_umem;
	if (!strcmp(alloc_name, "umem-preload")) {
		ops = &allocator_umem_preload;
		if (ops->alloc == NULL) {
			fprintf(stderr, "umem-preload not available: run with "
			    "LD_PRELOAD=.libs/libumem_malloc.so\n");
			return (77);
		}
	} else if (strcmp(alloc_name, "umem") != 0) {
		fprintf(stderr, "-a must be umem or umem-preload\n");
		return (1);
	}

	/*
	 * operation_count is the TOTAL budget; workload_multi_thread and
	 * workload_producer_consumer each divide it by their own thread count
	 * (subject to BENCH_MIN_OPS_PER_THREAD).  Do NOT pre-divide here: that
	 * was the P2.1 double division.
	 */
	workload_config_t wl = {
		.name = is_prodcons ? "producer-consumer" :
		    is_frag ? "fragmentation" : "multi-thread",
		.fn = is_prodcons ? workload_producer_consumer :
		    is_frag ? workload_fragmentation : workload_multi_thread,
		.thread_count = thread_count,
		.operation_count = operation_count,
		.min_size = min_size,
		.max_size = max_size,
		.custom_data = NULL,
	};

	bench_stats_t stats;
	if (bench_run_n(ops, &wl, &stats, 0, 1) != 0) {
		fprintf(stderr, "bench run failed\n");
		return (1);
	}

	printf("# %s %s t=%d size=%zu:%zu ops=%llu alloc_failures=%llu\n",
	    alloc_name, workload_name, thread_count, min_size, max_size,
	    (unsigned long long)stats.total_operations,
	    (unsigned long long)stats.alloc_failures);
	printf("throughput_mops = %.3f\n", stats.ops_per_second / 1e6);
	printf("lat_p50_ns = %.0f\n", stats.latency_p50);
	printf("lat_p99_ns = %.0f\n", stats.latency_p99);
	printf("lat_p999_ns = %.0f\n", stats.latency_p999);
	printf("lat_max_ns = %.0f\n", stats.latency_max);
	fflush(stdout);

	umem_dump_contention(stdout);
	return (0);
}

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
 * Usage: bench_contention [-t threads] [-n ops] [-s min:max]
 *   Run under the same pinning as matrix.sh, e.g.
 *     numactl --physcpubind=0-127 --localalloc -- bench_contention -t 128
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

	int opt;
	while ((opt = getopt(argc, argv, "t:n:s:h")) != -1) {
		switch (opt) {
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
		case 'h':
		default:
			printf("Usage: %s [-t threads] [-n ops] [-s min:max]\n",
			    argv[0]);
			return (opt == 'h' ? 0 : 1);
		}
	}

	/* Divide ops across threads (matches matrix.sh multi semantics). */
	workload_config_t wl = {
		.name = "multi-thread",
		.fn = workload_multi_thread,
		.thread_count = thread_count,
		.operation_count = operation_count / (uint64_t)thread_count,
		.min_size = min_size,
		.max_size = max_size,
		.custom_data = NULL,
	};

	bench_stats_t stats;
	if (bench_run_n(&allocator_umem, &wl, &stats, 0, 1) != 0) {
		fprintf(stderr, "bench run failed\n");
		return (1);
	}

	printf("# umem multi t=%d size=%zu:%zu ops=%llu\n",
	    thread_count, min_size, max_size,
	    (unsigned long long)stats.total_operations);
	printf("throughput_mops = %.3f\n", stats.ops_per_second / 1e6);
	printf("lat_p50_ns = %.0f\n", stats.latency_p50);
	printf("lat_p99_ns = %.0f\n", stats.latency_p99);
	printf("lat_p999_ns = %.0f\n", stats.latency_p999);
	printf("lat_max_ns = %.0f\n", stats.latency_max);
	fflush(stdout);

	umem_dump_contention(stdout);
	return (0);
}

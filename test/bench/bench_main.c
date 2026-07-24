/*
 * Main benchmark runner
 */

#include "bench_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#define HISTORY_FILENAME "test/bench/results/history.toml"

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nOptions:\n");
    printf("  -a ALLOCATOR  Test specific allocator (libc,umem,jemalloc,tcmalloc,mimalloc,all)\n");
    printf("  -w WORKLOAD   Run specific workload (single,multi,prodcons,frag,all)\n");
    printf("  -t THREADS    Thread count for multithreaded workloads (default: CPU count)\n");
    printf("  -n COUNT      Operation count (default: 1000000)\n");
    printf("  -s MIN:MAX    Size range in bytes (default: 16:1024)\n");
    printf("  -r RUNS       Measured runs; report median + CoV (default: 1)\n");
    printf("  -W WARMUPS    Warm-up runs to discard before measuring (default: 0)\n");
    printf("  -c            Output CSV format\n");
    printf("  -H            Print CSV header only and exit\n");
    printf("  --compare     Compare results against historical data\n");
    printf("  --save        Save results to history file\n");
    printf("  -h            Show this help\n");
    printf("\nExample:\n");
    printf("  %s -a umem -w multi -t 8 -n 10000000\n", prog);
}

int main(int argc, char *argv[]) {
    const char *allocator_name = "all";
    const char *workload_name = "all";
    int thread_count = sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t operation_count = 1000000;
    size_t min_size = 16;
    size_t max_size = 1024;
    int runs = 1;
    int warmups = 0;
    bool csv_output = false;
    bool header_only = false;
    bool do_compare = false;
    bool do_save = false;

    /* Handle long options manually before getopt */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--compare") == 0) {
            do_compare = true;
            argv[i] = (char *)"-?";  /* consumed */
        } else if (strcmp(argv[i], "--save") == 0) {
            do_save = true;
            argv[i] = (char *)"-?";
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "a:w:t:n:s:r:W:cHh?")) != -1) {
        switch (opt) {
        case 'a':
            allocator_name = optarg;
            break;
        case 'w':
            workload_name = optarg;
            break;
        case 't':
            thread_count = atoi(optarg);
            if (thread_count < 1) thread_count = 1;
            break;
        case 'n':
            operation_count = strtoull(optarg, NULL, 10);
            break;
        case 'r':
            runs = atoi(optarg);
            if (runs < 1) runs = 1;
            break;
        case 'W':
            warmups = atoi(optarg);
            if (warmups < 0) warmups = 0;
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
        case 'c':
            csv_output = true;
            break;
        case 'H':
            header_only = true;
            csv_output = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        case '?':
            break;  /* Consumed long options */
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* If header-only requested, print and exit */
    if (header_only) {
        bench_print_csv_header();
        return 0;
    }

    /* Define allocators to test */
    allocator_ops_t *allocators[] = {
        &allocator_libc,
        &allocator_umem,
        &allocator_jemalloc,
        &allocator_tcmalloc,
        &allocator_mimalloc,
        NULL
    };

    /* Define workloads */
    workload_config_t workloads[] = {
        {
            .name = "single-thread",
            .fn = workload_single_thread,
            .thread_count = 1,
            .operation_count = operation_count,
            .min_size = min_size,
            .max_size = max_size,
            .custom_data = NULL
        },
        {
            .name = "multi-thread",
            .fn = workload_multi_thread,
            .thread_count = thread_count,
            .operation_count = operation_count / thread_count,
            .min_size = min_size,
            .max_size = max_size,
            .custom_data = NULL
        },
        {
            .name = "producer-consumer",
            .fn = workload_producer_consumer,
            .thread_count = thread_count,
            .operation_count = operation_count,
            .min_size = min_size,
            .max_size = max_size,
            .custom_data = NULL
        },
        {
            .name = "fragmentation",
            .fn = workload_fragmentation,
            .thread_count = 1,
            .operation_count = operation_count,
            .min_size = min_size,
            .max_size = max_size,
            .custom_data = NULL
        }
    };
    int num_workloads = sizeof(workloads) / sizeof(workloads[0]);

    /* Don't print header here - use -H flag for header-only output */

    /* Run benchmarks */
    for (int i = 0; allocators[i] != NULL; i++) {
        allocator_ops_t *alloc = allocators[i];

        /* Skip unavailable allocators */
        if (alloc->alloc == NULL) {
            continue;
        }

        /* Filter by allocator name */
        if (strcmp(allocator_name, "all") != 0 &&
            strcmp(allocator_name, alloc->name) != 0) {
            continue;
        }

        for (int j = 0; j < num_workloads; j++) {
            workload_config_t *workload = &workloads[j];

            /* Filter by workload name */
            if (strcmp(workload_name, "all") != 0) {
                if (strcmp(workload_name, "single") == 0 &&
                    strcmp(workload->name, "single-thread") != 0) {
                    continue;
                }
                if (strcmp(workload_name, "multi") == 0 &&
                    strcmp(workload->name, "multi-thread") != 0) {
                    continue;
                }
                if (strcmp(workload_name, "prodcons") == 0 &&
                    strcmp(workload->name, "producer-consumer") != 0) {
                    continue;
                }
                if (strcmp(workload_name, "frag") == 0 &&
                    strcmp(workload->name, "fragmentation") != 0) {
                    continue;
                }
            }

            bench_stats_t stats;
            if (bench_run_n(alloc, workload, &stats, warmups, runs) == 0) {
                if (csv_output) {
                    bench_print_csv_row(&stats);
                } else {
                    bench_print_stats(&stats);
                }
                if (do_compare) {
                    bench_compare_history(&stats, HISTORY_FILENAME);
                }
                if (do_save) {
                    bench_append_history(&stats, HISTORY_FILENAME);
                }
            }
        }
    }

    return 0;
}

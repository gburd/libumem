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
    printf("  -a ALLOCATOR  Test specific allocator (libc,umem,umem-preload,jemalloc,tcmalloc,mimalloc,snmalloc,scudo,rpmalloc,all)\n");
    printf("                umem-preload = plain malloc()/free() with LD_PRELOAD=libumem_malloc.so,\n");
    printf("                i.e. what a drop-in user gets; umem = the API + 16-byte wrapper header.\n");
    printf("  -w WORKLOAD   Run specific workload (single,multi,prodcons,frag,all)\n");
    printf("  -t THREADS    Thread count for multithreaded workloads (default: CPU count)\n");
    printf("  -n TOTAL_OPS  TOTAL operations across ALL threads (default: 1000000).\n");
    printf("                Each workload divides this by its own thread count;\n");
    printf("                do NOT pre-divide.  A per-thread share below %d is\n",
           BENCH_MIN_OPS_PER_THREAD);
    printf("                raised to that floor (a microsecond-scale point\n");
    printf("                measures scheduling noise, not the allocator), and the\n");
    printf("                raise is reported as ops_floor_raised=1.\n");
    printf("  -s MIN:MAX    Size range in bytes (default: 16:1024)\n");
    printf("  -r RUNS       Measured runs; report median + CoV (default: 1)\n");
    printf("  -W WARMUPS    Warm-up runs to discard before measuring (default: 0)\n");
    printf("  -A            Emit EVERY measured run as its own CSV row (windows)\n");
    printf("                instead of only the median.  Each row carries that\n");
    printf("                window's own latency percentiles and RSS, so a\n");
    printf("                sustained run can be read as a time series instead of\n");
    printf("                one whole-run aggregate.  Requires -c.  window= is\n");
    printf("                appended to the workload name.\n");
    printf("  -D SEC:COUNT  Interval mode (prodcons, frag): ONE process runs COUNT\n");
    printf("                consecutive SEC-second intervals, ends by time (not\n");
    printf("                by -n), and prints one CSV row per interval\n");
    printf("                (workload name + /interval=K).  -W K drops the first\n");
    printf("                K intervals' rows (they still run: they ARE the\n");
    printf("                warm-up of the same process).  -n only sizes frag's\n");
    printf("                live pool.  Requires -c; excludes -A and -r.\n");
    printf("  -c            Output CSV format\n");
    printf("  -H            Print CSV header only and exit\n");
    printf("  --compare     Compare results against historical data\n");
    printf("  --save        Save results to history file\n");
    printf("  -h            Show this help\n");
    printf("\nUnits:\n");
    printf("  -n is TOTAL operations, not per-thread.  The CSV reports both\n");
    printf("  total_ops and ops_per_thread so the two can never be confused.\n");
    printf("  'frag' reports fragmentation = peak RSS / live bytes at that same\n");
    printf("  instant.  single/multi/prodcons hold no live set, so they report\n");
    printf("  NO fragmentation value (the frag CSV column is empty for them).\n");
    printf("\nExample:\n");
    printf("  %s -a umem -w multi -t 8 -n 10000000   # 10M ops total, 1.25M/thread\n", prog);
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
    bool emit_each_run = false;
    int iv_count = 0;
    double iv_sec = 0;

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
    while ((opt = getopt(argc, argv, "a:w:t:n:s:r:W:D:AcHh?")) != -1) {
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
        case 'D': {
            char *colon = strchr(optarg, ':');
            iv_sec = strtod(optarg, NULL);
            iv_count = colon ? atoi(colon + 1) : 0;
            if (iv_sec <= 0 || iv_count < 1) {
                fprintf(stderr, "-D wants SEC:COUNT, e.g. -D 60:15\n");
                return 1;
            }
            break;
        }
        case 'c':
            csv_output = true;
            break;
        case 'A':
            emit_each_run = true;
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

    if (iv_count > 0 && (!csv_output || emit_each_run || runs > 1 ||
                         warmups >= iv_count)) {
        fprintf(stderr, "-D requires -c, excludes -A and -r, and needs "
                "-W < COUNT\n");
        return 1;
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
        &allocator_umem_preload,
        &allocator_jemalloc,
        &allocator_tcmalloc,
        &allocator_mimalloc,
        &allocator_snmalloc,
        &allocator_scudo,
        &allocator_rpmalloc,
        NULL
    };

    /* Define workloads.
     *
     * operation_count is the TOTAL budget in every entry.  Each workload
     * divides by its own thread count internally (bench_ops_per_thread).
     * This used to pre-divide for 'multi' -- on top of matrix.sh already
     * dividing -- so aggregate work fell as 1/threads^2 (P2.1). */
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
            .operation_count = operation_count,
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
            /* Honours -t: it used to hard-code 1 and report threads=1 while
             * the docs described a 192-thread fragmentation workload. */
            .name = "fragmentation",
            .fn = workload_fragmentation,
            .thread_count = thread_count,
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
            if (iv_count > 0) {
                /* One process, time-bounded; see workload_config_t.  Only
                 * the two workloads that implement it are accepted. */
                if (workload->fn != workload_producer_consumer &&
                    workload->fn != workload_fragmentation) {
                    if (strcmp(workload_name, "all") != 0) {
                        fprintf(stderr, "-D supports prodcons and frag only\n");
                        return 1;
                    }
                    continue;
                }
                bench_stats_t *iv = calloc((size_t)iv_count, sizeof(*iv));
                if (!iv) return 1;
                workload->intervals = iv_count;
                workload->interval_sec = iv_sec;
                workload->interval_out = iv;
                bench_run(alloc, workload, &stats);
                char wname[128];
                for (int k = warmups; k < iv_count; k++) {
                    snprintf(wname, sizeof(wname), "%s/interval=%d",
                             workload->name, k);
                    iv[k].workload_name = wname;
                    iv[k].allocator_name = alloc->name;
                    bench_print_csv_row(&iv[k]);
                }
                fflush(stdout);
                free(iv);
                workload->intervals = 0;
                workload->interval_out = NULL;
                continue;
            }
            if (emit_each_run) {
                /* Per-window mode: discard the warm-ups, then emit one row
                 * per measured run.  Each row is a window with its OWN
                 * latency distribution and RSS -- a whole-run aggregate
                 * cannot show tail latency or RSS growing over time, which
                 * is the whole point of a sustained run. */
                if (!csv_output) {
                    fprintf(stderr, "-A requires -c (CSV output)\n");
                    return 1;
                }
                for (int w = 0; w < warmups; w++) {
                    bench_stats_t scratch;
                    bench_run(alloc, workload, &scratch);
                }
                char wname[128];
                const char *base = workload->name;
                for (int r = 0; r < runs; r++) {
                    if (bench_run(alloc, workload, &stats) != 0)
                        continue;
                    snprintf(wname, sizeof(wname), "%s/window=%d", base, r);
                    stats.workload_name = wname;
                    stats.runs_measured = 1;
                    bench_print_csv_row(&stats);
                    fflush(stdout);
                }
                continue;
            }
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

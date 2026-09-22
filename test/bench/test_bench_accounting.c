/*
 * test_bench_accounting.c -- regression for the measurement defects P2.1 and
 * P2.2 in docs/plans/2026-09-21-production-readiness.md.
 *
 * These are harness defects, so the check is on the harness.  Each assertion
 * below FAILS against the pre-2026-09-22 bench_framework/bench_main and
 * passes after.  A deterministic mock allocator stands in for the real one so
 * the assertions are about accounting, not about performance.
 *
 * WHAT EACH CASE PINS DOWN
 *
 * P2.1 total-vs-per-thread budget
 *   workload_config.operation_count is a TOTAL across all threads.  Run the
 *   multi workload with the same total at 1, 2 and 4 threads: total_operations
 *   must stay ~constant, bounded on BOTH sides.  The lower bound catches the
 *   budget being divided more than once (matrix.sh divided, then bench_main.c
 *   divided again, so aggregate work fell as 1/threads^2).  The upper bound
 *   catches the workload treating operation_count as per-thread, which is what
 *   it did pre-fix and what made pre-dividing in every caller look necessary.
 *   The end-to-end version of this, through matrix.sh itself, is
 *   test/bench/check_budget.sh.
 *
 * P2.1 work floor
 *   A total budget too small to divide meaningfully must be raised to
 *   BENCH_MIN_OPS_PER_THREAD per thread and flagged, never silently run as a
 *   microsecond-scale point.
 *
 * P2.2 live bytes
 *   The fragmentation workload's reported live_bytes_at_peak must never exceed
 *   the bytes that could possibly be live at once (pool capacity x max size).
 *   Pre-fix, immediately-freed allocations were added to the live total, so it
 *   grew without bound with the length of the run and exceeded that ceiling by
 *   orders of magnitude.
 *
 * P2.2 peak RSS pairing
 *   peak_rss_bytes must be a sample taken DURING the run, paired with the live
 *   bytes at that instant, not post-cleanup RSS.  Checked as: the ratio equals
 *   peak_rss/live_at_peak exactly (it was previously peak_frag computed from
 *   one pair while peak_rss_bytes held a different, later sample).
 *
 * P2.2 the ratio must not be selected by maximising itself
 *   The FIRST attempt at this fix reported the sample with the worst rss/live
 *   ratio.  RSS is near-monotonic, so that rule finds the sample with the
 *   smallest DENOMINATOR, not the most overhead; at 192 threads it reported a
 *   ratio of 505 while implied RSS stayed flat at ~1.1GB across every thread
 *   count.  Guarded here by requiring the reported denominator to be the
 *   MAXIMUM of the live series (live_bytes_at_peak == peak_live_bytes, and
 *   >= the series median), so a dip cannot be selected.
 *
 * P2.2 a single sample is not a distribution
 *   Aggregate live bytes MOVES (threads desynchronise at high counts), so the
 *   summary must rest on a series, and the series size must be reported.  A
 *   run with fewer than 8 samples must not claim a defined ratio.
 *
 * P2.2 undefined ratios are absent, not zero
 *   single/multi/prodcons hold no live set, so has_fragmentation must be 0 and
 *   no ratio may be reported.  Pre-fix they reported RSS / CUMULATIVE
 *   allocation traffic, a number that tends to zero the longer the run.
 *
 * P2.2 honest thread count
 *   The fragmentation workload must report the number of threads it actually
 *   ran.  Pre-fix bench_main hard-coded 1 while docs described 192.
 *
 * Exit: 0 all pass, 1 any fail.
 */

#include "bench_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			printf("  FAIL: ");                                \
			printf(__VA_ARGS__);                               \
			printf("\n        (%s:%d: %s)\n", __FILE__,        \
			    __LINE__, #cond);                              \
			failures++;                                        \
		}                                                          \
	} while (0)

/* Deterministic mock: plain malloc/free.  The assertions are about the
 * harness's arithmetic, so the allocator only has to be correct and fast. */
static void *mock_alloc(size_t n) { return malloc(n); }
static void *mock_calloc(size_t n, size_t s) { return calloc(n, s); }
static void *mock_realloc(void *p, size_t n) { return realloc(p, n); }
static void mock_free(void *p) { free(p); }

static allocator_ops_t mock = {
	.name = "mock",
	.alloc = mock_alloc,
	.calloc = mock_calloc,
	.realloc = mock_realloc,
	.free = mock_free,
	.cleanup = NULL
};

static bench_stats_t
run(const char *name, workload_fn fn, int threads, uint64_t total_ops,
    size_t min_sz, size_t max_sz)
{
	workload_config_t cfg = {
		.name = name,
		.fn = fn,
		.thread_count = threads,
		.operation_count = total_ops,
		.min_size = min_sz,
		.max_size = max_sz,
		.custom_data = NULL
	};
	bench_stats_t st;
	if (bench_run(&mock, &cfg, &st) != 0) {
		printf("  FAIL: bench_run(%s) failed\n", name);
		failures++;
		memset(&st, 0, sizeof(st));
	}
	return (st);
}

/*
 * P2.1: operation_count is a TOTAL.  Same total, more threads => the same
 * aggregate work, not less.  Uses a total comfortably above
 * 4 * BENCH_MIN_OPS_PER_THREAD so the floor never applies here.
 */
static void
test_ops_budget_is_total(void)
{
	const uint64_t total = (uint64_t)BENCH_MIN_OPS_PER_THREAD * 8;
	printf("P2.1 operation_count is a total across threads (n=%llu)\n",
	    (unsigned long long)total);

	bench_stats_t t1 = run("multi-thread", workload_multi_thread, 1,
	    total, 64, 64);
	bench_stats_t t2 = run("multi-thread", workload_multi_thread, 2,
	    total, 64, 64);
	bench_stats_t t4 = run("multi-thread", workload_multi_thread, 4,
	    total, 64, 64);

	printf("   t=1 total_ops=%llu  t=2 %llu  t=4 %llu\n",
	    (unsigned long long)t1.total_operations,
	    (unsigned long long)t2.total_operations,
	    (unsigned long long)t4.total_operations);

	CHECK(t1.ops_floor_raised == 0 && t4.ops_floor_raised == 0,
	    "the floor should not apply at this budget; test is misconfigured");

	/* Integer division across threads means exact equality is not
	 * guaranteed.  BOTH bounds matter:
	 *   lower -- catches the budget being divided more than once (the
	 *            matrix.sh + bench_main.c stack, which gave 1/threads^2);
	 *   upper -- catches the workload treating operation_count as
	 *            PER-THREAD, which is what it did pre-fix and what made
	 *            pre-dividing in the caller look necessary.
	 * 1% is far tighter than the 4x error either direction produced. */
	CHECK(t2.total_operations > total * 99 / 100,
	    "2 threads ran %llu of %llu total ops -- the budget was divided "
	    "more than once", (unsigned long long)t2.total_operations,
	    (unsigned long long)total);
	CHECK(t2.total_operations < total * 101 / 100,
	    "2 threads ran %llu ops for a %llu total budget -- "
	    "operation_count is being treated as per-thread",
	    (unsigned long long)t2.total_operations,
	    (unsigned long long)total);
	CHECK(t4.total_operations > total * 99 / 100,
	    "4 threads ran %llu of %llu total ops -- the budget was divided "
	    "more than once", (unsigned long long)t4.total_operations,
	    (unsigned long long)total);
	CHECK(t4.total_operations < total * 101 / 100,
	    "4 threads ran %llu ops for a %llu total budget -- "
	    "operation_count is being treated as per-thread",
	    (unsigned long long)t4.total_operations,
	    (unsigned long long)total);
	CHECK(t1.thread_count == 1 && t2.thread_count == 2 &&
	    t4.thread_count == 4, "thread_count must be what actually ran");
}

/* P2.1: too little work must be raised to the floor and flagged. */
static void
test_work_floor(void)
{
	printf("P2.1 minimum per-thread work floor\n");
	/* 1000 total ops over 8 threads = 125 each: the regime that produced
	 * ~52k ops in ~3.8ms with >27% CoV. */
	bench_stats_t st = run("multi-thread", workload_multi_thread, 8,
	    1000, 64, 64);
	printf("   requested 1000 total over 8 threads -> total_ops=%llu "
	    "floor_raised=%d\n", (unsigned long long)st.total_operations,
	    st.ops_floor_raised);

	CHECK(st.ops_floor_raised == 1,
	    "a below-floor budget must set ops_floor_raised");
	CHECK(st.total_operations >=
	    (uint64_t)BENCH_MIN_OPS_PER_THREAD * 8 * 99 / 100,
	    "floor not applied: ran %llu ops, expected >= 8 x %d",
	    (unsigned long long)st.total_operations,
	    BENCH_MIN_OPS_PER_THREAD);

	/* And the helper itself, directly. */
	int raised = -1;
	uint64_t per = bench_ops_per_thread(10, 8, &raised);
	CHECK(per == (uint64_t)BENCH_MIN_OPS_PER_THREAD && raised == 1,
	    "bench_ops_per_thread(10,8) = %llu raised=%d",
	    (unsigned long long)per, raised);
	per = bench_ops_per_thread((uint64_t)BENCH_MIN_OPS_PER_THREAD * 4, 4,
	    &raised);
	CHECK(per == (uint64_t)BENCH_MIN_OPS_PER_THREAD && raised == 0,
	    "an exactly-at-floor budget must not be flagged as raised");
}

/*
 * P2.2: live bytes must be live bytes.  The frag workload's pool capacity is
 * derived from the per-thread budget (see workload_fragmentation), so the
 * absolute ceiling on simultaneously-live bytes is
 *     threads x pool_cap x max_size.
 * Pre-fix, immediately-freed buffers were added to the live total, so it grew
 * with the run's length and blew past this bound.
 */
static void
test_frag_live_bytes(void)
{
	const int threads = 2;
	const uint64_t total = (uint64_t)BENCH_MIN_OPS_PER_THREAD * 4;
	const size_t max_sz = 4096;
	printf("P2.2 fragmentation live-byte accounting\n");

	bench_stats_t st = run("fragmentation", workload_fragmentation,
	    threads, total, 16, max_sz);

	/* Mirrors workload_fragmentation's pool sizing. */
	uint64_t per = total / (uint64_t)threads;
	size_t pool_cap = (size_t)(per / 4);
	if (pool_cap < 1024) pool_cap = 1024;
	if (pool_cap > (size_t)4 << 20) pool_cap = (size_t)4 << 20;
	size_t ceiling = (size_t)threads * pool_cap * max_sz;

	printf("   threads=%d live_at_peak=%zu ceiling=%zu cumulative=%zu\n",
	    st.thread_count, st.live_bytes_at_peak, ceiling,
	    st.bytes_allocated);

	CHECK(st.has_fragmentation == 1,
	    "the fragmentation workload must define a fragmentation ratio");
	CHECK(st.live_bytes_at_peak > 0,
	    "live_bytes_at_peak must be sampled");
	CHECK(st.live_bytes_at_peak <= ceiling,
	    "live_bytes_at_peak=%zu exceeds the %zu bytes that can be live at "
	    "once -- freed bytes are still in the live total",
	    st.live_bytes_at_peak, ceiling);
	CHECK(st.peak_live_bytes <= ceiling,
	    "peak_live_bytes=%zu exceeds the live ceiling %zu",
	    st.peak_live_bytes, ceiling);

	/*
	 * The denominator must be the live-set PEAK, not whichever sample
	 * happened to dip lowest.  If these differ, the reporting rule is
	 * selecting on the ratio again.
	 */
	CHECK(st.live_bytes_at_peak == st.peak_live_bytes,
	    "live_bytes_at_peak=%zu != peak_live_bytes=%zu: the reported pair "
	    "is not the one at the live-set peak",
	    st.live_bytes_at_peak, st.peak_live_bytes);
	CHECK(st.frag_samples >= 8,
	    "only %zu (RSS,live) samples: a summary this thin is a "
	    "single-sample observation, not a distribution", st.frag_samples);
	CHECK(st.frag_live_median > 0 &&
	    st.live_bytes_at_peak >= st.frag_live_median,
	    "the live-set peak (%zu) must be >= the live series median (%zu)",
	    st.live_bytes_at_peak, st.frag_live_median);
	/* VmHWM bounds any RSS sample taken during the run. */
	CHECK(st.max_rss_bytes >= st.peak_rss_bytes,
	    "VmHWM (%zu) is below the RSS sampled at the live peak (%zu), "
	    "which is impossible -- one of them is not what it claims",
	    st.max_rss_bytes, st.peak_rss_bytes);
	printf("   samples=%zu live_median=%zu vmhwm=%zu frag_median=%.4f\n",
	    st.frag_samples, st.frag_live_median, st.max_rss_bytes,
	    st.frag_ratio_median);

	/* The reported ratio must be exactly the pair that was sampled
	 * together -- not a ratio from one sample against RSS from another. */
	if (st.live_bytes_at_peak > 0) {
		double expect = (double)st.peak_rss_bytes /
		    (double)st.live_bytes_at_peak;
		double got = st.fragmentation_ratio;
		double diff = got > expect ? got - expect : expect - got;
		printf("   frag=%.4f peak_rss=%zu -> rss/live=%.4f\n",
		    got, st.peak_rss_bytes, expect);
		CHECK(diff < expect * 0.001,
		    "fragmentation_ratio %.4f != peak_rss/live_at_peak %.4f: "
		    "the RSS reported is not the one that produced the ratio",
		    got, expect);
	}

	/* Honest thread count: not 1, and not the requested count if fewer
	 * threads actually ran. */
	CHECK(st.thread_count == threads,
	    "the fragmentation workload reported %d threads, ran %d",
	    st.thread_count, threads);
}

/*
 * P2.2: workloads with no live set must report NO ratio.  Reporting
 * RSS/cumulative-traffic produced a number that fell towards zero the longer
 * the run, which was then compared against other allocators' numbers.
 */
static void
test_no_bogus_fragmentation(void)
{
	printf("P2.2 workloads with no live set report no ratio\n");
	struct { const char *name; workload_fn fn; int threads; } cases[] = {
		{ "single-thread", workload_single_thread, 1 },
		{ "multi-thread", workload_multi_thread, 2 },
		{ "producer-consumer", workload_producer_consumer, 2 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bench_stats_t st = run(cases[i].name, cases[i].fn,
		    cases[i].threads,
		    (uint64_t)BENCH_MIN_OPS_PER_THREAD * 2, 64, 256);
		printf("   %-18s has_fragmentation=%d frag=%.4f\n",
		    cases[i].name, st.has_fragmentation,
		    st.fragmentation_ratio);
		CHECK(st.has_fragmentation == 0,
		    "%s must not report a fragmentation ratio (it holds no "
		    "live set)", cases[i].name);
		CHECK(st.total_operations > 0,
		    "%s completed no operations", cases[i].name);
	}
}

int
main(void)
{
	printf("=== bench harness accounting regressions (P2.1, P2.2) ===\n");
	test_ops_budget_is_total();
	test_work_floor();
	test_frag_live_bytes();
	test_no_bogus_fragmentation();

	printf("\nResult: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
	    failures, failures == 1 ? "" : "s");
	return (failures ? 1 : 0);
}

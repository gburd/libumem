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
#include <stdatomic.h>
#include <unistd.h>

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
	/* A run with allocation failures did less work than requested, so its
	 * throughput is not comparable to one without.  The count must be
	 * reported, not left to a stderr warning: the 192-thread sustained run
	 * had umem complete 16M operations against libc's 36.5M from the same
	 * -n and nothing in the output said why. */
	printf("   alloc_failures=%llu\n",
	    (unsigned long long)st.alloc_failures);
	CHECK(st.alloc_failures == 0,
	    "%llu allocation failures against the mock allocator, which cannot "
	    "fail -- the counter is wired up wrong",
	    (unsigned long long)st.alloc_failures);
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
	/*
	 * VmHWM vs a VmRSS sample: NOT an exact invariant, deliberately.
	 *
	 * On Linux with CONFIG_SPLIT_RSS_COUNTING the kernel batches per-thread
	 * RSS deltas and folds them into the mm-wide counters only every 64
	 * events or at task exit, so VmRSS and VmHWM are both approximate under
	 * many threads and are not mutually consistent instant to instant.
	 * Measured on c7i.metal-48xl, rss_at_live_peak exceeded VmHWM by
	 * 0.9MB at 1 thread rising monotonically to 63MB at 192 threads.
	 *
	 * So this checks the two agree to within a tolerance that scales with
	 * that skew, rather than asserting an ordering the kernel does not
	 * promise.  A GROSS disagreement would still mean one of the two is not
	 * the quantity it claims to be.
	 */
	if (st.max_rss_bytes > 0 && st.peak_rss_bytes > 0) {
		double skew = (double)st.peak_rss_bytes /
		    (double)st.max_rss_bytes;
		printf("   rss_at_live_peak/vmhwm = %.4f (kernel RSS counters "
		    "are batched per-thread; exact ordering is not guaranteed)\n",
		    skew);
		CHECK(skew < 1.25 && skew > 0.5,
		    "rss_at_live_peak (%zu) and VmHWM (%zu) disagree by more "
		    "than batched RSS accounting explains -- one is not the "
		    "quantity it claims", st.peak_rss_bytes,
		    st.max_rss_bytes);
	}
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
 * Frag live total from per-worker slots.  The harness no longer keeps one
 * shared live-bytes atomic (it was 96.5 % of frag's time at t=192); each
 * worker publishes its own `held` into a cache-line slot and the sampler sums
 * the slots.  This pins the sum to the truth: a counting allocator records the
 * true live bytes (requested sizes still allocated) and their peak.  With ONE
 * worker the sampler reads after each grow batch, which is where the true
 * peak is, so the reported peak must equal it to within one buffer (the
 * pool-full path allocates and immediately frees one buffer past the peak,
 * which the allocator sees and the live total rightly does not).
 */
static atomic_size_t cnt_live, cnt_peak, cnt_maxreq;
static void *cnt_alloc(size_t n)
{
	size_t *h = malloc(n + 16);
	if (h == NULL)
		return (NULL);
	h[0] = n;
	size_t mr = atomic_load(&cnt_maxreq);
	while (n > mr && !atomic_compare_exchange_weak(&cnt_maxreq, &mr, n))
		;
	size_t now = atomic_fetch_add(&cnt_live, n) + n;
	size_t pk = atomic_load(&cnt_peak);
	while (now > pk && !atomic_compare_exchange_weak(&cnt_peak, &pk, now))
		;
	return ((char *)h + 16);
}
static void cnt_free(void *p)
{
	if (p == NULL)
		return;
	size_t *h = (size_t *)((char *)p - 16);
	atomic_fetch_sub(&cnt_live, h[0]);
	free(h);
}

static void
test_frag_live_slots(void)
{
	const size_t max_sz = 4096;
	printf("frag live total = sum of per-worker slots\n");
	allocator_ops_t cnt = mock;
	cnt.alloc = cnt_alloc;
	cnt.free = cnt_free;
	atomic_store(&cnt_live, 0);
	atomic_store(&cnt_peak, 0);
	workload_config_t cfg = {
		.name = "fragmentation", .fn = workload_fragmentation,
		.thread_count = 1,
		.operation_count = (uint64_t)BENCH_MIN_OPS_PER_THREAD * 2,
		.min_size = 16, .max_size = max_sz,
	};
	bench_stats_t st;
	bench_run(&cnt, &cfg, &st);
	size_t truth = atomic_load(&cnt_peak);
	printf("   t=1 live_at_peak=%zu true_peak=%zu end_live=%zu\n",
	    st.live_bytes_at_peak, truth, atomic_load(&cnt_live));
	CHECK(st.live_bytes_at_peak <= truth &&
	    st.live_bytes_at_peak + max_sz >= truth,
	    "t=1 sampled live peak %zu != true live peak %zu (+/- one buffer)",
	    st.live_bytes_at_peak, truth);
	CHECK(atomic_load(&cnt_live) == 0,
	    "%zu bytes still allocated after the run", atomic_load(&cnt_live));

	/* Several workers: every slot must be summed.  Each worker's held
	 * oscillates between ~P/2 and P (free half, refill to cap), so the true
	 * peak is >= 2P and any full sum is >= 2P, while a sampler that read one
	 * slot reports <= P <= truth/2.  Upper bound: threads x pool_cap x max
	 * (the torn sum may exceed the instantaneous truth, never that). */
	atomic_store(&cnt_peak, 0);
	cfg.thread_count = 4;
	cfg.operation_count = (uint64_t)BENCH_MIN_OPS_PER_THREAD * 8;
	bench_run(&cnt, &cfg, &st);
	truth = atomic_load(&cnt_peak);
	printf("   t=4 live_at_peak=%zu true_peak=%zu\n",
	    st.live_bytes_at_peak, truth);
	CHECK(st.live_bytes_at_peak > truth / 2 &&
	    st.live_bytes_at_peak <= (size_t)4 *
	    (BENCH_MIN_OPS_PER_THREAD * 8 / 4 / 4) * max_sz,
	    "t=4 sampled live peak %zu is not the sum of the workers' slots "
	    "(true peak %zu)", st.live_bytes_at_peak, truth);
}

/*
 * frag must run the sizes asked for.  frag_worker clamped max_size to 4096,
 * so a 16-64 KB cell ran <= 4 KB objects and reported them as large.  With
 * -s 16:8192 some request must exceed 4096.  (A fixed 8192:8192 does NOT
 * catch the clamp: the old max_sz < min_sz fallback raised it back to 8192.)
 */
static void
test_frag_large_sizes(void)
{
	printf("frag runs sizes above 4096\n");
	allocator_ops_t cnt = mock;
	cnt.alloc = cnt_alloc;
	cnt.free = cnt_free;
	atomic_store(&cnt_maxreq, 0);
	workload_config_t cfg = {
		.name = "fragmentation", .fn = workload_fragmentation,
		.thread_count = 1,
		.operation_count = (uint64_t)BENCH_MIN_OPS_PER_THREAD,
		.min_size = 16, .max_size = 8192,
	};
	bench_stats_t st;
	bench_run(&cnt, &cfg, &st);
	printf("   asked 16:8192 -> largest request %zu\n",
	    atomic_load(&cnt_maxreq));
	CHECK(atomic_load(&cnt_maxreq) > 4096 &&
	    atomic_load(&cnt_maxreq) <= 8192,
	    "asked for 16:8192, the largest request was %zu (clamped?)",
	    atomic_load(&cnt_maxreq));
}

/*
 * Interval-mode frag sizes its pool from a live-bytes target, not from -n.
 * Pre-fix the pool was operation_count / threads / 4 with the 100k-op floor,
 * so >= 25,000 objects per thread whatever was asked: at 16-64 KB and t=192
 * that is ~196 GB live.  Here: 16 MiB total at 8192 B over 2 threads = 1024
 * objects per thread = 16 MiB peak; pre-fix it held 25,000 x 8 KiB x 2 =
 * ~400 MB.
 */
static void
test_frag_live_budget(void)
{
	printf("interval-mode frag live set follows the -L budget\n");
	bench_stats_t iv[2];
	memset(iv, 0, sizeof(iv));
	workload_config_t cfg = {
		.name = "fragmentation", .fn = workload_fragmentation,
		.thread_count = 2,
		.operation_count = 1000,	/* below the floor: raised to 100k */
		.min_size = 8192, .max_size = 8192,
		.intervals = 2, .interval_sec = 0.3, .interval_out = iv,
		.frag_live_mib = 16
	};
	bench_stats_t st;
	bench_run(&mock, &cfg, &st);
	size_t peak = iv[0].live_bytes_at_peak > iv[1].live_bytes_at_peak ?
	    iv[0].live_bytes_at_peak : iv[1].live_bytes_at_peak;
	printf("   -L 16 at 8192 B, t=2 -> sampled live peak %zu (budget %u)\n",
	    peak, 16u << 20);
	CHECK(peak > 0 && peak <= ((size_t)16 << 20),
	    "live peak %zu exceeds the 16 MiB budget: the ops floor sized the "
	    "pool", peak);
}

/*
 * prodcons ring topology.  One MPMC ring shared by every thread measured the
 * ring (79.6 % of prodcons's time in its CAS at t=192).  Now each consumer
 * owns a ring fed by its own producer(s), so frees stay cross-thread but a
 * producer's buffers are freed by exactly ONE consumer.  A tagging allocator
 * records, per buffer, which thread allocated it; the free side counts
 * (producer, consumer) pairs.  Pre-fix, 4 consumers popping one ring spread
 * every producer's buffers across several consumers.
 */
#define PC_MAXT 16
static atomic_int pc_next_tid;
static _Thread_local int pc_tid = -1;
static atomic_ulong pc_pair[PC_MAXT][PC_MAXT];
static int pc_self(void)
{
	if (pc_tid < 0)
		pc_tid = atomic_fetch_add(&pc_next_tid, 1);
	return (pc_tid);
}
static void *tag_alloc(size_t n)
{
	int *h = malloc(n + 16);
	if (h == NULL)
		return (NULL);
	h[0] = pc_self();
	return ((char *)h + 16);
}
static void tag_free(void *p)
{
	if (p == NULL)
		return;
	int *h = (int *)((char *)p - 16);
	int a = h[0], f = pc_self();
	if (a < PC_MAXT && f < PC_MAXT)
		atomic_fetch_add(&pc_pair[a][f], 1);
	free(h);
}

static void
test_prodcons_rings(void)
{
	printf("prodcons: independent rings, frees cross-thread\n");
	allocator_ops_t tag = mock;
	tag.alloc = tag_alloc;
	tag.free = tag_free;
	memset(pc_pair, 0, sizeof(pc_pair));
	atomic_store(&pc_next_tid, 0);
	workload_config_t cfg = {
		.name = "producer-consumer", .fn = workload_producer_consumer,
		.thread_count = 8,
		.operation_count = (uint64_t)BENCH_MIN_OPS_PER_THREAD * 4,
		.min_size = 16, .max_size = 64,
	};
	bench_stats_t st;
	bench_run(&tag, &cfg, &st);
	int nt = atomic_load(&pc_next_tid);
	unsigned long same = 0, total = 0;
	int worst = 0;
	for (int a = 0; a < nt && a < PC_MAXT; a++) {
		int consumers = 0;
		for (int f = 0; f < nt && f < PC_MAXT; f++) {
			unsigned long c = atomic_load(&pc_pair[a][f]);
			total += c;
			if (c && a == f)
				same += c;
			if (c)
				consumers++;
		}
		if (consumers > worst)
			worst = consumers;
	}
	printf("   threads=%d frees=%lu same-thread=%lu max consumers per "
	    "producer=%d\n", nt, total, same, worst);
	CHECK(total == st.total_operations / 2,
	    "%lu frees for %llu ops: buffers lost or double-counted", total,
	    (unsigned long long)st.total_operations);
	CHECK(same == 0, "%lu frees on the allocating thread: prodcons must "
	    "free cross-thread", same);
	CHECK(worst == 1, "a producer's buffers reached %d consumers: the "
	    "ring is shared, not per consumer", worst);
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
		CHECK(st.alloc_failures == 0,
		    "%s reported %llu allocation failures against a mock "
		    "allocator that cannot fail", cases[i].name,
		    (unsigned long long)st.alloc_failures);
	}
}

/*
 * Interval mode (bench_main -D): ONE process, time-bounded intervals, each
 * with its own stats.  Each interval must last its period (not its op budget),
 * carry its own work and latency, and frag intervals must carry their own
 * timer-sampled (RSS, live) series.
 */
static void
test_interval_mode(void)
{
	printf("interval mode: 3 x 0.3 s, one process\n");
	struct { const char *name; workload_fn fn; } cases[] = {
		{ "producer-consumer", workload_producer_consumer },
		{ "fragmentation", workload_fragmentation },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bench_stats_t iv[3];
		memset(iv, 0, sizeof(iv));
		workload_config_t cfg = {
			.name = cases[i].name, .fn = cases[i].fn,
			.thread_count = 4,
			.operation_count = (uint64_t)BENCH_MIN_OPS_PER_THREAD * 4,
			.min_size = 64, .max_size = 256,
			.intervals = 3, .interval_sec = 0.3, .interval_out = iv
		};
		bench_stats_t st;
		bench_run(&mock, &cfg, &st);
		for (int k = 0; k < 3; k++) {
			printf("   %-18s interval %d: %.3f s ops=%llu p50=%.0f "
			    "rss=%zu samples=%zu\n", cases[i].name, k,
			    iv[k].elapsed_seconds,
			    (unsigned long long)iv[k].total_operations,
			    iv[k].latency_p50, iv[k].peak_rss_bytes,
			    iv[k].frag_samples);
			CHECK(iv[k].elapsed_seconds > 0.29 &&
			    iv[k].elapsed_seconds < 0.6,
			    "%s interval %d lasted %.3f s, asked 0.3",
			    cases[i].name, k, iv[k].elapsed_seconds);
			CHECK(iv[k].total_operations > 0 &&
			    iv[k].latency_p50 > 0 && iv[k].peak_rss_bytes > 0,
			    "%s interval %d carries no work/latency/RSS",
			    cases[i].name, k);
			CHECK(iv[k].thread_count == 4,
			    "%s interval %d reports %d threads",
			    cases[i].name, k, iv[k].thread_count);
			if (cases[i].fn == workload_fragmentation)
				CHECK(iv[k].frag_samples >= 2 &&
				    iv[k].live_bytes_at_peak > 0,
				    "frag interval %d has no (RSS, live) "
				    "series of its own", k);
		}
	}
}

/*
 * Interval mode with a straggler: one operation that blocks for longer than an
 * interval (seen at 384 threads on 8 vCPUs, where one umem frag operation took
 * 12.7 s).  The clock must keep time regardless: every interval lasts its
 * period and carries its own RSS samples.  The stalled interval's row may be
 * late, but it must not stretch its own interval or starve the next one.
 */
static atomic_int stall_armed;
static void *stall_alloc(size_t n)
{
	int one = 1;
	if (atomic_compare_exchange_strong(&stall_armed, &one, 0))
		usleep(700000);	/* > 2 intervals of 0.3 s */
	return malloc(n);
}

static void
test_interval_straggler(void)
{
	printf("interval mode: one 0.7 s operation in 0.3 s intervals\n");
	allocator_ops_t stall = mock;
	stall.alloc = stall_alloc;
	bench_stats_t iv[5];
	memset(iv, 0, sizeof(iv));
	workload_config_t cfg = {
		.name = "fragmentation", .fn = workload_fragmentation,
		.thread_count = 4,
		.operation_count = (uint64_t)BENCH_MIN_OPS_PER_THREAD * 4,
		.min_size = 64, .max_size = 256,
		.intervals = 5, .interval_sec = 0.3, .interval_out = iv
	};
	bench_stats_t st;
	/* The first allocation after arming stalls; with 4 workers that lands in
	 * interval 0 and holds its worker's publish across intervals 0-2. */
	atomic_store(&stall_armed, 1);
	bench_run(&stall, &cfg, &st);
	for (int k = 0; k < 5; k++) {
		printf("   interval %d: %.3f s ops=%llu rss=%zu samples=%zu\n", k,
		    iv[k].elapsed_seconds,
		    (unsigned long long)iv[k].total_operations,
		    iv[k].max_rss_bytes, iv[k].frag_samples);
		CHECK(iv[k].elapsed_seconds > 0.29 &&
		    iv[k].elapsed_seconds < 0.4,
		    "interval %d lasted %.3f s, asked 0.3 (a straggler must "
		    "not stretch the clock)", k, iv[k].elapsed_seconds);
		CHECK(iv[k].max_rss_bytes > 0,
		    "interval %d has no RSS sample (the driver stopped "
		    "sampling while waiting)", k);
	}
}

int
main(void)
{
	printf("=== bench harness accounting regressions (P2.1, P2.2) ===\n");
	test_ops_budget_is_total();
	test_work_floor();
	test_frag_live_bytes();
	test_frag_live_slots();
	test_prodcons_rings();
	test_frag_large_sizes();
	test_frag_live_budget();
	test_no_bogus_fragmentation();
	test_interval_mode();
	test_interval_straggler();

	printf("\nResult: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
	    failures, failures == 1 ? "" : "s");
	return (failures ? 1 : 0);
}

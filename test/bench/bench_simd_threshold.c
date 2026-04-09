/*
 * SIMD Threshold Benchmark
 *
 * Measures the crossover point where SIMD becomes beneficial vs scalar code
 * for magazine operations. Tests arrays of varying sizes (1 to 127 elements)
 * to determine optimal threshold for adaptive SIMD selection.
 *
 * This benchmark addresses Sun Microsystems reviewer concern #5 about SIMD
 * overhead for small magazines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../umem_simd.h"

#define ITERATIONS 10000000
#define WARMUP_ITERATIONS 1000000

/*
 * Test sizes cover the full range of magazine sizes used in libumem:
 * 1, 3, 7, 15, 31, 47, 63, 95, 143
 * Plus intermediate values to find precise crossover point.
 */
static const int test_sizes[] = {
	1, 2, 4, 8, 12, 15, 16, 20, 24, 31, 47, 63, 95, 127
};
#define NUM_TEST_SIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))

/*
 * Get current time in nanoseconds
 */
static inline uint64_t
get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Scalar implementation of magazine scan (no SIMD)
 * This is the baseline for comparison.
 */
static int __attribute__((noinline))
mag_scan_notnull_scalar(void **array, int count)
{
	for (int i = 0; i < count; i++) {
		if (array[i] != NULL) {
			return 1;
		}
	}
	return 0;
}

/*
 * Scalar implementation of magazine initialization
 */
static void __attribute__((noinline))
mag_init_scalar(void **array, int count)
{
	for (int i = 0; i < count; i++) {
		array[i] = NULL;
	}
}

/*
 * Alternative scalar using memset (compiler may optimize)
 */
static void __attribute__((noinline))
mag_init_memset(void **array, int count)
{
	memset(array, 0, count * sizeof(void *));
}

/*
 * Benchmark scan operation (all-NULL case)
 * This is the worst case for SIMD as it must process all elements.
 */
static void
bench_scan_allnull(int size)
{
	void *mag[128];
	uint64_t start, end;
	double elapsed_simd, elapsed_scalar;
	volatile int result;

	memset(mag, 0, size * sizeof(void *));

	/* Warmup */
	for (int i = 0; i < WARMUP_ITERATIONS; i++) {
		result = umem_mag_scan_notnull(mag, size);
	}

	/* SIMD version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = umem_mag_scan_notnull(mag, size);
	}
	end = get_time_ns();
	elapsed_simd = (double)(end - start) / ITERATIONS;

	/* Warmup scalar */
	for (int i = 0; i < WARMUP_ITERATIONS; i++) {
		result = mag_scan_notnull_scalar(mag, size);
	}

	/* Scalar version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = mag_scan_notnull_scalar(mag, size);
	}
	end = get_time_ns();
	elapsed_scalar = (double)(end - start) / ITERATIONS;

	double ratio = elapsed_scalar / elapsed_simd;
	const char *winner = (ratio > 1.05) ? "SIMD" :
	                     (ratio < 0.95) ? "SCALAR" : "TIE";

	printf("  Size %3d (all-NULL):    SIMD=%6.2f ns  Scalar=%6.2f ns  "
	    "Ratio=%.3f  [%s]\n", size, elapsed_simd, elapsed_scalar,
	    ratio, winner);

	(void)result;
}

/*
 * Benchmark scan operation with early exit
 * Non-NULL pointer at position 0 - best case for scalar.
 */
static void
bench_scan_early_exit(int size, int exit_position)
{
	void *mag[128];
	uint64_t start, end;
	double elapsed_simd, elapsed_scalar;
	volatile int result;

	memset(mag, 0, size * sizeof(void *));
	if (exit_position < size) {
		mag[exit_position] = (void *)0x1;
	}

	/* Warmup */
	for (int i = 0; i < WARMUP_ITERATIONS; i++) {
		result = umem_mag_scan_notnull(mag, size);
	}

	/* SIMD version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = umem_mag_scan_notnull(mag, size);
	}
	end = get_time_ns();
	elapsed_simd = (double)(end - start) / ITERATIONS;

	/* Warmup scalar */
	for (int i = 0; i < WARMUP_ITERATIONS; i++) {
		result = mag_scan_notnull_scalar(mag, size);
	}

	/* Scalar version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = mag_scan_notnull_scalar(mag, size);
	}
	end = get_time_ns();
	elapsed_scalar = (double)(end - start) / ITERATIONS;

	double ratio = elapsed_scalar / elapsed_simd;
	const char *winner = (ratio > 1.05) ? "SIMD" :
	                     (ratio < 0.95) ? "SCALAR" : "TIE";

	printf("  Size %3d (exit@%2d):     SIMD=%6.2f ns  Scalar=%6.2f ns  "
	    "Ratio=%.3f  [%s]\n", size, exit_position, elapsed_simd,
	    elapsed_scalar, ratio, winner);

	(void)result;
}

/*
 * Benchmark initialization operation
 */
static void
bench_init(int size)
{
	void *mag[128];
	uint64_t start, end;
	double elapsed_simd, elapsed_scalar, elapsed_memset;

	/* Warmup SIMD */
	for (int i = 0; i < WARMUP_ITERATIONS; i++) {
		umem_mag_init_fast(mag, size);
	}

	/* SIMD version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		umem_mag_init_fast(mag, size);
	}
	end = get_time_ns();
	elapsed_simd = (double)(end - start) / ITERATIONS;

	/* Warmup scalar */
	for (int i = 0; i < WARMUP_ITERATIONS; i++) {
		mag_init_scalar(mag, size);
	}

	/* Scalar version (explicit loop) */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		mag_init_scalar(mag, size);
	}
	end = get_time_ns();
	elapsed_scalar = (double)(end - start) / ITERATIONS;

	/* Warmup memset */
	for (int i = 0; i < WARMUP_ITERATIONS; i++) {
		mag_init_memset(mag, size);
	}

	/* Memset version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		mag_init_memset(mag, size);
	}
	end = get_time_ns();
	elapsed_memset = (double)(end - start) / ITERATIONS;

	double ratio_scalar = elapsed_scalar / elapsed_simd;
	double ratio_memset = elapsed_memset / elapsed_simd;
	double best = (elapsed_scalar < elapsed_memset) ?
	    elapsed_scalar : elapsed_memset;
	double ratio_best = best / elapsed_simd;

	const char *winner = (ratio_best > 1.05) ? "SIMD" :
	                     (ratio_best < 0.95) ? "SCALAR" : "TIE";

	printf("  Size %3d:              SIMD=%6.2f ns  Loop=%6.2f ns  "
	    "Memset=%6.2f ns  Best ratio=%.3f  [%s]\n",
	    size, elapsed_simd, elapsed_scalar, elapsed_memset,
	    ratio_best, winner);
}

/*
 * Analyze results and recommend threshold
 */
static void
print_recommendations(void)
{
	printf("\n");
	printf("=================================================================\n");
	printf("THRESHOLD RECOMMENDATIONS\n");
	printf("=================================================================\n\n");

	printf("Analysis Guidelines:\n");
	printf("  - Ratio > 1.05: SIMD is faster (5%% or more)\n");
	printf("  - Ratio < 0.95: Scalar is faster (5%% or more)\n");
	printf("  - 0.95-1.05:    Performance is equivalent (within 5%%)\n\n");

	printf("Threshold Selection:\n");
	printf("  1. Find the size where SIMD consistently wins (ratio > 1.05)\n");
	printf("  2. Consider magazine size distribution (most are <= 31)\n");
	printf("  3. Balance performance gain vs regression risk\n\n");

	printf("Conservative Approach (Low Risk):\n");
	printf("  - Use threshold = 16 for both scan and init\n");
	printf("  - Guarantees no regression on small magazines\n");
	printf("  - May miss some SIMD gains in 8-15 range\n\n");

	printf("Aggressive Approach (High Performance):\n");
	printf("  - Use lowest size where SIMD wins by 10%% or more\n");
	printf("  - Maximizes SIMD usage\n");
	printf("  - Risk of small regression on some CPUs\n\n");

	printf("Platform-Specific Approach:\n");
	printf("  - Set different thresholds for AVX2, SSE2, NEON\n");
	printf("  - Requires benchmarking on each platform\n");
	printf("  - Best performance but higher complexity\n\n");

	printf("Example implementation in umem_simd.h:\n");
	printf("  #define UMEM_SIMD_SCAN_THRESHOLD 16\n");
	printf("  #define UMEM_SIMD_INIT_THRESHOLD 12\n");
	printf("  \n");
	printf("  if (count < UMEM_SIMD_SCAN_THRESHOLD) {\n");
	printf("      /* use scalar implementation */\n");
	printf("  } else {\n");
	printf("      /* use SIMD implementation */\n");
	printf("  }\n");
}

int
main(void)
{
	printf("\n");
	printf("=================================================================\n");
	printf("SIMD THRESHOLD BENCHMARK\n");
	printf("=================================================================\n\n");

	printf("Purpose: Determine crossover point where SIMD becomes beneficial\n");
	printf("Context: Addresses Sun Microsystems concern about SIMD overhead\n");
	printf("         for small magazine operations (typically 1-31 elements)\n\n");

	printf("Platform: ");
#if defined(HAVE_AVX2)
	printf("x86_64 with AVX2 (256-bit vectors, 4 pointers/op)\n");
#elif defined(HAVE_SSE2)
	printf("x86_64 with SSE2 (128-bit vectors, 2 pointers/op)\n");
#elif defined(HAVE_NEON)
	printf("ARM64 with NEON (128-bit vectors, 2 pointers/op)\n");
#else
	printf("Generic (no SIMD available)\n");
#endif

	printf("Iterations: %d (after %d warmup iterations)\n",
	    ITERATIONS, WARMUP_ITERATIONS);
	printf("Test sizes: ");
	for (size_t i = 0; i < NUM_TEST_SIZES; i++) {
		printf("%d%s", test_sizes[i],
		    (i < NUM_TEST_SIZES - 1) ? ", " : "\n");
	}
	printf("\n");

	/*
	 * Test 1: Magazine scanning with all-NULL pointers
	 * This is the worst case for SIMD (must check all elements).
	 * If SIMD wins here, it wins for all cases.
	 */
	printf("=================================================================\n");
	printf("TEST 1: Magazine Scanning (All NULL - Worst Case for SIMD)\n");
	printf("=================================================================\n");
	for (size_t i = 0; i < NUM_TEST_SIZES; i++) {
		bench_scan_allnull(test_sizes[i]);
	}

	/*
	 * Test 2: Magazine scanning with early exit
	 * Tests different exit positions to see if scalar's early-exit
	 * advantage outweighs SIMD's parallelism.
	 */
	printf("\n");
	printf("=================================================================\n");
	printf("TEST 2: Magazine Scanning (Early Exit - Best Case for Scalar)\n");
	printf("=================================================================\n");
	printf("Testing early exit at positions 0, 2, 4 for selected sizes\n\n");

	int early_exit_sizes[] = { 8, 16, 31, 63 };
	int exit_positions[] = { 0, 2, 4 };

	for (size_t i = 0; i < sizeof(early_exit_sizes) / sizeof(int); i++) {
		for (size_t j = 0; j < sizeof(exit_positions) / sizeof(int); j++) {
			if (exit_positions[j] < early_exit_sizes[i]) {
				bench_scan_early_exit(early_exit_sizes[i],
				    exit_positions[j]);
			}
		}
	}

	/*
	 * Test 3: Magazine initialization
	 * Compare SIMD vs scalar loop vs memset.
	 * Memset may be auto-vectorized by compiler.
	 */
	printf("\n");
	printf("=================================================================\n");
	printf("TEST 3: Magazine Initialization (Zero All Pointers)\n");
	printf("=================================================================\n");
	for (size_t i = 0; i < NUM_TEST_SIZES; i++) {
		bench_init(test_sizes[i]);
	}

	/*
	 * Print analysis and recommendations
	 */
	print_recommendations();

	printf("=================================================================\n");
	printf("Benchmark complete. Analyze results to choose threshold values.\n");
	printf("=================================================================\n\n");

	return 0;
}

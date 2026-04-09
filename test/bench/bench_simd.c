/*
 * SIMD Magazine Operations Benchmark
 *
 * Measures performance improvements from SIMD vectorization of magazine
 * scanning and initialization operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../umem_simd.h"

#define ITERATIONS 10000000
#define MAG_SIZE_SMALL 16
#define MAG_SIZE_MEDIUM 64
#define MAG_SIZE_LARGE 128

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
 * Scalar implementation for comparison
 */
static int
mag_scan_notnull_scalar(void **array, int count)
{
	for (int i = 0; i < count; i++) {
		if (array[i] != NULL) {
			return 1;
		}
	}
	return 0;
}

static void
mag_init_scalar(void **array, int count)
{
	memset(array, 0, count * sizeof(void *));
}

/*
 * Benchmark magazine scanning (empty magazine)
 */
static void
bench_scan_empty(int mag_size, const char *label)
{
	void *mag[MAG_SIZE_LARGE];
	uint64_t start, end;
	double elapsed_simd, elapsed_scalar;
	volatile int result;

	memset(mag, 0, mag_size * sizeof(void *));

	/* SIMD version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = umem_mag_scan_notnull(mag, mag_size);
	}
	end = get_time_ns();
	elapsed_simd = (double)(end - start) / ITERATIONS;

	/* Scalar version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = mag_scan_notnull_scalar(mag, mag_size);
	}
	end = get_time_ns();
	elapsed_scalar = (double)(end - start) / ITERATIONS;

	printf("%-20s (size=%3d, empty): SIMD=%.2f ns, Scalar=%.2f ns, "
	    "Speedup=%.2fx\n", label, mag_size, elapsed_simd, elapsed_scalar,
	    elapsed_scalar / elapsed_simd);
	(void)result;
}

/*
 * Benchmark magazine scanning (half full)
 */
static void
bench_scan_half(int mag_size, const char *label)
{
	void *mag[MAG_SIZE_LARGE];
	uint64_t start, end;
	double elapsed_simd, elapsed_scalar;
	volatile int result;

	/* Fill first half */
	for (int i = 0; i < mag_size / 2; i++) {
		mag[i] = (void *)(uintptr_t)(i + 1);
	}
	for (int i = mag_size / 2; i < mag_size; i++) {
		mag[i] = NULL;
	}

	/* SIMD version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = umem_mag_scan_notnull(mag, mag_size);
	}
	end = get_time_ns();
	elapsed_simd = (double)(end - start) / ITERATIONS;

	/* Scalar version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		result = mag_scan_notnull_scalar(mag, mag_size);
	}
	end = get_time_ns();
	elapsed_scalar = (double)(end - start) / ITERATIONS;

	printf("%-20s (size=%3d, half):  SIMD=%.2f ns, Scalar=%.2f ns, "
	    "Speedup=%.2fx\n", label, mag_size, elapsed_simd, elapsed_scalar,
	    elapsed_scalar / elapsed_simd);
	(void)result;
}

/*
 * Benchmark magazine initialization
 */
static void
bench_init(int mag_size, const char *label)
{
	void *mag[MAG_SIZE_LARGE];
	uint64_t start, end;
	double elapsed_simd, elapsed_scalar;

	/* SIMD version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		umem_mag_init_fast(mag, mag_size);
	}
	end = get_time_ns();
	elapsed_simd = (double)(end - start) / ITERATIONS;

	/* Scalar version */
	start = get_time_ns();
	for (int i = 0; i < ITERATIONS; i++) {
		mag_init_scalar(mag, mag_size);
	}
	end = get_time_ns();
	elapsed_scalar = (double)(end - start) / ITERATIONS;

	printf("%-20s (size=%3d, init):  SIMD=%.2f ns, Scalar=%.2f ns, "
	    "Speedup=%.2fx\n", label, mag_size, elapsed_simd, elapsed_scalar,
	    elapsed_scalar / elapsed_simd);
}

int
main(void)
{
	printf("SIMD Magazine Operations Benchmark\n");
	printf("===================================\n\n");

	printf("Architecture: ");
#if defined(HAVE_AVX2)
	printf("x86_64 with AVX2 (256-bit)\n");
#elif defined(HAVE_SSE2)
	printf("x86_64 with SSE2 (128-bit)\n");
#elif defined(HAVE_NEON)
	printf("ARM64 with NEON (128-bit)\n");
#else
	printf("Generic (no SIMD)\n");
#endif
	printf("Iterations: %d\n\n", ITERATIONS);

	/* Benchmark scanning empty magazines */
	printf("Magazine Scanning (Empty):\n");
	bench_scan_empty(MAG_SIZE_SMALL, "Small magazine");
	bench_scan_empty(MAG_SIZE_MEDIUM, "Medium magazine");
	bench_scan_empty(MAG_SIZE_LARGE, "Large magazine");
	printf("\n");

	/* Benchmark scanning half-full magazines */
	printf("Magazine Scanning (Half Full):\n");
	bench_scan_half(MAG_SIZE_SMALL, "Small magazine");
	bench_scan_half(MAG_SIZE_MEDIUM, "Medium magazine");
	bench_scan_half(MAG_SIZE_LARGE, "Large magazine");
	printf("\n");

	/* Benchmark magazine initialization */
	printf("Magazine Initialization:\n");
	bench_init(MAG_SIZE_SMALL, "Small magazine");
	bench_init(MAG_SIZE_MEDIUM, "Medium magazine");
	bench_init(MAG_SIZE_LARGE, "Large magazine");
	printf("\n");

	printf("Expected speedup:\n");
	printf("  SSE2: 2x for scanning and init\n");
	printf("  AVX2: 4x for scanning, 2-3x for init\n");
	printf("  NEON: 2x for scanning and init\n");

	return 0;
}

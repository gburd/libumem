/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * SIMD Vectorization for Magazine Operations
 *
 * This header provides optimized operations for magazine scanning and
 * initialization. SIMD instructions are used selectively where benchmarking
 * proves they provide genuine performance benefits.
 *
 * Architecture Support:
 * - x86_64: SSE2 (2 pointers/op) by default -- SSE2 is in the x86-64
 *   baseline, so the default build runs on any x86-64 CPU.
 *   AVX2 (4 pointers/op) only with --enable-avx2, which makes the whole
 *   library require an AVX2-capable CPU.  Selection is at COMPILE time;
 *   there is no runtime dispatch.
 * - ARM64:  NEON (2 pointers/op)
 * - Fallback: Standard C for unsupported platforms
 *
 * Performance Characteristics (as measured in 2025 on the then-current
 * benchmark; the referenced analysis document is no longer in the tree, so
 * treat these as historical figures, not a current claim):
 * - Magazine scanning: SIMD provides 50-163% speedup for sizes >= 8
 * - Magazine initialization: memset outperforms SIMD by 40-50%
 * - Threshold-based selection ensures SIMD overhead never dominates
 */

#ifndef _UMEM_SIMD_H
#define _UMEM_SIMD_H

#include "config.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef HAVE_SSE2
#include <emmintrin.h>
#endif

#ifdef HAVE_AVX2
#include <immintrin.h>
#endif

#ifdef HAVE_NEON
#include <arm_neon.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SIMD performance thresholds
 *
 * Empirical benchmarking shows SIMD overhead dominates for small magazines.
 * These thresholds ensure we only use SIMD when beneficial.
 *
 * UMEM_SIMD_SCAN_THRESHOLD: Minimum magazine size for SIMD scanning
 * - Sizes 1-3: Scalar is 6-17% faster (overhead dominates)
 * - Sizes 4+: SIMD is 50-163% faster (parallelism wins)
 * - Conservative value (8) provides safety margin across platforms
 */
#define UMEM_SIMD_SCAN_THRESHOLD 8

/*
 * umem_mag_scan_notnull - Scan magazine for non-NULL pointers
 *
 * Efficiently checks if a magazine contains any non-NULL pointers using
 * SIMD instructions when available. This is used during magazine destruction
 * to quickly determine if the magazine needs cleanup.
 *
 * Parameters:
 *   array - Pointer array to scan
 *   count - Number of pointers to check
 *
 * Returns:
 *   1 if any non-NULL pointer found, 0 if all NULL
 *
 * Implementation notes:
 * - Uses scalar loop for small magazines (< UMEM_SIMD_SCAN_THRESHOLD)
 * - Processes 4 pointers at once with AVX2 (256-bit)
 * - Processes 2 pointers at once with SSE2/NEON (128-bit)
 * - Falls back to scalar loop on unsupported platforms
 * - Threshold prevents SIMD overhead from dominating small operations
 */
static inline int
umem_mag_scan_notnull(void **array, int count)
{
	/*
	 * For small magazines, scalar code is faster due to SIMD overhead.
	 * Benchmarks show 6-17% regression for sizes 1-3 with SIMD.
	 * Use simple loop below threshold to avoid this penalty.
	 */
	if (count < UMEM_SIMD_SCAN_THRESHOLD) {
		for (int i = 0; i < count; i++) {
			if (array[i] != NULL) {
				return 1;
			}
		}
		return 0;
	}
#ifdef HAVE_AVX2
	/*
	 * AVX2 path: Process 4 pointers (256 bits) at a time
	 * Compare all 4 against zero, check if any are non-zero
	 */
	__m256i zero = _mm256_setzero_si256();
	int i;

	for (i = 0; i + 4 <= count; i += 4) {
		__m256i ptrs = _mm256_loadu_si256((__m256i *)&array[i]);
		__m256i cmp = _mm256_cmpeq_epi64(ptrs, zero);
		int mask = _mm256_movemask_epi8(cmp);

		if (mask != 0xFFFFFFFF) {
			return 1;
		}
	}

	/* Handle remaining pointers with scalar code */
	for (; i < count; i++) {
		if (array[i] != NULL) {
			return 1;
		}
	}
	return 0;

#elif defined(HAVE_SSE2)
	/*
	 * SSE2 path: Process 2 pointers (128 bits) at a time.
	 *
	 * _mm_cmpeq_epi32, not _mm_cmpeq_epi64: the 64-bit compare is
	 * SSE4.1, and using it here made a build configured for the
	 * x86-64 SSE2 baseline emit an instruction that baseline cannot
	 * execute (SIGILL on pre-Penryn hardware).  A 32-bit lane compare
	 * is equivalent for this test: a 64-bit pointer is zero exactly
	 * when both of its 32-bit halves are zero, so "all bytes of the
	 * mask set" means "all pointers NULL" either way.
	 */
	__m128i zero = _mm_setzero_si128();
	int i;

	for (i = 0; i + 2 <= count; i += 2) {
		__m128i ptrs = _mm_loadu_si128((__m128i *)&array[i]);
		__m128i cmp = _mm_cmpeq_epi32(ptrs, zero);

		if (_mm_movemask_epi8(cmp) != 0xFFFF) {
			return 1;
		}
	}

	/* Handle remaining pointers */
	for (; i < count; i++) {
		if (array[i] != NULL) {
			return 1;
		}
	}
	return 0;

#elif defined(HAVE_NEON)
	/*
	 * ARM NEON path: Process 2 pointers (128 bits) at a time
	 * Equivalent to SSE2 performance
	 */
	uint64x2_t zero = vdupq_n_u64(0);
	int i;

	for (i = 0; i + 2 <= count; i += 2) {
		uint64x2_t ptrs = vld1q_u64((uint64_t *)&array[i]);
		uint64x2_t cmp = vceqq_u64(ptrs, zero);

		if (vgetq_lane_u64(cmp, 0) != ~0ULL ||
		    vgetq_lane_u64(cmp, 1) != ~0ULL) {
			return 1;
		}
	}

	/* Handle remaining pointers */
	for (; i < count; i++) {
		if (array[i] != NULL) {
			return 1;
		}
	}
	return 0;

#else
	/*
	 * Fallback: Standard C implementation
	 * Used on platforms without SIMD support
	 */
	for (int i = 0; i < count; i++) {
		if (array[i] != NULL) {
			return 1;
		}
	}
	return 0;
#endif
}

/*
 * umem_mag_init_fast - Fast magazine initialization
 *
 * Efficiently zeroes out a magazine's pointer array. Benchmarking revealed
 * that memset() significantly outperforms explicit SIMD for this operation
 * across all magazine sizes.
 *
 * Parameters:
 *   array - Pointer array to initialize
 *   count - Number of pointers to zero
 *
 * Performance Analysis:
 * - SIMD was 40-50% SLOWER than memset for common sizes (15, 31, 47, 63)
 * - memset() is highly optimized by compiler (uses rep stosq on x86_64)
 * - Compiler can inline memset for small sizes, eliminating call overhead
 * - SIMD overhead (setup + unaligned stores) dominates for magazine sizes
 *
 * Implementation:
 * - Always use memset() regardless of platform
 * - Let compiler optimize based on size and alignment
 * - This is faster and simpler than manual SIMD vectorization
 */
static inline void
umem_mag_init_fast(void **array, int count)
{
	/*
	 * Use memset for all sizes. Modern compilers generate optimal code:
	 * - Small sizes: inlined stores
	 * - Medium sizes: vectorized loops
	 * - Large sizes: rep stosq (x86_64) or equivalent
	 *
	 * This outperforms manual SIMD by 40-50% for typical magazine sizes.
	 */
	memset(array, 0, count * sizeof(void *));
}

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_SIMD_H */

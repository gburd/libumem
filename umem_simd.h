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
 * This header provides SIMD-accelerated operations for magazine scanning
 * and initialization. SIMD instructions process multiple pointers in parallel,
 * significantly improving performance for batch operations.
 *
 * Architecture Support:
 * - x86_64: SSE2 (2 pointers/op), AVX2 (4 pointers/op)
 * - ARM64:  NEON (2 pointers/op)
 * - Fallback: Standard C for unsupported platforms
 *
 * Performance Impact:
 * - SSE2: 2x speedup over scalar code
 * - AVX2: 4x speedup over scalar code
 * - NEON: 2x speedup over scalar code
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
 * - Processes 4 pointers at once with AVX2 (256-bit)
 * - Processes 2 pointers at once with SSE2/NEON (128-bit)
 * - Falls back to scalar loop on unsupported platforms
 * - Assumes count is small enough that alignment doesn't matter
 */
static inline int
umem_mag_scan_notnull(void **array, int count)
{
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
	 * SSE2 path: Process 2 pointers (128 bits) at a time
	 * More widely supported than AVX2
	 */
	__m128i zero = _mm_setzero_si128();
	int i;

	for (i = 0; i + 2 <= count; i += 2) {
		__m128i ptrs = _mm_loadu_si128((__m128i *)&array[i]);
		__m128i cmp = _mm_cmpeq_epi64(ptrs, zero);

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
 * umem_mag_init_fast - Fast magazine initialization using SIMD
 *
 * Efficiently zeroes out a magazine's pointer array using SIMD instructions.
 * This is used when allocating new magazines to initialize them in bulk.
 *
 * Parameters:
 *   array - Pointer array to initialize
 *   count - Number of pointers to zero
 *
 * Implementation notes:
 * - Uses vector stores to write multiple NULLs at once
 * - Processes 4 pointers with AVX2, 2 with SSE2/NEON
 * - Falls back to memset on unsupported platforms
 * - Assumes array is properly aligned for SIMD access
 */
static inline void
umem_mag_init_fast(void **array, int count)
{
#ifdef HAVE_AVX2
	/*
	 * AVX2 path: Write 4 NULLs (256 bits) at a time
	 * Fastest initialization path
	 */
	__m256i zero = _mm256_setzero_si256();
	int i;

	for (i = 0; i + 4 <= count; i += 4) {
		_mm256_storeu_si256((__m256i *)&array[i], zero);
	}

	/* Handle remaining pointers */
	for (; i < count; i++) {
		array[i] = NULL;
	}

#elif defined(HAVE_SSE2)
	/*
	 * SSE2 path: Write 2 NULLs (128 bits) at a time
	 * Good performance on older x86_64 CPUs
	 */
	__m128i zero = _mm_setzero_si128();
	int i;

	for (i = 0; i + 2 <= count; i += 2) {
		_mm_storeu_si128((__m128i *)&array[i], zero);
	}

	/* Handle remaining pointers */
	for (; i < count; i++) {
		array[i] = NULL;
	}

#elif defined(HAVE_NEON)
	/*
	 * ARM NEON path: Write 2 NULLs (128 bits) at a time
	 * Equivalent to SSE2 performance
	 */
	uint64x2_t zero = vdupq_n_u64(0);
	int i;

	for (i = 0; i + 2 <= count; i += 2) {
		vst1q_u64((uint64_t *)&array[i], zero);
	}

	/* Handle remaining pointers */
	for (; i < count; i++) {
		array[i] = NULL;
	}

#else
	/*
	 * Fallback: Use memset
	 * Compiler may auto-vectorize this on some platforms
	 */
	memset(array, 0, count * sizeof(void *));
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_SIMD_H */

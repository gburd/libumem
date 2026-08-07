/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024 Gregory Burd <greg@burd.me>.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if !defined(_MSC_VER)
#include <sys/types.h>
#endif

/* Expose the full struct definition from <sm.h> to this translation
 * unit; the library needs the layout, consumers get it only via
 * SM_EXPOSE_STRUCT. */
#define SM_INTERNAL
#include "sm.h"
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Portability shims.
 *
 * sparsemap leans on four compiler builtins in its hot paths.  On gcc
 * and clang they expand to the corresponding __builtin_*; on MSVC to
 * the matching intrinsic; on unknown compilers they fall back to a
 * no-op (prefetch) or a portable scalar implementation (popcount,
 * ctz, clz).  Kept inline here so the library is exactly two files,
 * sm.h and sm.c, with nothing else to vendor.
 *
 *	SM_PREFETCH(addr)   Hot-loop prefetch hint.  Non-binding; safe
 *			    to drop on toolchains without the intrinsic.
 *	SM_POPCOUNT64(x)    Population count of a 64-bit value.
 *	SM_CTZ64(x)	    Count trailing zeros (undefined on x == 0;
 *			    the caller must guard).
 *	SM_CLZ64(x)	    Count leading zeros (undefined on x == 0;
 *			    the caller must guard).
 *
 * Each macro may be overridden by the consumer: define it before
 * including this translation unit (e.g. on the compiler command line
 * with -DSM_POPCOUNT64=my_popcount) and sparsemap uses your version
 * verbatim, skipping the built-in detection below.  This lets a host
 * environment route these primitives through its own intrinsics
 * (for example PostgreSQL's pg_popcount64 / pg_rightmost_one_pos64).
 * The override must have the same call signature and return an int
 * (popcount/ctz/clz) or evaluate to void (prefetch).
 */

#ifndef SM_PREFETCH
#if defined(__GNUC__) || defined(__clang__)
#define SM_PREFETCH(addr) __builtin_prefetch((addr), 0, 1)
#elif defined(_MSC_VER)
#include <intrin.h>
#if defined(_M_ARM64) || defined(_M_ARM)
#define SM_PREFETCH(addr) __prefetch((const void *)(addr))
#elif defined(_M_X64) || defined(_M_IX86)
#define SM_PREFETCH(addr) _mm_prefetch((const char *)(addr), _MM_HINT_T0)
#else
#define SM_PREFETCH(addr) ((void)0)
#endif
#else
#define SM_PREFETCH(addr) ((void)0)
#endif
#endif /* SM_PREFETCH */

#ifndef SM_POPCOUNT64
#if defined(__GNUC__) || defined(__clang__)
#define SM_POPCOUNT64(x) ((int)__builtin_popcountll((unsigned long long)(x)))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#define SM_POPCOUNT64(x) ((int)__popcnt64((unsigned __int64)(x)))
#else
/*
 * SWAR fallback (Sebastiano Vigna, broadword popcount).  Twelve ops,
 * no table, roughly 3-5x slower than a hardware popcnt.
 */
static inline int
sm_swar_popcount64(uint64_t x)
{
	x = x - ((x >> 1) & 0x5555555555555555ULL);
	x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
	x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
	return ((int)((x * 0x0101010101010101ULL) >> 56));
}
#define SM_POPCOUNT64(x) sm_swar_popcount64((uint64_t)(x))
#endif
#endif /* SM_POPCOUNT64 */

#ifndef SM_CTZ64
#if defined(__GNUC__) || defined(__clang__)
#define SM_CTZ64(x) __builtin_ctzll((unsigned long long)(x))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
static inline int
sm_msvc_ctz64(uint64_t x)
{
	unsigned long idx;
	_BitScanForward64(&idx, (unsigned __int64)x);
	return ((int)idx);
}
#define SM_CTZ64(x) sm_msvc_ctz64((uint64_t)(x))
#else
/* Portable bit-binary-search fallback.  Six branches, no intrinsics. */
static inline int
sm_swar_ctz64(uint64_t x)
{
	int n = 0;
	if (!(x & 0xFFFFFFFFULL)) {
		n += 32;
		x >>= 32;
	}
	if (!(x & 0xFFFFULL)) {
		n += 16;
		x >>= 16;
	}
	if (!(x & 0xFFULL)) {
		n += 8;
		x >>= 8;
	}
	if (!(x & 0xFULL)) {
		n += 4;
		x >>= 4;
	}
	if (!(x & 0x3ULL)) {
		n += 2;
		x >>= 2;
	}
	if (!(x & 0x1ULL)) {
		n += 1;
	}
	return (n);
}
#define SM_CTZ64(x) sm_swar_ctz64((uint64_t)(x))
#endif
#endif /* SM_CTZ64 */

#ifndef SM_CLZ64
#if defined(__GNUC__) || defined(__clang__)
#define SM_CLZ64(x) __builtin_clzll((unsigned long long)(x))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
static inline int
sm_msvc_clz64(uint64_t x)
{
	unsigned long idx;
	_BitScanReverse64(&idx, (unsigned __int64)x);
	return (63 - (int)idx);
}
#define SM_CLZ64(x) sm_msvc_clz64((uint64_t)(x))
#else
static inline int
sm_swar_clz64(uint64_t x)
{
	int n = 0;
	if (!(x & 0xFFFFFFFF00000000ULL)) {
		n += 32;
		x <<= 32;
	}
	if (!(x & 0xFFFF000000000000ULL)) {
		n += 16;
		x <<= 16;
	}
	if (!(x & 0xFF00000000000000ULL)) {
		n += 8;
		x <<= 8;
	}
	if (!(x & 0xF000000000000000ULL)) {
		n += 4;
		x <<= 4;
	}
	if (!(x & 0xC000000000000000ULL)) {
		n += 2;
		x <<= 2;
	}
	if (!(x & 0x8000000000000000ULL)) {
		n += 1;
	}
	return (n);
}
#define SM_CLZ64(x) sm_swar_clz64((uint64_t)(x))
#endif
#endif /* SM_CLZ64 */

/*
 * Diagnostic and assertion hooks.
 *
 * sparsemap reports internal invariant violations through three
 * macros, each independently overridable by the consumer (define it
 * before this translation unit is compiled, e.g. with -D on the
 * command line):
 *
 *	__sm_assert(expr)
 *		Evaluated wherever the library checks an internal
 *		invariant.  The built-in forms below are active only
 *		under SPARSEMAP_DIAGNOSTIC; in a normal build the
 *		default is ((void)0).  A host that wants its own
 *		assertion machinery (PostgreSQL's Assert(), the C
 *		standard assert(), an abort-on-fail check) defines
 *		__sm_assert to route there.
 *
 *	__sm_diag(fmt, ...)
 *		printf-style debug logging.  Default is ((void)0)
 *		outside SPARSEMAP_DIAGNOSTIC.  A host that wants the
 *		messages routed to its logger (PostgreSQL's elog,
 *		syslog, a ring buffer) defines __sm_diag.
 *
 *	__sm_when_diag(stmt)
 *		Guards diagnostic-only statement blocks (chunk dumps
 *		and the like).  Expands to `if (1) stmt` when
 *		diagnostics are on, `if (0) stmt` otherwise, so the
 *		compiler still type-checks the block but drops it.
 *
 * If a consumer overrides __sm_diag but not __sm_assert (or vice
 * versa) the un-overridden macro keeps its default.  When the
 * consumer overrides __sm_diag, the built-in __sm_diag_ sink below
 * is not compiled, so it costs nothing.
 */

#if defined(SPARSEMAP_DIAGNOSTIC) && !defined(__sm_diag)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __sm_diag(format, ...) \
	__sm_diag_(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#pragma GCC diagnostic pop
void
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    __sm_diag_(const char *file,
    const int line, const char *func, const char *format, ...)
{
	va_list args = { 0 };
	fprintf(stderr, "%s:%d:%s(): ", file, line, func);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}
#endif

#if defined(SPARSEMAP_DIAGNOSTIC) && !defined(__sm_assert)
#define __sm_assert(expr)                                               \
	if (!(expr))                                                    \
	fprintf(stderr, "%s:%d:%s(): assertion failed! %s\n", __FILE__, \
	    __LINE__, __func__, #expr)
#endif

#if defined(SPARSEMAP_DIAGNOSTIC) && !defined(__sm_when_diag)
#define __sm_when_diag(expr) \
	if (1)               \
	expr
#endif

/* Defaults for any hook the consumer did not supply and that the
 * diagnostic build did not define above. */
#ifndef __sm_diag
#define __sm_diag(format, ...) ((void)0)
#endif
#ifndef __sm_assert
#define __sm_assert(expr) ((void)0)
#endif
#ifndef __sm_when_diag
#define __sm_when_diag(expr) \
	if (0)               \
	expr
#endif

#define IS_8_BYTE_ALIGNED(addr) (((uintptr_t)(addr) & 0x7) == 0)

/*
 * Branch-prediction hints.  These are no-ops on compilers that don't
 * understand __builtin_expect; on gcc/clang they let the optimizer
 * lay out the hot path inline and push the cold path off the icache.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SM_LIKELY(cond)   __builtin_expect(!!(cond), 1)
#define SM_UNLIKELY(cond) __builtin_expect(!!(cond), 0)
#else
#define SM_LIKELY(cond)   (cond)
#define SM_UNLIKELY(cond) (cond)
#endif

/*
 * Function-attribute shims (see sm.h for SM_ALIGNED / ssize_t).
 * SM_ALWAYS_INLINE is a complete declaration prefix that replaces the
 * usual "static inline": on gcc/clang it forces inlining, on MSVC it
 * uses __forceinline, elsewhere it degrades to a plain static inline.
 * SM_HOT marks a hot function.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SM_ALWAYS_INLINE static inline __attribute__((always_inline))
#define SM_HOT __attribute__((hot))
#elif defined(_MSC_VER)
#define SM_ALWAYS_INLINE static __forceinline
#define SM_HOT
#else
#define SM_ALWAYS_INLINE static inline
#define SM_HOT
#endif

typedef uint64_t __sm_bitvec_t;

/*
 * __sm_idx_t: the type of a chunk-start offset -- the absolute,
 * chunk-aligned bit index that prefixes every chunk in the serialized
 * buffer.  It is uint64_t so the map addresses the full 64-bit index
 * space the public API advertises (SM_IDX_MAX == UINT64_MAX).
 *
 * This is an internal type, never exposed in <sm.h>; the public API
 * uses uint64_t for every bit location.  It exists as the single point
 * of control for the on-disk index width: SM_SIZEOF_OVERHEAD and the
 * __sm_load_idx / __sm_store_idx helpers all derive their width from
 * sizeof(__sm_idx_t), so the serialized format width is defined in
 * exactly one place.  (A 32-bit __sm_idx_t was the cause of the
 * pre-4.0 truncation bug for indices >= 2^32; keeping the width here,
 * named, makes it a one-line, reviewable decision.)
 */
typedef uint64_t __sm_idx_t;

/*
 * __sm_bitvec_unaligned_t: a 64-bit unsigned alias that the compiler
 * treats as having 1-byte alignment, so loads and stores through a
 * pointer of this type emit unaligned-safe code.
 *
 * The on-disk layout prefixes each chunk with an 8-byte start offset
 * (and the map with an 8-byte chunk-count header), so chunk
 * descriptors are naturally 8-aligned within m_data.  This typedef is
 * retained as defense-in-depth: a caller-supplied wrap() buffer may be
 * arbitrarily aligned, and accessing the descriptor through a plain
 * uint64_t * would then trip UBSan and trap on strict-alignment cpus.
 *
 * gcc and clang lower the unaligned access to whatever the platform
 * requires (a single load on x86_64, two byte-shuffled half-loads on
 * a strict-alignment cpu).  Zero overhead on the common targets.
 */
#if defined(__GNUC__) || defined(__clang__)
typedef uint64_t __attribute__((aligned(1))) __sm_bitvec_unaligned_t;
#elif defined(_MSC_VER)
typedef uint64_t __unaligned __sm_bitvec_unaligned_t;
#else
typedef uint64_t __sm_bitvec_unaligned_t;
#endif

/*
 * __sm_chunk_t holds only a pointer; the unaligned-safe access is a
 * property of the pointee type (__sm_bitvec_unaligned_t), so the
 * struct itself needs no special alignment.
 */
typedef struct {
	__sm_bitvec_unaligned_t *m_data;
} __sm_chunk_t;

typedef struct {
	size_t rem;
	size_t pos;
} __sm_chunk_rank_t;

/*
 * Unaligned-safe load and store helpers.
 *
 * sparsemap's on-disk layout places the 8-byte chunk-count header and
 * the 8-byte per-chunk start offsets at 8-byte boundaries within
 * m_data, naturally aligned for the cpu.  A caller-supplied wrap()
 * buffer, however, may be arbitrarily aligned.
 *
 * However, the pre-fix idiom (`*(__sm_idx_t *)p`) is technically UB
 * under the strict-aliasing rule when `p` is `uint8_t *`, and UBSan
 * complains.  More importantly, on strict-alignment cpus (some
 * RISC-V configurations, ARMv5, certain embedded platforms) a
 * misaligned access traps.  The memcpy idiom below is portable
 * across all of these.  Modern compilers (gcc 4.8+, clang 3.x+)
 * lower a `memcpy` of a fixed small size to a single native
 * load/store -- zero overhead on x86_64 and aarch64.
 *
 * See docs/ARCHITECTURE.md for the on-disk layout invariants.
 */
static inline __sm_idx_t
__sm_load_idx(const uint8_t *p)
{
	__sm_idx_t v;
	memcpy(&v, p, sizeof(v));
	return (v);
}

static inline void
__sm_store_idx(uint8_t *p, const __sm_idx_t v)
{
	memcpy(p, &v, sizeof(v));
}

/*
 * The chunk-count header occupies SM_SIZEOF_OVERHEAD (8) bytes at the
 * start of m_data.  Through v5.0 the count was stored as a uint32_t
 * (the low 4 bytes), capping a map at 2^32-1 chunks; the high 4 bytes
 * were always zero (sm_create / sm_clear memset the whole buffer, and
 * sm_deserialize copies a body whose high bytes are likewise zero).
 *
 * v5.1 widens the count to uint64_t using the full 8-byte slot.  This
 * is wire-compatible in both directions: a v5.1 reader sees the same
 * value in a v5.0 stream (high bytes zero), and a v5.1 writer storing
 * a count < 2^32 produces byte-identical output.  The ceiling now
 * exceeds the addressable index space, so the chunk count is no
 * longer a binding limit for any consumer.
 */
static inline uint64_t
__sm_load_u64(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return (v);
}

static inline void
__sm_store_u64(uint8_t *p, const uint64_t v)
{
	memcpy(p, &v, sizeof(v));
}

enum __SM_CHUNK_INFO {
	/* metadata overhead: sizeof(__sm_idx_t) bytes for the chunk-start
	 * offset / chunk-count header (8 bytes) */
	SM_SIZEOF_OVERHEAD = sizeof(__sm_idx_t),

	/* number of bits that can be stored in a __sm_bitvec_t */
	SM_BITS_PER_VECTOR = sizeof(__sm_bitvec_t) * 8,

	/* number of flags that can be stored in a single index byte */
	SM_FLAGS_PER_INDEX_BYTE = 4,

	/* number of flags that can be stored in the index */
	SM_FLAGS_PER_INDEX = sizeof(__sm_bitvec_t) * SM_FLAGS_PER_INDEX_BYTE,

	/* maximum capacity of a __sm_chunk_t (in bits) */
	SM_CHUNK_MAX_CAPACITY = SM_BITS_PER_VECTOR * SM_FLAGS_PER_INDEX,

	/* maximum capacity of a __sm_chunk_t (31 bits of the RLE) */
	SM_CHUNK_RLE_MAX_CAPACITY = 0x7FFFFFFF,

	/* minimum capacity of a __sm_chunk_t (in bits) */
	SM_CHUNK_MIN_CAPACITY = SM_BITS_PER_VECTOR - 2,

	/* maximum length of a __sm_chunk_t (31 bits of the RLE) */
	SM_CHUNK_RLE_MAX_LENGTH = 0x7FFFFFFF,

	/* __sm_bitvec_t payload is all zeros (2#00) */
	SM_PAYLOAD_ZEROS = 0,

	/* __sm_bitvec_t payload is all ones (2#11) */
	SM_PAYLOAD_ONES = 3,

	/* __sm_bitvec_t payload is mixed (2#10) */
	SM_PAYLOAD_MIXED = 2,

	/* __sm_bitvec_t is not used (2#01) */
	SM_PAYLOAD_NONE = 1,

	/* a mask for checking flags (2 bits, 2#11) */
	SM_FLAG_MASK = 3,

	/* return code for set(): ok, no further action required */
	SM_OK = 0,

	/* return code for set(): needs to grow this __sm_chunk_t */
	SM_NEEDS_TO_GROW = 1,

	/* return code for set(): needs to shrink this __sm_chunk_t */
	SM_NEEDS_TO_SHRINK = 2
};

/* Used when separating an RLE chunk into 2-3 chunks */
typedef struct {
	struct {
		uint8_t *p;          /* pointer into m_data */
		size_t offset;       /* offset in m_data */
		__sm_chunk_t *chunk; /* chunk to be split */
		__sm_idx_t start;    /* start of chunk */
		size_t length;       /* initial length of chunk */
		size_t capacity;     /* the capacity of this RLE chunk */
	} target;

	struct {
		uint8_t *p;   /* location in buf */
		uint64_t idx; /* chunk-aligned to idx */
		size_t size;  /* byte size of this chunk */
	} pivot;

	struct {
		__sm_idx_t start;
		uint64_t end;
		uint8_t *p;
		size_t size;
		__sm_chunk_t c;
	} ex[2]; /* 0 is "on the left", 1 is "on the right" */

	SM_ALIGNAS(
	    __sm_bitvec_t) uint8_t buf[(SM_SIZEOF_OVERHEAD * (unsigned long)3) +
	    (sizeof(__sm_bitvec_t) * 6)];
	size_t expand_by;
	size_t count;
} __sm_chunk_sep_t;

/*
 * SM_ENOUGH_SPACE: if growing m_data_used by `need` bytes would push
 * past m_capacity, return SM_IDX_MAX with errno=ENOSPC.
 *
 * The +SM_SIZEOF_OVERHEAD slack accounts for an off-by-4 read in
 * __sm_insert_data: the memmove length there is `m_data_used -
 * offset`, which over-counts by SM_SIZEOF_OVERHEAD when m_data_used
 * includes the chunk-count header (the post-sm_clear()
 * convention).  Without the slack the over-read writes
 * SM_SIZEOF_OVERHEAD bytes past the buffer end at the boundary.
 * Fixing the off-by-4 in __sm_insert_data directly is preferable
 * but breaks the alternate convention used by
 * sm_wrap()-without-clear callers, where m_data_used does
 * not include the header.
 */
#define SM_ENOUGH_SPACE(need)                                        \
	do {                                                         \
		if (map->m_data_used + (need) + SM_SIZEOF_OVERHEAD > \
		    __sm_cap(map)) {                                  \
			errno = ENOSPC;                              \
			return (SM_IDX_MAX);                         \
		}                                                    \
	} while (0)

#define SM_CHUNK_GET_FLAGS(data, at) \
	((((data)) & ((__sm_bitvec_t)SM_FLAG_MASK << ((at) * 2))) >> ((at) * 2))
#define SM_CHUNK_SET_FLAGS(data, at, to)                                    \
	((data) = ((data) & ~((__sm_bitvec_t)SM_FLAG_MASK << ((at) * 2))) | \
	        ((__sm_bitvec_t)(to) << ((at) * 2)))
#define SM_IS_CHUNK_RLE(chunk)                                       \
	(((*((__sm_bitvec_unaligned_t *)(chunk)->m_data) &           \
	      (((__sm_bitvec_t)0x3) << (SM_BITS_PER_VECTOR - 2))) >> \
	     (SM_BITS_PER_VECTOR - 2)) == SM_PAYLOAD_NONE)

/*
 * RLE (Run-Length Encoding) Format
 *
 * RLE chunks encode a contiguous run of set bits (1s) starting at offset 0.
 * The entire chunk is represented by a single 64-bit descriptor word:
 *
 * Bits 63:62 = 01 (RLE flag, matches SM_PAYLOAD_NONE to distinguish from sparse)
 * Bits 61:31 = Chunk capacity in bits (31 bits, max 2,147,483,647)
 * Bits 30:0  = Run length in bits (31 bits, max 2,147,483,647)
 *
 * Example: If length=1000 and capacity=2048, bits 0-999 are set (1), bits 1000-2047 are unset (0).
 *
 * RLE chunks are immutable by design - any modification that would create gaps or
 * partial runs causes the chunk to be converted to sparse encoding.
 */
#define SM_RLE_FLAGS      0x4000000000000000ULL /* Bits 63:62 = 01 */
#define SM_RLE_FLAGS_MASK 0xC000000000000000ULL /* Mask for bits 63:62 */
#define SM_RLE_CAPACITY_MASK \
	0x3FFFFFFF80000000ULL         /* Mask for bits 61:31 (capacity) */
#define SM_RLE_LENGTH_MASK 0x7FFFFFFFULL /* Mask for bits 30:0 (length) */

/**
 * @brief Checks if the given chunk is flagged as RLE encoded.
 *
 * This function examines the first element in the chunk's data array to determine
 * if the chunk is run-length encoded (RLE).
 *
 * @param[in] chunk The chunk to check.
 * @return True if the chunk is flagged as RLE encoded, false otherwise.
 */
SM_ALWAYS_INLINE bool
__sm_chunk_is_rle(const __sm_chunk_t *chunk)
{
	const __sm_bitvec_t w = chunk->m_data[0];
	return ((w & SM_RLE_FLAGS_MASK) == SM_RLE_FLAGS);
}

/**
 * @brief Sets the Run-Length Encoding (RLE) flag on the specified chunk.
 *
 * This function modifies the first element in the chunk's data array to set
 * the RLE flag, indicating that the chunk is encoded using run-length encoding.
 *
 * @param[in,out] chunk The chunk to be flagged as RLE encoded.
 */
static void
__sm_chunk_set_rle(const __sm_chunk_t *chunk)
{
	__sm_bitvec_t w = chunk->m_data[0];
	/* Clear flag bits, capacity bits, and length bits */
	w &= ~(SM_RLE_FLAGS_MASK | SM_RLE_CAPACITY_MASK | SM_RLE_LENGTH_MASK);
	/* Set the RLE flag (01 in bits 63:62) */
	w |= ((((__sm_bitvec_t)1) << (SM_BITS_PER_VECTOR - 2)) &
	    SM_RLE_FLAGS_MASK);
	chunk->m_data[0] = w;
}

/**
 * @brief Retrieves the capacity of a run-length encoded (RLE) chunk.
 *
 * This function extracts and returns the capacity of an RLE chunk by masking
 * the relevant bits from the first element of the chunk's data array.
 *
 * @param[in] chunk The chunk whose capacity is to be retrieved.
 * @return The capacity of the RLE chunk.
 */
static size_t
__sm_chunk_rle_get_capacity(const __sm_chunk_t *chunk)
{
	__sm_bitvec_t w =
	    chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_CAPACITY_MASK;
	w >>= 31;
	return (w);
}

/**
 * @brief Sets the capacity of an RLE encoded chunk.
 *
 * This function modifies the first element of the chunk's data array to set
 * the given capacity for a run-length encoded (RLE) chunk. The capacity is
 * masked and bit-shifted according to the RLE encoding specifications.
 *
 * This does not check the chunk type, if the chunk isn't RLE then this
 * function will overwrite flags data in a sparse chunk corrupting it.
 *
 * @param[in] chunk The chunk whose capacity is to be set.
 * @param[in] capacity The capacity to set for the RLE chunk.
 */
static void
__sm_chunk_rle_set_capacity(const __sm_chunk_t *chunk, const size_t capacity)
{
	__sm_assert(capacity <= SM_CHUNK_RLE_MAX_CAPACITY);
	__sm_bitvec_t w = chunk->m_data[0];
	w &= ~SM_RLE_CAPACITY_MASK;
	w |= ((__sm_bitvec_t)capacity << 31) & SM_RLE_CAPACITY_MASK;
	chunk->m_data[0] = w;
}

/**
 * @brief Retrieves the run-length for a given RLE encoded chunk.
 *
 * This function extracts and returns the run-length information from the first
 * element of the chunk's data array using a predefined mask.
 *
 * A "run" is a set of adjacent ones that starts at the 0th bit of this
 * chunk. For an RLE chunk that's encoded in the descriptor.  For a sparse
 * chunk we must see how many flags are SM_PAYLOAD_ONES and then if we find an
 * SM_PAYLOAD_MIXED count the additional adjacent ones if they exist
 *
 * @param[in] chunk The RLE encoded chunk whose run-length is to be retrieved.
 * @return The run-length of the given chunk.
 */
static size_t
__sm_chunk_rle_get_length(const __sm_chunk_t *chunk)
{
	const __sm_bitvec_t w =
	    chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_LENGTH_MASK;
	return (w);
}

/**
 * @brief Sets the length of a run-length encoded (RLE) chunk.
 *
 * This function updates the length field of a run-length encoded (RLE) chunk by
 * first validating that the new length is within the permissible maximum length,
 * then modifying the length bits within the chunk's data array accordingly.
 *
 * @param[in] chunk The chunk whose length is to be set.
 * @param[in] length The new length to set for the chunk.
 */
static void
__sm_chunk_rle_set_length(const __sm_chunk_t *chunk, const size_t length)
{
	__sm_assert(length <= SM_CHUNK_RLE_MAX_LENGTH);
	__sm_assert(length <= __sm_chunk_rle_get_capacity(chunk));
	__sm_bitvec_t w = chunk->m_data[0];
	w &= ~SM_RLE_LENGTH_MASK;
	w |= length & SM_RLE_LENGTH_MASK;
	chunk->m_data[0] = w;
}

/**
 * @brief Gets the run length of a given chunk.
 *
 * This function calculates the run length of a given chunk. If the chunk is
 * run-length encoded (RLE), the length is obtained directly. Otherwise, it
 * calculates the run length by analyzing the bit vector data.
 *
 * @param[in] chunk The chunk to evaluate.
 * @return The run length of the chunk. Returns 0 if the chunk is not RLE
 *  encoded and cannot be determined to have a valid run length.
 */
static size_t
__sm_chunk_get_run_length(const __sm_chunk_t *chunk)
{
	size_t length = 0;

	if (__sm_chunk_is_rle(chunk)) {
		length = __sm_chunk_rle_get_length(chunk);
	} else {
		size_t count = 0;
		int j = SM_FLAGS_PER_INDEX, k = SM_BITS_PER_VECTOR;
		__sm_bitvec_t w = chunk->m_data[0];

		switch (w) {
		case 0:
			return (0);
		case ~(__sm_bitvec_t)0:
			/* This returns max capacity but actual run might be shorter.
			 * This is used during coalescing to determine if chunks can be merged.
			 * The caller must account for the actual chunk capacity. */
			return (SM_CHUNK_MAX_CAPACITY);
		default:
			while (j && (w & SM_PAYLOAD_ONES) == SM_PAYLOAD_ONES) {
				count++;
				w >>= 2;
				j--;
			}
			if (count) {
				count *= SM_BITS_PER_VECTOR;
				if ((w & SM_PAYLOAD_MIXED) ==
				    SM_PAYLOAD_MIXED) {
					/*
					 * Only now is m_data[1] guaranteed
					 * to exist: a leading run of all-ones
					 * vectors followed by a MIXED vector
					 * means a payload word was stored.
					 * Loading it earlier would read past a
					 * single-word chunk.
					 */
					__sm_bitvec_t v = chunk->m_data[1];
					w >>= 2;
					j--;
					while (k && (v & 1) == 1) {
						count++;
						v >>= 1;
						k--;
					}
					while (k && (v & 1) == 0) {
						v >>= 1;
						k--;
					}
					if (k) {
						return (0);
					}
				}
				while (j--) {
					switch (w & 0x3) {
					case SM_PAYLOAD_NONE:
					case SM_PAYLOAD_ZEROS:
						w >>= 2;
						break;
					default:
						return (0);
					}
				}
				__sm_assert(count < SM_CHUNK_MAX_CAPACITY);
				length = count;
			}
		}
	}
	return (length);
}

/*
 * struct sparsemap is defined in <sm.h> (visible here because this
 * file defines SM_INTERNAL before including it; consumers get it via
 * SM_EXPOSE_STRUCT).  Keeping the single definition in the header
 * lets callers embed an sm_t by value without the layout drifting
 * from the library.
 */

/*
 * Allocation lineage.  Tracked per sm_t so the grow / dispose
 * paths know what they may safely realloc or free.
 *
 * SM_OWNED_CONTIGUOUS  Single calloc(1, sizeof(sm_t) + size).
 *                      Both the struct and m_data live in one heap
 *                      block; m_data sits immediately after the struct.
 *                      Set by sparsemap() and sm_copy().  May be
 *                      grown via realloc, and disposed with free(map).
 *                      Default for zero-initialized memory.
 *
 * SM_WRAPPED           m_data points to a buffer the caller owns.  Set
 *                      by sm_wrap(), sm_init(), and
 *                      sm_open().  Cannot be realloc'd in place;
 *                      sm_set_data_size with data == NULL will
 *                      transparently promote to SM_OWNED_SPLIT by
 *                      allocating a fresh library-owned buffer and
 *                      copying the m_data_used prefix into it.  The
 *                      caller's original buffer is left untouched and
 *                      remains theirs to free.
 *
 * SM_OWNED_SPLIT       The struct is heap-allocated; m_data is
 *                      separately heap-allocated and owned by the
 *                      library (typically the result of promoting an
 *                      SM_WRAPPED map via grow).  Disposed with
 *                      sm_free, which does free(m_data) +
 *                      free(map).
 */
enum sm_alloc_kind {
	SM_OWNED_CONTIGUOUS = 0,
	SM_WRAPPED = 1,
	SM_OWNED_SPLIT = 2,
};

/* -------------------------------------------------------------------
 * Allocator hooks
 *
 * Sparsemap routes every malloc/realloc/free through these helpers,
 * which consult a single process-global allocator (CRoaring style).
 * Any individual hook may be NULL; the helper falls back to libc for
 * that operation.  An all-zero allocator therefore means "use libc
 * throughout", which is the default.
 * ------------------------------------------------------------------- */

static sm_allocator_t __sm_g_allocator = { 0 };

void
sm_set_allocator(sm_allocator_t a)
{
	__sm_g_allocator = a;
}

static inline void *
__sm_alloc(size_t n)
{
	if (__sm_g_allocator.malloc != NULL) {
		return (__sm_g_allocator.malloc(n));
	}
	return (malloc(n));
}

static inline void *
__sm_alloc_zero(size_t n)
{
	void *p = __sm_alloc(n);
	if (p != NULL) {
		memset(p, 0, n);
	}
	return (p);
}

static inline void *
__sm_realloc(void *p, size_t n)
{
	if (__sm_g_allocator.realloc != NULL) {
		return (__sm_g_allocator.realloc(p, n));
	}
	return (realloc(p, n));
}

static inline void
__sm_free(void *p)
{
	if (__sm_g_allocator.free != NULL) {
		__sm_g_allocator.free(p);
		return;
	}
	free(p);
}

/* -------------------------------------------------------------------
 * Capacity / lineage accessors
 *
 * The allocation-lineage tag (how m_data was provisioned) is folded
 * into the low 3 bits of m_capacity.  Capacity is always rounded up to
 * an 8-byte boundary before being stored, so those bits are free.
 * Read the byte capacity with __sm_cap() and the lineage with
 * __sm_kind(); never touch m_capacity directly.
 * ------------------------------------------------------------------- */

static inline size_t
__sm_cap(const sm_t *m)
{
	return (m->m_capacity & ~(size_t)7);
}

static inline uint8_t
__sm_kind(const sm_t *m)
{
	return ((uint8_t)(m->m_capacity & 7u));
}

static inline void
__sm_set_cap_kind(sm_t *m, size_t cap, uint8_t kind)
{
	/* Round capacity DOWN to an 8-byte boundary so the tag bits are
	 * free and we never report more usable bytes than the buffer
	 * actually has.  Library-owned allocators round the allocation
	 * size UP before calling here, so for them this is a no-op; for
	 * caller-supplied (wrapped) buffers a non-8-aligned size loses up
	 * to 7 trailing bytes, which is the documented behavior. */
	m->m_capacity = (cap & ~(size_t)7) | (kind & 7u);
}

static inline void
__sm_set_kind(sm_t *m, uint8_t kind)
{
	m->m_capacity = (m->m_capacity & ~(size_t)7) | (kind & 7u);
}

/*
 * Internal-invariant check.  No-op in production builds; under
 * SPARSEMAP_TESTING / SPARSEMAP_DIAGNOSTIC it asserts:
 *
 *   - map is non-NULL
 *   - m_data is non-NULL when the byte capacity > 0
 *   - m_data_used <= byte capacity (no buffer overrun)
 *   - m_data is 8-byte aligned (the chunk codec assumes this)
 *   - the lineage tag (low bits of m_capacity) is one of the three
 *     known values
 *
 * The intent is to fail at the moment a corrupted map is touched,
 * rather than three operations later when the libc heap finally
 * notices.  Called at the top of every public mutating or query
 * function in the heisenbug-fix series.
 */
static inline void
__sm_check_invariants(const struct sparsemap *map)
{
	__sm_when_diag({
		__sm_assert(map != NULL);
		if (map == NULL)
			return;
		__sm_assert(__sm_cap(map) == 0 || map->m_data != NULL);
		__sm_assert(map->m_data_used <= __sm_cap(map));
		__sm_assert(IS_8_BYTE_ALIGNED(map->m_data));
		__sm_assert(__sm_kind(map) == SM_OWNED_CONTIGUOUS ||
		    __sm_kind(map) == SM_WRAPPED ||
		    __sm_kind(map) == SM_OWNED_SPLIT);
	});
}

/**
 * @brief Calculates the vector size for a given byte value.
 *
 * This function uses a lookup table to determine the vector size associated
 * with a given byte value.
 *
 * Each entry in the lookup table represents a possible combination of 4 2-bit
 * values (00, 01, 10, 11).  The value at each index corresponds to the count
 * of "10" patterns in that 4-bit combination.  For example, lookup[10] is 2
 * because the binary representation of 10 (0000 1010) contains the "1010"
 * pattern twice.
 *
 * @param[in] b The byte value for which the vector size needs to be calculated.
 * @return The vector size associated with the given byte value.
 * @see scripts/gen_chunk_vector_size_table.py
 */
static size_t
__sm_chunk_calc_vector_size(const uint8_t b)
{
	/* clang-format off */
  static int lookup[] = {
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    2,  2,  3,  2,  2,  2,  3,  2,  3,  3,  4,  3,  2,  2,  3,  2,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0
  };
	/* clang-format on */
	return (lookup[b]);
}

/**
 * @brief Retrieves the position within the chunk corresponding to the specified bit vector index.
 *
 * This function calculates the position in the chunk's data array that
 * corresponds to the given bit vector index. It handles both run-length
 * encoded (RLE) and non-RLE chunks.
 *
 * @param[in] chunk The chunk from which to retrieve the position.
 * @param[in] bv The bit vector index within the chunk.
 * @return The position within the chunk's data array corresponding to the specified bit vector index.
 */
SM_ALWAYS_INLINE size_t
__sm_chunk_get_position(const __sm_chunk_t *chunk, size_t bv)
{
	/* Defense-in-depth: callers compute `bv` as `idx / SM_BITS_PER_VECTOR`
	 * after subtracting the chunk's start offset; on a corrupt buffer
	 * (sm_open of attacker-controlled bytes) the start offset can be
	 * wildly wrong, making `bv` arbitrarily large.  Clamp to the
	 * physical chunk capacity so the loop below never walks past the
	 * 8-byte header word.  Returning 0 here causes the caller to read
	 * chunk->m_data[1] which is also bounded by the chunk_size that
	 * __sm_get_size_impl validated. */
	if (bv >= SM_FLAGS_PER_INDEX) {
		return (0);
	}

	/* Handle 4 indices (1 byte) at a time. */
	size_t position = 0;
	register uint8_t *p = (uint8_t *)chunk->m_data;

	/* Handle RLE by examining the first byte. */
	if (!__sm_chunk_is_rle(chunk)) {
		const size_t num_bytes =
		    bv / ((size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR);
		for (size_t i = 0; i < num_bytes; i++, p++) {
			position += __sm_chunk_calc_vector_size(*p);
		}

		bv -= num_bytes * SM_FLAGS_PER_INDEX_BYTE;
		for (size_t i = 0; i < bv; i++) {
			const size_t flags =
			    SM_CHUNK_GET_FLAGS(*chunk->m_data, i);
			if (flags == SM_PAYLOAD_MIXED) {
				position++;
			}
		}
	}

	return (position);
}

/**
 * @brief Initializes an __sm_chunk_t structure with the given data.
 *
 * This function sets the m_data member of the provided __sm_chunk_t structure to point
 * to the given data, cast as a pointer to __sm_bitvec_t.
 *
 * @param[in,out] chunk The chunk to initialize.
 * @param[in] data The data to associate with the chunk.
 */
static void
__sm_chunk_init(__sm_chunk_t *chunk, uint8_t *data)
{
	chunk->m_data = (__sm_bitvec_unaligned_t *)data;
}

/**
 * @brief Retrieves the capacity of the given chunk.
 *
 * This function calculates the total capacity of the specified chunk,
 * considering if the chunk is run-length encoded (RLE) or not. For RLE
 * encoded chunks, the capacity is directly retrieved from the chunk's data.
 * For non-RLE encoded chunks, the capacity is computed by examining the
 * data and assessing the available, unused sections.
 *
 * @param[in] chunk The chunk whose capacity is to be determined.
 * @return The capacity of the chunk.
 */
SM_ALWAYS_INLINE size_t
__sm_chunk_get_capacity(const __sm_chunk_t *chunk)
{
	/* Handle RLE which encodes the capacity in the vector. */
	if (SM_UNLIKELY(__sm_chunk_is_rle(chunk))) {
		return (__sm_chunk_rle_get_capacity(chunk));
	}

	size_t capacity = SM_CHUNK_MAX_CAPACITY;
	register uint8_t *p = (uint8_t *)chunk->m_data;

	for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
		if (!*p || *p == 0xff) {
			continue;
		}
		for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
			if (flags == SM_PAYLOAD_NONE) {
				capacity -= SM_BITS_PER_VECTOR;
			}
		}
	}
	return (capacity);
}

/**
 * @brief Increases the capacity of a chunk to the specified value.
 *
 * This function adjusts the capacity of a given chunk, ensuring that the new capacity
 * is a multiple of SM_BITS_PER_VECTOR, does not exceed the maximum allowed capacity,
 * and is greater than the current capacity of the chunk. The capacity is increased by
 * marking payload bits in the chunk's data array.
 *
 * @param[in,out] chunk The chunk whose capacity is to be increased.
 * @param[in] capacity The new capacity to set for the chunk.
 */
static void
__sm_chunk_increase_capacity(const __sm_chunk_t *chunk, const size_t capacity)
{
	__sm_assert(capacity % SM_BITS_PER_VECTOR == 0);
	__sm_assert(capacity <= SM_CHUNK_MAX_CAPACITY);
	__sm_assert(capacity > __sm_chunk_get_capacity(chunk));

	const size_t initial_capacity = __sm_chunk_get_capacity(chunk);
	if (capacity <= initial_capacity || capacity > SM_CHUNK_MAX_CAPACITY) {
		return;
	}

	size_t increased = 0;
	register uint8_t *p = (uint8_t *)chunk->m_data;
	for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
		if (!*p || *p == 0xff) {
			continue;
		}
		for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
			if (flags == SM_PAYLOAD_NONE) {
				*p &= (uint8_t)~(
				    (__sm_bitvec_t)SM_PAYLOAD_ONES << j * 2);
				*p |= (uint8_t)((__sm_bitvec_t)SM_PAYLOAD_ZEROS
				    << j * 2);
				increased += SM_BITS_PER_VECTOR;
				if (increased + initial_capacity == capacity) {
					__sm_assert(__sm_chunk_get_capacity(
					                chunk) == capacity);
					return;
				}
			}
		}
	}
	__sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
}

/**
 * @brief Determines if a given chunk is empty.
 *
 * This function checks if all flags within the chunk's data are either
 * SM_PAYLOAD_ZEROS or SM_PAYLOAD_NONE. If any flag doesn't meet these
 * criteria, the chunk is considered not empty.
 *
 * @param[in] chunk The chunk to be evaluated.
 * @return True if the chunk is empty, otherwise false.
 */
static bool
__sm_chunk_is_empty(const __sm_chunk_t *chunk)
{
	if (chunk->m_data[0] != 0) {
		/* A chunk is considered empty if all flags are SM_PAYLOAD_ZERO or _NONE. */
		register uint8_t *p = (uint8_t *)chunk->m_data;
		for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
			if (*p) {
				for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE;
				     j++) {
					const size_t flags =
					    SM_CHUNK_GET_FLAGS(*p, j);
					if (flags != SM_PAYLOAD_NONE &&
					    flags != SM_PAYLOAD_ZEROS) {
						return (false);
					}
				}
			}
		}
	}
	/* The __sm_chunk_t is empty if all flags (in m_data[0]) are zero. */
	return (true);
}

/**
 * @brief Retrieves the size of the specified chunk.
 *
 * This function calculates the memory size required by the given chunk.
 * If the chunk is not run-length encoded (RLE), the function iterates
 * over the chunk's data array and computes the size using a lookup table.
 *
 * @param[in] chunk The chunk whose size is to be determined.
 * @return The size of the chunk in bytes.
 */
SM_ALWAYS_INLINE size_t
__sm_chunk_get_size(const __sm_chunk_t *chunk)
{
	/* At least one __sm_bitvec_t is required for the flags (m_data[0]) */
	size_t size = sizeof(__sm_bitvec_t);
	if (SM_LIKELY(!__sm_chunk_is_rle(chunk))) {
		/* Use a lookup table for each byte of the flags */
		register uint8_t *p = (uint8_t *)chunk->m_data;
		for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
			size += sizeof(__sm_bitvec_t) *
			    __sm_chunk_calc_vector_size(*p);
		}
	}
	return (size);
}

/**
 * @brief Checks if a specific bit is set in a given chunk.
 *
 * This function determines if a bit at a specific index within a chunk is set. The
 * chunk can be either run-length encoded (RLE) or contain a mixture of payloads.
 *
 * @param[in] chunk The chunk to check.
 * @param[in] idx The index of the bit to check within the chunk.
 * @return True if the bit at the specified index is set, false otherwise.
 */
SM_ALWAYS_INLINE bool
__sm_chunk_is_set(const __sm_chunk_t *chunk, const size_t idx)
{
	if (SM_UNLIKELY(__sm_chunk_is_rle(chunk))) {
		if (idx < __sm_chunk_rle_get_length(chunk)) {
			return (true);
		}
		return (false);
	}
	/* Defense-in-depth: on a corrupt buffer (attacker-controlled
	 * chunk start offset) the caller's `idx - start` can wrap to a
	 * value way beyond SM_CHUNK_MAX_CAPACITY.  Reject those without
	 * trying to compute `bv`. */
	if (idx >= SM_CHUNK_MAX_CAPACITY) {
		return (false);
	}
	/* in which __sm_bitvec_t is |idx| stored? */
	const size_t bv = idx / SM_BITS_PER_VECTOR;
	__sm_assert(bv < SM_FLAGS_PER_INDEX);

	/* now retrieve the flags of that __sm_bitvec_t */
	const size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, bv);
	switch (flags) {
	case SM_PAYLOAD_ZEROS:
	case SM_PAYLOAD_NONE:
		return (false);
	case SM_PAYLOAD_ONES:
		return (true);
	default:
		__sm_assert(flags == SM_PAYLOAD_MIXED);
		/* FALLTHROUGH */
	}

	/* get the __sm_bitvec_t at |bv| */
	const __sm_bitvec_t w =
	    chunk->m_data[1 + __sm_chunk_get_position(chunk, bv)];
	/* and finally check the bit in that __sm_bitvec_t */
	return ((w & (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR) > 0);
}

/**
 * @brief Clears a specific bit in a chunk.
 *
 * This function attempts to clear a specified bit within a given chunk.
 * Based on the payload flags in the chunk, it will update the position of
 * the bit and handle transitions between different payload states
 * (ZEROS, ONES, MIXED). If the bit is already clear, it performs a no-op.
 * If the bit is set, it updates the relevant data structures accordingly,
 * possibly requiring the chunk to grow or shrink.
 *
 * @param[in] chunk The chunk in which to clear the bit.
 * @param[in] idx The index of the bit to be cleared.
 * @param[out] pos The position of the bit to be cleared; updated internally.
 * @return An integer status code indicating the result:
 *         - SM_OK if the operation was successful,
 *         - SM_NEEDS_TO_GROW if the chunk needs to grow,
 *         - SM_NEEDS_TO_SHRINK if the chunk needs to shrink.
 */
static int
__sm_chunk_clr_bit(const __sm_chunk_t *chunk, const uint64_t idx, size_t *pos)
{
	__sm_bitvec_t w;
	const size_t bv = idx / SM_BITS_PER_VECTOR;

	__sm_assert(bv < SM_FLAGS_PER_INDEX);

	switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
	case SM_PAYLOAD_ZEROS:
		/* The bit is already clear, no-op. */
		*pos = 0;
		return (SM_OK);
		break;
	case SM_PAYLOAD_ONES:
		/* What was all ones transitions to mixed, which requires another vector. */
		if (*pos == 0) {
			*pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
			return (SM_NEEDS_TO_GROW);
		}
		SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
		w = chunk->m_data[*pos];
		w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
		/* Update the mixed vector. */
		chunk->m_data[*pos] = w;
		return (SM_OK);
		break;
	case SM_PAYLOAD_MIXED:
		*pos = 1 + __sm_chunk_get_position(chunk, bv);
		w = chunk->m_data[*pos];
		w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
		/* Did the vector transition from mixed to all zeros? If so, remove it. */
		if (w == 0) {
			SM_CHUNK_SET_FLAGS(*chunk->m_data, bv,
			    SM_PAYLOAD_ZEROS);
			return (SM_NEEDS_TO_SHRINK);
		}
		/* Update the mixed vector. */
		chunk->m_data[*pos] = w;
		break;
	case SM_PAYLOAD_NONE:
		/* FALLTHROUGH */
	default:
		__sm_assert(!"shouldn't be here");
#ifdef DEBUG
		abort();
#endif
		break;
	}
	return (SM_OK);
}

/**
 * @brief Sets a bit within a chunk at the specified index.
 *
 * This function sets a bit in the given chunk at the location specified by the index.
 * It handles different payload states (all ones, all zeros, and mixed) and updates
 * the chunk's data and flags accordingly.
 *
 * @param[in] chunk The chunk to modify.
 * @param[in] idx The index within the chunk where the bit should be set.
 * @param[out] pos Pointer to a size_t that will be set to the position of the bit.
 * @return An integer indicating the status of the operation. Possible return values are:
 *         - SM_OK: The bit was successfully set.
 *         - SM_NEEDS_TO_GROW: The chunk needs additional space.
 *         - SM_NEEDS_TO_SHRINK: The chunk has excess space that can be reclaimed.
 */
static int
__sm_chunk_set_bit(const __sm_chunk_t *chunk, const uint64_t idx, size_t *pos)
{
	/* Where in the descriptor does this idx fall, which flag should we examine? */
	const size_t bv = idx / SM_BITS_PER_VECTOR;
	__sm_assert(bv < SM_FLAGS_PER_INDEX);
	__sm_assert(__sm_chunk_is_rle(chunk) == false);

	switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
	case SM_PAYLOAD_ONES:
		/* The bit is already set, no-op. */
		*pos = 0;
		return (SM_OK);
		break;
	case SM_PAYLOAD_ZEROS:
		/* What was all zeros transitions to mixed, which requires another vector. */
		if (*pos == 0) {
			*pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
			return (SM_NEEDS_TO_GROW);
		}
		SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
		/* FALLTHROUGH */
	case SM_PAYLOAD_MIXED:
		*pos = 1 + __sm_chunk_get_position(chunk, bv);
		__sm_bitvec_t w = chunk->m_data[*pos];
		w |= (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR;
		/* Did the vector transition from mixed to all ones? If so, remove it. */
		if (w == ~(__sm_bitvec_t)0) {
			SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_ONES);
			return (SM_NEEDS_TO_SHRINK);
		}
		/* Update the mixed vector. */
		chunk->m_data[*pos] = w;
		break;
	case SM_PAYLOAD_NONE:
		/* FALLTHROUGH */
	default:
#ifdef DEBUG
		abort();
#endif
		break;
	}
	return (SM_OK);
}

/**
 * @brief Selects the nth bit with the specified value from a chunk.
 *
 * This function scans a chunk of data to find the nth occurrence of a bit
 * with the specified value (true for 1, false for 0) after skipping offset
 * bits (of any value).
 *
 * @param[in] chunk The chunk to scan for the bit.
 * @param[in] n The number of bits of value to count before returning.
 * @param[in,out] offset The number of bits to skip before starting to count.
 * @param[in] value The bit value to search for (true for 1, false for 0).
 * @return The index within this chunk of the bit when found, otherwise the
 * number of bits scanned (at most SM_BITS_PER_VECTOR).
 */
static size_t
__sm_chunk_select(const __sm_chunk_t *chunk, ssize_t n, ssize_t *offset,
    const bool value)
{
	/* RLE fast path */
	if (SM_UNLIKELY(__sm_chunk_is_rle(chunk))) {
		const size_t length = __sm_chunk_rle_get_length(chunk);
		const size_t capacity = __sm_chunk_rle_get_capacity(chunk);

		if (value) {
			/* Selecting nth set bit (1) */
			/* RLE has run of 1s from index 0 to length-1 */
			if (n < (ssize_t)length) {
				*offset = -1;
				return (n); /* nth set bit is at index n */
			} else {
				*offset = n -
				    length; /* propagate remainder to next chunk */
				return (capacity);
			}
		} else {
			/* Selecting nth unset bit (0) */
			/* Unset bits start at index length */
			if (length >= capacity) {
				/* No unset bits in this chunk */
				*offset = n;
				return (capacity);
			}
			const size_t unset_count = capacity - length;
			if (n < (ssize_t)unset_count) {
				*offset = -1;
				return (length +
				    n); /* nth unset bit is at (length + n) */
			} else {
				*offset =
				    n - unset_count; /* propagate remainder */
				return (capacity);
			}
		}
	}

	/*
	 * Sparse encoding path
	 *
	 * Algorithm: Iterate through flag bytes examining 2-bit descriptors for each 64-bit vector.
	 * Skip vectors that can't contain the target value (ZEROS when searching for 1s, ONES when
	 * searching for 0s). For MIXED vectors, use popcount to quickly check if we need to scan
	 * individual bits. Accumulate bit positions until we've found the nth occurrence.
	 */
	size_t ret = 0;
	register uint8_t *p = (uint8_t *)chunk->m_data;
	for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
		/* Quick skip: if flag byte is 0 (all NONE descriptors) and seeking 1s, skip 4 vectors */
		if (*p == 0 && value) {
			ret += (size_t)SM_FLAGS_PER_INDEX_BYTE *
			    SM_BITS_PER_VECTOR;
			continue;
		}

		for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
			if (flags == SM_PAYLOAD_NONE) {
				continue;
			}
			if (flags == SM_PAYLOAD_ZEROS) {
				if (value == true) {
					ret += SM_BITS_PER_VECTOR;
					continue;
				}
				if (n > SM_BITS_PER_VECTOR) {
					n -= SM_BITS_PER_VECTOR;
					ret += SM_BITS_PER_VECTOR;
					continue;
				}
				*offset = -1;
				return (ret + n);
			}
			if (flags == SM_PAYLOAD_ONES) {
				if (value == true) {
					if (n > SM_BITS_PER_VECTOR) {
						n -= SM_BITS_PER_VECTOR;
						ret += SM_BITS_PER_VECTOR;
						continue;
					}
					*offset = -1;
					return (ret + n);
				}
				ret += SM_BITS_PER_VECTOR;
				continue;
			}
			if (flags == SM_PAYLOAD_MIXED) {
				const __sm_bitvec_t w = chunk->m_data[1 +
				    __sm_chunk_get_position(chunk,
				        (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
				/* Use ctzll for fast bit extraction */
				__sm_bitvec_t target_bits = value ? w : ~w;
				__sm_bitvec_t remaining = target_bits;
				while (remaining) {
					int k = SM_CTZ64(remaining);
					if (n == 0) {
						*offset = -1;
						return (ret + (size_t)k);
					}
					n--;
					remaining &= remaining -
					    1; /* clear lowest set bit */
				}
				ret += SM_BITS_PER_VECTOR;
			}
		}
	}
	*offset = n;
	return (ret);
}

/**
 * @brief Calculates the rank of a bit in a chunk between specified indices.
 *
 * This function computes the number of bits set to a particular state (true
 * or false) within a chunk of data, starting from a specified index and ending
 * at a specified index. The chunk can either be run-length encoded (RLE) or
 * sparsely encoded.
 *
 * Invoking this function with `from = 0` and `to = 0` (the range [0, 0]), will
 * compare 1 bit at the position 0 against value. The range [0, 9] will examine
 * 10 bits, starting with the 0th and ending with the 9th and return at most a
 * count of 10.
 *
 * @param[out] rank Pointer to the rank data structure to populate.
 * @param[in] value The bit state to calculate the rank for (true or false).
 * @param[in] chunk Pointer to the chunk to be examined.
 * @param[in] from The starting index within the chunk.
 * @param[in] to The ending index within the chunk.
 * @return The number of bits in the specified state between the indices [from, to].
 */
static size_t
__sm_chunk_rank(__sm_chunk_rank_t *rank, const bool value,
    const __sm_chunk_t *chunk, size_t from, size_t to)
{
	size_t amt = 0;
	const size_t cap = __sm_chunk_get_capacity(chunk);

	__sm_assert(to >= from);
	rank->rem = cap;
	rank->pos = 0;

	if (from >= cap) {
		rank->pos = cap;
		rank->rem = 0;
		return (amt);
	}

	if (SM_UNLIKELY(SM_IS_CHUNK_RLE(chunk))) {
		/* This is a run-length (RLE) encoded chunk. */
		const size_t length = __sm_chunk_rle_get_length(chunk);
		const size_t end = length - 1;
		/* Clamp to within chunk capacity */
		if (to >= cap) {
			to = cap - 1;
		}
		rank->rem = 0;
		if (value) {
			if (from <= end) {
				amt = (to > end ? end : to) - from + 1;
				rank->pos = to + 1;
			} else {
				rank->pos = cap;
			}
		} else {
			if (from > end) {
				amt = to - from + 1;
				rank->pos = to + 1;
			} else if (to > end) {
				amt = to - end;
				rank->pos = to + 1;
			} else {
				rank->pos = to + 1;
			}
		}
	} else {
		/*
		 * Sparse encoding rank algorithm
		 *
		 * Strategy: Iterate through flag bytes and use popcounts for efficient bit counting.
		 * For ZEROS/ONES payloads, we know the count immediately (0 or 64). For MIXED payloads,
		 * extract the 64-bit vector and use hardware popcount. Apply range masks to only count
		 * bits within [from, to] range. This achieves O(chunks) performance instead of O(bits).
		 */
		uint8_t *vec = (uint8_t *)chunk->m_data;
		__sm_bitvec_t w, mw;
		uint64_t mask;
		size_t pc;

		for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, vec++) {
			for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
				const size_t flags =
				    SM_CHUNK_GET_FLAGS(*vec, j);

				switch (flags) {
				case SM_PAYLOAD_ZEROS:
					rank->rem = 0;
					if (to >= SM_BITS_PER_VECTOR) {
						rank->pos += SM_BITS_PER_VECTOR;
						to -= SM_BITS_PER_VECTOR;
						if (from >=
						    SM_BITS_PER_VECTOR) {
							from = from -
							    SM_BITS_PER_VECTOR;
						} else {
							if (!value) {
								amt +=
								    SM_BITS_PER_VECTOR -
								    from;
							}
							from = 0;
						}
					} else {
						rank->pos += to + 1;
						if (!value) {
							if (from > to) {
								from -= to;
							} else {
								amt += to + 1 -
								    from;
								goto done;
							}
						} else {
							goto done;
						}
					}
					break;

				case SM_PAYLOAD_ONES:
					rank->rem = UINT64_MAX;
					if (to >= SM_BITS_PER_VECTOR) {
						rank->pos += SM_BITS_PER_VECTOR;
						to -= SM_BITS_PER_VECTOR;
						if (from >=
						    SM_BITS_PER_VECTOR) {
							from = from -
							    SM_BITS_PER_VECTOR;
						} else {
							if (value) {
								amt +=
								    SM_BITS_PER_VECTOR -
								    from;
							}
							from = 0;
						}
					} else {
						rank->pos += to + 1;
						if (value) {
							if (from > to) {
								from =
								    from - to;
							} else {
								amt += to + 1 -
								    from;
								goto done;
							}
						} else {
							goto done;
						}
					}
					break;

				case SM_PAYLOAD_MIXED:
					w = chunk->m_data[1 +
					    __sm_chunk_get_position(chunk,
					        (i * SM_FLAGS_PER_INDEX_BYTE) +
					            j)];
					if (to >= SM_BITS_PER_VECTOR) {
						rank->pos += SM_BITS_PER_VECTOR;
						to -= SM_BITS_PER_VECTOR;
						mask = from == 0 ?
						    UINT64_MAX :
						    ~(UINT64_MAX >>
						        (SM_BITS_PER_VECTOR -
						            (from >= 64 ?
						                    64 :
						                    from)));
						mw = (value ? w : ~w) & mask;
						pc = SM_POPCOUNT64(mw);
						amt += pc;
						from =
						    from > SM_BITS_PER_VECTOR ?
						    from - SM_BITS_PER_VECTOR :
						    0;
					} else {
						rank->pos += to + 1;
						const uint64_t to_mask =
						    (to == 63) ?
						    UINT64_MAX :
						    ((uint64_t)1 << (to + 1)) -
						        1;
						const uint64_t from_mask =
						    from == 0 ?
						    UINT64_MAX :
						    ~(UINT64_MAX >>
						        (SM_BITS_PER_VECTOR -
						            (from >= 64 ?
						                    64 :
						                    from)));
						/* Create a mask for the range [from, to] and use popcount. */
						mask = to_mask & from_mask;
						mw = (value ? w : ~w) & mask;
						pc = SM_POPCOUNT64(mw);
						amt += pc;
						rank->rem = mw >>
						    (from > 63 ? 63 : from);
						goto done;
					}
					break;

				case SM_PAYLOAD_NONE:
				default:
					continue;
				}
			}
		}
	}
done:;
	return (amt);
}

/**
 * @brief Scans a chunk allowing the callee to process each vector.
 *
 * This function iterates through a chunk's data and processes these
 * payloads using the provided scanner function.
 *
 * @param[in] chunk The chunk to scan.
 * @param[in] start The starting index for the scan.
 * @param[in] scanner The callback function to process discovered vectors.
 * @param[in] skip The number of vectors to skip before processing.
 * @param[in] aux Auxiliary data to pass to the scanner function.
 * @return The total number of processed vectors.
 */
static size_t
__sm_chunk_scan(const __sm_chunk_t *chunk, const __sm_idx_t start,
    void (*scanner)(uint64_t[], size_t, void *aux), size_t skip, void *aux)
{
	/* RLE fast path */
	if (SM_UNLIKELY(__sm_chunk_is_rle(chunk))) {
		const size_t length = __sm_chunk_rle_get_length(chunk);

		/* RLE chunks only contain set bits from 0 to length-1 */
		if (skip >= length) {
			return (length); /* Skipped all bits in this chunk */
		}

		/* Skip first `skip` bits, then scan the rest */
		const size_t scan_start = skip;

		/* Process in batches using same buffer size as sparse code */
		uint64_t buffer[SM_BITS_PER_VECTOR];

		for (size_t i = scan_start; i < length;) {
			size_t batch_size = SM_BITS_PER_VECTOR;
			if (i + batch_size > length) {
				batch_size = length - i;
			}

			/* Fill buffer with consecutive indices */
			for (size_t j = 0; j < batch_size; j++) {
				buffer[j] = start + i + j;
			}

			scanner(&buffer[0], batch_size, aux);
			i += batch_size;
		}

		return (skip); /* Return number of bits skipped in this chunk */
	}

	/* Sparse encoding path.
	 * 'pos' tracks the bit offset within the chunk (each vector = SM_BITS_PER_VECTOR).
	 * 'skip' counts set bits remaining to skip before scanning.
	 * Returns the number of set bits skipped in this chunk. */
	size_t pos = 0;
	size_t skipped = 0;
	register uint8_t *p = (uint8_t *)chunk->m_data;
	uint64_t buffer[SM_BITS_PER_VECTOR];
	for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
		if (*p == 0) {
			/* All 4 flag slots in this byte are ZEROS -- no set bits, advance position. */
			pos += SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
			continue;
		}

		for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
			if (flags == SM_PAYLOAD_NONE) {
				/* No capacity in this slot, do not advance position. */
			} else if (flags == SM_PAYLOAD_ZEROS) {
				/* All zeroes -- no set bits to skip or scan. */
				pos += SM_BITS_PER_VECTOR;
			} else if (flags == SM_PAYLOAD_ONES) {
				if (skip >= SM_BITS_PER_VECTOR) {
					skip -= SM_BITS_PER_VECTOR;
					skipped += SM_BITS_PER_VECTOR;
					pos += SM_BITS_PER_VECTOR;
				} else if (skip > 0) {
					size_t n = 0;
					for (size_t b = skip;
					     b < SM_BITS_PER_VECTOR; b++) {
						buffer[n++] = start + pos + b;
					}
					skipped += skip;
					skip = 0;
					scanner(&buffer[0], n, aux);
					pos += SM_BITS_PER_VECTOR;
				} else {
					for (size_t b = 0;
					     b < SM_BITS_PER_VECTOR; b++) {
						buffer[b] = start + pos + b;
					}
					scanner(&buffer[0], SM_BITS_PER_VECTOR,
					    aux);
					pos += SM_BITS_PER_VECTOR;
				}
			} else if (flags == SM_PAYLOAD_MIXED) {
				__sm_bitvec_t remaining = chunk->m_data[1 +
				    __sm_chunk_get_position(chunk,
				        (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
				size_t n = 0;
				while (remaining) {
					int b = SM_CTZ64(remaining);
					if (skip > 0) {
						skip--;
						skipped++;
					} else {
						buffer[n++] = start + pos + b;
					}
					remaining &= remaining -
					    1; /* clear lowest set bit */
				}
				if (n > 0) {
					scanner(&buffer[0], n, aux);
				}
				pos += SM_BITS_PER_VECTOR;
			}
		}
	}
	return (skipped);
}

/* -------------------------------------------------------------------
 * Map structure: chunk navigation, the tail cursor, and the
 * byte-level insert/remove/coalesce primitives
 * ------------------------------------------------------------------- */

/**
 * @brief Retrieves the count of chunks in the sparse map.
 *
 * This function reads the first 32-bit integer from the `m_data` array
 * of the given sparse map to determine and return the number of chunks.
 *
 * @param[in] map The sparse map from which to retrieve the chunk count.
 * @return The number of chunks in the sparse map.
 */
static size_t
__sm_get_chunk_count(const sm_t *map)
{
	/*
	 * The chunk-count slot lives in the first SM_SIZEOF_OVERHEAD bytes of
	 * m_data.  When m_data_used == 0 the slot has not been initialized
	 * (e.g. a freshly sm_wrap'd buffer that has not yet been
	 * sm_clear'd or sm_open'd), so reading it would return
	 * whatever happened to be in the caller's buffer.
	 *
	 * Pre-fix, downstream loops in sm_intersection / _union /
	 * _maximum / __sm_rank_vec walked off the end of the buffer when
	 * the slot held garbage; pg_tre carried four "BUG FIX: m_data_used
	 * = 0 but garbage chunk count" patches at every call site.  The
	 * canonical fix is here: an uninitialized chunk-count slot
	 * means "no chunks", full stop.
	 */
	if (map->m_data_used < SM_SIZEOF_OVERHEAD) {
		return (0);
	}
	return ((size_t)__sm_load_u64(&map->m_data[0]));
}

/**
 * @brief Retrieves a pointer to the data at the specified offset within the sparse map.
 *
 * This function calculates the address of the data starting after a predefined
 * overhead and adds the provided offset to this start point. The resulting
 * pointer points to the actual data within the sparse map.
 *
 * @param[in] map A pointer to the sparse map.
 * @param[in] offset The offset within the sparse map where the data starts.
 * @return A pointer to the data at the specified offset within the sparse map.
 */
static uint8_t *
__sm_get_chunk_data(const sm_t *map, const size_t offset)
{
	return (&map->m_data[SM_SIZEOF_OVERHEAD + offset]);
}

/**
 * @brief Calculates the capacity limit for a run-length encoded (RLE) chunk.
 *
 * This function determines the capacity limit of a run-length encoded (RLE)
 * chunk in a sparse map, based on the provided map, start index, and offset.
 *
 * @param[in] map The sparse map containing the chunk.
 * @param[in] start The starting index of the chunk.
 * @param[in] offset The offset within the sparse map's data.
 * @return The capacity limit of the RLE chunk.
 */
static size_t
__sm_chunk_rle_capacity_limit(const sm_t *map, const __sm_idx_t start,
    const size_t length, const size_t offset)
{
	/* Calculate where the data extends to */
	const size_t data_end = start + length;

	/* Round up to next VEC boundary (2048-aligned) */
	size_t capacity =
	    ((data_end + SM_CHUNK_MAX_CAPACITY - 1) / SM_CHUNK_MAX_CAPACITY) *
	        SM_CHUNK_MAX_CAPACITY -
	    start;

	/* Check if there's a next chunk that limits available space */
	const size_t next_offset =
	    offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
	if (next_offset <
	    map->m_data_used - (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t))) {
		uint8_t *p = __sm_get_chunk_data(map, next_offset);
		const __sm_idx_t next_start = __sm_load_idx((const uint8_t *)p);
		const size_t available = next_start - start;

		/* Use whichever is smaller: VEC-aligned or available space */
		if (available < capacity) {
			capacity = available;
		}
	}

	/* Capacity must be large enough for the actual data */
	if (capacity < length) {
		capacity = length;
	}

	/* Clamp to RLE max */
	if (capacity > SM_CHUNK_RLE_MAX_CAPACITY) {
		capacity = SM_CHUNK_RLE_MAX_CAPACITY;
	}

	return (capacity);
}

/**
 * @brief Computes the end pointer of the chunk data in the sparse map.
 *
 * This function calculates the end of the chunk data by iterating through all
 * the chunks present in the sparse map, taking into account the overhead size
 * and the size of each chunk.
 *
 * @param[in] map The sparse map whose chunk end pointer needs to be calculated.
 * @return A pointer to the end of the chunk data in the sparse map.
 */
static uint8_t *
__sm_get_chunk_end(const sm_t *map)
{
	uint8_t *p = __sm_get_chunk_data(map, 0);
	const size_t count = __sm_get_chunk_count(map);
	for (size_t i = 0; i < count; i++) {
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		p += chunk_size;
	}
	return (p);
}

/**
 * @brief Computes the aligned offset for a given index based on chunk capacity.
 *
 * This function calculates the offset for the provided index such that
 * it aligns with the chunk boundaries defined by the maximum chunk capacity.
 *
 * @param[in] idx The index for which the aligned offset is to be computed.
 * @return The aligned offset corresponding to the given index.
 */
static __sm_idx_t
__sm_get_chunk_aligned_offset(const uint64_t idx)
{
	const uint64_t capacity = SM_CHUNK_MAX_CAPACITY;
	return (idx / capacity * capacity);
}

/**
 * @brief Calculates the total size of the sparse map's used data.
 *
 * This function iterates through each chunk in the sparse map and computes
 * the total memory used by the map, including overhead.
 *
 * @param[in] map Pointer to the sparse map.
 * @return Total size of the used data in the sparse map.
 *
 * Bounds-safe: when called on a possibly-corrupt buffer (after
 * sm_open) the walker validates each chunk against m_capacity and
 * truncates the on-disk chunk count if any chunk would extend past
 * the buffer.  The returned size therefore corresponds to the
 * largest valid chunk-stream prefix; if the input is
 * well-formed, behavior is unchanged.
 */
static void __sm_set_chunk_count(const sm_t *map, size_t new_count);

static size_t
__sm_get_size_impl(const sm_t *map)
{
	uint8_t *start = __sm_get_chunk_data(map, 0);
	uint8_t *p = start;
	uint8_t *end = map->m_data + __sm_cap(map);

	/* Defensive: a chunk-data start outside the data buffer means the
	 * map header itself is corrupt.  Return the empty-map size. */
	if (start < map->m_data || start > end) {
		return (SM_SIZEOF_OVERHEAD);
	}

	const size_t count = __sm_get_chunk_count(map);
	size_t valid_count = 0;
	for (size_t i = 0; i < count; i++) {
		/* Each chunk needs at least SM_SIZEOF_OVERHEAD bytes for its
		 * aligned-offset prefix plus sizeof(__sm_bitvec_t) bytes for the
		 * mandatory chunk header word.  If less remains, the on-disk
		 * count is bogus. */
		if ((size_t)(end - p) <
		    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t)) {
			break;
		}
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);
		/* __sm_chunk_get_size returns at minimum sizeof(__sm_bitvec_t).
		 * A chunk that claims to extend past `end` indicates corrupt
		 * flags; stop walking. */
		if (chunk_size < sizeof(__sm_bitvec_t) ||
		    (size_t)(end - p) < chunk_size) {
			/* Roll back the SM_SIZEOF_OVERHEAD we just advanced; we want
			 * to report the size up to the last *complete* chunk. */
			p -= SM_SIZEOF_OVERHEAD;
			break;
		}
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		p += chunk_size;
		valid_count++;
	}

	/* If the walker truncated, fix up the on-disk chunk count so
	 * subsequent operations see only the valid prefix.  This is the
	 * only place we mutate the map during what is logically a
	 * read; the const cast is intentional and the mutation is safe
	 * (we're correcting attacker-controlled corruption to a
	 * consistent, harmless state). */
	if (valid_count != count) {
		__sm_set_chunk_count((sm_t *)map, valid_count);
	}
	return (SM_SIZEOF_OVERHEAD + (p - start));
}

/**
 * @brief Retrieves the offset of a specified chunk within the sparse map.
 *
 * This function iterates through the chunks in the sparse map to find the
 * offset of the chunk that either contains or would logically contain the
 * given index.
 *
 * @param[in] map The sparse map to search within.
 * @param[in] idx The index to find the corresponding chunk offset for.
 * @param[in,out] cur Optional caller-owned cursor; NULL = no acceleration.
 * @return The offset of the chunk if found, otherwise -1 if no appropriate chunk is found.
 *
 * Read-cursor optimization:
 *
 * The naive implementation walks from chunk 0 on every call.  For a
 * map with N chunks this is O(N) per lookup, so a sequence of N
 * ascending lookups is O(N^2).  When the caller threads a cursor
 * (see sm_cursor_t) the walk resumes from the most-recently-located
 * chunk whenever the new idx is at or after that chunk's start,
 * making an ascending sequence O(N) overall.  Passing NULL (every
 * mutator does) walks from chunk 0.
 *
 * The cursor is caller-owned and purely an in-memory speedup; the
 * on-disk format is unchanged.  ANY mutation of the map invalidates
 * the caller's cursor (the caller must reset it); the library no
 * longer tracks cursor validity in the struct.
 */
static ssize_t
__sm_get_chunk_offset(const sm_t *map, const uint64_t idx, sm_cursor_t *cur)
{
	const size_t count = __sm_get_chunk_count(map);

	if (count == 0) {
		return (-1);
	}

	uint8_t *base = __sm_get_chunk_data(map, 0);
	uint8_t *p = base;
	/* Offsets returned here are relative to `base` (the first chunk);
	 * m_data_used is relative to m_data and includes the
	 * SM_SIZEOF_OVERHEAD chunk-count header, so the chunk stream
	 * occupies [0, stream_end) in base-relative offsets.  Bounding the
	 * walk by stream_end is correct no matter where we resume from
	 * (unlike an ordinal count, which would over-run when resuming
	 * partway through the chunk list). */
	const size_t stream_end = (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;

	/* Byte offset (base-relative) of the chunk immediately BEFORE the
	 * chunk we finally return, or SIZE_MAX if none.  Captured for free
	 * during the forward walk and handed back to the caller so the
	 * coalescing path can find the left neighbor without a head-walk. */
	size_t prev_off = SIZE_MAX;

	/*
	 * Cursor fast-path.  If the caller passed a valid cursor whose
	 * cached chunk starts at or before idx, resume the walk from the
	 * cached byte offset instead of from the head.  Otherwise walk
	 * from chunk 0.  We never index chunks by ordinal here, so a
	 * relocated buffer is fine as long as the offset is still in range.
	 */
	if (cur != NULL && cur->offset != SIZE_MAX &&
	    cur->offset + sizeof(__sm_idx_t) <= stream_end &&
	    idx >= cur->start_idx) {
		/* Self-validate the cached offset before trusting it: a prior
		 * mutation (a chunk shrinking to RLE, a leftward coalesce, a
		 * separate) can shift or remove the cached chunk without the
		 * caller resetting.  If the chunk now at the cached offset no
		 * longer starts where we recorded, the cursor is stale -- walk
		 * from the head instead. */
		const __sm_idx_t at = __sm_load_idx(base + cur->offset);
		if (at == cur->start_idx) {
			p = base + cur->offset;
			/* A resume that lands in this same chunk (no loop
			 * advance below) must still carry the left-neighbor
			 * hint the caller cached, or it would be lost. */
			prev_off = cur->prev_offset;
		}
	}

	/* Walk to the chunk that contains idx, or the last chunk if idx is
	 * past the end. */
	for (;;) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		__sm_assert(s == __sm_get_chunk_aligned_offset(s));
		const size_t next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (idx >= s + __sm_chunk_get_capacity(&chunk) &&
		    next_off < stream_end) {
			prev_off = (size_t)(p - base);
			p = base + next_off;
			continue;
		}
		if (cur != NULL) {
			cur->offset = (size_t)(p - base);
			cur->start_idx = s;
			cur->prev_offset = prev_off;
		}
		return (p - base);
	}
}

/**
 * @brief Sets the chunk count for the sparsemap to a new value.
 *
 * This function updates the chunk count stored in the map's data array
 * to the specified new count.
 *
 * @param[in,out] map The sparsemap in which to set the chunk count.
 * @param[in] new_count The new chunk count to set.
 */
static void
__sm_set_chunk_count(const sm_t *map, const size_t new_count)
{
	__sm_store_u64((uint8_t *)&map->m_data[0], (uint64_t)new_count);
}

/**
 * @brief Appends data to the sparsemap's internal buffer.
 *
 * This function appends the provided buffer to the sparsemap's internal data
 * storage, ensuring that there is enough capacity in the buffer to accommodate
 * the new data.
 *
 * @param[in] map Pointer to the sparsemap structure where data will be appended.
 * @param[in,out] buffer Pointer to the data buffer to be appended to the sparsemap.
 * @param[in] buffer_size Size of the data buffer to be appended.
 */
static void
__sm_append_data(sm_t *map, const uint8_t *buffer, const size_t buffer_size)
{
	__sm_assert(map->m_data_used + buffer_size <= __sm_cap(map));

	memcpy(&map->m_data[map->m_data_used], buffer, buffer_size);
	map->m_data_used += buffer_size;
}

/**
 * @brief Inserts data into the sparse map at the specified offset.
 *
 * This function asserts that there is enough capacity in the map to accommodate
 * the new data, retrieves the appropriate chunk of data from the map, and then
 * inserts the provided buffer at the given offset. The existing data is moved
 * to make space for the new data, and the map's used data size is updated accordingly.
 *
 * @param[in,out] map Pointer to the sparse map where data will be inserted.
 * @param[in] offset Offset in the map where the data should be inserted.
 * @param[in] buffer Pointer to the buffer containing the data to be inserted.
 * @param[in] buffer_size Size of the buffer in bytes.
 */
static void
__sm_insert_data(sm_t *map, const size_t offset, const uint8_t *buffer,
    const size_t buffer_size)
{
	__sm_assert(map->m_data_used + buffer_size <= __sm_cap(map));
	__sm_assert(offset <= map->m_data_used);

	uint8_t *p = __sm_get_chunk_data(map, offset);
	memmove(p + buffer_size, p, map->m_data_used - offset);
	memcpy(p, buffer, buffer_size);
	map->m_data_used += buffer_size;
}

/**
 * @brief Removes a contiguous block of data from the sparsemap.
 *
 * This function removes a block of data from the sparsemap at the specified offset
 * and reduces the size of the data used accordingly.
 *
 * @param[in,out] map A pointer to the sparsemap from which data will be removed.
 * @param[in] offset The starting position of the block to be removed.
 * @param[in] gap_size The size of the block to be removed.
 */
static void
__sm_remove_data(sm_t *map, const size_t offset, const size_t gap_size)
{
	__sm_assert(map->m_data_used >= gap_size);
	uint8_t *p = __sm_get_chunk_data(map, offset);
	memmove(p, p + gap_size, map->m_data_used - offset - gap_size);
	map->m_data_used -= gap_size;
}

/**
 * @brief Coalesces the specified chunk with adjacent chunks if conditions are met.
 *
 * This function attempts to merge the provided chunk with its adjacent chunks
 * in a sparse map if they meet certain conditions. The goal is to reduce the
 * number of chunks by combining adjacent ones that form continuous runs.
 *
 * @param[in] map The sparse map that contains the chunk.
 * @param[in] chunk The chunk to be potentially coalesced.
 * @param[in] offset The offset of the chunk in the sparse map.
 * @param[in] start The starting index of the chunk.
 * @param[in,out] p Pointer to the chunk's data.
 * @return The number of chunks that were removed during the coalescing process.
 */
static int
__sm_coalesce_chunk(sm_t *map, __sm_chunk_t *chunk, size_t offset,
    __sm_idx_t start, uint8_t *p, uint64_t idx, bool is_set_op,
    size_t left_hint)
{
	/*
	 * This is called from __sm_chunk_set/unset/merge/split functions when a
	 * there is a chance that chunks should combine into runs to use less
	 * space in the map.
	 *
	 * The provided chunk may have two adjacent chunks, this function first
	 * processes the chunk to the left and then the one to the right.
	 *
	 * In the case that there is a chunk to the left (with a lower starting index)
	 * we examine its type and ending offset as well as it's run length.  Either
	 * type of chunk (sparse and RLE) can have a run.  In the case of an RLE chunk
	 * that's all it can express.  With a sparse chunk a run is defined as adjacent
	 * set bits starting at the 0th index of the chunk and extending up to at most
	 * the maximum size of a chunk without gaps ([1..SM_CHUNK_MAX_CAPACITY] in
	 * length).  When the left chunk's run ends at the starting index of this chunk
	 * we can combine them. Combining these two will always result in an RLE chunk.
	 *
	 * Once that is finished... we may have something to the right as well.  We look
	 * for an adjacent chunk, then determine if it has a run with a starting point
	 * adjacent to the end of a run in this chunk.  At this point we may have
	 * mutated and coalesced the left into the center chunk which we further mutate
	 * and combine with the right.  At most, we can combine three chunks into one in
	 * these two phases.
	 */
	int num_removed = 0;
	const size_t run_length = __sm_chunk_get_run_length(chunk);
	const size_t capacity = __sm_chunk_get_capacity(chunk);
	const bool is_rle = __sm_chunk_is_rle(chunk);

	/* Guard: do not coalesce an invalid RLE chunk */
	if (is_rle && run_length > capacity) {
		return (num_removed);
	}
	/* Did this chunk become all ones, can we compact it with adjacent chunks? */
	if (run_length > 0) {
		__sm_chunk_t adj;

		/* Is there a previous chunk? */
		if (offset > 0) {
			/* Use the caller's left-neighbor hint when present and
			 * pointing strictly left of this chunk; otherwise fall
			 * back to a head-walk.  The hint is only a shortcut: the
			 * `adj_offset < offset` test below plus the
			 * `adj_start + adj_length == start` alignment guard
			 * still fully validate it, so a stale hint is slow
			 * (walks) or rejected, never a wrong merge. */
			const size_t adj_offset =
			    (left_hint != SIZE_MAX && left_hint < offset)
			    ? left_hint
			    : (size_t)__sm_get_chunk_offset(map, start - 1,
			          NULL);
			if (adj_offset < offset) {
				uint8_t *adj_p =
				    __sm_get_chunk_data(map, adj_offset);
				const __sm_idx_t adj_start =
				    __sm_load_idx((const uint8_t *)adj_p);
				__sm_chunk_init(&adj,
				    adj_p + SM_SIZEOF_OVERHEAD);
				/* Is the adjacent chunk on the left RLE or a sparse chunk of all ones? */
				const size_t adj_length =
				    __sm_chunk_get_run_length(&adj);
				if (adj_length > 0) {
					/* Does it align with this chunk? */
					if (adj_start + adj_length == start) {
						if (SM_CHUNK_MAX_CAPACITY +
						        run_length <
						    SM_CHUNK_RLE_MAX_LENGTH) {
							/* Validate before coalescing */
							const size_t adj_capacity =
							    __sm_chunk_get_capacity(
							        &adj);
							const bool adj_is_rle =
							    __sm_chunk_is_rle(
							        &adj);
							bool can_coalesce =
							    true;

							if (adj_is_rle &&
							    adj_length >
							        adj_capacity) {
								can_coalesce =
								    false;
							}

							/* Calculate new length as span from adjacent start to end of current run */
							size_t new_length =
							    (start +
							        run_length) -
							    adj_start;

							/*
							 * Derive capacity from VEC-aligned boundaries, looking past the
							 * current chunk (being absorbed) to find the real next neighbor.
							 */
							const size_t
							    merge_data_end =
							        adj_start +
							    new_length;
							size_t new_capacity =
							    ((merge_data_end +
							         SM_CHUNK_MAX_CAPACITY -
							         1) /
							        SM_CHUNK_MAX_CAPACITY) *
							        SM_CHUNK_MAX_CAPACITY -
							    adj_start;
							const size_t
							    post_offset =
							        offset +
							    SM_SIZEOF_OVERHEAD +
							    __sm_chunk_get_size(
							        chunk);
							if (post_offset <
							    map->m_data_used -
							        (SM_SIZEOF_OVERHEAD +
							            sizeof(
							                __sm_bitvec_t))) {
								const __sm_idx_t next_start =
								    __sm_load_idx(
								        __sm_get_chunk_data(
								            map,
								            post_offset));
								const size_t avail =
								    next_start -
								    adj_start;
								if (avail <
								    new_capacity) {
									new_capacity =
									    avail;
								}
							}
							if (new_capacity <
							    new_length) {
								new_capacity =
								    new_length;
							}
							if (new_capacity >
							    SM_CHUNK_RLE_MAX_CAPACITY) {
								new_capacity =
								    SM_CHUNK_RLE_MAX_CAPACITY;
							}

							/* Validate that new length fits in available capacity */
							if (can_coalesce &&
							    new_length >
							        new_capacity) {
								can_coalesce =
								    false;
							}

							if (can_coalesce) {
								__sm_chunk_set_rle(
								    &adj);
								__sm_chunk_rle_set_capacity(
								    &adj,
								    new_capacity);
								__sm_chunk_rle_set_length(
								    &adj,
								    new_length);
								__sm_remove_data(
								    map, offset,
								    SM_SIZEOF_OVERHEAD +
								        __sm_chunk_get_size(
								            chunk));
								__sm_set_chunk_count(
								    map,
								    __sm_get_chunk_count(
								        map) -
								        1);

								/* Now chunk is shifted to the left, it becomes the adjacent chunk. */
								p = adj_p;
								offset =
								    adj_offset;
								start =
								    adj_start;
								__sm_chunk_init(
								    chunk,
								    p + SM_SIZEOF_OVERHEAD);
								num_removed +=
								    1;
							}
						}
					}
				}
			}
		}

		/* Is there a next chunk? */
		if (__sm_chunk_is_rle(chunk) ||
		    chunk->m_data[0] == ~(__sm_bitvec_t)0) {
			const size_t adj_offset =
			    offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
			if (adj_offset < map->m_data_used -
			        (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t))) {
				uint8_t *adj_p =
				    __sm_get_chunk_data(map, adj_offset);
				const __sm_idx_t adj_start =
				    __sm_load_idx((const uint8_t *)adj_p);
				__sm_chunk_init(&adj,
				    adj_p + SM_SIZEOF_OVERHEAD);
				/* Is the adjacent right chunk RLE or a sparse with a run of ones? */
				size_t adj_length =
				    __sm_chunk_get_run_length(&adj);
				/* If this is a SET operation and idx is valid and within the adjacent chunk,
				 * use it to calculate accurate run length (prevents overestimation) */
				if (is_set_op && idx != SM_IDX_MAX &&
				    idx >= adj_start) {
					const size_t idx_based_length =
					    idx - adj_start + 1;
					if (idx_based_length < adj_length) {
						adj_length = idx_based_length;
					}
				}
				if (adj_length) {
					/* Does it align with this full sparse chunk? */
					const size_t length =
					    __sm_chunk_get_run_length(chunk);
					if (start + length == adj_start) {
						if (adj_length + length <
						    SM_CHUNK_RLE_MAX_LENGTH) {
							/* Validate adjacent chunk before coalescing */
							const size_t adj_capacity =
							    __sm_chunk_get_capacity(
							        &adj);
							const bool adj_is_rle =
							    __sm_chunk_is_rle(
							        &adj);
							bool can_coalesce =
							    true;

							if (adj_is_rle &&
							    adj_length >
							        adj_capacity) {
								can_coalesce =
								    false;
							}

							/* Calculate new length as span from this start to end of adjacent run */
							size_t new_length =
							    (adj_start +
							        adj_length) -
							    start;

							/*
							 * Derive capacity from VEC-aligned boundaries, looking past the
							 * adjacent chunk (being absorbed) to find the real next neighbor.
							 */
							const size_t
							    r_data_end = start +
							    new_length;
							size_t new_capacity =
							    ((r_data_end +
							         SM_CHUNK_MAX_CAPACITY -
							         1) /
							        SM_CHUNK_MAX_CAPACITY) *
							        SM_CHUNK_MAX_CAPACITY -
							    start;
							const size_t r_adj_size =
							    __sm_chunk_get_size(
							        &adj);
							const size_t r_post =
							    adj_offset +
							    SM_SIZEOF_OVERHEAD +
							    r_adj_size;
							if (r_post <
							    map->m_data_used -
							        (SM_SIZEOF_OVERHEAD +
							            sizeof(
							                __sm_bitvec_t))) {
								const __sm_idx_t nxt =
								    __sm_load_idx(
								        __sm_get_chunk_data(
								            map,
								            r_post));
								const size_t
								    avail =
								        nxt -
								    start;
								if (avail <
								    new_capacity) {
									new_capacity =
									    avail;
								}
							}
							if (new_capacity <
							    new_length) {
								new_capacity =
								    new_length;
							}
							if (new_capacity >
							    SM_CHUNK_RLE_MAX_CAPACITY) {
								new_capacity =
								    SM_CHUNK_RLE_MAX_CAPACITY;
							}

							/* Validate that new length fits in available capacity */
							if (can_coalesce &&
							    new_length >
							        new_capacity) {
								can_coalesce =
								    false;
							}

							if (can_coalesce) {
								__sm_chunk_set_rle(
								    chunk);
								__sm_chunk_rle_set_capacity(
								    chunk,
								    new_capacity);
								__sm_chunk_rle_set_length(
								    chunk,
								    new_length);
								__sm_remove_data(
								    map,
								    adj_offset,
								    SM_SIZEOF_OVERHEAD +
								        r_adj_size);
								__sm_set_chunk_count(
								    map,
								    __sm_get_chunk_count(
								        map) -
								        1);
								num_removed +=
								    1;
							}
						}
					}
				}
			}
		}
	}

	return (num_removed);
}

/**
 * @brief Coalesces adjacent chunks in a sparse map, optimizing its structure.
 *
 * This function iterates through the chunks in the provided sparse map and
 * attempts to coalesce adjacent chunks to reduce fragmentation and improve
 * efficiency.
 *
 * @param[in] map The sparse map to coalesce.
 * @return The number of bytes coalesced during the operation.
 */
static size_t
__sm_coalesce_map(sm_t *map)
{
	__sm_chunk_t chunk;
	size_t n = 0, count = __sm_get_chunk_count(map);
	const size_t offset = 0;
	uint8_t *p = __sm_get_chunk_data(map, offset);

	while (count > 1) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);
		if (count > 1) {
			SM_PREFETCH(p + SM_SIZEOF_OVERHEAD + chunk_size +
			    SM_SIZEOF_OVERHEAD);
		}
		const size_t amt = __sm_coalesce_chunk(map, &chunk, offset,
		    start, p, SM_IDX_MAX, false, SIZE_MAX);
		if (amt > 0) {
			n += amt;
			count = __sm_get_chunk_count(map);
		} else {
			p += SM_SIZEOF_OVERHEAD + chunk_size;
			count--;
		}
	}

	return (n);
}

/**
 * @brief Separates a run-length encoded (RLE) chunk into new chunks based on the provided parameters.
 *
 * This function is called from various chunk manipulation functions such as
 * set, unset, merge, and split when an RLE chunk needs to be mutated into one
 * or more new chunks. It determines the separation and alignment of the pivot
 * chunk with respect to the target chunk.
 *
 * @param[in] map The sparse map containing the chunks.
 * @param[in] sep The separation information required to perform the chunk separation.
 * @param[in] idx The index within the chunk where the separation or mutation is required.
 * @param[in] state The state representing the operation: 0 for clearing a bit, 1 for setting a bit,
 *                  and -1 for splitting without modifying the map.
 * @return Integer value indicating the status of the operation:
 *         0 if the operation is successful,
 *         an error code otherwise.
 */
static int
__sm_separate_rle_chunk(sm_t *map, __sm_chunk_sep_t *sep, const uint64_t idx,
    const int state)
{
	/*
	 * This is called from __sm_chunk_set/unset/merge/split functions when a
	 * run-length encoded (RLE) chunk must be mutated into one or more new chunks.
	 *
	 * This function expects that the separation information is complete and that
	 * the pivot chunk has yet to be created.  The target will always be RLE and the
	 * pivot will always be a new sparse chunk.  The hard part is where the pivot
	 * lies in relation to the target.
	 *
	 * - left aligned
	 * - right aligned
	 * - centrally aligned
	 *
	 * When left aligned the chunk-aligned starting index of the pivot matches the
	 * starting index of the target. This results in two chunks, one new (the pivot)
	 * on the left, and one shortened RLE on the right.
	 *
	 * When right aligned there are two cases, the second more common one is when
	 * the chunk-aligned starting index of the pivot plus its length extends beyond
	 * the end of the run length of the target RLE chunk but is still within the
	 * capacity of the RLE chunk. This again results in two chunks, one on the left
	 * for the remainder of the run and one to the right.  In rare cases the end of
	 * the pivot chunk perfectly aligns with the end of the target's length.
	 *
	 * The last case is when the chunk-aligned starting index is somewhere within
	 * the body of the target.  This results in three chunks; left, right, and pivot
	 * (or center).
	 *
	 * In all three cases the new chunks (left and right) may be either RLE or
	 * sparse encoded, that's TBD based on their sizes after the pivot area is
	 * removed from the body of the run.
	 */

	__sm_chunk_t pivot_chunk;
	__sm_chunk_t lrc;

	__sm_assert(state == 0 || state == 1 || state == -1);
	__sm_assert(SM_IS_CHUNK_RLE(sep->target.chunk));

	if (state == 1) {
		/* setting a bit beyond the run but within capacity */
		__sm_assert(idx >= sep->target.start);
		__sm_assert(idx < sep->target.start + sep->target.capacity);
	} else if (state == 0) {
		/* clearing a bit */
		__sm_assert(idx >= sep->target.start);
		__sm_assert(idx < sep->target.length + sep->target.start);
	} else if (state == -1) {
		/* if `state == -1` we are splitting at idx but leaving map unmodified */
	}

	memset(sep->buf, 0,
	    (SM_SIZEOF_OVERHEAD * (unsigned long)3) +
	        (sizeof(__sm_bitvec_t) * 6));

	/* Find the starting offset for our pivot chunk ... */
	const uint64_t aligned_idx = __sm_get_chunk_aligned_offset(idx);
	__sm_assert(
	    idx >= aligned_idx && idx < aligned_idx + SM_CHUNK_MAX_CAPACITY);
	/* avoid changing the map->m_data and for now work in our buf ... */
	sep->pivot.p = sep->buf;
	__sm_store_idx((uint8_t *)sep->pivot.p, aligned_idx);
	__sm_chunk_init(&pivot_chunk, sep->pivot.p + SM_SIZEOF_OVERHEAD);

	/* The pivot, extracted from a run, starts off as all 1s. */
	pivot_chunk.m_data[0] = ~(__sm_bitvec_t)0;

	if (state == 0) {
		/* To unset, change the flag at the position of the idx to "mixed" ... */
		const size_t vec_idx = (idx - aligned_idx) / SM_BITS_PER_VECTOR;
		const size_t bit_pos = (idx - aligned_idx) % SM_BITS_PER_VECTOR;
		SM_CHUNK_SET_FLAGS(pivot_chunk.m_data[0], vec_idx,
		    SM_PAYLOAD_MIXED);
		/* and clear only the bit at that index in this chunk. */
		pivot_chunk.m_data[1] =
		    ~(__sm_bitvec_t)0 & ~((__sm_bitvec_t)1 << bit_pos);
		sep->pivot.size =
		    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
	} else if (state == 1) {
		if (idx >= sep->target.start &&
		    idx < sep->target.start + sep->target.length) {
			/* It's a no-op to set a bit in a range of bits already set. */
			return (0);
		}
		sep->pivot.size =
		    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
	} else if (state == -1) {
		/* Unmodified */
		sep->pivot.size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
	}

	/* Where did the pivot chunk fall within the original chunk? */
	do {
		if (aligned_idx == sep->target.start) {
			/* The pivot is left aligned, there will be two chunks in total. */
			sep->count = 2;
			sep->ex[1].start = aligned_idx + SM_CHUNK_MAX_CAPACITY;
			sep->ex[1].end = aligned_idx + sep->target.length - 1;
			sep->ex[1].p =
			    (uint8_t *)((uintptr_t)sep->buf + sep->pivot.size);
			__sm_assert(sep->ex[1].start <= sep->ex[1].end);
			__sm_assert(sep->ex[0].p == 0);
			break;
		}

		if (aligned_idx + SM_CHUNK_MAX_CAPACITY >=
		    sep->target.start + sep->target.length) {
			/* The pivot is right aligned, there will be two chunks in total. */
			sep->count = 2;
			/* Does our pivot extend beyond the end of the run. */
			const uint64_t amt_over = aligned_idx +
			    SM_CHUNK_MAX_CAPACITY -
			    (sep->target.start + sep->target.length);
			if (amt_over > 0) {
				/* The index of the first 0 bit. */
				const size_t first_zero =
				    SM_CHUNK_MAX_CAPACITY - amt_over;
				const size_t bv =
				    first_zero / SM_BITS_PER_VECTOR;
				/* Shorten the pivot chunk because it extends beyond the end of the run ... */
				if (amt_over > SM_BITS_PER_VECTOR) {
					pivot_chunk.m_data[0] &=
					    ~(__sm_bitvec_t)0 >>
					    amt_over / SM_BITS_PER_VECTOR * 2;
				}
				if (amt_over % SM_BITS_PER_VECTOR) {
					/* Change only the flag at the position of the last index to "mixed" ... */
					SM_CHUNK_SET_FLAGS(
					    pivot_chunk.m_data[0], bv,
					    SM_PAYLOAD_MIXED);
					/* Partial run-tail vector: bits [0, first_zero%64) set. */
					const __sm_bitvec_t tail_mask =
					    ~(~(__sm_bitvec_t)0 << first_zero %
					            SM_BITS_PER_VECTOR);
					if (state == 0 && bv == (idx - aligned_idx) /
					        SM_BITS_PER_VECTOR) {
						/*
						 * The cleared bit shares the run-tail
						 * vector: keep the single MIXED payload
						 * already written by the state==0 setup
						 * (all-ones-minus-cleared-bit) and just
						 * mask off the bits past the run end.  No
						 * new payload, no size change.
						 */
						pivot_chunk.m_data[1] &= tail_mask;
					} else if (state == 0) {
						/*
						 * Distinct vectors (bv > vec_idx, since the
						 * cleared bit lies within the run).  The
						 * state==0 setup already placed the cleared
						 * bit's payload at m_data[1]; the run-tail
						 * MIXED needs its own payload at m_data[2]
						 * (higher flag index sorts after).  Writing
						 * it to m_data[1] as the non-state==0 path
						 * does would clobber the cleared-bit
						 * payload and leave pivot.size one vector
						 * short -- corrupting the chunk stream.
						 */
						pivot_chunk.m_data[2] = tail_mask;
						sep->pivot.size +=
						    sizeof(__sm_bitvec_t);
					} else {
						/* and unset the bits beyond that. */
						pivot_chunk.m_data[1] = tail_mask;
						if (state == -1) {
							sep->pivot.size +=
							    sizeof(__sm_bitvec_t);
						}
					}
				}
			}

			/* Move the pivot chunk over to make room for the new left chunk. */
			memmove((uint8_t *)((uintptr_t)sep->buf +
			            SM_SIZEOF_OVERHEAD +
			            (sizeof(__sm_bitvec_t) * 2)),
			    sep->buf, sep->pivot.size);
			memset(sep->buf, 0,
			    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
			sep->pivot.p +=
			    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2);

			/* Re-initialize pivot_chunk after the move */
			__sm_chunk_init(&pivot_chunk,
			    sep->pivot.p + SM_SIZEOF_OVERHEAD);

			/* Are we setting a bit beyond the length where we partially overlap? */
			if (state == 1 &&
			    idx > sep->target.start + sep->target.length) {
				const size_t vec_idx =
				    (idx - aligned_idx) / SM_BITS_PER_VECTOR;
				const size_t bit_pos =
				    (idx - aligned_idx) % SM_BITS_PER_VECTOR;
				const size_t existing_mixed =
				    __sm_chunk_get_size(&pivot_chunk) /
				        sizeof(__sm_bitvec_t) -
				    1;
				const size_t cur_flags = SM_CHUNK_GET_FLAGS(
				    pivot_chunk.m_data[0], vec_idx);
				if (cur_flags == SM_PAYLOAD_MIXED) {
					/* Same vector as the partial run -- just OR the bit in. */
					const size_t pos = 1 +
					    __sm_chunk_get_position(
					        &pivot_chunk, vec_idx);
					pivot_chunk.m_data[pos] |=
					    (__sm_bitvec_t)1 << bit_pos;
				} else {
					/* Different vector -- add a new MIXED flag and payload vector. */
					SM_CHUNK_SET_FLAGS(
					    pivot_chunk.m_data[0], vec_idx,
					    SM_PAYLOAD_MIXED);
					const size_t pos = 1 +
					    __sm_chunk_get_position(
					        &pivot_chunk, vec_idx);
					/* Shift existing vectors after this position to make room. */
					const size_t vecs_after =
					    existing_mixed - (pos - 1);
					if (vecs_after > 0) {
						memmove(&pivot_chunk
						             .m_data[pos + 1],
						    &pivot_chunk.m_data[pos],
						    vecs_after *
						        sizeof(__sm_bitvec_t));
					}
					pivot_chunk.m_data[pos] =
					    (__sm_bitvec_t)1 << bit_pos;
					sep->pivot.size +=
					    sizeof(__sm_bitvec_t);
				}
				/*
				 * The incremental size accounting above assumes
				 * the initial state==1 reservation (one payload
				 * vector) was consumed by a run-tail MIXED flag.
				 * When the run tail ended on a vector boundary
				 * (amt_over % SM_BITS_PER_VECTOR == 0) there is no
				 * run-tail MIXED, the reserved slot is free, and
				 * the += above over-counts pivot.size by one
				 * vector -- inflating expand_by and inserting a
				 * stray 8 bytes that desync the sequential chunk
				 * walk.  Recompute the pivot size from the chunk's
				 * actual flags so it is exact regardless of which
				 * combination of run-tail / new-bit vectors is
				 * present.
				 */
				sep->pivot.size = SM_SIZEOF_OVERHEAD +
				    __sm_chunk_get_size(&pivot_chunk);
			}
			/* Record information necessary to construct the left chunk. */
			sep->ex[0].start = sep->target.start;
			sep->ex[0].end = aligned_idx - 1;
			sep->ex[0].p = sep->buf;
			__sm_assert(sep->ex[0].start <= sep->ex[0].end);
			__sm_assert(sep->ex[1].p == 0);
			break;
		}

		if (aligned_idx >= sep->target.start + sep->target.length) {
			/* The pivot is beyond the run but within the capacity, two chunks. */
			sep->count = 2;
			/* Ensure the aligned chunk is fully in the range (length, capacity). */
			if (aligned_idx + SM_CHUNK_MAX_CAPACITY <
			    sep->target.capacity) {
				pivot_chunk.m_data[0] = (__sm_bitvec_t)0;
				/* Move the pivot chunk over to make room for the new left chunk. */
				memmove((uint8_t *)((uintptr_t)sep->buf +
				            SM_SIZEOF_OVERHEAD +
				            (sizeof(__sm_bitvec_t) * 2)),
				    sep->buf, sep->pivot.size);
				memset(sep->buf, 0,
				    SM_SIZEOF_OVERHEAD +
				        (sizeof(__sm_bitvec_t) * 2));
				sep->pivot.p += SM_SIZEOF_OVERHEAD +
				    sizeof(__sm_bitvec_t) * 2;

				/* Re-initialize pivot_chunk after the move */
				__sm_chunk_init(&pivot_chunk,
				    sep->pivot.p + SM_SIZEOF_OVERHEAD);

				if (state == 1) {
					/* Change only the flag at the position of the index to "mixed" ... */
					const size_t vec_idx =
					    (idx - aligned_idx) /
					    SM_BITS_PER_VECTOR;
					const size_t bit_pos =
					    (idx - aligned_idx) %
					    SM_BITS_PER_VECTOR;
					SM_CHUNK_SET_FLAGS(
					    pivot_chunk.m_data[0], vec_idx,
					    SM_PAYLOAD_MIXED);
					/* and set the bit at that index in this chunk. */
					pivot_chunk.m_data[1] |=
					    (__sm_bitvec_t)1 << bit_pos;
				}
				/* Record information necessary to construct the left chunk. */
				sep->ex[0].start = sep->target.start;
				sep->ex[0].end =
				    sep->target.start + sep->target.length - 1;
				sep->ex[0].p = sep->buf;
				break;
			}
			/*
			 * No `else`: the "pivot window does not fit within
			 * capacity" case is unreachable.  The RLE capacity is
			 * never allowed to extend a full empty window past the
			 * run's window-rounded end (see __sm_chunk_rle_capacity_
			 * limit), so start + capacity <= roundup(start + length,
			 * SM_CHUNK_MAX_CAPACITY).  With aligned_idx a window
			 * multiple and aligned_idx < start + capacity, that
			 * forces aligned_idx < start + length -- i.e. the
			 * enclosing (A) test above is itself never true, so the
			 * inner test is always true when reached.  Proven by the
			 * capacity invariant plus an exhaustive state==1 sweep
			 * (4740 state-1 separates over the full capacity/length
			 * regime, zero counter-examples).  An assert on the
			 * invariant guards against future capacity-policy
			 * changes reintroducing the case.
			 */
			__sm_assert(aligned_idx + SM_CHUNK_MAX_CAPACITY <
			    sep->target.capacity);
		}

		/* The pivot's range is central, there will be three chunks in total. */
		sep->count = 3;
		/* Move the pivot chunk over to make room for the new left chunk. */
		memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD +
		            (sizeof(__sm_bitvec_t) * 2)),
		    sep->buf, sep->pivot.size);
		memset(sep->buf, 0,
		    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
		sep->pivot.p +=
		    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2);
		/* Record information necessary to construct the left & right chunks. */
		sep->ex[0].start = sep->target.start;
		sep->ex[0].end = aligned_idx - 1;
		sep->ex[0].p = sep->buf;
		sep->ex[1].start = aligned_idx + SM_CHUNK_MAX_CAPACITY;
		sep->ex[1].end = sep->target.start + sep->target.length - 1;
		sep->ex[1].p = (uint8_t *)((uintptr_t)sep->buf +
		    (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2) +
		    sep->pivot.size);
		__sm_assert(sep->ex[0].start < sep->ex[0].end);
		__sm_assert(sep->ex[1].start < sep->ex[1].end);
	} while (0);

	for (int i = 0; i < 2; i++) {
		if (sep->ex[i].p) {
			/* First assign the starting offset ... */
			__sm_store_idx((uint8_t *)sep->ex[i].p,
			    sep->ex[i].start);
			/* ... then, construct a chunk ... */
			__sm_chunk_init(&lrc,
			    sep->ex[i].p + SM_SIZEOF_OVERHEAD);
			/* ... determine the type of chunk required ... */
			if (sep->ex[i].end - sep->ex[i].start + 1 >
			    SM_CHUNK_MAX_CAPACITY) {
				/* ... we need a run-length encoding (RLE), chunk ... */
				__sm_chunk_set_rle(&lrc);
				/* ... a few things differ left to right ... */
				if (i == 0) {
					/* ... left: extend capacity to the start of the pivot chunk ... */
					__sm_chunk_rle_set_capacity(&lrc,
					    aligned_idx - sep->ex[i].start);
					/* ... and shift the pivot chunk and start of lr[1] left one vector ... */
					memmove(
					    (uint8_t *)((uintptr_t)sep->buf +
					        SM_SIZEOF_OVERHEAD +
					        sizeof(__sm_bitvec_t)),
					    sep->pivot.p, sep->pivot.size);
					memset((uint8_t *)((uintptr_t)sep->buf +
					           SM_SIZEOF_OVERHEAD +
					           sizeof(__sm_bitvec_t) +
					           sep->pivot.size),
					    0, sizeof(__sm_bitvec_t));
					if (sep->ex[1].p) {
						sep->ex[1].p =
						    (uint8_t *)((uintptr_t)sep
						                    ->ex[1]
						                    .p -
						        sizeof(__sm_bitvec_t));
					}
				} else {
					/* ... right: capacity spans from THIS
					 * chunk's start to the end of the original
					 * target's capacity.  Use ex[i].start (the
					 * right chunk's actual aligned start), NOT
					 * aligned_idx (the pivot's start): the two
					 * differ by SM_CHUNK_MAX_CAPACITY whenever the
					 * pivot sits to the left of the right chunk
					 * (every left-aligned and central split).
					 * Using aligned_idx over-counts the capacity by
					 * one window, so the right RLE's capacity
					 * overruns into the following chunk's index
					 * range and the sequential walk resolves
					 * lookups against the wrong chunk. */
					size_t right_cap =
					    (sep->target.start +
					        sep->target.capacity) -
					    sep->ex[i].start;
					if (right_cap >
					    SM_CHUNK_RLE_MAX_CAPACITY) {
						right_cap =
						    SM_CHUNK_RLE_MAX_CAPACITY;
					}
					__sm_chunk_rle_set_capacity(&lrc,
					    right_cap);
				}
				/* Capacity is set before length to satisfy the invariant */
				const size_t rle_length =
				    sep->ex[i].end - sep->ex[i].start + 1;
				__sm_chunk_rle_set_length(&lrc, rle_length);
				/* ... and record our chunk size. */
				sep->ex[i].size =
				    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
			} else {
				/* ... we need a new sparse chunk, how long should it be? ... */
				const size_t lrl =
				    sep->ex[i].end - sep->ex[i].start + 1;
				/* ... how many flags can we mark as all ones? ... */
				if (lrl >= SM_BITS_PER_VECTOR) {
					/*
					 * `>=` not `>`: a run of exactly one vector
					 * (lrl == SM_BITS_PER_VECTOR) still needs its
					 * single ONES flag set.  With `>` the lrl ==
					 * 64 case fell through with an all-zero flags
					 * word, producing an empty chunk that dropped
					 * a full vector of set bits.  lrl < 64 is
					 * handled by the MIXED branch below, so it
					 * never reaches the UB-shift here.
					 */
					lrc.m_data[0] = ~(__sm_bitvec_t)0 >>
					    (SM_FLAGS_PER_INDEX -
					        lrl / SM_BITS_PER_VECTOR) *
					        2;
				}
				/* ... do we have a mixed flag to create and vector to assign? ... */
				if (lrl % SM_BITS_PER_VECTOR) {
					/*
					 * The vector index is *within* the chunk, not absolute.
					 * Pre-fix this was `(aligned_idx + lrl) / SM_BITS_PER_VECTOR`
					 * which mixes absolute bit position (aligned_idx) with a
					 * chunk-relative length (lrl) and produces shift exponents
					 * way past 64 -- UBSan flagged this with shift-exponent
					 * errors of 64 / 92 / 638 / 702.
					 */
					SM_CHUNK_SET_FLAGS(lrc.m_data[0],
					    lrl / SM_BITS_PER_VECTOR,
					    SM_PAYLOAD_MIXED);
					lrc.m_data[1] |= ~(__sm_bitvec_t)0 >>
					    (SM_BITS_PER_VECTOR - lrl) %
					        SM_BITS_PER_VECTOR;
					/* ... record our chunk size ... */
					sep->ex[i].size = SM_SIZEOF_OVERHEAD +
					    sizeof(__sm_bitvec_t) * 2;
				} else {
					/* ... earlier size estimates were all pessimistic, adjust them ... */
					if (i == 0) {
						/* ... and shift the pivot chunk and start of lr[1] left one vector ... */
						memmove(
						    (uint8_t *)((uintptr_t)
						                    sep->buf +
						        SM_SIZEOF_OVERHEAD +
						        sizeof(__sm_bitvec_t)),
						    sep->pivot.p,
						    sep->pivot.size);
						memset(
						    (uint8_t *)((uintptr_t)
						                    sep->buf +
						        SM_SIZEOF_OVERHEAD +
						        sizeof(__sm_bitvec_t) +
						        sep->pivot.size),
						    0, sizeof(__sm_bitvec_t));
						if (sep->ex[1].p) {
							sep->ex[1].p = (uint8_t
							        *)((uintptr_t)sep
							               ->ex[1]
							               .p -
							    sizeof(
							        __sm_bitvec_t));
						}
					}
					/* ... record our chunk size ... */
					sep->ex[i].size = SM_SIZEOF_OVERHEAD +
					    sizeof(__sm_bitvec_t);
				}
			}
		}
	}

	/* Determine if we have room for this construct. */
	/*
	 * Defense in depth: pre-fix this could compute a negative size_t
	 * if pivot/ex sizes hadn't been populated, propagating into
	 * __sm_insert_data as a SIZE_MAX-ish length and tripping stack
	 * canaries / heap corruption.
	 */
	const size_t base = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
	const size_t total =
	    sep->pivot.size + sep->ex[0].size + sep->ex[1].size;
	if (total < base) {
		__sm_when_diag({
			__sm_assert(0 &&
			    "__sm_separate_rle_chunk: pivot/ex sizes uninitialized");
		});
		errno = EINVAL;
		return (-1);
	}
	sep->expand_by = total - base;
	/*
	 * __sm_insert_data's memmove length (m_data_used - offset) treats
	 * `offset` as m_data-relative while the caller passes a data-region
	 * offset, so the shift writes to m_data + m_data_used + expand_by +
	 * SM_SIZEOF_OVERHEAD -- SM_SIZEOF_OVERHEAD past m_data_used +
	 * expand_by.  The SM_ENOUGH_SPACE macro carries the same slack for
	 * this reason; without it here the separate overruns the buffer by
	 * SM_SIZEOF_OVERHEAD bytes at the exact-fit boundary (used +
	 * expand_by == cap) instead of cleanly returning ENOSPC so
	 * sm_add_grow can grow and retry.
	 */
	if (map->m_data_used + sep->expand_by + SM_SIZEOF_OVERHEAD >
	    __sm_cap(map)) {
		errno = ENOSPC;
		return (-1);
	}

	/* Let's knit this into place within the map. */
	__sm_insert_data(map,
	    sep->target.offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t),
	    sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t),
	    sep->expand_by);
	memcpy(sep->target.p, sep->buf,
	    sep->expand_by + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
	__sm_set_chunk_count(map, __sm_get_chunk_count(map) + (sep->count - 1));

	return (0);
}

/* -------------------------------------------------------------------
 * Lifecycle: construction, copy, disposal, and buffer resize
 * ------------------------------------------------------------------- */

/**
 * @brief Clears the given sparse map.
 *
 * This function resets the sparse map by setting all its data to zero and updating
 * its metadata to reflect an empty map.
 *
 * @param[in] map The sparse map to clear.
 */
void
sm_clear(sm_t *map)
{
	if (map == NULL) {
		return;
	}
	memset(map->m_data, 0, __sm_cap(map));
	map->m_data_used = SM_SIZEOF_OVERHEAD;
	__sm_set_chunk_count(map, 0);
}

/**
 * @brief Allocates and initializes a sparsemap of the given size.
 *
 * This function creates a new sparsemap structure with allocated memory.
 * If the specified size is zero, a default size of 1024 is used. The function
 * ensures that the internal data array is 8-byte aligned and initializes the sparsemap
 * structure.
 *
 * @param[in] size The size of the sparsemap to allocate.
 * @return A pointer to the allocated sparsemap structure, or NULL if allocation fails.
 */
sm_t *
sparsemap(size_t size)
{
	return (sm_create(size));
}

/**
 * @brief Allocates and initializes a sparsemap of the given size.
 *
 * This function creates a new sparsemap structure with allocated memory.
 * If the specified size is zero, a default size of 1024 is used. The function
 * ensures that the internal data array is 8-byte aligned and initializes the sparsemap
 * structure.
 *
 * @param[in] size The size of the sparsemap to allocate.
 * @return A pointer to the allocated sparsemap structure, or NULL if allocation fails.
 */
sm_t *
sm_create(size_t size)
{
	if (size == 0) {
		size = 1024;
	}

	/* Round up to an 8-byte boundary so the data region we allocate
	 * and the (low-bit-tagged) stored capacity agree exactly. */
	size = (size + 7u) & ~(size_t)7;

	const size_t data_size = size * sizeof(uint8_t);

	/* Ensure that m_data is 8-byte aligned. */
	size_t total_size = sizeof(sm_t) + data_size;
	const size_t padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
	total_size += padding;

	sm_t *map = (sm_t *)__sm_alloc_zero(total_size);
	if (map) {
		uint8_t *data = (uint8_t *)(((uintptr_t)map + sizeof(sm_t)) &
		    ~(uintptr_t)7);
		sm_init(map, data, size);
		/*
		 * sm_init tags the map as SM_WRAPPED (caller-supplied
		 * buffer); override here because the buffer is contiguous with the
		 * struct and we own both.
		 */
		__sm_set_kind(map, SM_OWNED_CONTIGUOUS);
		__sm_when_diag(
		    { __sm_assert(IS_8_BYTE_ALIGNED(map->m_data)); });
	}
	return (map);
}

/**
 * @brief Disposes of a sparsemap, regardless of allocation lineage.
 *
 * SM_OWNED_CONTIGUOUS  free(map) -- the struct and buffer share one block.
 * SM_OWNED_SPLIT       free(map->m_data) + free(map).
 * SM_WRAPPED           free(map) only -- the data buffer is the caller's
 *                      and is left untouched.
 *
 * Calling with NULL is a no-op.
 */
void
sm_free(sm_t *map)
{
	if (map == NULL) {
		return;
	}
	switch (__sm_kind(map)) {
	case SM_OWNED_SPLIT:
		__sm_free(map->m_data);
		/* fallthrough */
	case SM_OWNED_CONTIGUOUS:
	case SM_WRAPPED:
	default:
		__sm_free(map);
		break;
	}
}

/**
 * @brief Returns a guaranteed-owned, guaranteed-growable copy of \a map.
 *
 * The result is always SM_OWNED_CONTIGUOUS (single calloc, struct +
 * buffer in one heap block).  Use this when you have a sparsemap whose
 * lineage you don't trust and need a self-contained copy that's safe to
 * grow and dispose with sm_free() or libc free().
 */
sm_t *
sm_owned_copy(const sm_t *map)
{
	if (map == NULL) {
		return (NULL);
	}
	const size_t cap = sm_get_capacity(map);
	sm_t *out = sm_create(cap);
	if (out == NULL) {
		return (NULL);
	}
	out->m_data_used = map->m_data_used;
	/* m_capacity is already cap; lineage is SM_OWNED_CONTIGUOUS. */
	if (cap > 0 && map->m_data != NULL) {
		memcpy(out->m_data, map->m_data, cap);
	}
	return (out);
}

/**
 * @brief Creates a copy of the given sparse map.
 *
 * This function duplicates the provided sparse map, allocating a new sparse
 * map instance with the same capacity and copying over the used data.
 *
 * @param[in] other The sparse map to be copied.
 * @return A pointer to the newly created sparse map that is a copy of the input,
 *         or NULL if the memory allocation fails.
 */
sm_t *
sm_copy(const sm_t *other)
{
	const size_t cap = sm_get_capacity(other);
	sm_t *map = sparsemap(cap);
	if (map) {
		__sm_set_cap_kind(map, cap, SM_OWNED_CONTIGUOUS);
		map->m_data_used = other->m_data_used;
		memcpy(map->m_data, other->m_data, cap);
	}
	return (map);
}

/**
 * @brief Wraps a given data array into a sparsemap structure.
 *
 * Allocates and initializes a sm_t structure to manage a provided data array.
 * The sparsemap structure will point to the data array and will track its capacity.
 *
 * @param[in] data Pointer to the data array to be managed by the sparsemap.
 * @param[in] size The size of the data array.
 * @return A pointer to the initialized sm_t structure, or NULL if allocation fails.
 */
sm_t *
sm_wrap(uint8_t *data, const size_t size)
{
	/* Wrap allocates only the struct (caller owns the data buffer);
	 * route through the global allocator so sm_free works correctly. */
	sm_t *map = (sm_t *)__sm_alloc_zero(sizeof(sm_t));
	if (map) {
		map->m_data = data;
		map->m_data_used = 0;
		__sm_set_cap_kind(map, size, SM_WRAPPED);
	}
	return (map);
}

/**
 * @brief Initializes a sparsemap with the provided data and size.
 *
 * This function sets up the initial state of a sparsemap by assigning the given
 * data buffer and capacity. It also clears the sparsemap to ensure it starts empty.
 *
 * @param[in] map A pointer to the sparsemap to initialize.
 * @param[in] data A pointer to the data buffer to be used by the sparsemap.
 * @param[in] size The size of the data buffer in bytes.
 */
void
sm_init(sm_t *map, uint8_t *data, const size_t size)
{
	map->m_data = data;
	map->m_data_used = 0;
	__sm_set_cap_kind(map, size, SM_WRAPPED);
	/*
	 * Caller-allocated struct + caller-allocated buffer.  The buffer is
	 * not owned by the library; sm_set_data_size will treat any
	 * grow as a wrap-style promotion (allocate fresh, copy, transition
	 * to SM_OWNED_SPLIT).  sparsemap() overrides this to
	 * SM_OWNED_CONTIGUOUS after calling us.
	 */
	sm_clear(map);
}

/**
 * @brief Initializes a sparse map with given data and size.
 *
 * This function sets up the sparse map by assigning the provided data array and
 * size, and calculates the initial data usage.
 *
 * @param[in,out] map The sparse map to initialize.
 * @param[in] data Pointer to the data array to be used by the sparse map.
 * @param[in] size The capacity of the data array.
 */
void
sm_open(sm_t *map, uint8_t *data, const size_t size)
{
	map->m_data = data;
	/*
	 * Set m_capacity and a temporary m_data_used = capacity *before*
	 * calling __sm_get_size_impl.  __sm_get_size_impl walks chunks via
	 * __sm_get_chunk_count, which since v1.0.0 short-circuits to 0
	 * when m_data_used < SM_SIZEOF_OVERHEAD (the empty-map guard for
	 * the heisenbug-related fix).  Without the temporary, sm_open of
	 * a fully-populated buffer reads its chunk count as 0 and produces
	 * a stunt-map with m_data_used = 4 -- which then trips a size_t
	 * underflow downstream when something tries to insert at the
	 * supposed-end of the chunks region.
	 *
	 * sm_open is for deserializing into a caller-supplied
	 * struct + buffer; lineage matches sm_init (SM_WRAPPED).
	 */
	__sm_set_cap_kind(map, size, SM_WRAPPED);
	map->m_data_used = __sm_cap(map);
	map->m_data_used = __sm_get_size_impl(map);
}

sm_t *
sm_open_copy(const uint8_t *data, size_t n, size_t slack)
{
	if (data == NULL && n > 0)
		return (NULL);
	/* sm_create needs at least SM_SIZEOF_OVERHEAD bytes; bump up if the
	 * caller asked for less. */
	size_t cap = n + slack;
	if (cap < SM_SIZEOF_OVERHEAD)
		cap = SM_SIZEOF_OVERHEAD;
	sm_t *m = sm_create(cap);
	if (m == NULL)
		return (NULL);
	if (n > 0) {
		memcpy(sm_get_data(m), data, n);
		/* sm_open re-derives m_data_used from the chunk count + walk;
		 * temporarily set m_data_used = capacity so the empty-map guard
		 * in __sm_get_chunk_count doesn't short-circuit during the walk. */
		m->m_data_used = __sm_cap(m);
		m->m_data_used = __sm_get_size_impl(m);
	}
	/* sm_open's regular implementation transitions the lineage to
	 * SM_WRAPPED -- but here the buffer is contiguous with the struct
	 * because we got it from sm_create.  Restore the correct lineage so
	 * sm_free does the right thing and so subsequent grows can use the
	 * single-block realloc path. */
	__sm_set_kind(m, SM_OWNED_CONTIGUOUS);
	return (m);
}

/**
 * @brief Resizes the data buffer of the sparsemap.
 *
 * Behaviour depends on the calling form and the map's allocation
 * lineage:
 *
 *   sm_set_data_size(map, NULL, size)
 *     Library-managed grow / shrink.  Always succeeds (returning a
 *     possibly-relocated map pointer) or returns NULL on allocation
 *     failure.  Never silently no-ops the resize.
 *
 *       SM_OWNED_CONTIGUOUS -- realloc the single struct+buffer block.
 *                             Caller must update all map references to
 *                             the returned pointer.
 *       SM_OWNED_SPLIT      -- realloc m_data; map struct stays put.
 *       SM_WRAPPED          -- if size <= m_capacity, simply update
 *                             m_capacity (caller's buffer is still
 *                             theirs).  If size > m_capacity, allocate
 *                             a fresh library-owned buffer of the
 *                             requested size, memcpy the m_data_used
 *                             prefix into it, redirect m_data, and
 *                             transition lineage to SM_OWNED_SPLIT.
 *                             The caller's original buffer is left
 *                             untouched and remains theirs.
 *
 *   sm_set_data_size(map, data, size)  [data != NULL]
 *     Re-point the map at a caller-supplied buffer.  m_capacity is
 *     updated; copying any existing bits is the caller's
 *     responsibility.  Lineage transitions to SM_WRAPPED -- the library
 *     does not own the new buffer and will not realloc/free it on the
 *     caller's behalf.
 *
 * @param[in,out] map   The sparsemap to resize.  Must be non-NULL.
 * @param[in]     data  Optional caller-supplied buffer; NULL means
 *                      "library decides".
 * @param[in]     size  New buffer size in bytes.
 * @return The (possibly relocated) sparsemap pointer on success,
 *         or NULL on allocation failure.
 */
sm_t *
sm_set_data_size(sm_t *map, uint8_t *data, const size_t size)
{
	if (map == NULL) {
		return (NULL);
	}

	/* Caller-driven re-point: trust them, transition to SM_WRAPPED. */
	if (data != NULL) {
		if (data != map->m_data) {
			map->m_data = data;
		}
		__sm_set_cap_kind(map, size, SM_WRAPPED);
		return (map);
	}

	/* Library-managed resize.  Round the requested size up to an
	 * 8-byte boundary so the allocated buffer and the stored (low-bit-
	 * tagged) capacity agree exactly; __sm_set_cap_kind rounds down,
	 * so an already-aligned size round-trips unchanged. */
	const size_t asize = (size + 7u) & ~(size_t)7;
	const size_t cur_cap = __sm_cap(map);
	switch (__sm_kind(map)) {
	case SM_OWNED_CONTIGUOUS: {
		if (size == cur_cap) {
			return (map);
		}
		/*
		 * Realloc the single block.  Allocate room for the struct + the
		 * new data buffer + alignment padding so m_data lands on an 8-byte
		 * boundary.
		 */
		size_t total_size = sizeof(sm_t) + size;
		const size_t padding =
		    total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
		total_size += padding;

		const size_t old_capacity = cur_cap;
		sm_t *m = (sm_t *)__sm_realloc(map, total_size);
		if (!m) {
			/* Original block still valid; leave map untouched. */
			return (NULL);
		}
		m->m_data =
		    (uint8_t *)(((uintptr_t)m + sizeof(sm_t)) & ~(uintptr_t)7);
		if (size > old_capacity) {
			/* Zero the newly-acquired tail so chunk metadata stays clean. */
			memset(m->m_data + old_capacity, 0,
			    size - old_capacity);
		}
		__sm_set_cap_kind(m, size, SM_OWNED_CONTIGUOUS);
		/*
		 * m_data_used does not change on grow; on shrink the caller is
		 * responsible for ensuring m_data_used <= size before calling.
		 */
		if (m->m_data_used > __sm_cap(m)) {
			m->m_data_used = __sm_cap(m);
		}
		__sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(m->m_data)); });
		return (m);
	}

	case SM_OWNED_SPLIT: {
		if (size == cur_cap) {
			return (map);
		}
		uint8_t *new_data =
		    (uint8_t *)__sm_realloc(map->m_data, size);
		if (!new_data) {
			return (NULL);
		}
		if (size > cur_cap) {
			memset(new_data + cur_cap, 0, size - cur_cap);
		}
		map->m_data = new_data;
		__sm_set_cap_kind(map, size, SM_OWNED_SPLIT);
		if (map->m_data_used > __sm_cap(map)) {
			map->m_data_used = __sm_cap(map);
		}
		return (map);
	}

	case SM_WRAPPED: {
		/*
		 * Caller owns m_data.  Two cases:
		 *
		 *   size <= capacity (shrink or same):
		 *     We do not own the buffer, so we cannot realloc/free it.  Just
		 *     update the recorded capacity to "use no more than `size`
		 *     bytes of the caller's buffer".  The caller's buffer is
		 *     unchanged and remains theirs to free.
		 *
		 *   size > capacity (grow):
		 *     Allocate a fresh library-owned buffer of the requested size,
		 *     copy the in-use prefix (m_data_used bytes), redirect m_data,
		 *     transition lineage to SM_OWNED_SPLIT.  The caller's original
		 *     buffer is untouched and remains theirs.
		 *
		 *     This is the path that fixes the heisenbug from
		 *     HEISENBUG_REPORT.md: pre-fix, the function silently set
		 *     the capacity without allocating storage, and the next
		 *     sm_add corrupted the heap.
		 */
		if (size <= cur_cap) {
			__sm_set_cap_kind(map, size, SM_WRAPPED);
			if (map->m_data_used > __sm_cap(map)) {
				map->m_data_used = __sm_cap(map);
			}
			return (map);
		}

		uint8_t *new_data = (uint8_t *)__sm_alloc_zero(asize);
		if (!new_data) {
			return (NULL);
		}
		const size_t copy_bytes = map->m_data_used <= cur_cap ?
		    map->m_data_used :
		    cur_cap;
		if (copy_bytes > 0 && map->m_data != NULL) {
			memcpy(new_data, map->m_data, copy_bytes);
		}
		map->m_data = new_data;
		__sm_set_cap_kind(map, asize, SM_OWNED_SPLIT);
		return (map);
	}
	}

	/* Unreachable. */
	__sm_when_diag(
	    { __sm_assert(0 && "unknown sparsemap allocation lineage"); });
	return (NULL);
}

/**
 * @brief Calculates the remaining capacity of the sparsemap.
 *
 * This function returns the percentage of unused capacity in the sparse map.
 * If the used capacity is equal to or exceeds the total capacity, it returns 0.
 * If the total capacity is 0, it returns 100. Otherwise, it returns the
 * percentage of capacity remaining.
 *
 * @param[in] map The sparsemap for which the remaining capacity is calculated.
 * @return The percentage of remaining capacity in the sparsemap.
 */
double
sm_capacity_remaining(const sm_t *map)
{
	const size_t cap = __sm_cap(map);
	if (map->m_data_used >= cap) {
		return (0);
	}
	if (cap == 0) {
		return (100.0);
	}
	return ((1.0 - ((double)map->m_data_used / (double)cap)) * 100.0);
}

/**
 * @brief Retrieves the capacity of the sparse map.
 *
 * This function returns the total capacity of the given sparse map, which is
 * the size of the underlying data structure.
 *
 * @param[in] map Pointer to the sparse map.
 * @return The capacity of the sparse map.
 */
size_t
sm_get_capacity(const sm_t *map)
{
	return (__sm_cap(map));
}

/* -------------------------------------------------------------------
 * Single-bit operations: test, set, and clear
 * ------------------------------------------------------------------- */

/**
 * @brief Checks if a specific bit is set in the sparse map.
 *
 * This function determines whether the bit at the given index is set in the
 * sparse map. It performs various checks and traverses to the appropriate
 * chunk to verify the bit's state.
 *
 * @param[in] map The sparse map to check.
 * @param[in] idx The index of the bit to check.
 * @return True if the bit is set, false otherwise.
 */
SM_HOT bool
sm_contains(const sm_t *map, uint64_t idx, sm_cursor_t *cur)
{
	/* Defensive: NULL or empty maps contain nothing.  Accepting NULL is
	 * cheap insurance for consumers that pass the result of
	 * sm_intersection / sm_difference / sm_xor unchecked, which
	 * legitimately return NULL when the result is empty. */
	if (map == NULL) {
		return (false);
	}
	__sm_assert(sm_get_size((sm_t *)map) >= SM_SIZEOF_OVERHEAD);

	/* Get the __sm_chunk_t which manages this index */
	const ssize_t offset = __sm_get_chunk_offset(map, idx, cur);

	/* No __sm_chunk_t's available -> the bit is not set */
	if (offset == -1) {
		return (false);
	}

	/* Otherwise load the __sm_chunk_t */
	uint8_t *p = __sm_get_chunk_data(map, offset);
	const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
	__sm_chunk_t chunk;
	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);

	/*
	 * Determine if the bit is out of bounds of the __sm_chunk_t; if yes then
	 * the bit is not set.
	 */
	if (idx < start ||
	    (__sm_idx_t)idx - start >= __sm_chunk_get_capacity(&chunk)) {
		return (false);
	}

	/* Otherwise ask the __sm_chunk_t whether the bit is set. */
	return (__sm_chunk_is_set(&chunk, idx - start));
}

/**
 * @brief Unsets a bit at a specified index in the given sparse map.
 *
 * This function clears the bit at the given index in the sparse map. It handles
 * different scenarios, including chunks that do not exist for the specified index,
 * run-length encoded (RLE) chunks, and sparse chunks.
 *
 * The function also optionally performs chunk coalescing if the `coalesce` flag is set.
 *
 * @param[in,out] map The sparse map in which the bit needs to be unset.
 * @param[in] idx The index of the bit to be unset.
 * @param[in] coalesce A flag indicating whether to perform chunk coalescing.
 * @return The index of the bit that was unset.
 */
/*
 * Sentinel stored in the size_t byte-offset variable `offset` to gate chunk
 * coalescing off (the chunk was never located or its pointers are now stale).
 * It MUST be size_t-width: a uint64_t sentinel (SM_IDX_MAX == UINT64_MAX)
 * truncates to 0xFFFFFFFF on ILP32 targets, so the `!=` gate test (which
 * promotes offset back to 64 bits) never matches and coalescing runs on an
 * uninitialized chunk -- a 32-bit-only crash.
 */
#define SM_UNSET_NO_COALESCE ((size_t)-1)
static __sm_idx_t
__sm_map_unset(sm_t *map, uint64_t idx, const bool coalesce)
{
	const uint64_t ret_idx = idx;
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);

	/* Clearing a bit could require an additional vector, let's ensure we have that
	 * space available in the buffer first, or ENOMEM now. */
	SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

	/* Determine if there is a chunk that could contain this index. */
	size_t offset = __sm_get_chunk_offset(map, idx, NULL);
	size_t chunk_offset = offset;

	if ((ssize_t)offset == -1) {
		/* There are no chunks in the map, there is nothing to clear, this is a
		 * no-op. */
		offset =
		    SM_UNSET_NO_COALESCE; /* gate coalesce off; chunk is uninitialized */
		goto done;
	}

	/*
	 * Try to locate a chunk for this idx.  We could find that:
	 * - the first chunk's offset is greater than the index, or
	 * - the index is beyond the end of the last chunk, or
	 * - we found a chunk that can contain this index.
	 */
	uint8_t *p = __sm_get_chunk_data(map, offset);
	const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
	__sm_assert(start == __sm_get_chunk_aligned_offset(start));

	if (idx < start) {
		/* Our search resulted in the first chunk that starts after the index but
		 * that means there is no chunk that contains this index, so again this is
		 * a no-op. */
		offset =
		    SM_UNSET_NO_COALESCE; /* gate coalesce off; chunk is uninitialized */
		goto done;
	}

	__sm_chunk_t chunk;
	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	const size_t capacity = __sm_chunk_get_capacity(&chunk);

	if (idx - start >= capacity) {
		/*
		 * Our search resulted in a chunk however it's capacity doesn't encompass
		 * this index, so again a no-op.
		 */
		offset = SM_UNSET_NO_COALESCE; /* gate coalesce off; chunk untouched */
		goto done;
	}

	if (__sm_chunk_is_rle(&chunk)) {
		/*
		 * Our search resulted in a chunk that is run-length encoded (RLE).  There
		 * are three possibilities at this point: 1) the index is at the end of the
		 * run, so we just shorten then length; 2) the index is between start and
		 * end [start, end) so we have to split this chunk up; 3) the index is
		 * beyond the length but within the capacity, then clearing it is a no-op.
		 * If the chunk length shrinks to the max capacity of sparse encoding we
		 * have to transition its encoding.
		 */

		/* Is the 0-based index beyond the run length? */
		const size_t length = __sm_chunk_rle_get_length(&chunk);
		if (idx >= start + length) {
			goto done;
		}

		/* Is the 0-based index referencing the last bit in the run? */
		if (idx - start + 1 == length) {
			/* Should the run-length chunk transition into a sparse chunk? */
			if (length - 1 == SM_CHUNK_MAX_CAPACITY) {
				chunk.m_data[0] = ~(__sm_bitvec_t)0;
			} else {
				__sm_chunk_rle_set_length(&chunk, length - 1);
			}
			goto done;
		}

		/*
		 * Now that we've addressed (1) and (3) we have to work on (2) where the
		 * index is within the body of this RLE chunk. Chunks must have an aligned
		 * starting offset, so let's first find what we'll call the "pivot" chunk
		 * wherein we'll find the index we need to clear. That chunk will be sparse.
		 */
		__sm_chunk_sep_t sep = { .target = { .p = p,
			                     .offset = offset,
			                     .chunk = &chunk,
			                     .start = start,
			                     .length = length,
			                     .capacity = capacity } };
		if (__sm_separate_rle_chunk(map, &sep, idx, 0) != 0) {
			/* Out of space (or invalid): the map was left
			 * unmodified.  Propagate ENOSPC so sm_add_grow /
			 * sm_remove callers can grow and retry. */
			return (SM_IDX_MAX);
		}
		/* Skip coalescing after RLE separation - the pointers are now invalid */
		offset = SM_UNSET_NO_COALESCE;
		goto done;
	}

	size_t pos = 0;
	__sm_bitvec_t vec = ~(__sm_bitvec_t)0;
	switch (__sm_chunk_clr_bit(&chunk, idx - start, &pos)) {
	case SM_OK:
		break;
	case SM_NEEDS_TO_GROW:
		SM_ENOUGH_SPACE(sizeof(__sm_bitvec_t));
		offset += SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
		__sm_insert_data(map, offset, (uint8_t *)&vec,
		    sizeof(__sm_bitvec_t));
		__sm_chunk_clr_bit(&chunk, idx - start, &pos);
		break;
	case SM_NEEDS_TO_SHRINK:
		/* The vector is empty, perhaps the entire chunk is empty? */
		if (__sm_chunk_is_empty(&chunk)) {
			__sm_remove_data(map, offset,
			    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
			__sm_set_chunk_count(map,
			    __sm_get_chunk_count(map) - 1);
		} else {
			offset +=
			    SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
			__sm_remove_data(map, offset, sizeof(__sm_bitvec_t));
		}
		break;
	default:
		__sm_assert(!"shouldn't be here");
#ifdef DEBUG
		abort();
#endif
		break;
	}

done:;
	if (coalesce && offset != SM_UNSET_NO_COALESCE) {
		__sm_coalesce_chunk(map, &chunk, chunk_offset, start, p, idx,
		    false, SIZE_MAX);
	}
	return (ret_idx);
}

/**
 * @brief Unsets the value at a specific index in the sparse map.
 *
 * This function calls the internal __sm_map_unset function with the coalesce parameter
 * set to true, which removes an entry at the specified index and attempts to merge adjacent
 * segments to maintain the map's sparsity.
 *
 * @param[in] map The sparse map in which the value will be unset.
 * @param[in] idx The index at which the value will be unset.
 * @return The index that was unset.
 */
SM_HOT uint64_t
sm_remove(sm_t *map, const uint64_t idx)
{
	return (__sm_map_unset(map, idx, true));
}

/**
 * @brief Sets a bit in a chunk within the sparse map and manages chunk resizing.
 *
 * This function sets a bit in the chunk of a sparse map corresponding to the
 * given index. It handles the initialization, setting the bit, and necessary
 * memory adjustments for growing or shrinking chunks, including allocation and
 * deallocation of bit vectors.
 *
 * @param[in,out] map The sparse map where the bit will be set.
 * @param[in] idx The index within the sparse map where the bit will be set.
 * @param[in] p A pointer to the chunk data within the sparse map.
 * @param[in] offset The offset within the sparse map's data where the chunk is located.
 * @param[in] v A bit vector, when non-NULL, indicates that a new chunk has been added.
 *
 * @return The index at which the bit was set.
 */
static __sm_idx_t
__sparsemap_add(sm_t *map, const uint64_t idx, uint8_t *p, size_t offset,
    const void *v)
{
	/*
	 * When v is non-NULL we've just added a new chunk, and we knew in advance that a
	 * new chunk would result in an SM_PAYLOAD_MIXED which in turn requires space to
	 * store the bit pattern, so given that we allocated the space ahead of time we
	 * don't need to allocate it now.
	 */
	size_t pos = v ? (size_t)-1 : 0;
	__sm_chunk_t chunk;
	const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);

	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	__sm_assert(__sm_chunk_is_rle(&chunk) == false);

	switch (__sm_chunk_set_bit(&chunk, idx - start, &pos)) {
	case SM_OK:
		break;
	case SM_NEEDS_TO_GROW:
		if (!v) {
			__sm_bitvec_t vec = 0;
			SM_ENOUGH_SPACE(sizeof(__sm_bitvec_t));
			offset +=
			    SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
			__sm_insert_data(map, offset, (uint8_t *)&vec,
			    sizeof(__sm_bitvec_t));
			pos = (size_t)-1;
		}
		__sm_chunk_set_bit(&chunk, idx - start, &pos);
		break;
	case SM_NEEDS_TO_SHRINK:
		/* The vector is empty, perhaps the entire chunk is empty? */
		if (__sm_chunk_is_empty(&chunk)) {
			__sm_remove_data(map, offset,
			    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
			__sm_set_chunk_count(map,
			    __sm_get_chunk_count(map) - 1);
		} else {
			offset +=
			    SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
			__sm_remove_data(map, offset, sizeof(__sm_bitvec_t));
		}
		break;
	default:
		__sm_assert(!"shouldn't be here");
#ifdef DEBUG
		abort();
#endif
		break;
	}

	return (idx);
}

/**
 * @brief Sets a bit in the sparse bit map.
 *
 * This function sets a bit at the given index in the provided sparse bit map.
 * It performs various internal checks and operations to ensure the data integrity of the map,
 * including initializing, inserting new chunks, and transitioning chunk states when necessary.
 *
 * @param[in,out] map The sparse bit map to be modified.
 * @param[in] idx The index of the bit to set.
 * @param[in] coalesce A flag indicating whether to attempt chunk coalescing.
 * @return Returns the adjusted index within the sparse bit map or the given index.
 */
static __sm_idx_t
__sm_map_set(sm_t *map, uint64_t idx, const bool coalesce, sm_cursor_t *cur)
{
	__sm_chunk_t chunk;
	uint64_t ret_idx = idx;
	__sm_idx_t start;
	uint8_t *p;
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);

	/*
	 * Setting a bit could require an additional vector, let's ensure we have that
	 * space available in the buffer first, or ENOMEM now.
	 */
	SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

	/* Determine if there is a chunk that could contain this index. */
	size_t offset = __sm_get_chunk_offset(map, idx, cur);

	/* Free left-neighbor hint for the coalescing path: the forward walk
	 * above already passed over the chunk immediately before the located
	 * chunk and recorded its byte offset.  It stays valid ONLY while the
	 * located chunk keeps its position; every path below that inserts,
	 * separates, or shifts chunk layout at/before `offset` resets it to
	 * SIZE_MAX so a stale hint is never produced.  A SIZE_MAX hint just
	 * makes __sm_coalesce_chunk fall back to a head-walk. */
	size_t left_hint = (cur != NULL) ? cur->prev_offset : SIZE_MAX;

	if ((ssize_t)offset == -1) {
		/*
		 * No chunks exist, the map is empty, so we must append a new chunk to the
		 * end of the buffer and initialize it so that it can contain this index.
		 */
		const uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)] = { 0 };
		__sm_append_data(map, &buf[0], sizeof(buf));
		p = __sm_get_chunk_data(map, 0);
		__sm_store_idx((uint8_t *)p,
		    __sm_get_chunk_aligned_offset(idx));
		__sm_set_chunk_count(map, 1);

		const __sm_bitvec_unaligned_t *v =
		    (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		        SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
		ret_idx = __sparsemap_add(map, idx, p, 0, v);

		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		start = __sm_load_idx((const uint8_t *)p);
		offset = 0;
		left_hint = SIZE_MAX; /* fresh append; no left neighbor */
		goto done;
	}

	/*
	 * Try to locate a chunk for this idx.  We could find that:
	 *  - the first chunk's offset is greater than the index, or
	 *  - the index is beyond the end of the last chunk, or
	 *  - we found a chunk that can contain this index.
	 */
	p = __sm_get_chunk_data(map, offset);
	start = __sm_load_idx((const uint8_t *)p);
	__sm_assert(start == __sm_get_chunk_aligned_offset(start));

	if (idx < start) {
		/*
		 * Our search resulted in the first chunk, but it starts after the index,
		 * so that means there is no chunk that can contain this index.  We need
		 * to insert a new chunk before this one and initialize it so that it can
		 * contain this index.
		 */
		const uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)] = { 0 };
		SM_ENOUGH_SPACE(sizeof(buf));
		__sm_insert_data(map, offset, &buf[0], sizeof(buf));
		__sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

		/* NOTE: insert moves the memory over meaning `p` is now the new chunk */
		__sm_store_idx((uint8_t *)p,
		    __sm_get_chunk_aligned_offset(idx));
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);

		const __sm_bitvec_unaligned_t *v =
		    (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		        SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
		ret_idx = __sparsemap_add(map, idx, p, offset, v);
		left_hint = SIZE_MAX; /* inserted a chunk before this one */
		goto done;
	}

	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	size_t capacity = __sm_chunk_get_capacity(&chunk);

	if (capacity < SM_CHUNK_MAX_CAPACITY &&
	    idx - start < SM_CHUNK_MAX_CAPACITY) {
		/*
		 * Special case, we have a sparse chunk with one or more flags set to
		 * SM_PAYLOAD_NONE which reduces the carrying capacity of the chunk. In
		 * this case we should remove those flags and try again.
		 */
		__sm_assert(__sm_chunk_is_rle(&chunk) == false);
		__sm_chunk_increase_capacity(&chunk, SM_CHUNK_MAX_CAPACITY);
		capacity = __sm_chunk_get_capacity(&chunk);
	}

	if (chunk.m_data[0] == ~(__sm_bitvec_t)0 &&
	    idx - start == SM_CHUNK_MAX_CAPACITY) {
		/*
		 * Our search resulted in a chunk that is full of ones and this index is the
		 * next one after the capacity, we have a run of ones longer than the
		 * capacity of the sparse encoding, let's transition this chunk to
		 * run-length encoding (RLE).
		 *
		 * NOTE: Keep in mind that idx is 0-based, so idx=2048 is the 2049th bit.
		 * When a chunk is at maximum capacity it is storing indexes [0, 2048).
		 *
		 * ALSO: Keep in mind the RLE "length" is the current length of 1s in the
		 * run, so in this case we transition from 2048 to a length of 2049.
		 * in this run.
		 */

		__sm_chunk_set_rle(&chunk);
		const size_t rle_length = SM_CHUNK_MAX_CAPACITY + 1;
		__sm_chunk_rle_set_capacity(&chunk,
		    __sm_chunk_rle_capacity_limit(map, start, rle_length,
		        offset));
		__sm_chunk_rle_set_length(&chunk, rle_length);
		goto done;
	}

	/* is this an RLE chunk */
	if (__sm_chunk_is_rle(&chunk)) {
		const size_t length = __sm_chunk_rle_get_length(&chunk);

		/* Is the index within its range, at the end, or just past the end? */
		if (idx >= start && idx - start <= capacity) {
			/*
			 * This RLE contains the bits in [start, start + length] so the index of
			 * the last bit in this RLE chunk is `start + length - 1` which is why
			 * we test index (0-based) against current length (1-based) below.
			 */
			if (idx - start < length) {
				/* Bit is already set within the run, no-op. */
				goto done;
			}
			if (idx - start == length) {
				/* Extend the run by one. If length == capacity, grow capacity first. */
				if (length == capacity) {
					__sm_chunk_rle_set_capacity(&chunk,
					    __sm_chunk_rle_capacity_limit(map,
					        start, length + 1, offset));
				}
				__sm_chunk_rle_set_length(&chunk, length + 1);
				__sm_assert(__sm_chunk_rle_get_length(&chunk) ==
				    length + 1);
				goto done;
			}
		}

		/*
		 * We've been asked to set a bit that is within this RLE chunk's capacity
		 * but not within its run.  That means this chunk's capacity must shrink,
		 * and we need a new sparse chunk to hold this value.
		 *
		 * If the bit is beyond the capacity, fall through to the generic
		 * "insert new chunk" path below.
		 */
		if (idx >= start && idx - start < capacity) {
			__sm_chunk_sep_t sep = { .target = { .p = p,
				                     .offset = offset,
				                     .chunk = &chunk,
				                     .start = start,
				                     .length = length,
				                     .capacity = capacity } };
			if (__sm_separate_rle_chunk(map, &sep, idx, 1) != 0) {
				/* Out of space (or invalid): the map was left
				 * unmodified.  Propagate ENOSPC so sm_add_grow
				 * can grow and retry. */
				return (SM_IDX_MAX);
			}
			left_hint = SIZE_MAX; /* separate shifted layout */
			goto done;
		}
	}

	if (idx - start >= capacity) {
		/*
		 * Our search resulted in a chunk however it's capacity doesn't encompass
		 * this index, so we need to insert a new chunk after this one and
		 * initialize it so that it can contain this index.
		 */
		const uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)] = { 0 };
		const size_t size = __sm_chunk_get_size(&chunk);
		SM_ENOUGH_SPACE(sizeof(buf));
		offset += SM_SIZEOF_OVERHEAD + size;
		p += SM_SIZEOF_OVERHEAD + size;
		__sm_insert_data(map, offset, &buf[0], sizeof(buf));

		start = __sm_get_chunk_aligned_offset(idx);
		__sm_store_idx((uint8_t *)p, start);
		__sm_assert(start == __sm_get_chunk_aligned_offset(start));
		__sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

		const __sm_bitvec_unaligned_t *v =
		    (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		        SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
		ret_idx = __sparsemap_add(map, idx, p, offset, v);
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		left_hint = SIZE_MAX; /* inserted a new chunk after this one;
		                       * hint pointed at the old chunk's
		                       * predecessor, wrong for the new offset */
		goto done;
	}

	ret_idx = __sparsemap_add(map, idx, p, offset, NULL);
	if (ret_idx != idx) {
		goto done;
	}

done:;
	if (coalesce) {
		__sm_coalesce_chunk(map, &chunk, offset, start, p, idx, true,
		    left_hint);
	}
	/*
	 * Re-seat the caller's cursor at the chunk we just touched so an
	 * ascending bulk insert (sm_add_many / sm_add_many_grow) resumes
	 * the next __sm_get_chunk_offset walk here instead of from the
	 * head -- the difference between O(N) and O(N^2) when a hot
	 * trigram accumulates tens of thousands of TIDs.  We record the
	 * byte offset and the chunk's start index; __sm_get_chunk_offset
	 * self-validates this (re-walking from the head if a later
	 * mutation shifted the chunk), so a stale seat is merely slow,
	 * never wrong.  Coalescing may have moved the chunk, so seat
	 * AFTER it and let the next call's validation sort out any drift.
	 */
	if (cur != NULL) {
		cur->offset = offset;
		cur->start_idx = start;
	}
	return (ret_idx);
}

/**
 * @brief Sets the specified index in the sparsemap.
 *
 * This function marks the given index in the sparsemap as set.
 * Internally, it calls the __sm_map_set function with coalesce set to true.
 *
 * @param[in] map The sparsemap to modify.
 * @param[in] idx The index to set in the sparsemap.
 * @return The index that was set in the sparsemap.
 */
SM_HOT uint64_t
sm_add(sm_t *map, const uint64_t idx)
{
	return (__sm_map_set(map, idx, true, NULL));
}

/* Cursor-threading variant of sm_add for O(N) bulk construction.
 * Internal only; the cursor accelerates ascending inserts.  See
 * sm_add_many / sm_add_many_grow.
 *
 * Inserting a new chunk or coalescing existing ones changes the chunk
 * layout at or before the cached chunk, which would leave a recorded
 * cursor offset pointing at the wrong place (or past the end after a
 * coalesce removes trailing chunks).  Detect that by comparing the
 * chunk count before and after: on any change, reset the cursor so the
 * next lookup walks from the head.  Pure in-place updates (the common
 * case in an ascending run) leave the count unchanged and keep the
 * cursor hot, preserving the amortized O(N) build cost. */
static uint64_t
__sm_add_c(sm_t *map, uint64_t idx, sm_cursor_t *cur)
{
	/*
	 * __sm_map_set re-seats *cur at the touched chunk (see its done:
	 * label), so we no longer reset the cursor here on a chunk-count
	 * change -- that blanket reset defeated the ascending-append fast
	 * path (every new chunk forced the next lookup back to the head,
	 * making bulk insert O(N^2)).  __sm_get_chunk_offset self-validates
	 * the seat, so an occasionally-stale cursor is safe.
	 */
	return (__sm_map_set(map, idx, true, cur));
}

uint64_t
sm_add_grow(sm_t **mapp, uint64_t idx)
{
	if (mapp == NULL || *mapp == NULL)
		return (SM_IDX_MAX);
	sm_t *m = *mapp;
	uint64_t rc = sm_add(m, idx);
	if (rc != SM_IDX_MAX)
		return (rc);

	/* ENOSPC: grow geometrically with a 4 KiB floor. */
	size_t new_cap = sm_get_capacity(m) * 2;
	if (new_cap < 4096)
		new_cap = 4096;
	sm_t *grown = sm_set_data_size(m, NULL, new_cap);
	if (grown == NULL)
		return (SM_IDX_MAX);
	*mapp = grown;
	return (sm_add(grown, idx));
}

uint64_t
sm_add_grow_cursor(sm_t **mapp, uint64_t idx, sm_cursor_t *cur)
{
	if (mapp == NULL || *mapp == NULL)
		return (SM_IDX_MAX);
	sm_t *m = *mapp;
	uint64_t rc = __sm_add_c(m, idx, cur);
	if (rc != SM_IDX_MAX)
		return (rc);

	/* ENOSPC: grow geometrically with a 4 KiB floor. */
	size_t new_cap = sm_get_capacity(m) * 2;
	if (new_cap < 4096)
		new_cap = 4096;
	sm_t *grown = sm_set_data_size(m, NULL, new_cap);
	if (grown == NULL)
		return (SM_IDX_MAX);
	*mapp = grown;
	/* The grow relocated the buffer; the cursor's byte offset is stale. */
	if (cur != NULL)
		*cur = (sm_cursor_t)SM_CURSOR_INIT;
	return (__sm_add_c(grown, idx, cur));
}

/**
 * @brief Sets or unsets a value in the sparse map at the specified index.
 *
 * This function assigns a value to the sparse map at the given index.
 * It either sets or unsets (clears) the bit at the index based on
 * the provided boolean value.
 *
 * @param[in,out] map Pointer to the sparsemap structure.
 * @param[in] idx The index at which the value should be assigned.
 * @param[in] value Boolean value indicating whether to set (true) or unset (false) the bit.
 * @return The index at which the operation was performed.
 */
uint64_t
sm_assign(sm_t *map, const uint64_t idx, const bool value)
{
	__sm_check_invariants(map);
	return (value ? sm_add(map, idx) : sm_remove(map, idx));
}

/* -------------------------------------------------------------------
 * Aggregate queries: minimum, maximum, fill factor, cardinality
 * ------------------------------------------------------------------- */

/**
 * @brief Retrieves the starting offset in a sparse map.
 *
 * This function determines the starting offset of a sparse map by analyzing
 * the chunks within the map. It iterates over the chunk data to find the first
 * payload of interest, either `ones` or `mixed`, and returns the corresponding
 * offset. If the chunk is run-length encoded (RLE), it shortcuts to this calculation.
 *
 * @param[in] map Pointer to the sparse map to analyze.
 * @return The starting offset within the sparse map.
 */
uint64_t
sm_minimum(const sm_t *map)
{
	__sm_check_invariants(map);
	uint64_t offset = 0;
	const size_t count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (0);
	}
	uint8_t *p = __sm_get_chunk_data(map, 0);
	uint64_t relative_position = __sm_load_idx((const uint8_t *)p);
	p += SM_SIZEOF_OVERHEAD;
	__sm_chunk_t chunk;
	__sm_chunk_init(&chunk, p);
	if (__sm_chunk_is_rle(&chunk)) {
		offset = relative_position;
		goto done;
	}
	for (size_t m = 0; m < sizeof(__sm_bitvec_t); m++, p++) {
		for (int n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
			if (flags == SM_PAYLOAD_NONE) {
				continue;
			} else if (flags == SM_PAYLOAD_ZEROS) {
				relative_position += SM_BITS_PER_VECTOR;
			} else if (flags == SM_PAYLOAD_ONES) {
				offset = relative_position;
				goto done;
			} else if (flags == SM_PAYLOAD_MIXED) {
				const __sm_bitvec_t w = chunk.m_data[1 +
				    __sm_chunk_get_position(&chunk,
				        (m * SM_FLAGS_PER_INDEX_BYTE) + n)];
				for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
					if (w & (__sm_bitvec_t)1 << k) {
						offset = relative_position + k;
						goto done;
					}
				}
				relative_position += SM_BITS_PER_VECTOR;
			}
		}
	}
done:;
	return (offset);
}

/**
 * @brief Retrieves the ending offset of a sparse map.
 *
 * This function calculates the ending offset of a sparse map by examining
 * each chunk within the map. If the map is empty, the offset is zero. For
 * maps with chunks, it iterates over the chunks, evaluating their data and
 * calculating the final offset.
 *
 * @param[in] map Pointer to the sparse map structure.
 * @return The calculated ending offset of the map.
 */
uint64_t
sm_maximum(const sm_t *map)
{
	__sm_check_invariants(map);
	const size_t count = __sm_get_chunk_count(map);

	/* the ending offset of a map containing zero chunks is zero */
	if (count == 0) {
		return (0);
	}

	/* the ending offset will be the last offset in the last chunk */
	uint8_t *p = __sm_get_chunk_data(map, 0);
	for (size_t i = 0; i < count - 1; i++) {
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		p += __sm_chunk_get_size(&chunk);
	}

	/* examine the last chunk in the map */
	const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
	p += SM_SIZEOF_OVERHEAD;
	__sm_chunk_t chunk;
	__sm_chunk_init(&chunk, p);

	/* the ending offset of an RLE chunk is its starting offset + length */
	if (SM_IS_CHUNK_RLE(&chunk)) {
		return (start + __sm_chunk_rle_get_length(&chunk) - 1);
	}

	/* the last chunk is not RLE, let's examine it further */
	uint64_t offset = 0;
	uint64_t relative_position = start;
	for (size_t m = 0; m < sizeof(__sm_bitvec_t); m++, p++) {
		for (int n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
			switch (flags) {
			case SM_PAYLOAD_ZEROS:
				relative_position += SM_BITS_PER_VECTOR;
				break;
			case SM_PAYLOAD_ONES:
				offset =
				    relative_position + SM_BITS_PER_VECTOR - 1;
				relative_position += SM_BITS_PER_VECTOR;
				break;
			case SM_PAYLOAD_MIXED: {
				const __sm_bitvec_t w = chunk.m_data[1 +
				    __sm_chunk_get_position(&chunk,
				        (m * SM_FLAGS_PER_INDEX_BYTE) + n)];
				int idx = 0;
				for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
					if (w & (__sm_bitvec_t)1 << k) {
						idx = k;
					}
				}
				offset = relative_position + idx;
				relative_position += SM_BITS_PER_VECTOR;
				break;
			}
			case SM_PAYLOAD_NONE:
			default:
				continue;
			}
		}
	}
	return (offset);
}

/**
 * @brief Calculates the fill factor of a sparse map.
 *
 * This function computes the fill factor of a sparse map by determining
 * the proportion of occupied elements relative to its total offset.
 * The fill factor is expressed as a percentage.
 *
 * @param[in] map A pointer to the sparse map.
 * @return The fill factor of the map as a percentage.
 */
double
sm_fill_factor(sm_t *map)
{
	__sm_check_invariants(map);
	const size_t rank = sm_rank(map, 0, SM_IDX_MAX, true);
	if (rank == 0) {
		return (0.0);
	}
	const uint64_t lo = sm_minimum(map);
	const uint64_t hi = sm_maximum(map);
	/* range = hi - lo + 1 (the inclusive span containing all set bits). */
	const uint64_t range = hi - lo + 1;
	if (range == 0) {
		return (0.0);
	}
	return ((double)rank / (double)range);
}

/**
 * @brief Retrieves the serialized bitmap data from a sparse map.
 *
 * This function returns a pointer to the serialized data contained within
 * a given sparse map.
 *
 * @param[in] map Pointer to the sparse map from which to retrieve the data.
 * @return Pointer to the serialized bitmap data.
 */
void *
sm_get_data(const sm_t *map)
{
	return (map->m_data);
}

/**
 * @brief Retrieves the size of the sparse map.
 *
 * This function calculates the utilized size of the sparse map. If the stored
 * size does not match the calculated size, it updates the stored size.
 *
 * @param[in] map Pointer to the sparse map.
 * @return The size of the sparse map.
 */
size_t
sm_get_size(sm_t *map)
{
	if (map->m_data_used) {
		const size_t size = __sm_get_size_impl(map);
		if (size != map->m_data_used) {
			map->m_data_used = size;
		}
		__sm_when_diag({
			__sm_assert(
			    map->m_data_used == __sm_get_size_impl(map));
		});
		return (map->m_data_used);
	}
	return (map->m_data_used = __sm_get_size_impl(map));
}

/**
 * @brief Counts the number of elements in a sparse map.
 *
 * This function returns the total count of elements stored in a given
 * sm_t instance by invoking the sm_rank function.
 *
 * @param[in] map A pointer to the sm_t instance to be counted.
 * @return The total number of elements in the sparse map.
 */
size_t
sm_cardinality(sm_t *map)
{
	return (sm_rank(map, 0, SM_IDX_MAX, true));
}

/* -------------------------------------------------------------------
 * Iteration: batched callback scan of set bits
 * ------------------------------------------------------------------- */

/**
 * @brief Scans through each chunk in a sparse map and applies a scanning function to each chunk.
 *
 * This function iterates over all chunks in the provided sparse map, initializing each chunk
 * and applying a user-defined scanning function to it. The scan may optionally skip a specified
 * number of elements before commencing.
 *
 * @param[in] map Pointer to the sparse map to scan.
 * @param[in] scanner User-defined scanning function to be applied to each chunk.
 * @param[in] skip Number of elements to skip before starting the scan.
 * @param[in] aux Auxiliary data to pass to the scanning function.
 */
void
sm_scan(const sm_t *map, void (*scanner)(uint64_t[], size_t, void *aux),
    size_t skip, void *aux)
{
	uint8_t *p = __sm_get_chunk_data(map, 0);
	const size_t count = __sm_get_chunk_count(map);

	for (size_t i = 0; i < count; i++) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		const size_t skipped =
		    __sm_chunk_scan(&chunk, start, scanner, skip, aux);
		if (skip) {
			__sm_assert(skip >= skipped);
			skip -= skipped;
		}
		p += chunk_size;
	}
}

/**
 * @brief Creates a new sparsemap with all bits shifted by a given offset.
 *
 * Every set bit at position i in the source map appears at position i + offset
 * in the result. Bits shifted below 0 are silently dropped.
 *
 * Uses direct chunk copying and bit-vector shifting for performance.
 *
 * @param[in] map    The source sparsemap.
 * @param[in] offset Signed shift amount (positive = right, negative = left).
 * @return A newly allocated sparsemap (caller must free()), or NULL if all
 *         bits are shifted away or on allocation failure.
 */

/* -------------------------------------------------------------------
 * Set operations: scratch-word codec, append helpers, and the
 * bitwise shift (sm_offset)
 * ------------------------------------------------------------------- */

/**
 * @brief Expand a sparse chunk's descriptor into 32 full 64-bit words.
 *
 * For each of the 32 descriptor flag slots:
 *   ZEROS -> 0x0000000000000000
 *   ONES  -> 0xFFFFFFFFFFFFFFFF
 *   MIXED -> the stored bit-vector word
 *   NONE  -> 0x0000000000000000 (treated as zeros for shifting)
 *
 * @param[in]  chunk     The sparse chunk to expand.
 * @param[out] words     Array of 32 uint64_t to receive expanded words.
 * @param[out] cap_flags Array of 32 flags: 1 if slot contributes to capacity, 0 if NONE.
 */
static void
__sm_expand_sparse_chunk(const __sm_chunk_t *chunk, __sm_bitvec_t words[32],
    int cap_flags[32])
{
	const __sm_bitvec_t desc = chunk->m_data[0];

	/* Pass 1: prefix-sum of MIXED flag counts to break serial vec_idx dependency. */
	int vec_offsets[SM_FLAGS_PER_INDEX];
	int running = 0;
	for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		vec_offsets[i] = running;
		running +=
		    (((desc >> (i * 2)) & SM_FLAG_MASK) == SM_PAYLOAD_MIXED);
	}

	/* Pass 2: each slot computed independently using precomputed offsets. */
	for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		const unsigned f = (desc >> (i * 2)) & SM_FLAG_MASK;
		cap_flags[i] = (f != SM_PAYLOAD_NONE);
		words[i] = (f == SM_PAYLOAD_MIXED) ?
		    chunk->m_data[1 + vec_offsets[i]] :
		    (f == SM_PAYLOAD_ONES) ? ~(__sm_bitvec_t)0 :
		                             0;
	}
}

/**
 * @brief Encode 32 expanded words back into a sparse chunk format.
 *
 * Builds a descriptor and vector array from the expanded words.
 * Only slots where cap_flags[i] == 1 contribute to capacity.
 *
 * @param[in]  words      Array of 32 uint64_t words.
 * @param[in]  cap_flags  Array of 32 flags indicating capacity slots.
 * @param[out] out_desc   The output descriptor word.
 * @param[out] out_vecs   Output vector array (up to 32 words).
 * @param[out] out_nvecs  Number of output vectors written.
 * @return true if the chunk has any set bits, false if completely empty.
 */
static bool
__sm_encode_sparse_chunk(__sm_bitvec_t words[32], int cap_flags[32],
    __sm_bitvec_t *out_desc, __sm_bitvec_t out_vecs[32], int *out_nvecs)
{
	/* Slot 31 (the highest) must never be NONE, because NONE in bits 63:62
     of the descriptor would be misidentified as the RLE flag.  Force it
     to ZEROS (adding 64 bits of harmless zero capacity) when needed. */
	if (!cap_flags[SM_FLAGS_PER_INDEX - 1]) {
		cap_flags[SM_FLAGS_PER_INDEX - 1] = 1;
		words[SM_FLAGS_PER_INDEX - 1] = 0;
	}

	/* Pass 1: compute flags for each slot (no inter-iteration dependency). */
	__sm_bitvec_t desc = 0;
	bool has_bits = false;
	unsigned flags[SM_FLAGS_PER_INDEX];
	for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		unsigned f;
		if (!cap_flags[i]) {
			f = SM_PAYLOAD_NONE;
		} else if (words[i] == 0) {
			f = SM_PAYLOAD_ZEROS;
		} else if (words[i] == ~(__sm_bitvec_t)0) {
			f = SM_PAYLOAD_ONES;
			has_bits = true;
		} else {
			f = SM_PAYLOAD_MIXED;
			has_bits = true;
		}
		flags[i] = f;
		desc |= (__sm_bitvec_t)f << (i * 2);
	}

	/* Pass 2: compact MIXED vectors (serial but only touches MIXED slots). */
	int nvecs = 0;
	for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		if (flags[i] == SM_PAYLOAD_MIXED) {
			out_vecs[nvecs++] = words[i];
		}
	}

	*out_desc = desc;
	*out_nvecs = nvecs;
	return (has_bits);
}

/**
 * @brief Expand an RLE chunk's set bits into a 32-word array aligned at a
 *        target sparse chunk's start offset.
 *
 * For each of the 32 word slots at target_start + i*64:
 *   - If entirely within the RLE run -> words[i] = ~0ULL
 *   - If entirely outside -> words[i] = 0
 *   - If at boundary -> words[i] = partial bit mask
 *   - cap_flags[i] = 1 for slots within the target's capacity range
 *
 * @param[in]  rle_chunk     The RLE chunk.
 * @param[in]  rle_start     The absolute start offset of the RLE chunk.
 * @param[in]  target_start  The aligned start offset of the target sparse chunk.
 * @param[out] words         Array of 32 uint64_t words.
 * @param[out] cap_flags     Array of 32 capacity flags.
 * @param[in]  target_cap_flags  If non-NULL, use these to determine which slots
 *                               have capacity (from the target sparse chunk).
 *                               If NULL, all 32 slots are considered to have capacity.
 */
static void
__sm_expand_rle_as_words(const __sm_chunk_t *rle_chunk, __sm_idx_t rle_start,
    __sm_idx_t target_start, __sm_bitvec_t words[32], int cap_flags[32],
    const int *target_cap_flags)
{
	const size_t rle_len = __sm_chunk_rle_get_length(rle_chunk);
	const size_t rle_set_start = (size_t)rle_start;
	const size_t rle_set_end = rle_set_start + rle_len;

	for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		const size_t slot_start =
		    (size_t)target_start + (size_t)i * SM_BITS_PER_VECTOR;
		const size_t slot_end = slot_start + SM_BITS_PER_VECTOR;

		if (target_cap_flags) {
			cap_flags[i] = target_cap_flags[i];
		} else {
			cap_flags[i] = 1;
		}

		if (slot_end <= rle_set_start || slot_start >= rle_set_end) {
			/* Slot entirely outside the RLE run */
			words[i] = 0;
		} else if (slot_start >= rle_set_start &&
		    slot_end <= rle_set_end) {
			/* Slot entirely within the RLE run */
			words[i] = ~(__sm_bitvec_t)0;
		} else {
			/* Boundary slot: partial overlap */
			__sm_bitvec_t mask = 0;
			size_t lo = (rle_set_start > slot_start) ?
			    (rle_set_start - slot_start) :
			    0;
			size_t hi = (rle_set_end < slot_end) ?
			    (rle_set_end - slot_start) :
			    SM_BITS_PER_VECTOR;
			if (hi == SM_BITS_PER_VECTOR) {
				mask = ~((__sm_bitvec_t)0) << lo;
			} else if (lo == 0) {
				mask = ((__sm_bitvec_t)1 << hi) - 1;
			} else {
				mask = (((__sm_bitvec_t)1 << hi) - 1) &
				    (~((__sm_bitvec_t)0) << lo);
			}
			words[i] = mask;
		}
	}
}

/**
 * @brief Merge (OR) carry words into an existing set of expanded words.
 *
 * @param[in,out] words     The destination 32-word array.
 * @param[in,out] cap_flags The destination capacity flags.
 * @param[in]     carry     The carry 32-word array to merge in.
 * @param[in]     carry_cap The carry capacity flags.
 */
static void
__sm_merge_carry(__sm_bitvec_t words[32], int cap_flags[32],
    __sm_bitvec_t carry[32], int carry_cap[32])
{
	for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		if (carry_cap[i]) {
			words[i] |= carry[i];
			cap_flags[i] = 1;
		}
	}
}

/* ---- SIMD-accelerated word-level operations ---- */

#if defined(__AVX2__)
#include <immintrin.h>

static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i += 4) {
		__m256i va = _mm256_loadu_si256((const __m256i *)&a[i]);
		__m256i vb = _mm256_loadu_si256((const __m256i *)&b[i]);
		_mm256_storeu_si256((__m256i *)&dst[i],
		    _mm256_or_si256(va, vb));
	}
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i += 4) {
		__m256i va = _mm256_loadu_si256((const __m256i *)&a[i]);
		__m256i vb = _mm256_loadu_si256((const __m256i *)&b[i]);
		_mm256_storeu_si256((__m256i *)&dst[i],
		    _mm256_and_si256(va, vb));
	}
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	/* dst = a & ~b */
	for (int i = 0; i < 32; i += 4) {
		__m256i va = _mm256_loadu_si256((const __m256i *)&a[i]);
		__m256i vb = _mm256_loadu_si256((const __m256i *)&b[i]);
		_mm256_storeu_si256((__m256i *)&dst[i],
		    _mm256_andnot_si256(vb, va));
	}
}

#elif defined(__SSE2__)
#include <emmintrin.h>

static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i += 2) {
		__m128i va = _mm_loadu_si128((const __m128i *)&a[i]);
		__m128i vb = _mm_loadu_si128((const __m128i *)&b[i]);
		_mm_storeu_si128((__m128i *)&dst[i], _mm_or_si128(va, vb));
	}
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i += 2) {
		__m128i va = _mm_loadu_si128((const __m128i *)&a[i]);
		__m128i vb = _mm_loadu_si128((const __m128i *)&b[i]);
		_mm_storeu_si128((__m128i *)&dst[i], _mm_and_si128(va, vb));
	}
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	/* dst = a & ~b */
	for (int i = 0; i < 32; i += 2) {
		__m128i va = _mm_loadu_si128((const __m128i *)&a[i]);
		__m128i vb = _mm_loadu_si128((const __m128i *)&b[i]);
		_mm_storeu_si128((__m128i *)&dst[i], _mm_andnot_si128(vb, va));
	}
}

#else

/* Scalar fallback */
static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] | b[i];
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] & b[i];
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] & ~b[i];
}

#endif

/**
 * @brief Ensure the result map has enough capacity, growing if needed.
 *
 * @param[in,out] resultp Pointer to result map pointer (may be reallocated).
 * @param[in]     needed  Number of bytes needed beyond current usage.
 * @return true on success, false on allocation failure.
 */
static bool
__sm_ensure_capacity(sm_t **resultp, size_t needed)
{
	sm_t *result = *resultp;
	/*
	 * Defense in depth: the only callers of __sm_ensure_capacity are
	 * sm_union / _intersection / _difference, which all allocate
	 * their result via sm_create() (SM_OWNED_CONTIGUOUS).  Any
	 * other lineage at this point indicates an internal API misuse --
	 * fail loudly under SPARSEMAP_TESTING so we catch it now rather
	 * than three operations downstream when the heap finally notices.
	 */
	__sm_when_diag({
		__sm_assert(__sm_kind(result) == SM_OWNED_CONTIGUOUS ||
		    __sm_kind(result) == SM_OWNED_SPLIT);
	});
	if (result->m_data_used + needed <= __sm_cap(result)) {
		return (true);
	}
	size_t cap = __sm_cap(result);
	size_t new_cap = cap + (cap / 2 > needed ? cap / 2 : needed + 256);
	sm_t *grown = sm_set_data_size(result, NULL, new_cap);
	if (grown == NULL) {
		return (false);
	}
	*resultp = grown;
	return (true);
}

/**
 * @brief Append a sparse chunk (descriptor + vectors) to the result map.
 *
 * @param[in,out] resultp    Pointer to result map pointer (may grow).
 * @param[in]     start      The chunk start offset (__sm_idx_t).
 * @param[in]     desc       The descriptor word.
 * @param[in]     vecs       The vector array.
 * @param[in]     nvecs      Number of vectors.
 * @return true on success, false on allocation failure.
 */
static bool
__sm_append_sparse_chunk(sm_t **resultp, __sm_idx_t start, __sm_bitvec_t desc,
    __sm_bitvec_t vecs[], int nvecs)
{
	const size_t chunk_size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) +
	    (size_t)nvecs * sizeof(__sm_bitvec_t);
	if (!__sm_ensure_capacity(resultp, chunk_size)) {
		return (false);
	}
	sm_t *result = *resultp;

	/* Write start offset */
	__sm_append_data(result, (const uint8_t *)&start, SM_SIZEOF_OVERHEAD);
	/* Write descriptor */
	__sm_append_data(result, (const uint8_t *)&desc, sizeof(__sm_bitvec_t));
	/* Write vectors */
	for (int i = 0; i < nvecs; i++) {
		__sm_append_data(result, (const uint8_t *)&vecs[i],
		    sizeof(__sm_bitvec_t));
	}

	__sm_set_chunk_count(result, __sm_get_chunk_count(result) + 1);
	return (true);
}

/**
 * @brief Append an RLE chunk to the result map.
 *
 * @param[in,out] resultp    Pointer to result map pointer (may grow).
 * @param[in]     start      The chunk start offset.
 * @param[in]     capacity   RLE capacity.
 * @param[in]     length     RLE length (number of set bits from start).
 * @return true on success, false on allocation failure.
 */
static bool
__sm_append_rle_chunk(sm_t **resultp, __sm_idx_t start, size_t capacity,
    size_t length)
{
	sm_t *result = *resultp;

	/* Inline coalescing: try to merge with the last emitted chunk. */
	const size_t count = __sm_get_chunk_count(result);
	if (count > 0) {
		/* Find the last chunk in the result */
		uint8_t *p = __sm_get_chunk_data(result, 0);
		uint8_t *last_p = p;
		for (size_t i = 0; i < count; i++) {
			last_p = p;
			__sm_chunk_t c;
			__sm_chunk_init(&c, p + SM_SIZEOF_OVERHEAD);
			p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&c);
		}

		const __sm_idx_t last_start =
		    __sm_load_idx((const uint8_t *)last_p);
		__sm_chunk_t last_chunk;
		__sm_chunk_init(&last_chunk, last_p + SM_SIZEOF_OVERHEAD);

		if (__sm_chunk_is_rle(&last_chunk)) {
			/* Last chunk is RLE -- check if this new RLE is contiguous */
			const size_t last_len =
			    __sm_chunk_rle_get_length(&last_chunk);
			if ((size_t)last_start + last_len == (size_t)start) {
				/* Contiguous: extend the last chunk in place */
				size_t new_len = last_len + length;
				size_t new_cap = (size_t)start + capacity -
				    (size_t)last_start;
				if (new_len <= SM_CHUNK_RLE_MAX_LENGTH &&
				    new_cap <= SM_CHUNK_RLE_MAX_CAPACITY) {
					__sm_chunk_rle_set_capacity(&last_chunk,
					    new_cap);
					__sm_chunk_rle_set_length(&last_chunk,
					    new_len);
					return (
					    true); /* Merged -- no new chunk needed */
				}
			}
		} else {
			/* Last chunk is sparse -- check if it's all-ones and contiguous */
			const size_t last_run =
			    __sm_chunk_get_run_length(&last_chunk);
			const size_t last_cap =
			    __sm_chunk_get_capacity(&last_chunk);
			if (last_run == last_cap && last_run > 0 &&
			    (size_t)last_start + last_run == (size_t)start) {
				/* All-ones sparse chunk contiguous with this RLE: replace sparse with RLE */
				size_t new_len = last_run + length;
				size_t new_cap = (size_t)start + capacity -
				    (size_t)last_start;
				if (new_len <= SM_CHUNK_RLE_MAX_LENGTH &&
				    new_cap <= SM_CHUNK_RLE_MAX_CAPACITY) {
					/* Rewrite last chunk as RLE in place */
					const size_t last_size =
					    __sm_chunk_get_size(&last_chunk);
					const size_t rle_size =
					    sizeof(__sm_bitvec_t);
					if (last_size > rle_size) {
						/* Remove the extra bytes (sparse vectors) */
						size_t last_offset =
						    (size_t)(last_p -
						        __sm_get_chunk_data(
						            result, 0));
						__sm_remove_data(result,
						    last_offset +
						        SM_SIZEOF_OVERHEAD +
						        rle_size,
						    last_size - rle_size);
						/* Re-init after data shift */
						last_p = __sm_get_chunk_data(
						    result, 0);
						for (size_t i = 0;
						     i < count - 1; i++) {
							__sm_chunk_t c;
							__sm_chunk_init(&c,
							    last_p +
							        SM_SIZEOF_OVERHEAD);
							last_p +=
							    SM_SIZEOF_OVERHEAD +
							    __sm_chunk_get_size(
							        &c);
						}
						__sm_chunk_init(&last_chunk,
						    last_p +
						        SM_SIZEOF_OVERHEAD);
					}
					__sm_chunk_set_rle(&last_chunk);
					__sm_chunk_rle_set_capacity(&last_chunk,
					    new_cap);
					__sm_chunk_rle_set_length(&last_chunk,
					    new_len);
					return (true);
				}
			}
		}
	}

	/* No merge possible: append new RLE chunk */
	const size_t chunk_size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
	if (!__sm_ensure_capacity(resultp, chunk_size)) {
		return (false);
	}
	result = *resultp;

	/* Write start offset */
	__sm_append_data(result, (const uint8_t *)&start, SM_SIZEOF_OVERHEAD);

	/* Build and write the RLE word */
	SM_ALIGNAS(__sm_bitvec_t) uint8_t rle_buf[sizeof(__sm_bitvec_t)] = { 0 };
	__sm_chunk_t tmp;
	__sm_chunk_init(&tmp, rle_buf);
	__sm_chunk_set_rle(&tmp);
	__sm_chunk_rle_set_capacity(&tmp, capacity);
	__sm_chunk_rle_set_length(&tmp, length);
	__sm_append_data(result, rle_buf, sizeof(__sm_bitvec_t));

	__sm_set_chunk_count(result, __sm_get_chunk_count(result) + 1);
	return (true);
}

/**
 * @brief Helper: flush carry buffer as a sparse chunk into the result.
 */
static bool
__sm_flush_carry(sm_t **resultp, __sm_bitvec_t carry_words[32],
    int carry_cap[32], __sm_idx_t carry_start)
{
	__sm_bitvec_t cd;
	__sm_bitvec_t cv[32];
	int cnv;
	if (__sm_encode_sparse_chunk(carry_words, carry_cap, &cd, cv, &cnv)) {
		if (!__sm_append_sparse_chunk(resultp, carry_start, cd, cv,
		        cnv)) {
			return (false);
		}
	}
	return (true);
}

sm_t *
sm_offset(const sm_t *map, ssize_t offset)
{
	__sm_check_invariants(map);
	if (map == NULL) {
		return (NULL);
	}

	/* offset == 0: just copy */
	if (offset == 0) {
		return (sm_copy(map));
	}

	const size_t count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (NULL);
	}

	/* Check for overflow: if shifting right and max bit would overflow */
	if (offset > 0) {
		uint64_t max = sm_maximum(map);
		if (max > SM_IDX_MAX - (uint64_t)offset) {
			errno = ERANGE;
			return (NULL);
		}
	}

	/* Check if all bits would be shifted below 0 */
	if (offset < 0) {
		uint64_t max = sm_maximum(map);
		if ((ssize_t)max + offset < 0) {
			return (NULL); /* all bits shifted away */
		}
	}

	/* Allocate result */
	size_t cap = map->m_data_used;
	sm_t *result = sparsemap(cap > 0 ? cap : 1024);
	if (result == NULL) {
		return (NULL);
	}

	/* Carry buffer from previous chunk's overflow into the next output chunk */
	__sm_bitvec_t carry_words[32] = { 0 };
	int carry_cap[32] = { 0 };
	bool have_carry = false;
	__sm_idx_t carry_start = 0;

	/* Walk source chunks */
	uint8_t *p = __sm_get_chunk_data(map, 0);

	for (size_t i = 0; i < count; i++) {
		const __sm_idx_t src_start = __sm_load_idx((const uint8_t *)p);
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);

		if (__sm_chunk_is_rle(&chunk)) {
			const size_t rle_len =
			    __sm_chunk_rle_get_length(&chunk);

			/* RLE set bits occupy [src_start, src_start + rle_len).
         After offset: [src_start + offset, src_start + offset + rle_len). */
			ssize_t final_start = (ssize_t)src_start + offset;
			ssize_t final_end = final_start + (ssize_t)rle_len;

			/* Clip to >= 0 */
			if (final_end <= 0) {
				goto next_chunk;
			}
			if (final_start < 0) {
				final_start = 0;
			}

			size_t new_len = (size_t)(final_end - final_start);
			if (new_len == 0) {
				goto next_chunk;
			}

			/* Flush carry before emitting RLE chunk(s) */
			if (have_carry) {
				if (!__sm_flush_carry(&result, carry_words,
				        carry_cap, carry_start)) {
					sm_free(result);
					return (NULL);
				}
				memset(carry_words, 0, sizeof(carry_words));
				memset(carry_cap, 0, sizeof(carry_cap));
				have_carry = false;
			}

			/* Align the start to chunk boundary */
			__sm_idx_t aligned_start =
			    (__sm_idx_t)__sm_get_chunk_aligned_offset(
			        (size_t)final_start);
			size_t rle_offset_in_chunk =
			    (size_t)final_start - aligned_start;

			if (rle_offset_in_chunk == 0) {
				/* Starts on chunk boundary, emit as pure RLE */
				size_t new_cap =
				    ((new_len + SM_CHUNK_MAX_CAPACITY - 1) /
				        SM_CHUNK_MAX_CAPACITY) *
				    SM_CHUNK_MAX_CAPACITY;
				if (new_cap < new_len) {
					new_cap = new_len;
				}
				if (!__sm_append_rle_chunk(&result,
				        aligned_start, new_cap, new_len)) {
					sm_free(result);
					return (NULL);
				}
			} else {
				/* Emit first partial chunk as sparse */
				size_t first_chunk_bits =
				    SM_CHUNK_MAX_CAPACITY - rle_offset_in_chunk;
				if (first_chunk_bits > new_len) {
					first_chunk_bits = new_len;
				}

				__sm_bitvec_t fw[32] = { 0 };
				int fc[32] = { 0 };
				/* Mark capacity for all slots up to and including the data */
				size_t last_data_slot =
				    (rle_offset_in_chunk + first_chunk_bits +
				        SM_BITS_PER_VECTOR - 1) /
				    SM_BITS_PER_VECTOR;
				for (size_t s = 0; s < last_data_slot && s < 32;
				     s++) {
					fc[s] = 1;
				}
				/* Set the actual bits */
				size_t bp = rle_offset_in_chunk;
				size_t bl = first_chunk_bits;
				while (bl > 0) {
					size_t slot = bp / SM_BITS_PER_VECTOR;
					size_t bit_in_vec =
					    bp % SM_BITS_PER_VECTOR;
					size_t can_set =
					    SM_BITS_PER_VECTOR - bit_in_vec;
					if (can_set > bl)
						can_set = bl;
					fc[slot] = 1;
					if (can_set == SM_BITS_PER_VECTOR) {
						fw[slot] = ~(__sm_bitvec_t)0;
					} else {
						fw[slot] |= (((__sm_bitvec_t)1
						                 << can_set) -
						                1)
						    << bit_in_vec;
					}
					bp += can_set;
					bl -= can_set;
				}

				__sm_bitvec_t fd;
				__sm_bitvec_t fv[32];
				int fnv;
				if (__sm_encode_sparse_chunk(fw, fc, &fd, fv,
				        &fnv)) {
					if (!__sm_append_sparse_chunk(&result,
					        aligned_start, fd, fv, fnv)) {
						sm_free(result);
						return (NULL);
					}
				}

				size_t remaining = new_len - first_chunk_bits;
				__sm_idx_t cur_start =
				    aligned_start + SM_CHUNK_MAX_CAPACITY;

				/* Emit middle RLE for full chunks */
				if (remaining >= SM_CHUNK_MAX_CAPACITY) {
					size_t rle_mid =
					    (remaining /
					        SM_CHUNK_MAX_CAPACITY) *
					    SM_CHUNK_MAX_CAPACITY;
					if (!__sm_append_rle_chunk(&result,
					        cur_start, rle_mid, rle_mid)) {
						sm_free(result);
						return (NULL);
					}
					cur_start += (__sm_idx_t)rle_mid;
					remaining -= rle_mid;
				}

				/* Emit last partial chunk */
				if (remaining > 0) {
					__sm_bitvec_t lw[32] = { 0 };
					int lc[32] = { 0 };
					size_t lbit = 0, lrem = remaining;
					while (lrem > 0) {
						size_t slot =
						    lbit / SM_BITS_PER_VECTOR;
						size_t can_set =
						    SM_BITS_PER_VECTOR;
						if (can_set > lrem)
							can_set = lrem;
						lc[slot] = 1;
						if (can_set ==
						    SM_BITS_PER_VECTOR) {
							lw[slot] =
							    ~(__sm_bitvec_t)0;
						} else {
							lw[slot] =
							    ((__sm_bitvec_t)1
							        << can_set) -
							    1;
						}
						lbit += can_set;
						lrem -= can_set;
					}
					__sm_bitvec_t ld;
					__sm_bitvec_t lv[32];
					int lnv;
					if (__sm_encode_sparse_chunk(lw, lc,
					        &ld, lv, &lnv)) {
						if (!__sm_append_sparse_chunk(
						        &result, cur_start, ld,
						        lv, lnv)) {
							sm_free(result);
							return (NULL);
						}
					}
				}
			}
		} else {
			/* Sparse chunk: expand to 32 words, compute final absolute positions,
         place into correct output chunk(s). */
			__sm_bitvec_t words[32];
			int cf[32];
			__sm_expand_sparse_chunk(&chunk, words, cf);

			/* Each bit at absolute position src_start + slot*64 + bit_offset
         maps to src_start + offset + slot*64 + bit_offset in the output.

         The output chunk aligned start = align(src_start + offset).
         The intra-chunk shift = (src_start + offset) - aligned_start.

         If intra >= 0: right-shift within the 32-word array, overflow to carry.
         If intra < 0 (new start negative): left-shift, dropping low bits. */

			ssize_t new_abs_start = (ssize_t)src_start + offset;

			/* Compute aligned output chunk start and intra-chunk shift */
			ssize_t out_aligned;
			ssize_t intra_shift;

			if (new_abs_start >= 0) {
				out_aligned =
				    (ssize_t)__sm_get_chunk_aligned_offset(
				        (size_t)new_abs_start);
				intra_shift = new_abs_start - out_aligned;
			} else {
				/* new_abs_start < 0: bits below 0 get dropped, surviving bits start at 0 */
				out_aligned = 0;
				intra_shift =
				    new_abs_start; /* negative = left shift */
			}

			/* Build the shifted 32-word arrays for main output chunk and overflow */
			__sm_bitvec_t main_words[32] = { 0 };
			int main_cap[32] = { 0 };
			__sm_bitvec_t overflow_words[32] = { 0 };
			int overflow_cap[32] = { 0 };

			if (intra_shift >= 0) {
				/* Right-shift by intra_shift bits */
				size_t word_shift =
				    (size_t)intra_shift / SM_BITS_PER_VECTOR;
				size_t bit_rem =
				    (size_t)intra_shift % SM_BITS_PER_VECTOR;

				for (int w = 31; w >= 0; w--) {
					if (!cf[w] && words[w] == 0)
						continue;

					size_t dst = (size_t)w + word_shift;
					if (bit_rem == 0) {
						if (dst < 32) {
							main_words[dst] |=
							    words[w];
							main_cap[dst] = 1;
						} else if (dst < 64) {
							overflow_words[dst -
							    32] |= words[w];
							overflow_cap[dst - 32] =
							    1;
						}
					} else {
						__sm_bitvec_t lo = words[w]
						    << bit_rem;
						__sm_bitvec_t hi = words[w] >>
						    (SM_BITS_PER_VECTOR -
						        bit_rem);

						if (dst < 32) {
							main_words[dst] |= lo;
							main_cap[dst] = 1;
						} else if (dst < 64) {
							overflow_words[dst -
							    32] |= lo;
							overflow_cap[dst - 32] =
							    1;
						}

						size_t dst1 = dst + 1;
						if (dst1 < 32) {
							main_words[dst1] |= hi;
							main_cap[dst1] = 1;
						} else if (dst1 < 64) {
							overflow_words[dst1 -
							    32] |= hi;
							overflow_cap[dst1 -
							    32] = 1;
						}
					}
				}

				/* Mark shifted-in zero slots as capacity */
				for (size_t w = 0; w < word_shift && w < 32;
				     w++) {
					main_cap[w] = 1;
				}
			} else {
				/* intra_shift < 0: left-shift by |intra_shift| bits (dropping low bits) */
				size_t drop = (size_t)(-intra_shift);
				size_t word_drop = drop / SM_BITS_PER_VECTOR;
				size_t bit_drop = drop % SM_BITS_PER_VECTOR;

				for (size_t w = 0; w < 32; w++) {
					size_t src_w = w + word_drop;
					if (src_w >= 32)
						break;
					main_cap[w] = 1;
					if (bit_drop == 0) {
						main_words[w] = words[src_w];
					} else {
						main_words[w] =
						    words[src_w] >> bit_drop;
						if (src_w + 1 < 32) {
							main_words[w] |=
							    words[src_w + 1]
							    << (SM_BITS_PER_VECTOR -
							           bit_drop);
						}
					}
				}
			}

			/* Merge pending carry into main_words if it targets the same output chunk */
			if (have_carry &&
			    carry_start == (__sm_idx_t)out_aligned) {
				__sm_merge_carry(main_words, main_cap,
				    carry_words, carry_cap);
				memset(carry_words, 0, sizeof(carry_words));
				memset(carry_cap, 0, sizeof(carry_cap));
				have_carry = false;
			} else if (have_carry) {
				/* Carry targets a different chunk, flush it first */
				if (!__sm_flush_carry(&result, carry_words,
				        carry_cap, carry_start)) {
					sm_free(result);
					return (NULL);
				}
				memset(carry_words, 0, sizeof(carry_words));
				memset(carry_cap, 0, sizeof(carry_cap));
				have_carry = false;
			}

			/* Emit main chunk if it has any set bits */
			__sm_bitvec_t desc;
			__sm_bitvec_t vecs[32];
			int nvecs;
			if (__sm_encode_sparse_chunk(main_words, main_cap,
			        &desc, vecs, &nvecs)) {
				if (!__sm_append_sparse_chunk(&result,
				        (__sm_idx_t)out_aligned, desc, vecs,
				        nvecs)) {
					sm_free(result);
					return (NULL);
				}
			}

			/* Check for overflow into next chunk */
			bool has_overflow = false;
			for (int w = 0; w < 32; w++) {
				if (overflow_cap[w] && overflow_words[w] != 0) {
					has_overflow = true;
					break;
				}
			}
			if (has_overflow) {
				memcpy(carry_words, overflow_words,
				    sizeof(carry_words));
				memcpy(carry_cap, overflow_cap,
				    sizeof(carry_cap));
				have_carry = true;
				carry_start = (__sm_idx_t)out_aligned +
				    SM_CHUNK_MAX_CAPACITY;
			}
		}

	next_chunk:
		p += chunk_size;
	}

	/* Flush any remaining carry */
	if (have_carry) {
		if (!__sm_flush_carry(&result, carry_words, carry_cap,
		        carry_start)) {
			sm_free(result);
			return (NULL);
		}
	}

	/* If no chunks were added, return NULL */
	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	/* Coalesce adjacent chunks where possible */
	__sm_coalesce_map(result);

	return (result);
}

/* -------------------------------------------------------------------
 * Predicates and member-by-member iteration
 * ------------------------------------------------------------------- */

bool
sm_is_empty(const sm_t *map)
{
	if (map == NULL) {
		return (true);
	}
	__sm_check_invariants(map);
	return (__sm_get_chunk_count(map) == 0);
}

/*
 * Iterate set bits in `chunk` (anchored at absolute `start`),
 * starting strictly after `lower_excl`.  Returns the first set bit
 * found, or SM_IDX_MAX if none.  Pass UINT64_MAX as lower_excl to
 * mean "start before bit 0" (return the first bit at or after start).
 */
static __sm_idx_t
__sm_chunk_next_set(const __sm_chunk_t *chunk, uint64_t start,
    uint64_t lower_excl)
{
	if (__sm_chunk_is_rle(chunk)) {
		const size_t length = __sm_chunk_rle_get_length(chunk);
		if (length == 0) {
			return (SM_IDX_MAX);
		}
		const uint64_t run_lo = start;
		const uint64_t run_hi = start + length - 1;
		if (lower_excl != UINT64_MAX && lower_excl >= run_hi) {
			return (SM_IDX_MAX);
		}
		if (lower_excl == UINT64_MAX || lower_excl < run_lo) {
			return (run_lo);
		}
		return (lower_excl + 1);
	}

	for (size_t v = 0; v < SM_FLAGS_PER_INDEX; v++) {
		const uint64_t vec_lo = start + v * SM_BITS_PER_VECTOR;
		const uint64_t vec_hi = vec_lo + SM_BITS_PER_VECTOR - 1;
		if (lower_excl != UINT64_MAX && vec_hi <= lower_excl) {
			continue;
		}
		const size_t flags = SM_CHUNK_GET_FLAGS(chunk->m_data[0], v);
		if (flags == SM_PAYLOAD_NONE || flags == SM_PAYLOAD_ZEROS) {
			continue;
		}
		if (flags == SM_PAYLOAD_ONES) {
			if (lower_excl == UINT64_MAX || lower_excl < vec_lo) {
				return (vec_lo);
			}
			return (lower_excl + 1);
		}
		/* SM_PAYLOAD_MIXED: scan the payload word for a 1-bit > lower_excl. */
		const __sm_bitvec_t w =
		    chunk->m_data[1 + __sm_chunk_get_position(chunk, v)];
		uint64_t skip = 0;
		if (lower_excl != UINT64_MAX && lower_excl >= vec_lo) {
			skip = lower_excl - vec_lo + 1;
			if (skip >= SM_BITS_PER_VECTOR)
				continue;
		}
		const __sm_bitvec_t masked = w & (~(__sm_bitvec_t)0 << skip);
		if (masked == 0) {
			continue;
		}
		return (vec_lo + (uint64_t)SM_CTZ64(masked));
	}
	return (SM_IDX_MAX);
}

/*
 * Iterate set bits in `chunk` (anchored at absolute `start`),
 * looking for the highest set bit strictly less than `upper_excl`.
 */
static __sm_idx_t
__sm_chunk_prev_set(const __sm_chunk_t *chunk, uint64_t start,
    uint64_t upper_excl)
{
	if (__sm_chunk_is_rle(chunk)) {
		const size_t length = __sm_chunk_rle_get_length(chunk);
		if (length == 0 || upper_excl <= start) {
			return (SM_IDX_MAX);
		}
		const uint64_t run_hi = start + length - 1;
		return (upper_excl - 1 < run_hi ? upper_excl - 1 : run_hi);
	}

	for (ssize_t v = SM_FLAGS_PER_INDEX - 1; v >= 0; v--) {
		const uint64_t vec_lo =
		    start + (uint64_t)v * SM_BITS_PER_VECTOR;
		if (vec_lo >= upper_excl) {
			continue;
		}
		const size_t flags =
		    SM_CHUNK_GET_FLAGS(chunk->m_data[0], (size_t)v);
		if (flags == SM_PAYLOAD_NONE || flags == SM_PAYLOAD_ZEROS) {
			continue;
		}
		const uint64_t vec_hi = vec_lo + SM_BITS_PER_VECTOR - 1;
		if (flags == SM_PAYLOAD_ONES) {
			return (
			    upper_excl - 1 < vec_hi ? upper_excl - 1 : vec_hi);
		}
		/* SM_PAYLOAD_MIXED. */
		__sm_bitvec_t w =
		    chunk
		        ->m_data[1 + __sm_chunk_get_position(chunk, (size_t)v)];
		if (upper_excl - 1 < vec_hi) {
			const uint64_t bits_to_keep = upper_excl - vec_lo;
			if (bits_to_keep == 0)
				continue;
			w &= (~(__sm_bitvec_t)0) >>
			    (SM_BITS_PER_VECTOR - bits_to_keep);
		}
		if (w == 0)
			continue;
		return (vec_lo +
		    (uint64_t)(SM_BITS_PER_VECTOR - 1 - (size_t)SM_CLZ64(w)));
	}
	return (SM_IDX_MAX);
}

uint64_t
sm_next_member(const sm_t *map, uint64_t prev_idx, sm_cursor_t *cur)
{
	if (map == NULL)
		return (SM_IDX_MAX);
	__sm_check_invariants(map);
	const size_t count = __sm_get_chunk_count(map);
	if (count == 0)
		return (SM_IDX_MAX);

	uint8_t *base = __sm_get_chunk_data(map, 0);
	uint8_t *p = base;
	const size_t stream_end = map->m_data_used - SM_SIZEOF_OVERHEAD;

	/*
	 * Cursor fast-path.  Sequential forward iteration
	 *   while ((i = sm_next_member(map, i, &c)) != SM_IDX_MAX) ...
	 * is the dominant scan-side hot path.  Without a cursor each
	 * call walks from chunk 0 -- O(N) per call, O(N^2) per scan.
	 * Resume from the cached chunk when prev_idx is not earlier than
	 * that chunk's start.
	 */
	if (prev_idx != SM_IDX_MAX && cur != NULL &&
	    cur->offset != SIZE_MAX && cur->offset < stream_end &&
	    cur->start_idx <= prev_idx) {
		p = base + cur->offset;
	}

	while ((size_t)(p - base) < stream_end) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t cap = __sm_chunk_get_capacity(&chunk);
		/* Skip chunks entirely below the lower bound. */
		if (prev_idx != SM_IDX_MAX && start + cap - 1 <= prev_idx) {
			p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
			continue;
		}
		const uint64_t hit =
		    __sm_chunk_next_set(&chunk, start, prev_idx);
		if (hit != SM_IDX_MAX) {
			if (cur != NULL) {
				cur->offset = (size_t)(p - base);
				cur->start_idx = start;
			}
			return (hit);
		}
		p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
	}
	return (SM_IDX_MAX);
}

uint64_t
sm_prev_member(const sm_t *map, uint64_t prev_idx, sm_cursor_t *cur)
{
	/* The cursor accelerates forward (non-decreasing) lookups only;
	 * reverse iteration always walks from the head, so cur is accepted
	 * for API symmetry but unused. */
	(void)cur;
	if (map == NULL)
		return (SM_IDX_MAX);
	__sm_check_invariants(map);
	const size_t count = __sm_get_chunk_count(map);
	if (count == 0)
		return (SM_IDX_MAX);

	/* SM_IDX_MAX as input means "start past the end". */
	const uint64_t upper_excl =
	    (prev_idx == SM_IDX_MAX) ? UINT64_MAX : prev_idx;

	/* Walk forward to the last chunk that starts before upper_excl,
	 * remembering each chunk so we can step back if needed. */
	uint8_t *p = __sm_get_chunk_data(map, 0);
	/* Track up to `count` candidate chunk pointers. */
	uint8_t *last = NULL;
	size_t last_idx = 0;
	for (size_t i = 0; i < count; i++) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		if (start >= upper_excl)
			break;
		last = p;
		last_idx = i;
		__sm_chunk_t tmp;
		__sm_chunk_init(&tmp, p + SM_SIZEOF_OVERHEAD);
		p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&tmp);
	}
	if (last == NULL)
		return (SM_IDX_MAX);

	/* Step back through chunks until we find a hit. */
	while (true) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)last);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, last + SM_SIZEOF_OVERHEAD);
		const uint64_t hit =
		    __sm_chunk_prev_set(&chunk, start, upper_excl);
		if (hit != SM_IDX_MAX)
			return (hit);
		if (last_idx == 0)
			break;
		/* Walk forward to find the chunk preceding `last`. */
		uint8_t *q = __sm_get_chunk_data(map, 0);
		for (size_t j = 0; j + 1 < last_idx; j++) {
			__sm_chunk_t tmp;
			__sm_chunk_init(&tmp, q + SM_SIZEOF_OVERHEAD);
			q += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&tmp);
		}
		last = q;
		last_idx--;
	}
	return (SM_IDX_MAX);
}

bool
sm_equals(const sm_t *a, const sm_t *b)
{
	const bool a_empty = (a == NULL) || sm_is_empty(a);
	const bool b_empty = (b == NULL) || sm_is_empty(b);
	if (a_empty && b_empty)
		return (true);
	if (a_empty != b_empty)
		return (false);

	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX && ib != SM_IDX_MAX) {
		if (ia != ib)
			return (false);
		ia = sm_next_member(a, ia, NULL);
		ib = sm_next_member(b, ib, NULL);
	}
	return (ia == ib);
}

bool
sm_is_subset(const sm_t *a, const sm_t *b)
{
	if (a == NULL || sm_is_empty(a))
		return (true);
	if (b == NULL || sm_is_empty(b))
		return (false);

	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX) {
		while (ib != SM_IDX_MAX && ib < ia) {
			ib = sm_next_member(b, ib, NULL);
		}
		if (ib != ia)
			return (false);
		ia = sm_next_member(a, ia, NULL);
	}
	return (true);
}

bool
sm_is_superset(const sm_t *a, const sm_t *b)
{
	return (sm_is_subset(b, a));
}

bool
sm_overlap(const sm_t *a, const sm_t *b)
{
	if (a == NULL || b == NULL)
		return (false);
	if (sm_is_empty(a) || sm_is_empty(b))
		return (false);

	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX && ib != SM_IDX_MAX) {
		if (ia == ib)
			return (true);
		if (ia < ib)
			ia = sm_next_member(a, ia, NULL);
		else
			ib = sm_next_member(b, ib, NULL);
	}
	return (false);
}

sm_membership_t
sm_membership(const sm_t *map)
{
	if (map == NULL || sm_is_empty(map))
		return (SM_EMPTY);
	const uint64_t first = sm_next_member(map, SM_IDX_MAX, NULL);
	if (first == SM_IDX_MAX)
		return (SM_EMPTY);
	const uint64_t second = sm_next_member(map, first, NULL);
	return ((second == SM_IDX_MAX) ? SM_SINGLETON : SM_MULTIPLE);
}

uint64_t
sm_singleton_member(const sm_t *map)
{
	if (map == NULL || sm_is_empty(map))
		return (SM_IDX_MAX);
	const uint64_t first = sm_next_member(map, SM_IDX_MAX, NULL);
	if (first == SM_IDX_MAX)
		return (SM_IDX_MAX);
	const uint64_t second = sm_next_member(map, first, NULL);
	return ((second == SM_IDX_MAX) ? first : SM_IDX_MAX);
}

/* -------------------------------------------------------------------
 * Cardinality without allocation, bulk add, array conversion
 * ------------------------------------------------------------------- */

/*
 * The cardinality functions walk both maps in lockstep using
 * sm_next_member.  This is O(|a|+|b|) bit lookups, dominated by
 * the cost of skipping past whole chunks (sm_next_member is O(1)
 * per RLE chunk, O(vectors) per sparse chunk).  An optimized
 * chunk-pair-walk would be faster but more complex; if profiling
 * shows this matters in pg_tre's hot path, that's the next step.
 */

size_t
sm_union_cardinality(const sm_t *a, const sm_t *b)
{
	if (sm_is_empty(a))
		return (b ? sm_cardinality((sm_t *)b) : 0);
	if (sm_is_empty(b))
		return (sm_cardinality((sm_t *)a));

	size_t count = 0;
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX || ib != SM_IDX_MAX) {
		if (ia == ib) {
			count++;
			ia = sm_next_member(a, ia, NULL);
			ib = sm_next_member(b, ib, NULL);
		} else if (ia != SM_IDX_MAX && (ib == SM_IDX_MAX || ia < ib)) {
			count++;
			ia = sm_next_member(a, ia, NULL);
		} else {
			count++;
			ib = sm_next_member(b, ib, NULL);
		}
	}
	return (count);
}

size_t
sm_intersection_cardinality(const sm_t *a, const sm_t *b)
{
	if (sm_is_empty(a) || sm_is_empty(b))
		return (0);
	size_t count = 0;
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX && ib != SM_IDX_MAX) {
		if (ia == ib) {
			count++;
			ia = sm_next_member(a, ia, NULL);
			ib = sm_next_member(b, ib, NULL);
		} else if (ia < ib) {
			ia = sm_next_member(a, ia, NULL);
		} else {
			ib = sm_next_member(b, ib, NULL);
		}
	}
	return (count);
}

size_t
sm_difference_cardinality(const sm_t *a, const sm_t *b)
{
	if (sm_is_empty(a))
		return (0);
	if (sm_is_empty(b))
		return (sm_cardinality((sm_t *)a));

	size_t count = 0;
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX) {
		/* Advance b past anything < ia. */
		while (ib != SM_IDX_MAX && ib < ia) {
			ib = sm_next_member(b, ib, NULL);
		}
		if (ib == ia) {
			/* In both, skip from a's count. */
			ib = sm_next_member(b, ib, NULL);
		} else {
			count++;
		}
		ia = sm_next_member(a, ia, NULL);
	}
	return (count);
}

bool
sm_nonempty_difference(const sm_t *a, const sm_t *b)
{
	if (sm_is_empty(a))
		return (false);
	if (sm_is_empty(b))
		return (true);

	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX) {
		while (ib != SM_IDX_MAX && ib < ia) {
			ib = sm_next_member(b, ib, NULL);
		}
		if (ib != ia) {
			return (true);
		}
		ia = sm_next_member(a, ia, NULL);
		ib = sm_next_member(b, ib, NULL);
	}
	return (false);
}

double
sm_jaccard_index(const sm_t *a, const sm_t *b)
{
	/* Walk both lockstep, accumulating intersection and union counts
	 * in a single pass. */
	if (sm_is_empty(a) && sm_is_empty(b))
		return (0.0);
	size_t intersect = 0, union_ = 0;
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX || ib != SM_IDX_MAX) {
		if (ia == ib) {
			intersect++;
			union_++;
			ia = sm_next_member(a, ia, NULL);
			ib = sm_next_member(b, ib, NULL);
		} else if (ia != SM_IDX_MAX && (ib == SM_IDX_MAX || ia < ib)) {
			union_++;
			ia = sm_next_member(a, ia, NULL);
		} else {
			union_++;
			ib = sm_next_member(b, ib, NULL);
		}
	}
	return (union_ == 0 ? 0.0 : (double)intersect / (double)union_);
}

/* Ascending uint64_t comparator for the bulk-insert sort below. */
static int
__sm_cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;
	return ((x > y) - (x < y));
}

bool
sm_add_many(sm_t *map, const uint64_t *arr, size_t n)
{
	uint64_t *sorted;
	bool ok = true;

	if (map == NULL || (arr == NULL && n > 0))
		return (false);
	if (n == 0)
		return (true);
	if (n == 1)
		return (sm_add(map, arr[0]) != SM_IDX_MAX);

	/*
	 * Sort a private copy ascending before inserting.  The bulk path
	 * threads an internal cursor that only makes ascending inserts O(N)
	 * total; for unsorted input each insert would fall back to a full
	 * chunk walk plus a byte-shift, making the loop O(N^2).  Sorting
	 * first guarantees O(N log N + N) regardless of caller order.  The
	 * caller's array is const and left untouched.
	 */
	sorted = (uint64_t *)__sm_alloc(n * sizeof(uint64_t));
	if (sorted == NULL)
		return (false);
	memcpy(sorted, arr, n * sizeof(uint64_t));
	qsort(sorted, n, sizeof(uint64_t), __sm_cmp_u64);
	sm_cursor_t cur = SM_CURSOR_INIT;
	for (size_t i = 0; i < n; i++) {
		if (__sm_add_c(map, sorted[i], &cur) == SM_IDX_MAX) {
			ok = false;
			break;
		}
	}
	__sm_free(sorted);
	return (ok);
}

/*
 * Growing bulk insert: like sm_add_many but takes sm_t** and uses
 * sm_add_grow, so the buffer is realloc'd geometrically on ENOSPC
 * instead of failing.  Sorts a private copy first (same O(N) rationale
 * as sm_add_many).  Returns true on success; false only if a scratch
 * allocation fails or sm_add_grow exhausts its grow retries.
 */
bool
sm_add_many_grow(sm_t **map, const uint64_t *arr, size_t n)
{
	uint64_t *sorted;
	bool ok = true;

	if (map == NULL || *map == NULL || (arr == NULL && n > 0))
		return (false);
	if (n == 0)
		return (true);

	sorted = (uint64_t *)__sm_alloc(n * sizeof(uint64_t));
	if (sorted == NULL)
		return (false);
	memcpy(sorted, arr, n * sizeof(uint64_t));
	if (n > 1)
		qsort(sorted, n, sizeof(uint64_t), __sm_cmp_u64);
	sm_cursor_t cur = SM_CURSOR_INIT;
	for (size_t i = 0; i < n; i++) {
		int retries = 0;
		sm_t *before = *map;
		while (__sm_add_c(*map, sorted[i], &cur) == SM_IDX_MAX) {
			if (++retries > 16) {
				ok = false;
				break;
			}
			/* ENOSPC: grow geometrically with a 4 KiB floor. */
			size_t new_cap = sm_get_capacity(*map) * 2;
			if (new_cap < 4096)
				new_cap = 4096;
			sm_t *grown = sm_set_data_size(*map, NULL, new_cap);
			if (grown == NULL) {
				ok = false;
				break;
			}
			*map = grown;
		}
		if (!ok)
			break;
		/* A grow may have relocated the buffer; the cursor's byte
		 * offset is then meaningless.  Reset it when *map moved. */
		if (*map != before)
			cur = (sm_cursor_t)SM_CURSOR_INIT;
	}
	__sm_free(sorted);
	return (ok);
}

void
sm_to_array(const sm_t *map, uint64_t *out, size_t *n_out)
{
	if (n_out == NULL)
		return;
	const size_t cap = (out == NULL) ? 0 : *n_out;
	size_t written = 0;

	if (out == NULL) {
		/* Query: just count. */
		*n_out = sm_is_empty(map) ? 0 : sm_cardinality((sm_t *)map);
		return;
	}

	uint64_t i = SM_IDX_MAX;
	while ((i = sm_next_member(map, i, NULL)) != SM_IDX_MAX) {
		if (written >= cap)
			break;
		out[written++] = i;
	}
	*n_out = written;
}

/* -------------------------------------------------------------------
 * Range ops, symmetric difference, set-op synonyms, constructors,
 * hashing and ordering, destructive iteration
 * ------------------------------------------------------------------- */

bool
sm_add_range(sm_t *map, uint64_t lo, uint64_t hi)
{
	if (map == NULL || lo >= hi)
		return (lo >= hi); /* empty range = OK */
	for (uint64_t i = lo; i < hi; i++) {
		if (sm_add(map, i) == SM_IDX_MAX) {
			return (false);
		}
	}
	return (true);
}

bool
sm_remove_range(sm_t *map, uint64_t lo, uint64_t hi)
{
	if (map == NULL || lo >= hi)
		return (lo >= hi);
	for (uint64_t i = lo; i < hi; i++) {
		if (sm_remove(map, i) == SM_IDX_MAX) {
			return (false);
		}
	}
	return (true);
}

sm_t *
sm_xor(const sm_t *a, const sm_t *b)
{
	if (sm_is_empty(a) && sm_is_empty(b))
		return (NULL);
	if (sm_is_empty(a))
		return (sm_copy(b));
	if (sm_is_empty(b))
		return (sm_copy(a));

	/* Allocate a result big enough for the union (upper bound). */
	const size_t cap = sm_get_capacity(a) + sm_get_capacity(b);
	sm_t *r = sm_create(cap > 1024 ? cap : 1024);
	if (r == NULL)
		return (NULL);

	/* Walk both lockstep, emit bits set in exactly one. */
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX || ib != SM_IDX_MAX) {
		if (ia == ib) {
			/* In both: skip from XOR. */
			ia = sm_next_member(a, ia, NULL);
			ib = sm_next_member(b, ib, NULL);
		} else if (ia != SM_IDX_MAX && (ib == SM_IDX_MAX || ia < ib)) {
			if (sm_add(r, ia) == SM_IDX_MAX) {
				sm_free(r);
				return (NULL);
			}
			ia = sm_next_member(a, ia, NULL);
		} else {
			if (sm_add(r, ib) == SM_IDX_MAX) {
				sm_free(r);
				return (NULL);
			}
			ib = sm_next_member(b, ib, NULL);
		}
	}
	if (sm_is_empty(r)) {
		sm_free(r);
		return (NULL);
	}
	return (r);
}

sm_t *
sm_or(const sm_t *a, const sm_t *b)
{
	return (sm_union(a, b));
}

sm_t *
sm_and(const sm_t *a, const sm_t *b)
{
	return (sm_intersection(a, b));
}

sm_t *
sm_andnot(const sm_t *a, const sm_t *b)
{
	return (sm_difference(a, b));
}

sm_t *
sm_extract_range(const sm_t *map, uint64_t lo, uint64_t hi)
{
	if (map == NULL || sm_is_empty(map) || lo >= hi)
		return (NULL);

	/* Estimate result capacity from the input -- worst case is the same
	 * shape, capped to the requested range size. */
	size_t cap = sm_get_size((sm_t *)map) + 64;
	if (cap < 1024)
		cap = 1024;
	sm_t *r = sm_create(cap);
	if (r == NULL)
		return (NULL);

	/* Walk set bits in [lo, hi) and add them to the result.
	 * sm_next_member supports a lower-exclusive bound; pass lo - 1 if
	 * lo > 0, else SM_IDX_MAX (start sentinel). */
	uint64_t cursor = (lo == 0) ? SM_IDX_MAX : lo - 1;
	while ((cursor = sm_next_member(map, cursor, NULL)) != SM_IDX_MAX &&
	    cursor < hi) {
		if (sm_add(r, cursor) == SM_IDX_MAX) {
			/* Grow and retry once. */
			sm_t *grown = sm_set_data_size(r, NULL,
			    sm_get_capacity(r) * 2 + 256);
			if (grown == NULL) {
				sm_free(r);
				return (NULL);
			}
			r = grown;
			if (sm_add(r, cursor) == SM_IDX_MAX) {
				sm_free(r);
				return (NULL);
			}
		}
	}

	if (sm_is_empty(r)) {
		sm_free(r);
		return (NULL);
	}
	return (r);
}

size_t
sm_xor_cardinality(const sm_t *a, const sm_t *b)
{
	if (sm_is_empty(a) && sm_is_empty(b))
		return (0);
	if (sm_is_empty(a))
		return (sm_cardinality((sm_t *)b));
	if (sm_is_empty(b))
		return (sm_cardinality((sm_t *)a));

	size_t count = 0;
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX || ib != SM_IDX_MAX) {
		if (ia == ib) {
			ia = sm_next_member(a, ia, NULL);
			ib = sm_next_member(b, ib, NULL);
		} else if (ia != SM_IDX_MAX && (ib == SM_IDX_MAX || ia < ib)) {
			count++;
			ia = sm_next_member(a, ia, NULL);
		} else {
			count++;
			ib = sm_next_member(b, ib, NULL);
		}
	}
	return (count);
}

sm_t *
sm_create_singleton(uint64_t idx)
{
	sm_t *m = sm_create(1024);
	if (m && sm_add(m, idx) == SM_IDX_MAX) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

sm_t *
sm_create_from_range(uint64_t lo, uint64_t hi)
{
	/* Estimate buffer size: each chunk is at most ~24 bytes; range
	 * spans (hi-lo)/2048 chunks plus partial-edge chunks. */
	size_t chunks = (hi - lo) / 2048 + 2;
	size_t bytes = 32 + chunks * 24;
	sm_t *m = sm_create(bytes < 1024 ? 1024 : bytes);
	if (m == NULL)
		return (NULL);
	if (!sm_add_range(m, lo, hi)) {
		/* Try once with a bigger buffer. */
		sm_t *grown = sm_set_data_size(m, NULL, bytes * 4);
		if (grown == NULL) {
			sm_free(m);
			return (NULL);
		}
		sm_clear(grown);
		if (!sm_add_range(grown, lo, hi)) {
			sm_free(grown);
			return (NULL);
		}
		return (grown);
	}
	return (m);
}

sm_t *
sm_create_from_array(const uint64_t *arr, size_t n)
{
	sm_t *m = sm_create(1024);
	if (m == NULL)
		return (NULL);
	if (!sm_add_many(m, arr, n)) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

uint64_t
sm_hash(const sm_t *map)
{
	/* FNV-1a 64-bit over the sequence of set bits.  Content-based
	 * (encoding-independent): two maps that compare equal under
	 * sm_equals() hash to the same value. */
	uint64_t h = 0xcbf29ce484222325ULL;
	if (sm_is_empty(map))
		return (h);
	uint64_t i = SM_IDX_MAX;
	while ((i = sm_next_member(map, i, NULL)) != SM_IDX_MAX) {
		/* Mix all 8 bytes of the index. */
		for (int b = 0; b < 8; b++) {
			h ^= (i >> (b * 8)) & 0xffULL;
			h *= 0x100000001b3ULL;
		}
	}
	return (h);
}

int
sm_compare(const sm_t *a, const sm_t *b)
{
	/* Lexicographic: walk both lockstep and return the difference at
	 * the first point of divergence. */
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX && ib != SM_IDX_MAX) {
		if (ia < ib)
			return (-1);
		if (ia > ib)
			return (1);
		ia = sm_next_member(a, ia, NULL);
		ib = sm_next_member(b, ib, NULL);
	}
	if (ia == SM_IDX_MAX && ib == SM_IDX_MAX)
		return (0);
	return ((ia == SM_IDX_MAX) ? -1 : 1); /* shorter sequence sorts first */
}

sm_subset_relation_t
sm_subset_compare(const sm_t *a, const sm_t *b)
{
	bool a_subset_b = true; /* every bit in a is in b */
	bool b_subset_a = true; /* every bit in b is in a */

	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX || ib != SM_IDX_MAX) {
		if (ia == ib) {
			ia = sm_next_member(a, ia, NULL);
			ib = sm_next_member(b, ib, NULL);
		} else if (ia != SM_IDX_MAX && (ib == SM_IDX_MAX || ia < ib)) {
			/* a has a bit b doesn't. */
			a_subset_b = false;
			ia = sm_next_member(a, ia, NULL);
		} else {
			/* b has a bit a doesn't. */
			b_subset_a = false;
			ib = sm_next_member(b, ib, NULL);
		}
		if (!a_subset_b && !b_subset_a) {
			return (SM_REL_DIFFERENT);
		}
	}
	if (a_subset_b && b_subset_a)
		return (SM_REL_EQUAL);
	if (a_subset_b)
		return (SM_REL_SUBSET_A);
	return (SM_REL_SUBSET_B);
}

uint64_t
sm_pop_first(sm_t *map)
{
	if (sm_is_empty(map))
		return (SM_IDX_MAX);
	const uint64_t lowest = sm_next_member(map, SM_IDX_MAX, NULL);
	if (lowest == SM_IDX_MAX)
		return (SM_IDX_MAX);
	if (sm_remove(map, lowest) == SM_IDX_MAX) {
		/* Should never happen on a populated map (remove only fails on
		 * ENOSPC for chunk separation, and we're removing not adding). */
		return (SM_IDX_MAX);
	}
	return (lowest);
}

uint64_t
sm_pop_last(sm_t *map)
{
	if (sm_is_empty(map))
		return (SM_IDX_MAX);
	const uint64_t highest = sm_prev_member(map, SM_IDX_MAX, NULL);
	if (highest == SM_IDX_MAX)
		return (SM_IDX_MAX);
	if (sm_remove(map, highest) == SM_IDX_MAX)
		return (SM_IDX_MAX);
	return (highest);
}

/* -------------------------------------------------------------------
 * In-place set operations.  These mutate `dst` and return it (or a
 * possibly-relocated pointer if dst grew).
 * ------------------------------------------------------------------- */

/*
 * In-place set ops are implemented as "compute via the chunk-pair-walk
 * in sm_union/sm_intersection/sm_difference, then memcpy the result's
 * bytes back into dst's buffer".  This delegates the actual merge to
 * the chunk-aware out-of-place version, paying one allocation for the
 * temporary result.  An alternative would be a two-pointer chunk walk
 * that writes directly into dst's buffer; that's a substantial refactor
 * with minimal speedup over the current approach (sm_union's own walk
 * is already chunk-aware and the memcpy is a single block copy).
 */
static sm_t *
__sm_replace_buffer(sm_t *dst, sm_t *result)
{
	if (result == NULL) {
		/* Empty result -- clear dst. */
		sm_clear(dst);
		return (dst);
	}
	const size_t result_size = result->m_data_used;
	if (__sm_cap(dst) < result_size) {
		sm_t *grown = sm_set_data_size(dst, NULL, result_size + 64);
		if (grown == NULL) {
			sm_free(result);
			return (NULL);
		}
		dst = grown;
	}
	memcpy(dst->m_data, result->m_data, result_size);
	dst->m_data_used = result_size;
	sm_free(result);
	return (dst);
}

sm_t *
sm_union_inplace(sm_t *dst, const sm_t *src)
{
	if (dst == NULL)
		return (NULL);
	if (sm_is_empty(src))
		return (dst);
	if (sm_is_empty(dst)) {
		/* dst becomes a copy of src.  Use the chunk-aware copy path. */
		sm_t *copy = sm_copy(src);
		if (copy == NULL)
			return (NULL);
		return (__sm_replace_buffer(dst, copy));
	}
	return (__sm_replace_buffer(dst, sm_union(dst, src)));
}

sm_t *
sm_intersection_inplace(sm_t *dst, const sm_t *src)
{
	if (dst == NULL)
		return (NULL);
	if (sm_is_empty(dst))
		return (dst);
	if (sm_is_empty(src)) {
		sm_clear(dst);
		return (dst);
	}
	return (__sm_replace_buffer(dst, sm_intersection(dst, src)));
}

sm_t *
sm_difference_inplace(sm_t *dst, const sm_t *src)
{
	if (dst == NULL)
		return (NULL);
	if (sm_is_empty(dst) || sm_is_empty(src))
		return (dst);
	return (__sm_replace_buffer(dst, sm_difference(dst, src)));
}

/* -------------------------------------------------------------------
 * Maintenance and introspection: range flip, validate, statistics,
 * shrink_to_fit
 * ------------------------------------------------------------------- */

bool
sm_flip_range(sm_t *map, uint64_t lo, uint64_t hi)
{
	if (map == NULL || lo >= hi)
		return (lo >= hi);
	for (uint64_t i = lo; i < hi; i++) {
		const bool was_set = sm_contains(map, i, NULL);
		if (sm_assign(map, i, !was_set) == SM_IDX_MAX) {
			return (false);
		}
	}
	return (true);
}

bool
sm_validate(const sm_t *map)
{
	if (map == NULL)
		return (true);
	if (map->m_data == NULL && __sm_cap(map) > 0)
		return (false);
	if (map->m_data_used > __sm_cap(map))
		return (false);
	if (map->m_data_used == 0) {
		return (true);
	}
	if (map->m_data_used < SM_SIZEOF_OVERHEAD)
		return (false);

	const size_t count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (map->m_data_used == SM_SIZEOF_OVERHEAD);
	}

	uint8_t *p = __sm_get_chunk_data(map, 0);
	uint8_t *end = map->m_data + map->m_data_used;
	__sm_idx_t prev_start = 0;
	bool first = true;
	for (size_t i = 0; i < count; i++) {
		if (p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) > end) {
			return (false);
		}
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		if (!first && start <= prev_start) {
			return (false);
		}
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);
		if (p + SM_SIZEOF_OVERHEAD + chunk_size > end) {
			return (false);
		}
		p += SM_SIZEOF_OVERHEAD + chunk_size;
		prev_start = start;
		first = false;
	}
	return (p == end);
}

void
sm_statistics(const sm_t *map, sm_stats_t *stats)
{
	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (map == NULL)
		return;

	stats->bytes_used = sm_get_size((sm_t *)map);
	stats->bytes_capacity = sm_get_capacity(map);

	const size_t count = __sm_get_chunk_count(map);
	stats->chunks_total = count;
	if (count == 0)
		return;

	uint8_t *p = __sm_get_chunk_data(map, 0);
	for (size_t i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);
		if (__sm_chunk_is_rle(&chunk)) {
			stats->chunks_rle++;
			stats->bits_in_rle += __sm_chunk_rle_get_length(&chunk);
		} else {
			stats->chunks_sparse++;
			const __sm_bitvec_t desc = chunk.m_data[0];
			size_t pos = 1;
			for (size_t v = 0; v < SM_FLAGS_PER_INDEX; v++) {
				const size_t flags =
				    SM_CHUNK_GET_FLAGS(desc, v);
				if (flags == SM_PAYLOAD_ONES) {
					stats->bits_in_sparse +=
					    SM_BITS_PER_VECTOR;
				} else if (flags == SM_PAYLOAD_MIXED) {
					stats->bits_in_sparse +=
					    (uint64_t)SM_POPCOUNT64(
					        chunk.m_data[pos]);
					pos++;
				}
			}
		}
		p += SM_SIZEOF_OVERHEAD + chunk_size;
	}
	stats->bits_set = stats->bits_in_rle + stats->bits_in_sparse;
	stats->bytes_per_set_bit = stats->bits_set == 0 ?
	    0.0 :
	    (double)stats->bytes_used / (double)stats->bits_set;
}

sm_t *
sm_shrink_to_fit(sm_t *map)
{
	if (map == NULL)
		return (NULL);
	if (__sm_kind(map) == SM_WRAPPED)
		return (map);

	const size_t target =
	    map->m_data_used > 0 ? map->m_data_used : SM_SIZEOF_OVERHEAD;
	if (target == __sm_cap(map))
		return (map);

	return (sm_set_data_size(map, NULL, target));
}

/* -------------------------------------------------------------------
 * Portable serialization
 * ------------------------------------------------------------------- */

#define SM_WIRE_MAGIC      0x30316d73u /* "sm10" little-endian */
#define SM_WIRE_VERSION    2u
#define SM_WIRE_HEADER_LEN 16u
#define SM_WIRE_FLAG_LE    0x01u

static bool
__sm_host_is_little_endian(void)
{
	const uint16_t one = 1;
	return (((const uint8_t *)&one)[0] == 1);
}

size_t
sm_serialized_size(const sm_t *map)
{
	if (map == NULL)
		return (SM_WIRE_HEADER_LEN + SM_SIZEOF_OVERHEAD);
	return (SM_WIRE_HEADER_LEN + sm_get_size((sm_t *)map));
}

size_t
sm_serialize(const sm_t *map, uint8_t *out, size_t out_size)
{
	if (out == NULL)
		return (0);
	const size_t needed = sm_serialized_size(map);
	if (out_size < needed)
		return (0);

	const uint64_t cardinality =
	    (map == NULL || sm_is_empty(map)) ? 0 : sm_cardinality((sm_t *)map);
	const uint8_t flags =
	    __sm_host_is_little_endian() ? SM_WIRE_FLAG_LE : 0;

	/* Header: writes via memcpy so it works on strict-alignment cpus. */
	const uint32_t magic = SM_WIRE_MAGIC;
	memcpy(out + 0, &magic, 4);
	out[4] = SM_WIRE_VERSION;
	out[5] = flags;
	out[6] = 0;
	out[7] = 0;
	memcpy(out + 8, &cardinality, 8);

	/* Body: existing internal format (or just an SM_SIZEOF_OVERHEAD
	 * zeroed header for NULL/empty maps). */
	if (map == NULL || sm_is_empty(map)) {
		memset(out + SM_WIRE_HEADER_LEN, 0, SM_SIZEOF_OVERHEAD);
	} else {
		memcpy(out + SM_WIRE_HEADER_LEN, sm_get_data((sm_t *)map),
		    sm_get_size((sm_t *)map));
	}
	return (needed);
}

sm_t *
sm_deserialize(const uint8_t *in, size_t n)
{
	if (in == NULL || n < SM_WIRE_HEADER_LEN + SM_SIZEOF_OVERHEAD) {
		return (NULL);
	}
	uint32_t magic;
	memcpy(&magic, in + 0, 4);
	if (magic != SM_WIRE_MAGIC)
		return (NULL);

	const uint8_t version = in[4];
	const uint8_t flags = in[5];
	if (version != SM_WIRE_VERSION)
		return (NULL);

	const bool wire_is_le = (flags & SM_WIRE_FLAG_LE) != 0;
	const bool host_is_le = __sm_host_is_little_endian();
	if (wire_is_le != host_is_le) {
		/* Cross-endian read not yet supported. */
		return (NULL);
	}

	/* Body: starts at offset SM_WIRE_HEADER_LEN. */
	const size_t body_len = n - SM_WIRE_HEADER_LEN;
	sm_t *map = sm_create(body_len + 64);
	if (map == NULL)
		return (NULL);

	/* Copy the body into the map's data buffer.  The first SM_SIZEOF_OVERHEAD
	 * bytes are the chunk count; the rest is chunks. */
	memcpy(map->m_data, in + SM_WIRE_HEADER_LEN, body_len);
	/* Force m_data_used to its expected value: the first 4 bytes contain
	 * chunk_count, then we need to walk to compute total size.
	 * sm_open's pattern handles this. */
	map->m_data_used = body_len;

	/* Validate the result; reject malformed input. */
	if (!sm_validate(map)) {
		sm_free(map);
		return (NULL);
	}
	return (map);
}

/**
 * @brief Copy a raw chunk (start offset + descriptor + vectors) into result.
 */
static bool
__sm_copy_chunk_to_result(sm_t **resultp, const uint8_t *chunk_ptr)
{
	const __sm_chunk_t chunk = { .m_data =
		                         (__sm_bitvec_unaligned_t *)(chunk_ptr +
		                             SM_SIZEOF_OVERHEAD) };
	const size_t chunk_bytes =
	    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
	if (!__sm_ensure_capacity(resultp, chunk_bytes)) {
		return (false);
	}
	__sm_append_data(*resultp, chunk_ptr, chunk_bytes);
	__sm_set_chunk_count(*resultp, __sm_get_chunk_count(*resultp) + 1);
	return (true);
}

/* -------------------------------------------------------------------
 * Set operations: chunk-merge intersection, difference, union
 * ------------------------------------------------------------------- */

/**
 * @brief Create a new sparsemap containing the intersection of a and b.
 *
 * Uses a two-pointer chunk merge walk for O(chunks) performance instead
 * of the previous O(cardinality x chunks) bit-by-bit scan+contains.
 */
sm_t *
sm_intersection(const sm_t *a, const sm_t *b)
{
	__sm_check_invariants(a);
	__sm_check_invariants(b);
	if (a == NULL || b == NULL) {
		return (NULL);
	}

	const size_t a_count = __sm_get_chunk_count(a);
	const size_t b_count = __sm_get_chunk_count(b);

	if (a_count == 0 || b_count == 0) {
		return (NULL);
	}

	size_t cap = a->m_data_used;
	{
		size_t cap_b = b->m_data_used;
		if (cap_b > cap)
			cap = cap_b;
	}
	if (cap < 1024)
		cap = 1024;

	sm_t *result = sparsemap(cap);
	if (result == NULL) {
		return (NULL);
	}

	uint8_t *ap = __sm_get_chunk_data(a, 0);
	uint8_t *bp = __sm_get_chunk_data(b, 0);
	size_t ai = 0, bi = 0;

	while (ai < a_count && bi < b_count) {
		/* Read chunk a metadata */
		const __sm_idx_t a_start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t a_chunk;
		__sm_chunk_init(&a_chunk, ap + SM_SIZEOF_OVERHEAD);
		const bool a_rle = SM_IS_CHUNK_RLE(&a_chunk);
		const size_t a_cap = __sm_chunk_get_capacity(&a_chunk);
		const size_t a_size = __sm_chunk_get_size(&a_chunk);
		const size_t a_end =
		    (size_t)a_start + a_cap; /* one past last bit */

		/* Read chunk b metadata */
		const __sm_idx_t b_start = __sm_load_idx((const uint8_t *)bp);
		__sm_chunk_t b_chunk;
		__sm_chunk_init(&b_chunk, bp + SM_SIZEOF_OVERHEAD);
		const bool b_rle = SM_IS_CHUNK_RLE(&b_chunk);
		const size_t b_cap = __sm_chunk_get_capacity(&b_chunk);
		const size_t b_size = __sm_chunk_get_size(&b_chunk);
		const size_t b_end = (size_t)b_start + b_cap;

		/* Prefetch next chunks */
		if (ai + 1 < a_count) {
			SM_PREFETCH(ap + SM_SIZEOF_OVERHEAD + a_size);
		}
		if (bi + 1 < b_count) {
			SM_PREFETCH(bp + SM_SIZEOF_OVERHEAD + b_size);
		}

		/* No overlap: a is entirely before b */
		if (a_end <= b_start) {
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			continue;
		}

		/* No overlap: b is entirely before a */
		if (b_end <= a_start) {
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
			continue;
		}

		/* Chunks overlap. Handle the common aligned sparse case fast. */
		if (!a_rle && !b_rle && a_start == b_start) {
			/* Word-level AND of two aligned sparse chunks */
			__sm_bitvec_t aw[32], bw[32];
			int ac[32], bc[32];
			__sm_expand_sparse_chunk(&a_chunk, aw, ac);
			__sm_expand_sparse_chunk(&b_chunk, bw, bc);

			__sm_bitvec_t rw[32];
			int rc[32];
			__sm_words_and(rw, aw, bw);
			for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
				rc[i] = (ac[i] && bc[i]) ? 1 : 0;
				if (!rc[i])
					rw[i] = 0;
			}

			__sm_bitvec_t desc;
			__sm_bitvec_t vecs[32];
			int nvecs;
			if (__sm_encode_sparse_chunk(rw, rc, &desc, vecs,
			        &nvecs)) {
				if (!__sm_append_sparse_chunk(&result, a_start,
				        desc, vecs, nvecs)) {
					sm_free(result);
					return (NULL);
				}
			}
		} else if (a_rle && b_rle) {
			/* Both RLE: intersection is the overlap of two runs */
			const size_t a_len =
			    __sm_chunk_rle_get_length(&a_chunk);
			const size_t b_len =
			    __sm_chunk_rle_get_length(&b_chunk);
			/* a has set bits [a_start, a_start+a_len), b has [b_start, b_start+b_len) */
			const size_t overlap_start =
			    a_start > b_start ? a_start : b_start;
			const size_t a_set_end = (size_t)a_start + a_len;
			const size_t b_set_end = (size_t)b_start + b_len;
			const size_t overlap_end =
			    a_set_end < b_set_end ? a_set_end : b_set_end;
			if (overlap_start < overlap_end) {
				const size_t run_len =
				    overlap_end - overlap_start;
				const size_t run_cap =
				    run_len; /* tight capacity */
				if (!__sm_append_rle_chunk(&result,
				        (__sm_idx_t)overlap_start, run_cap,
				        run_len)) {
					sm_free(result);
					return (NULL);
				}
			}
		} else {
			/* Mixed types: expand both to words, AND, encode.
			 * Use the sparse chunk's start as the target alignment. */
			__sm_bitvec_t aw[SM_FLAGS_PER_INDEX],
			    bw[SM_FLAGS_PER_INDEX];
			int ac[SM_FLAGS_PER_INDEX], bc[SM_FLAGS_PER_INDEX];
			__sm_idx_t result_start;

			if (!a_rle && !b_rle) {
				/* Both sparse but misaligned (shouldn't normally happen) */
				__sm_expand_sparse_chunk(&a_chunk, aw, ac);
				__sm_expand_sparse_chunk(&b_chunk, bw, bc);
				result_start = a_start;
			} else if (a_rle && !b_rle) {
				/* a is RLE, b is sparse: expand a into b's alignment */
				__sm_expand_sparse_chunk(&b_chunk, bw, bc);
				__sm_expand_rle_as_words(&a_chunk, a_start,
				    b_start, aw, ac, bc);
				result_start = b_start;
			} else if (!a_rle && b_rle) {
				/* a is sparse, b is RLE: expand b into a's alignment */
				__sm_expand_sparse_chunk(&a_chunk, aw, ac);
				__sm_expand_rle_as_words(&b_chunk, b_start,
				    a_start, bw, bc, ac);
				result_start = a_start;
			} else {
				/* Both RLE: already handled above, should not reach here */
				result_start = a_start;
				for (int i = 0; i < (int)SM_FLAGS_PER_INDEX;
				     i++) {
					aw[i] = bw[i] = 0;
					ac[i] = bc[i] = 0;
				}
			}

			__sm_bitvec_t rw[SM_FLAGS_PER_INDEX];
			int rc[SM_FLAGS_PER_INDEX];
			__sm_words_and(rw, aw, bw);
			for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
				rc[i] = (ac[i] && bc[i]) ? 1 : 0;
				if (!rc[i])
					rw[i] = 0;
			}

			__sm_bitvec_t desc;
			__sm_bitvec_t vecs[SM_FLAGS_PER_INDEX];
			int nvecs;
			if (__sm_encode_sparse_chunk(rw, rc, &desc, vecs,
			        &nvecs)) {
				if (!__sm_append_sparse_chunk(&result,
				        result_start, desc, vecs, nvecs)) {
					sm_free(result);
					return (NULL);
				}
			}
		}

		/* Advance whichever chunk ends first */
		if (a_end <= b_end) {
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
		}
		if (b_end <= a_end) {
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
		}
	}

	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	return (result);
}

/**
 * @brief Emit set bits from a chunk within [from, to) into result.
 *
 * For sparse chunks, uses expand-mask-encode for bulk processing.
 * For RLE chunks, emits a single RLE chunk covering the set bit range.
 */
static bool
__sm_emit_chunk_bits(sm_t **resultp, const __sm_chunk_t *chunk, bool is_rle,
    __sm_idx_t chunk_start, size_t from, size_t to)
{
	if (from >= to)
		return (true);

	if (is_rle) {
		const size_t len = __sm_chunk_rle_get_length(chunk);
		const size_t set_start = (size_t)chunk_start;
		const size_t set_end = set_start + len;
		const size_t emit_start = from > set_start ? from : set_start;
		const size_t emit_end = to < set_end ? to : set_end;
		if (emit_start < emit_end) {
			const size_t emit_len = emit_end - emit_start;
			return (__sm_append_rle_chunk(resultp,
			    (__sm_idx_t)emit_start, emit_len, emit_len));
		}
		return (true);
	}

	/* Sparse: expand, mask to [from, to) range, encode and append */
	__sm_bitvec_t words[SM_FLAGS_PER_INDEX];
	int cap_flags[SM_FLAGS_PER_INDEX];
	__sm_expand_sparse_chunk(chunk, words, cap_flags);

	/* Mask out bits outside [from, to) range relative to chunk_start */
	const size_t rel_from = from - (size_t)chunk_start;
	const size_t rel_to = to - (size_t)chunk_start;
	const int start_word = (int)(rel_from / SM_BITS_PER_VECTOR);
	const int end_word =
	    (int)((rel_to + SM_BITS_PER_VECTOR - 1) / SM_BITS_PER_VECTOR);

	/* Zero words entirely before the range */
	for (int i = 0; i < start_word && i < (int)SM_FLAGS_PER_INDEX; i++) {
		words[i] = 0;
		cap_flags[i] = 0;
	}

	/* Mask partial start word */
	if (start_word < (int)SM_FLAGS_PER_INDEX) {
		const size_t start_bit = rel_from % SM_BITS_PER_VECTOR;
		if (start_bit > 0) {
			words[start_word] &= ~((__sm_bitvec_t)0) << start_bit;
		}
	}

	/* Zero words entirely after the range */
	for (int i = end_word; i < (int)SM_FLAGS_PER_INDEX; i++) {
		words[i] = 0;
		cap_flags[i] = 0;
	}

	/* Mask partial end word */
	if (end_word > 0 && end_word <= (int)SM_FLAGS_PER_INDEX) {
		const size_t end_bit = rel_to % SM_BITS_PER_VECTOR;
		if (end_bit > 0) {
			words[end_word - 1] &=
			    ((__sm_bitvec_t)1 << end_bit) - 1;
		}
	}

	__sm_bitvec_t desc;
	__sm_bitvec_t vecs[SM_FLAGS_PER_INDEX];
	int nvecs;
	if (__sm_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs)) {
		if (!__sm_append_sparse_chunk(resultp, chunk_start, desc, vecs,
		        nvecs)) {
			return (false);
		}
	}
	return (true);
}

/**
 * @brief Create a new sparsemap containing the difference a \ b (bits in a but not in b).
 *
 * Uses a two-pointer chunk merge walk with a cursor to track progress
 * through each a chunk, preventing double-counting when one a chunk
 * overlaps with multiple b chunks.
 */
sm_t *
sm_difference(const sm_t *a, const sm_t *b)
{
	__sm_check_invariants(a);
	__sm_check_invariants(b);
	if (a == NULL) {
		return (NULL);
	}

	const size_t a_count = __sm_get_chunk_count(a);
	if (a_count == 0) {
		return (NULL);
	}

	/* If b is NULL or empty, return a copy of a */
	if (b == NULL || __sm_get_chunk_count(b) == 0) {
		return (sm_copy(a));
	}

	const size_t b_count = __sm_get_chunk_count(b);

	size_t cap = a->m_data_used;
	if (cap < 1024)
		cap = 1024;

	sm_t *result = sparsemap(cap);
	if (result == NULL) {
		return (NULL);
	}

	uint8_t *ap = __sm_get_chunk_data(a, 0);
	uint8_t *bp = __sm_get_chunk_data(b, 0);
	size_t ai = 0, bi = 0;

	while (ai < a_count) {
		/* Read chunk a metadata */
		const __sm_idx_t a_start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t a_chunk;
		__sm_chunk_init(&a_chunk, ap + SM_SIZEOF_OVERHEAD);
		const bool a_rle = SM_IS_CHUNK_RLE(&a_chunk);
		const size_t a_cap_bits = __sm_chunk_get_capacity(&a_chunk);
		const size_t a_size = __sm_chunk_get_size(&a_chunk);
		const size_t a_end = (size_t)a_start + a_cap_bits;

		/* Prefetch next a chunk */
		if (ai + 1 < a_count) {
			SM_PREFETCH(ap + SM_SIZEOF_OVERHEAD + a_size);
		}

		/* If b is exhausted, copy remaining a chunks */
		if (bi >= b_count) {
			if (!__sm_copy_chunk_to_result(&result, ap)) {
				sm_free(result);
				return (NULL);
			}
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			continue;
		}

		/* Cursor: tracks how far into this a chunk we've processed */
		size_t a_cursor = (size_t)a_start;

		/* Save b state so we can iterate b within this a chunk */
		uint8_t *bp_save = bp;
		size_t bi_save = bi;

		/* Process all b chunks that overlap with this a chunk */
		while (bi < b_count) {
			const __sm_idx_t b_start =
			    __sm_load_idx((const uint8_t *)bp);
			__sm_chunk_t b_chunk;
			__sm_chunk_init(&b_chunk, bp + SM_SIZEOF_OVERHEAD);
			const bool b_rle = SM_IS_CHUNK_RLE(&b_chunk);
			const size_t b_cap_bits =
			    __sm_chunk_get_capacity(&b_chunk);
			const size_t b_size = __sm_chunk_get_size(&b_chunk);
			const size_t b_end = (size_t)b_start + b_cap_bits;

			/* b is past a: no more overlaps for this a chunk */
			if (a_end <= (size_t)b_start)
				break;

			/* b is entirely before cursor: skip b */
			if (b_end <= a_cursor) {
				bp += SM_SIZEOF_OVERHEAD + b_size;
				bi++;
				continue;
			}

			/* Overlap region */
			const size_t ov_start = (size_t)b_start > a_cursor ?
			    (size_t)b_start :
			    a_cursor;
			const size_t ov_end = a_end < b_end ? a_end : b_end;

			/* Emit a's surviving bits in the gap [a_cursor, ov_start) */
			if (!__sm_emit_chunk_bits(&result, &a_chunk, a_rle,
			        a_start, a_cursor, ov_start)) {
				sm_free(result);
				return (NULL);
			}

			/* Process overlap: aligned sparse fast path */
			if (!a_rle && !b_rle && a_start == b_start) {
				__sm_bitvec_t aw[32], bw[32];
				int ac[32], bc[32];
				__sm_expand_sparse_chunk(&a_chunk, aw, ac);
				__sm_expand_sparse_chunk(&b_chunk, bw, bc);

				__sm_bitvec_t rw[32];
				int rc[32];
				__sm_words_andnot(rw, aw, bw);
				for (int i = 0; i < (int)SM_FLAGS_PER_INDEX;
				     i++) {
					if (ac[i]) {
						if (!bc[i])
							rw[i] = aw
							    [i]; /* b has no cap: keep a unchanged */
						rc[i] = 1;
					} else {
						rw[i] = 0;
						rc[i] = 0;
					}
				}

				__sm_bitvec_t desc;
				__sm_bitvec_t vecs[32];
				int nvecs;
				if (__sm_encode_sparse_chunk(rw, rc, &desc,
				        vecs, &nvecs)) {
					if (!__sm_append_sparse_chunk(&result,
					        a_start, desc, vecs, nvecs)) {
						sm_free(result);
						return (NULL);
					}
				}
				a_cursor =
				    a_end; /* entire a chunk handled by word-level op */
			} else {
				/* Mixed types: expand both to words, AND-NOT, encode */
				__sm_bitvec_t aw2[SM_FLAGS_PER_INDEX],
				    bw2[SM_FLAGS_PER_INDEX];
				int ac2[SM_FLAGS_PER_INDEX],
				    bc2[SM_FLAGS_PER_INDEX];
				__sm_idx_t result_start;

				if (a_rle && !b_rle) {
					/* a is RLE, b is sparse */
					__sm_expand_sparse_chunk(&b_chunk, bw2,
					    bc2);
					__sm_expand_rle_as_words(&a_chunk,
					    a_start, b_start, aw2, ac2, bc2);
					result_start = b_start;
				} else if (!a_rle && b_rle) {
					/* a is sparse, b is RLE */
					__sm_expand_sparse_chunk(&a_chunk, aw2,
					    ac2);
					__sm_expand_rle_as_words(&b_chunk,
					    b_start, a_start, bw2, bc2, ac2);
					result_start = a_start;
				} else if (!a_rle && !b_rle) {
					/* Both sparse but misaligned */
					__sm_expand_sparse_chunk(&a_chunk, aw2,
					    ac2);
					__sm_expand_sparse_chunk(&b_chunk, bw2,
					    bc2);
					result_start = a_start;
				} else {
					/* Both RLE: should not reach here (handled by emit_chunk_bits path) */
					result_start = a_start;
					for (int i = 0;
					     i < (int)SM_FLAGS_PER_INDEX; i++) {
						aw2[i] = bw2[i] = 0;
						ac2[i] = bc2[i] = 0;
					}
				}

				__sm_bitvec_t rw2[SM_FLAGS_PER_INDEX];
				int rc2[SM_FLAGS_PER_INDEX];
				__sm_words_andnot(rw2, aw2, bw2);
				for (int i = 0; i < (int)SM_FLAGS_PER_INDEX;
				     i++) {
					if (ac2[i]) {
						if (!bc2[i])
							rw2[i] = aw2
							    [i]; /* b has no cap: keep a unchanged */
						rc2[i] = 1;
					} else {
						rw2[i] = 0;
						rc2[i] = 0;
					}
				}

				__sm_bitvec_t desc2;
				__sm_bitvec_t vecs2[SM_FLAGS_PER_INDEX];
				int nvecs2;
				if (__sm_encode_sparse_chunk(rw2, rc2, &desc2,
				        vecs2, &nvecs2)) {
					if (!__sm_append_sparse_chunk(&result,
					        result_start, desc2, vecs2,
					        nvecs2)) {
						sm_free(result);
						return (NULL);
					}
				}
				a_cursor = ov_end;
			}

			/* Advance b if it ends within or at a's boundary */
			if (b_end <= a_end) {
				bp += SM_SIZEOF_OVERHEAD + b_size;
				bi++;
			}
			/* If a ends within b, we're done with this a chunk */
			if (a_end <= b_end)
				break;
		}

		/* Emit remaining a bits [a_cursor, a_end) that had no b overlap */
		if (a_cursor < a_end) {
			if (!__sm_emit_chunk_bits(&result, &a_chunk, a_rle,
			        a_start, a_cursor, a_end)) {
				sm_free(result);
				return (NULL);
			}
		}

		/* Restore b pointer: next a chunk may overlap with same b chunks.
       But we only need b chunks that haven't been fully passed yet.
       Keep bi/bp at the furthest b that still overlaps or is ahead. */
		(void)bp_save;
		(void)bi_save;

		ap += SM_SIZEOF_OVERHEAD + a_size;
		ai++;
	}

	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	return (result);
}

/**
 * @brief Create a new sparsemap containing the union of a and b.
 *
 * Uses a two-pointer chunk merge walk for O(chunks) performance instead
 * of the previous O(cardinality x chunks) in-place mutation.  Cursors
 * track partially-consumed chunks when one chunk extends past the other.
 *
 * Fast paths:
 *   - Aligned sparse chunks: word-level OR via expand/encode helpers.
 *   - Both RLE chunks: direct run merge (handles contiguous and gapped runs).
 *   - Mixed/misaligned: bit-by-bit OR bounded by sparse chunk capacity.
 *
 * @param[in] a  First input sparsemap.
 * @param[in] b  Second input sparsemap.
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if both inputs are empty/NULL.
 */
sm_t *
sm_union(const sm_t *a, const sm_t *b)
{
	__sm_check_invariants(a);
	__sm_check_invariants(b);
	if (a == NULL && b == NULL) {
		return (NULL);
	}

	const size_t a_count = a ? __sm_get_chunk_count(a) : 0;
	const size_t b_count = b ? __sm_get_chunk_count(b) : 0;

	if (a_count == 0 && b_count == 0) {
		return (NULL);
	}
	if (a_count == 0) {
		return (sm_copy(b));
	}
	if (b_count == 0) {
		return (sm_copy(a));
	}

	/* Allocate result with combined data size (worst case: no overlap). */
	size_t cap = a->m_data_used + b->m_data_used;
	if (cap < 1024)
		cap = 1024;

	sm_t *result = sparsemap(cap);
	if (result == NULL) {
		return (NULL);
	}

	uint8_t *ap = __sm_get_chunk_data(a, 0);
	uint8_t *bp = __sm_get_chunk_data(b, 0);
	size_t ai = 0, bi = 0;

	/* Cursors track how far into each current chunk we've already emitted.
     A value of 0 means "fresh chunk" (reset after advancing).  When a
     chunk is partially consumed, the cursor holds the absolute bit
     position up to which bits have been emitted. */
	size_t a_cursor = 0;
	size_t b_cursor = 0;

	while (ai < a_count && bi < b_count) {
		/* ---- Read chunk a metadata ---- */
		const __sm_idx_t a_start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t a_chunk;
		__sm_chunk_init(&a_chunk, ap + SM_SIZEOF_OVERHEAD);
		const bool a_rle = SM_IS_CHUNK_RLE(&a_chunk);
		const size_t a_cap_bits = __sm_chunk_get_capacity(&a_chunk);
		const size_t a_size = __sm_chunk_get_size(&a_chunk);
		const size_t a_end = (size_t)a_start + a_cap_bits;

		/* Ensure cursor is at least at chunk start. */
		if (a_cursor < (size_t)a_start)
			a_cursor = (size_t)a_start;

		/* ---- Read chunk b metadata ---- */
		const __sm_idx_t b_start = __sm_load_idx((const uint8_t *)bp);
		__sm_chunk_t b_chunk;
		__sm_chunk_init(&b_chunk, bp + SM_SIZEOF_OVERHEAD);
		const bool b_rle = SM_IS_CHUNK_RLE(&b_chunk);
		const size_t b_cap_bits = __sm_chunk_get_capacity(&b_chunk);
		const size_t b_size = __sm_chunk_get_size(&b_chunk);
		const size_t b_end = (size_t)b_start + b_cap_bits;

		if (b_cursor < (size_t)b_start)
			b_cursor = (size_t)b_start;

		/* Prefetch next chunks for the merge loop. */
		if (ai + 1 < a_count)
			SM_PREFETCH(ap + SM_SIZEOF_OVERHEAD + a_size);
		if (bi + 1 < b_count)
			SM_PREFETCH(bp + SM_SIZEOF_OVERHEAD + b_size);

		/* ---- No overlap: a's remaining range ends before b's ---- */
		if (a_end <= b_cursor) {
			if (a_cursor == (size_t)a_start) {
				if (!__sm_copy_chunk_to_result(&result, ap))
					goto fail;
			} else {
				if (!__sm_emit_chunk_bits(&result, &a_chunk,
				        a_rle, a_start, a_cursor, a_end))
					goto fail;
			}
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			a_cursor = 0;
			continue;
		}

		/* ---- No overlap: b's remaining range ends before a's ---- */
		if (b_end <= a_cursor) {
			if (b_cursor == (size_t)b_start) {
				if (!__sm_copy_chunk_to_result(&result, bp))
					goto fail;
			} else {
				if (!__sm_emit_chunk_bits(&result, &b_chunk,
				        b_rle, b_start, b_cursor, b_end))
					goto fail;
			}
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
			b_cursor = 0;
			continue;
		}

		/* ---- Chunks overlap.  Compute overlap bounds. ---- */
		const size_t ov_start =
		    a_cursor > b_cursor ? a_cursor : b_cursor;
		const size_t ov_end = a_end < b_end ? a_end : b_end;

		/* ---- Fast path: both sparse, aligned ---- */
		/* When aligned, handle the full chunk with per-cursor masking.
       This avoids creating separate pre-overlap chunks at the same start. */
		if (!a_rle && !b_rle && a_start == b_start) {
			__sm_bitvec_t aw[SM_FLAGS_PER_INDEX],
			    bw[SM_FLAGS_PER_INDEX];
			int ac[SM_FLAGS_PER_INDEX], bc[SM_FLAGS_PER_INDEX];
			__sm_expand_sparse_chunk(&a_chunk, aw, ac);
			__sm_expand_sparse_chunk(&b_chunk, bw, bc);

			/* Mask a's words before a_cursor */
			if (a_cursor > (size_t)a_start) {
				const size_t rel = a_cursor - (size_t)a_start;
				const int sw = (int)(rel / SM_BITS_PER_VECTOR);
				for (int i = 0;
				     i < sw && i < (int)SM_FLAGS_PER_INDEX;
				     i++) {
					aw[i] = 0;
					ac[i] = 0;
				}
				const size_t sb = rel % SM_BITS_PER_VECTOR;
				if (sb > 0 && sw < (int)SM_FLAGS_PER_INDEX) {
					aw[sw] &= ~((__sm_bitvec_t)0) << sb;
				}
			}

			/* Mask b's words before b_cursor */
			if (b_cursor > (size_t)b_start) {
				const size_t rel = b_cursor - (size_t)b_start;
				const int sw = (int)(rel / SM_BITS_PER_VECTOR);
				for (int i = 0;
				     i < sw && i < (int)SM_FLAGS_PER_INDEX;
				     i++) {
					bw[i] = 0;
					bc[i] = 0;
				}
				const size_t sb = rel % SM_BITS_PER_VECTOR;
				if (sb > 0 && sw < (int)SM_FLAGS_PER_INDEX) {
					bw[sw] &= ~((__sm_bitvec_t)0) << sb;
				}
			}

			__sm_bitvec_t rw[SM_FLAGS_PER_INDEX];
			int rc[SM_FLAGS_PER_INDEX];
			__sm_words_or(rw, aw, bw);
			for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
				rc[i] = (ac[i] || bc[i]) ? 1 : 0;
			}

			__sm_bitvec_t desc;
			__sm_bitvec_t vecs[SM_FLAGS_PER_INDEX];
			int nvecs;
			if (__sm_encode_sparse_chunk(rw, rc, &desc, vecs,
			        &nvecs)) {
				if (!__sm_append_sparse_chunk(&result, a_start,
				        desc, vecs, nvecs))
					goto fail;
			}

			/* Both chunks fully consumed. */
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			a_cursor = 0;
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
			b_cursor = 0;

		} else {
			/* Emit pre-overlap bits from whichever cursor is behind. */
			if (a_cursor < ov_start) {
				if (!__sm_emit_chunk_bits(&result, &a_chunk,
				        a_rle, a_start, a_cursor, ov_start))
					goto fail;
				a_cursor = ov_start;
			}
			if (b_cursor < ov_start) {
				if (!__sm_emit_chunk_bits(&result, &b_chunk,
				        b_rle, b_start, b_cursor, ov_start))
					goto fail;
				b_cursor = ov_start;
			}

			if (a_rle && b_rle) {
				/* ---- Both RLE: merge set-bit runs in [ov_start, ov_end) ---- */
				const size_t a_len =
				    __sm_chunk_rle_get_length(&a_chunk);
				const size_t b_len =
				    __sm_chunk_rle_get_length(&b_chunk);

				/* Clamp each run to the overlap window. */
				const size_t a_set_end =
				    (size_t)a_start + a_len;
				const size_t b_set_end =
				    (size_t)b_start + b_len;
				const size_t as = ov_start > (size_t)a_start ?
				    ov_start :
				    (size_t)a_start;
				const size_t ae =
				    ov_end < a_set_end ? ov_end : a_set_end;
				const size_t bs = ov_start > (size_t)b_start ?
				    ov_start :
				    (size_t)b_start;
				const size_t be =
				    ov_end < b_set_end ? ov_end : b_set_end;

				const bool a_has = as < ae;
				const bool b_has = bs < be;

				if (a_has && b_has) {
					const size_t min_s = as < bs ? as : bs;
					const size_t max_e = ae > be ? ae : be;
					/* Check if runs overlap or are adjacent. */
					const size_t earlier_e =
					    as <= bs ? ae : be;
					const size_t later_s =
					    as <= bs ? bs : as;

					if (earlier_e >= later_s) {
						/* Contiguous: single merged RLE. */
						if (!__sm_append_rle_chunk(
						        &result,
						        (__sm_idx_t)min_s,
						        max_e - min_s,
						        max_e - min_s))
							goto fail;
					} else {
						/* Gap between runs: two separate RLE chunks. */
						const size_t r1_s =
						    as <= bs ? as : bs;
						const size_t r1_e =
						    as <= bs ? ae : be;
						const size_t r2_s =
						    as <= bs ? bs : as;
						const size_t r2_e =
						    as <= bs ? be : ae;
						if (!__sm_append_rle_chunk(
						        &result,
						        (__sm_idx_t)r1_s,
						        r1_e - r1_s,
						        r1_e - r1_s))
							goto fail;
						if (!__sm_append_rle_chunk(
						        &result,
						        (__sm_idx_t)r2_s,
						        r2_e - r2_s,
						        r2_e - r2_s))
							goto fail;
					}
				} else if (a_has) {
					if (!__sm_append_rle_chunk(&result,
					        (__sm_idx_t)as, ae - as,
					        ae - as))
						goto fail;
				} else if (b_has) {
					if (!__sm_append_rle_chunk(&result,
					        (__sm_idx_t)bs, be - bs,
					        be - bs))
						goto fail;
				}
				/* else: no set bits in overlap -- nothing to emit. */

				a_cursor = ov_end;
				b_cursor = ov_end;
				if (a_cursor >= a_end) {
					ap += SM_SIZEOF_OVERHEAD + a_size;
					ai++;
					a_cursor = 0;
				}
				if (b_cursor >= b_end) {
					bp += SM_SIZEOF_OVERHEAD + b_size;
					bi++;
					b_cursor = 0;
				}

			} else {
				/* ---- Mixed types or misaligned sparse: expand-OR-encode ---- */
				__sm_bitvec_t aw2[SM_FLAGS_PER_INDEX],
				    bw2[SM_FLAGS_PER_INDEX];
				int ac2[SM_FLAGS_PER_INDEX],
				    bc2[SM_FLAGS_PER_INDEX];
				__sm_idx_t result_start;

				if (a_rle && !b_rle) {
					__sm_expand_sparse_chunk(&b_chunk, bw2,
					    bc2);
					__sm_expand_rle_as_words(&a_chunk,
					    a_start, b_start, aw2, ac2, bc2);
					result_start = b_start;
				} else if (!a_rle && b_rle) {
					__sm_expand_sparse_chunk(&a_chunk, aw2,
					    ac2);
					__sm_expand_rle_as_words(&b_chunk,
					    b_start, a_start, bw2, bc2, ac2);
					result_start = a_start;
				} else if (!a_rle && !b_rle) {
					__sm_expand_sparse_chunk(&a_chunk, aw2,
					    ac2);
					__sm_expand_sparse_chunk(&b_chunk, bw2,
					    bc2);
					result_start = a_start;
				} else {
					/* Both RLE: handled above, should not reach here */
					result_start = a_start;
					for (int i = 0;
					     i < (int)SM_FLAGS_PER_INDEX; i++) {
						aw2[i] = bw2[i] = 0;
						ac2[i] = bc2[i] = 0;
					}
				}

				__sm_bitvec_t rw2[SM_FLAGS_PER_INDEX];
				int rc2[SM_FLAGS_PER_INDEX];
				__sm_words_or(rw2, aw2, bw2);
				for (int i = 0; i < (int)SM_FLAGS_PER_INDEX;
				     i++) {
					rc2[i] = (ac2[i] || bc2[i]) ? 1 : 0;
				}

				__sm_bitvec_t desc2;
				__sm_bitvec_t vecs2[SM_FLAGS_PER_INDEX];
				int nvecs2;
				if (__sm_encode_sparse_chunk(rw2, rc2, &desc2,
				        vecs2, &nvecs2)) {
					if (!__sm_append_sparse_chunk(&result,
					        result_start, desc2, vecs2,
					        nvecs2))
						goto fail;
				}

				a_cursor = ov_end;
				b_cursor = ov_end;
				if (a_cursor >= a_end) {
					ap += SM_SIZEOF_OVERHEAD + a_size;
					ai++;
					a_cursor = 0;
				}
				if (b_cursor >= b_end) {
					bp += SM_SIZEOF_OVERHEAD + b_size;
					bi++;
					b_cursor = 0;
				}
			}
		}
	}

	/* Copy remaining chunks from whichever map is not exhausted. */
	while (ai < a_count) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t c;
		__sm_chunk_init(&c, ap + SM_SIZEOF_OVERHEAD);
		const size_t sz = __sm_chunk_get_size(&c);
		if (a_cursor > 0 && a_cursor > (size_t)start) {
			/* Partially consumed: emit only remaining bits. */
			const bool rle = SM_IS_CHUNK_RLE(&c);
			const size_t cap_bits = __sm_chunk_get_capacity(&c);
			if (!__sm_emit_chunk_bits(&result, &c, rle, start,
			        a_cursor, (size_t)start + cap_bits))
				goto fail;
		} else {
			if (!__sm_copy_chunk_to_result(&result, ap))
				goto fail;
		}
		ap += SM_SIZEOF_OVERHEAD + sz;
		ai++;
		a_cursor = 0;
	}
	while (bi < b_count) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)bp);
		__sm_chunk_t c;
		__sm_chunk_init(&c, bp + SM_SIZEOF_OVERHEAD);
		const size_t sz = __sm_chunk_get_size(&c);
		if (b_cursor > 0 && b_cursor > (size_t)start) {
			const bool rle = SM_IS_CHUNK_RLE(&c);
			const size_t cap_bits = __sm_chunk_get_capacity(&c);
			if (!__sm_emit_chunk_bits(&result, &c, rle, start,
			        b_cursor, (size_t)start + cap_bits))
				goto fail;
		} else {
			if (!__sm_copy_chunk_to_result(&result, bp))
				goto fail;
		}
		bp += SM_SIZEOF_OVERHEAD + sz;
		bi++;
		b_cursor = 0;
	}

	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	return (result);

fail:
	sm_free(result);
	return (NULL);
}

/* -------------------------------------------------------------------
 * Split, select, rank, and span
 * ------------------------------------------------------------------- */

uint64_t
sm_split(sm_t *map, uint64_t idx, sm_t *other)
{
	__sm_check_invariants(map);
	__sm_check_invariants(other);
	size_t i;
	const size_t count = __sm_get_chunk_count(map);
	bool in_middle = false;

	__sm_assert(sm_cardinality(other) == 0);

	/*
	 * According to the API when idx is SM_IDX_MAX the client is
	 * requesting that we divide the bits in two equal portions, so we
	 * calculate that index here.
	 */
	if (idx == SM_IDX_MAX) {
		const uint64_t begin = sm_minimum(map);
		const uint64_t end = sm_maximum(map);
		if (begin != end) {
			const size_t rank = sm_rank(map, begin, end, true);
			idx = sm_select(map, rank / 2, true);
		} else {
			return (SM_IDX_MAX);
		}
	}

	/* Is the index beyond the last bit set in the source? */
	if (idx > sm_maximum(map)) {
		return (idx);
	}

	/*
	 * Here's how this is going to work, there are three phases.
	 * 1) Skip over any chunks before the idx.
	 * 2) If the idx falls within a chunk, ...
	 *  2a) If that chunk is RLE, separate the RLE into two or three chunks
	 *  2b) Recursively call sm_split() because now we have a sparse chunk
	 * 3) Split the sparse chunk
	 * 4) Keep half in the src and insert the other half into the dst
	 * 5) Move any remaining chunks to dst.
	 */
	uint8_t *src = __sm_get_chunk_data(map, 0);
	uint8_t *dst = __sm_get_chunk_end(other);

	/* (1): skip over chunks that are entirely to the left. */
	uint8_t *prev = src;
	for (i = 0; i < count; i++) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)src);
		if (start == idx) {
			break;
		}
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, src + SM_SIZEOF_OVERHEAD);
		if (start + __sm_chunk_get_capacity(&chunk) > idx) {
			in_middle = true;
			break;
		}
		if (start > idx) {
			src = prev;
			i--;
			break;
		}

		prev = src;
		src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
	}

	/* (2): The idx falls within a chunk then it has to be split. */
	if (in_middle) {
		__sm_chunk_t s_chunk, d_chunk;
		__sm_chunk_init(&s_chunk, src + SM_SIZEOF_OVERHEAD);
		__sm_chunk_init(&d_chunk, dst + SM_SIZEOF_OVERHEAD);
		__sm_idx_t src_start = __sm_load_idx((const uint8_t *)src);

		/* (2a) Does the idx fall within the range of an RLE chunk? */
		if (SM_IS_CHUNK_RLE(&s_chunk)) {
			/*
			 * There is a function that can split an RLE chunk at an index, but to use
			 * it and not mutate anything we'll need to jump through a few hoops.
			 * To perform this trick we need to first need a new static buffer
			 * that we can use with a new "stunt" map. Once we have the chunk we need
			 * to split in that new buffer wrapped into a new map we can call our API
			 * that separates the RLE chunk at the index.
			 */

			sm_t stunt;
			__sm_chunk_t chunk;
			SM_ALIGNAS(__sm_bitvec_t) uint8_t
			    buf[(SM_SIZEOF_OVERHEAD * (unsigned long)3) +
			        (sizeof(__sm_bitvec_t) * 6)] = { 0 };

			/* Copy the source chunk into the buffer. */
			memcpy(buf + SM_SIZEOF_OVERHEAD, src,
			    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
			/* Set the number of chunks to 1 in our stunt map. */
			__sm_store_u64((uint8_t *)buf, (uint64_t)1);
			/* And initialize the stunt double chunk we need to split. */
			sm_open(&stunt, buf,
			    (SM_SIZEOF_OVERHEAD * (unsigned long)3) +
			        (sizeof(__sm_bitvec_t) * 6));
			__sm_chunk_init(&chunk, buf + (SM_SIZEOF_OVERHEAD * 2));

			/* Finally, let's separate the RLE chunk at index. */
			__sm_chunk_sep_t sep = {
				.target = { .p = buf + SM_SIZEOF_OVERHEAD,
				    .offset = SM_SIZEOF_OVERHEAD,
				    .chunk = &chunk,
				    .start = src_start,
				    .length =
				        __sm_chunk_rle_get_length(&s_chunk),
				    .capacity =
				        __sm_chunk_get_capacity(&s_chunk) }
			};
			/*
			 * Pre-fix the return value here was discarded, then sep.expand_by
			 * was used unconditionally below.  If the separate function
			 * early-returned (the "can't fit a pivot in this space" punt path)
			 * sep.expand_by stayed at zero, but on some inputs the do-while
			 * exited with partially-populated sep state, leaving expand_by to
			 * underflow when computed below -- surfaced by ASan as a
			 * negative-size-param in __sm_insert_data and by glibc as
			 * stack-smashing.  Now we propagate the failure up.
			 */
			const int sep_rc =
			    __sm_separate_rle_chunk(&stunt, &sep, idx, -1);
			if (sep_rc != 0) {
				return (SM_IDX_MAX);
			}

			/*
			 * (2b) Assuming we have the space we'll update the source map with the
			 * separate, but equivalent chunks and then recurse confident that next time
			 * our index will fall inside a sparse chunk (that we just made).
			 */
			SM_ENOUGH_SPACE(sep.expand_by);
			/* Save src offset before insert, as insert will invalidate the pointer */
			size_t src_offset = src - map->m_data;
			__sm_insert_data(map,
			    src_offset + SM_SIZEOF_OVERHEAD +
			        sizeof(__sm_bitvec_t),
			    sep.buf + SM_SIZEOF_OVERHEAD +
			        sizeof(__sm_bitvec_t),
			    sep.expand_by);
			/* Recalculate src pointer after insert operation */
			src = map->m_data + src_offset;
			memcpy(src, sep.buf,
			    sep.expand_by + SM_SIZEOF_OVERHEAD +
			        sizeof(__sm_bitvec_t));
			__sm_set_chunk_count(map,
			    __sm_get_chunk_count(map) + (sep.count - 1));

			return (sm_split(map, idx, other));
		}

		/*
		 * (3) We're in the middle of a sparse chunk, let's split it.
		 */

		/* Zero out the space we'll need at the proper location in dst. */
		uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)] = { 0 };
		memcpy(dst, &buf, sizeof(buf));

		/* And add a chunk to the other map. */
		__sm_set_chunk_count(other, __sm_get_chunk_count(other) + 1);
		if (other->m_data_used != 0) {
			other->m_data_used +=
			    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
		}

		/* Copy the bits in the sparse chunk, at most SM_CHUNK_MAX_CAPACITY. */
		__sm_store_idx((uint8_t *)dst, src_start);
		for (size_t j = idx; j < src_start + SM_CHUNK_MAX_CAPACITY;
		     j++) {
			if (sm_contains(map, j, NULL)) {
				__sm_map_set(other, j, false, NULL);
				__sm_map_unset(map, j, false);
			}
		}
		src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&s_chunk);
		dst += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&d_chunk);
		i++;
	}

	/* Now continue with all remaining chunks. */
	/* Save the offset where moved chunks start, so we can truncate map later */
	size_t split_offset = src - map->m_data;
	size_t chunks_to_move = count - i;

	for (size_t j = 0; j < chunks_to_move; j++) {
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, src + SM_SIZEOF_OVERHEAD);
		size_t chunk_size =
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);

		/* Copy chunk to other */
		__sm_append_data(other, src, chunk_size);
		__sm_set_chunk_count(other, __sm_get_chunk_count(other) + 1);

		src += chunk_size;
	}

	/* Update chunk counts and force recalculation of data sizes */
	__sm_set_chunk_count(map, __sm_get_chunk_count(map) - chunks_to_move);
	map->m_data_used = split_offset;

	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);
	__sm_assert(sm_get_size(other) > SM_SIZEOF_OVERHEAD);

	__sm_coalesce_map(map);
	__sm_coalesce_map(other);

	return (idx);
}

uint64_t
sm_select(sm_t *map, uint64_t n, bool value)
{
	__sm_check_invariants(map);
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);
	const size_t count = __sm_get_chunk_count(map);

	if (count == 0 && value == false) {
		return (n);
	}

	uint8_t *p = __sm_get_chunk_data(map, 0);

	for (size_t i = 0; i < count; i++) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		/* Start of this chunk is greater than n meaning there are a set of 0s
		 * before the first 1 sufficient to consume n. */
		if (value == false && i == 0 && start > n) {
			return (n);
		}
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);

		ssize_t new_n = n;
		const size_t index =
		    __sm_chunk_select(&chunk, n, &new_n, value);
		if (new_n == -1) {
			return (start + index);
		}
		n = new_n;

		p += __sm_chunk_get_size(&chunk);
	}
	return (SM_IDX_MAX);
}

static size_t
__sm_rank_vec(sm_t *map, uint64_t begin, uint64_t end, bool value,
    __sm_bitvec_t *vec)
{
	(void)vec; /* retained for ABI/signature compatibility */
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);

	if (begin > end) {
		return (0);
	}

	/*
	 * Range width as a count.  When [begin, end] spans the entire
	 * 64-bit universe (begin == 0, end == UINT64_MAX) the +1
	 * overflows to 0; size_t saturates instead so the derived unset
	 * count below stays meaningful.  A full-universe unset query is
	 * degenerate (the answer is ~2^64) but must not wrap.
	 */
	const uint64_t span_width = end - begin;
	const size_t width =
	    (span_width == UINT64_MAX) ? SIZE_MAX : (size_t)(span_width + 1);

	/*
	 * Rank is computed from the set-bit count only.  A bit is set
	 * iff some chunk covers it, so the number of set bits in the
	 * inclusive range [begin, end] is the sum, over every chunk, of
	 * the matching bits in the overlap of [begin, end] with that
	 * chunk's covered span [start, start + capacity).  The unset
	 * count is then width - set; there is no cross-chunk gap
	 * bookkeeping to get wrong.
	 *
	 * __sm_chunk_rank does the per-chunk work (it takes from/to
	 * positions relative to the chunk start and is validated by the
	 * get_position / RLE property tests).  We only ever ask it for
	 * set bits here; unset is derived once at the end.
	 */
	const size_t count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (value ? 0 : width);
	}

	size_t set = 0;
	uint8_t *p = __sm_get_chunk_data(map, 0);
	for (size_t i = 0; i < count; i++) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		const size_t chunk_size = __sm_chunk_get_size(&chunk);
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		const size_t cap = __sm_chunk_get_capacity(&chunk);
		const uint64_t chunk_lo = start;
		/* Inclusive top of the chunk's covered span.  cap >= 1, and we
		 * form (cap - 1) as a distance so the comparison below never
		 * overflows even when chunk_lo is near UINT64_MAX (the
		 * top-of-universe case). */
		const uint64_t span = (uint64_t)cap - 1;
		const uint64_t chunk_hi_incl =
		    (chunk_lo > UINT64_MAX - span) ? UINT64_MAX
		                                  : chunk_lo + span;

		/* Chunks are ordered ascending.  Once a chunk starts past
		 * `end` no later chunk can overlap [begin, end]. */
		if (chunk_lo > end) {
			p += chunk_size;
			break;
		}
		/* Skip chunks entirely below `begin`. */
		if (chunk_hi_incl < begin) {
			p += chunk_size;
			continue;
		}

		/* Overlap of [begin, end] with [chunk_lo, chunk_hi_incl]. */
		const uint64_t ov_lo = begin > chunk_lo ? begin : chunk_lo;
		const uint64_t ov_hi_incl =
		    (end < chunk_hi_incl) ? end : chunk_hi_incl;
		/* Positions relative to the chunk start. */
		const size_t from = (size_t)(ov_lo - chunk_lo);
		const size_t to = (size_t)(ov_hi_incl - chunk_lo);

		__sm_chunk_rank_t rank;
		set += __sm_chunk_rank(&rank, true, &chunk, from, to);
		p += chunk_size;
	}

	if (value) {
		return (set);
	}
	__sm_assert((uint64_t)set <= width);
	return ((size_t)(width - set));
}

size_t
sm_rank(sm_t *map, uint64_t begin, uint64_t end, bool value)
{
	__sm_check_invariants(map);
	__sm_bitvec_t vec;
	return (__sm_rank_vec(map, begin, end, value, &vec));
}

uint64_t
sm_span(sm_t *map, uint64_t idx, size_t len, bool value)
{
	__sm_check_invariants(map);
	__sm_bitvec_t vec = 0;

	/* When skipping forward to `idx` offset in the map we can determine how
	 * many selects we can avoid by taking the rank of the range and starting
	 * at that bit. */
	size_t nth = (idx == 0) ? 0 : sm_rank(map, 0, idx - 1, value);
	/* Find the first bit that matches value, then... */
	uint64_t offset = sm_select(map, nth, value);
	do {
		/* See if the rank of the bits in the range starting at offset is equal
		 * to the desired amount. */
		size_t rank = (len == 1) ?
		    1 :
		    __sm_rank_vec(map, offset, offset + len - 1, value, &vec);
		if (rank >= len) {
			/* We've found what we're looking for, return the index of the first
			 * bit in the range. */
			break;
		}
		/* Now we try to jump forward as much as possible before we look for a
		 * new match. We do this by counting the remaining bits in the returned
		 * vec from the call to rank_vec(). */
		int amt = 1;
		if (vec > 0) {
			/* The returned vec had some set bits, let's move forward in the map as
			 * much as possible (max: 64 bit positions). */
			const int max = (int)(len > SM_BITS_PER_VECTOR ?
				SM_BITS_PER_VECTOR :
				len);
			while (amt < max && (vec & (UINT64_C(1) << amt))) {
				amt++;
			}
		}
		nth += amt;
		offset = sm_select(map, nth, value);
	} while (SM_FOUND(offset));

	return (offset);
}

/* -------------------------------------------------------------------
 * Point-lookup / rank / select acceleration (Ideas 3, 4, 5)
 *
 * These add caller-owned, transient acceleration state on TOP of the
 * plain O(chunks) path.  None of them grow sm_t or touch the wire
 * format; every one falls back to the plain path when it cannot be
 * both fast and correct.  Correctness is the invariant: a stale or
 * degenerate accelerator returns the SAME answer as sm_contains /
 * sm_rank / sm_select, just slower.
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 * Idea 5: sm_contains_many -- batched point lookups in one sweep.
 * ------------------------------------------------------------------- */

void
sm_contains_many(const sm_t *map, const uint64_t *idxs, bool *results,
    size_t n)
{
	if (n == 0) {
		return;
	}
	if (map == NULL) {
		for (size_t q = 0; q < n; q++) {
			results[q] = false;
		}
		return;
	}

#ifdef SPARSEMAP_DIAGNOSTIC
	/* Contract: idxs MUST be sorted ascending. */
	for (size_t q = 1; q < n; q++) {
		__sm_assert(idxs[q] >= idxs[q - 1]);
	}
#endif

	const size_t count = __sm_get_chunk_count(map);
	if (count == 0) {
		for (size_t q = 0; q < n; q++) {
			results[q] = false;
		}
		return;
	}

	uint8_t *base = __sm_get_chunk_data(map, 0);
	uint8_t *p = base;
	const size_t stream_end =
	    (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;
	size_t q = 0;

	/*
	 * One left-to-right sweep.  Walk chunks in order while draining the
	 * query cursor q into idxs[].  For each chunk [start, start+cap):
	 * queries strictly below start fall in a gap (false); queries below
	 * start+cap are answered by the within-chunk test; queries at or
	 * above start+cap belong to a later chunk, so advance the chunk.
	 * O(chunks + n).
	 */
	for (size_t i = 0; i < count && q < n; i++) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t cap = __sm_chunk_get_capacity(&chunk);
		const uint64_t hi = (uint64_t)s + cap; /* exclusive top */

		/* Drain queries that fall before this chunk (gap -> false). */
		while (q < n && idxs[q] < (uint64_t)s) {
			results[q] = false;
			q++;
		}
		/* Answer queries that fall inside this chunk's covered span. */
		while (q < n && idxs[q] < hi) {
			results[q] =
			    __sm_chunk_is_set(&chunk, idxs[q] - (uint64_t)s);
			q++;
		}

		/* Advance to the next chunk. */
		const size_t next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			break;
		}
		p = base + next_off;
	}

	/* Any queries past the last chunk are not set. */
	for (; q < n; q++) {
		results[q] = false;
	}
}

/* -------------------------------------------------------------------
 * Idea 4: sm_locator_t -- transient two-level sqrt(n) directory.
 * ------------------------------------------------------------------- */

/* Integer floor(sqrt(x)); avoids pulling in <math.h> and float determinism
 * worries.  x <= chunk count, so this is cheap. */
static size_t
__sm_isqrt(size_t x)
{
	if (x == 0) {
		return (0);
	}
	size_t r = 0;
	while ((r + 1) * (r + 1) <= x) {
		r++;
	}
	return (r);
}

/* True when the locator's cached shape no longer matches its map, i.e. the
 * caller mutated the map without rebuilding.  A stale locator is a usage
 * error; queries fall back to the plain path so results stay correct. */
static bool
__sm_locator_is_stale(const sm_locator_t *loc)
{
	if (loc == NULL || loc->map == NULL || loc->n_sb == 0) {
		return (true);
	}
	if (__sm_get_chunk_count(loc->map) != loc->count) {
		return (true);
	}
	/* First and last chunk starts are cheap O(1) fingerprints: a
	 * mutation that preserves the chunk count but shifts, splits, or
	 * coalesces chunks almost always moves one of them.  This is a
	 * best-effort check, not a proof of freshness -- but any miss still
	 * yields a correct answer via the fine-walk, which self-validates
	 * against the actual chunk bytes it reads. */
	uint8_t *base = __sm_get_chunk_data(loc->map, 0);
	const __sm_idx_t first = __sm_load_idx((const uint8_t *)base);
	if (first != loc->first_start) {
		return (true);
	}
	if ((size_t)loc->last_offset + sizeof(__sm_idx_t) >
	    (size_t)loc->map->m_data_used - SM_SIZEOF_OVERHEAD) {
		return (true);
	}
	const __sm_idx_t last =
	    __sm_load_idx((const uint8_t *)(base + loc->last_offset));
	if (last != loc->last_start) {
		return (true);
	}
	return (false);
}

sm_locator_t *
sm_locator_build(const sm_t *map)
{
	if (map == NULL) {
		return (NULL);
	}
	const size_t count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (NULL);
	}

	sm_locator_t *loc = (sm_locator_t *)__sm_alloc(sizeof(*loc));
	if (loc == NULL) {
		return (NULL);
	}

	const size_t stride = __sm_isqrt(count) > 0 ? __sm_isqrt(count) : 1;
	const size_t n_sb = (count + stride - 1) / stride;

	uint64_t *sb_start = (uint64_t *)__sm_alloc(n_sb * sizeof(uint64_t));
	size_t *sb_offset = (size_t *)__sm_alloc(n_sb * sizeof(size_t));
	size_t *sb_prefix = (size_t *)__sm_alloc(n_sb * sizeof(size_t));
	if (sb_start == NULL || sb_offset == NULL || sb_prefix == NULL) {
		__sm_free(sb_start);
		__sm_free(sb_offset);
		__sm_free(sb_prefix);
		__sm_free(loc);
		return (NULL);
	}

	/* One O(count) walk: sample every stride-th chunk into the
	 * superblock arrays and carry the running set-bit total. */
	uint8_t *base = __sm_get_chunk_data(map, 0);
	uint8_t *p = base;
	const size_t stream_end =
	    (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;
	size_t running = 0; /* set bits in chunks strictly before p */
	size_t sb = 0;
	size_t last_offset = 0;
	__sm_idx_t last_start = 0;
	for (size_t i = 0; i < count; i++) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t off = (size_t)(p - base);
		last_offset = off;
		last_start = s;
		if (i % stride == 0) {
			__sm_assert(sb < n_sb);
			sb_start[sb] = (uint64_t)s;
			sb_offset[sb] = off;
			sb_prefix[sb] = running;
			sb++;
		}
		/* Accumulate this chunk's set-bit count into the running total
		 * so the NEXT superblock's prefix is correct. */
		const size_t cap = __sm_chunk_get_capacity(&chunk);
		__sm_chunk_rank_t rank;
		running += __sm_chunk_rank(&rank, true, &chunk, 0, cap - 1);

		const size_t next_off =
		    off + SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			break;
		}
		p = base + next_off;
	}

	loc->map = map;
	loc->count = count;
	loc->first_start = (uint64_t)sb_start[0];
	loc->last_start = (uint64_t)last_start;
	loc->last_offset = last_offset;
	loc->stride = stride;
	loc->n_sb = n_sb;
	loc->sb_start = sb_start;
	loc->sb_offset = sb_offset;
	loc->sb_prefix = sb_prefix;
	return (loc);
}

void
sm_locator_free(sm_locator_t *loc)
{
	if (loc == NULL) {
		return;
	}
	__sm_free(loc->sb_start);
	__sm_free(loc->sb_offset);
	__sm_free(loc->sb_prefix);
	__sm_free(loc);
}

/* Binary search sb_start[] for the largest superblock sb with
 * sb_start[sb] <= idx.  Returns 0 when idx precedes the first sample
 * (fine-walk from superblock 0 then still answers correctly). */
static size_t
__sm_locator_find_sb(const sm_locator_t *loc, uint64_t idx)
{
	size_t lo = 0, hi = loc->n_sb; /* [lo, hi) */
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (loc->sb_start[mid] <= idx) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return (lo == 0 ? 0 : lo - 1);
}

/* Set bits in [0, x] via the prefix table: jump to x's superblock, seed the
 * count with sb_prefix[sb] (set bits in every chunk before that superblock),
 * then fine-walk at most `stride` chunks -- from the superblock's first chunk
 * up to and including the chunk containing x -- adding each chunk's set bits
 * in its overlap with [0, x].  O(log n_sb + stride) = O(sqrt count).  Chunks
 * are window-aligned and ascending, so the superblock boundary never splits a
 * chunk and sb_prefix is exact. */
static size_t
__sm_locator_rank_upto(const sm_locator_t *loc, uint64_t x)
{
	const sm_t *map = loc->map;
	uint8_t *base = __sm_get_chunk_data(map, 0);
	const size_t stream_end =
	    (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;
	const size_t sb = __sm_locator_find_sb(loc, x);
	size_t set = loc->sb_prefix[sb];
	uint8_t *p = base + loc->sb_offset[sb];
	for (;;) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		if ((uint64_t)s > x) {
			break; /* chunk starts past x: nothing more to count */
		}
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t cap = __sm_chunk_get_capacity(&chunk);
		const uint64_t chunk_lo = (uint64_t)s;
		const uint64_t span = (uint64_t)cap - 1;
		const uint64_t chunk_hi_incl =
		    (chunk_lo > UINT64_MAX - span) ? UINT64_MAX
		                                  : chunk_lo + span;
		const uint64_t ov_hi_incl =
		    (x < chunk_hi_incl) ? x : chunk_hi_incl;
		const size_t to = (size_t)(ov_hi_incl - chunk_lo);
		__sm_chunk_rank_t rank;
		set += __sm_chunk_rank(&rank, true, &chunk, 0, to);
		const size_t next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			break;
		}
		p = base + next_off;
	}
	return (set);
}

bool
sm_locator_contains(const sm_locator_t *loc, uint64_t idx)
{
	if (__sm_locator_is_stale(loc)) {
		__sm_assert(false);
		return (sm_contains(loc ? loc->map : NULL, idx, NULL));
	}

	const sm_t *map = loc->map;
	uint8_t *base = __sm_get_chunk_data(map, 0);
	const size_t stream_end =
	    (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;

	size_t sb = __sm_locator_find_sb(loc, idx);
	uint8_t *p = base + loc->sb_offset[sb];

	/* Fine-walk at most `stride` chunks from the superblock's first
	 * chunk to the chunk covering idx (same shape as
	 * __sm_get_chunk_offset, but bounded). */
	for (;;) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		const size_t cap = __sm_chunk_get_capacity(&chunk);
		if (idx < (uint64_t)s) {
			return (false); /* gap before this chunk */
		}
		if (idx < (uint64_t)s + cap) {
			return (__sm_chunk_is_set(&chunk, idx - (uint64_t)s));
		}
		const size_t next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			return (false); /* past the last chunk */
		}
		p = base + next_off;
	}
}

size_t
sm_locator_rank(const sm_locator_t *loc, uint64_t lo, uint64_t hi, bool value)
{
	/* value=false and staleness both fall back to the plain path: the
	 * unset count needs the range width (which the sqrt index does not
	 * carry), and a stale index must never return a wrong answer. */
	if (value == false || __sm_locator_is_stale(loc)) {
		if (value == true) {
			__sm_assert(false);
		}
		return (sm_rank((sm_t *)(loc ? loc->map : NULL), lo, hi,
		    value));
	}
	if (lo > hi) {
		return (0);
	}
	/* Set bits in [lo, hi] = rank_upto(hi) - rank_upto(lo - 1).  Each
	 * rank_upto jumps straight to the target's superblock, seeds the
	 * count from sb_prefix[] (all set bits in chunks before that
	 * superblock -- the whole point of the prefix table), and fine-walks
	 * at most `stride` chunks from there.  That is the O(sqrt n) path;
	 * seeding from chunk 0 as the previous version did made this an
	 * O(chunks) no-op identical to plain sm_rank. */
	const size_t hi_cnt = __sm_locator_rank_upto(loc, hi);
	const size_t lo_cnt =
	    (lo == 0) ? 0 : __sm_locator_rank_upto(loc, lo - 1);
	return (hi_cnt - lo_cnt);
}

uint64_t
sm_locator_select(const sm_locator_t *loc, uint64_t n, bool value)
{
	/* value=false and staleness fall back to sm_select: unset select
	 * needs the leading-zeros / cross-chunk gap accounting the sqrt
	 * prefix does not carry, and a stale index must stay correct. */
	if (value == false || __sm_locator_is_stale(loc)) {
		if (value == true) {
			__sm_assert(false);
		}
		return (sm_select((sm_t *)(loc ? loc->map : NULL), n, value));
	}

	const sm_t *map = loc->map;
	uint8_t *base = __sm_get_chunk_data(map, 0);
	const size_t stream_end =
	    (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;

	/* Find the last superblock whose cumulative set-bit prefix is <= n,
	 * subtract that prefix, and fine-walk from its first chunk.  The
	 * per-chunk select semantics mirror sm_select exactly. */
	size_t sb = 0;
	{
		size_t l = 0, r = loc->n_sb; /* largest sb with prefix<=n */
		while (l < r) {
			const size_t mid = l + (r - l) / 2;
			if ((uint64_t)loc->sb_prefix[mid] <= n) {
				l = mid + 1;
			} else {
				r = mid;
			}
		}
		sb = (l == 0) ? 0 : l - 1;
	}

	ssize_t rem = (ssize_t)(n - (uint64_t)loc->sb_prefix[sb]);
	uint8_t *p = base + loc->sb_offset[sb];
	for (;;) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		ssize_t new_n = rem;
		const size_t index =
		    __sm_chunk_select(&chunk, rem, &new_n, value);
		if (new_n == -1) {
			return ((uint64_t)s + index);
		}
		rem = new_n;
		const size_t next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			return (SM_IDX_MAX);
		}
		p = base + next_off;
	}
}

/* -------------------------------------------------------------------
 * Idea 3: sm_cursor_cached_t -- fixed 8-way MRU chunk cache.
 * ------------------------------------------------------------------- */

bool
sm_contains_cached(const sm_t *map, uint64_t idx, sm_cursor_cached_t *cache)
{
	if (map == NULL) {
		return (false);
	}
	if (cache == NULL) {
		return (sm_contains(map, idx, NULL));
	}

	/* 1. Probe the <=8 valid ways for a covering chunk (a hit). */
	for (uint8_t w = 0; w < SM_CACHE_WAYS; w++) {
		if ((cache->valid & (uint8_t)(1u << w)) == 0) {
			continue;
		}
		if (idx >= cache->start_idx[w] && idx < cache->end_idx[w]) {
			uint8_t *p =
			    __sm_get_chunk_data(map, cache->offset[w]);
			__sm_chunk_t chunk;
			__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
			return (__sm_chunk_is_set(&chunk,
			    idx - cache->start_idx[w]));
		}
	}

	/* 2. Miss: walk from the head, then insert the located chunk. */
	const ssize_t offset = __sm_get_chunk_offset(map, idx, NULL);
	if (offset == -1) {
		return (false);
	}
	uint8_t *p = __sm_get_chunk_data(map, (size_t)offset);
	const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
	__sm_chunk_t chunk;
	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	const size_t cap = __sm_chunk_get_capacity(&chunk);

	/* 3. Insert (start, start+cap, offset) at the round-robin slot. */
	const uint8_t slot = cache->mru;
	cache->start_idx[slot] = (uint64_t)start;
	cache->end_idx[slot] = (uint64_t)start + cap;
	cache->offset[slot] = (size_t)offset;
	cache->valid |= (uint8_t)(1u << slot);
	cache->mru = (uint8_t)((slot + 1) % SM_CACHE_WAYS);

	/* Out of bounds of the located chunk -> not set (matches
	 * sm_contains). */
	if (idx < (uint64_t)start || idx - (uint64_t)start >= cap) {
		return (false);
	}
	return (__sm_chunk_is_set(&chunk, idx - (uint64_t)start));
}

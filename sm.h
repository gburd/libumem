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

/**
 * @file sparsemap.h
 * @brief A sparse, compressed bitmap with run-length encoding (RLE).
 *
 * Sparsemap is a mutable, resizable, compressed bitmap optimized for workloads
 * that contain long runs of consecutive set or unset bits.
 *
 * ## Architecture
 *
 * The implementation uses a 3-tier hierarchy:
 *
 * **Tier 0 (bit vectors):** Individual bits are stored in 64-bit words
 * (`uint64_t`).
 *
 * **Tier 1 (chunks):** Groups of bit vectors are managed by chunk maps.
 * Chunks use one of two internal encodings:
 *
 *   - **Sparse encoding:** A descriptor word holds 2-bit flags for up to 32
 *     bit vectors (2048 bits total).  Only vectors with a mix of set and unset
 *     bits are stored; uniform vectors (all-zero or all-one) are represented
 *     by their flag alone:
 *
 *         00  all zeros  -- vector not stored
 *         11  all ones   -- vector not stored
 *         10  mixed      -- vector stored after the descriptor
 *         01  unused     -- reduces chunk capacity
 *
 *   - **RLE encoding:** A single 64-bit descriptor represents a contiguous
 *     run of set bits starting at index 0 within the chunk:
 *
 *         Bits 63:62 = 01  (RLE flag)
 *         Bits 61:31       chunk capacity in bits  (max ~2 billion)
 *         Bits 30:0        run length in bits      (max ~2 billion)
 *
 *     Bits [0, length) are set; bits [length, capacity) are unset.
 *
 * **Tier 2 (map):** The top-level sparsemap manages an ordered sequence of
 * chunks, each tagged with a 4-byte starting offset.  The map grows and
 * shrinks the underlying byte buffer as chunks are added or removed.
 *
 * ## Encoding transitions
 *
 * - A sparse chunk whose vectors are all ones (2048 consecutive set bits)
 *   transitions to RLE when the next adjacent bit is set, extending the run
 *   beyond 2048.
 * - Modifying bits inside an RLE run (clearing a bit in the middle, for
 *   example) causes the RLE chunk to be separated back into one or more
 *   sparse chunks plus (optionally) smaller RLE chunks for the remaining
 *   contiguous runs.
 * - Adjacent chunks (sparse or RLE) that form a contiguous run of set bits
 *   are coalesced into a single RLE chunk automatically.
 *
 * ## Thread safety
 *
 * Sparsemap is **not** thread-safe.  Concurrent reads are safe only when no
 * writer is active.  All mutating operations must be externally synchronized.
 *
 * ## Error handling
 *
 * Functions that mutate the map return `SM_IDX_MAX` and set `errno` to
 * `ENOSPC` when the backing buffer is full.  The caller can grow the buffer
 * with sm_set_data_size() and retry.
 *
 * Allocation functions (sm_create(), sm_copy(),
 * sm_owned_copy(), sm_wrap()) return `NULL` on allocation
 * failure.
 *
 * ## Allocation lineage and disposal
 *
 * Every sm_t has an internal allocation lineage tag that determines
 * which functions may safely realloc its data buffer and how it must be
 * disposed.  The lineage is set by the constructor:
 *
 * | Constructor              | Lineage              | Disposal                              |
 * |--------------------------|----------------------|----------------------------------------|
 * | sparsemap()              | owned-contiguous     | sm_free() *or* libc free()      |
 * | sm_create()       | owned-contiguous     | sm_free() *or* libc free()      |
 * | sm_copy()         | owned-contiguous     | sm_free() *or* libc free()      |
 * | sm_owned_copy()   | owned-contiguous     | sm_free() *or* libc free()      |
 * | sm_wrap()         | wrapped              | sm_free() (caller frees buffer) |
 * | sm_init()         | wrapped              | (caller-allocated; free both manually) |
 * | sm_open()         | wrapped              | (caller-allocated; free both manually) |
 *
 * ### The wrap-and-grow case
 *
 * Calling sm_set_data_size(map, NULL, new_size) on a wrapped map with
 * `new_size > capacity` transparently promotes the map: a new library-owned
 * buffer is allocated, the in-use prefix is copied into it, m_data is
 * redirected, and the lineage transitions to owned-split.  The caller's
 * original buffer is left untouched and remains theirs.  The promoted map
 * **must** be disposed with sm_free() because libc free() can no
 * longer dispose both the struct and the separately-allocated buffer.
 *
 * Shrinking a wrapped map (size <= capacity) does not promote: m_capacity
 * is updated in place, and the caller's buffer remains theirs.
 *
 * ### When in doubt, normalize
 *
 * sm_owned_copy() returns a guaranteed owned-contiguous copy of any
 * sparsemap.  Use it when you have a map whose lineage you don't trust or
 * whose lifetime is intertwined with someone else's: the result is
 * self-contained, growable, and disposable with sm_free() or libc
 * free().
 */
#ifndef SPARSEMAP_H
#define SPARSEMAP_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if !defined(_MSC_VER)
#include <sys/types.h>
#endif

/*
 * Compiler-portability shims.  The library is written in GNU C style
 * (bare __attribute__, __builtin_*, POSIX ssize_t); these macros give
 * MSVC (and any non-GNU compiler) a working spelling or a no-op.  The
 * bit intrinsics (SM_POPCOUNT64 / SM_CTZ64 / SM_CLZ64 / SM_PREFETCH)
 * have their own guarded blocks in sm.c.
 *
 * SM_ALIGNED(n) attaches to a struct/typedef; SM_UNALIGNED qualifies a
 * pointer target for byte-granular access.  Each may be overridden by
 * a consumer with -DSM_ALIGNED=... before including this header.
 */
#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#ifndef SM_ALIGNED
#if defined(__GNUC__) || defined(__clang__)
#define SM_ALIGNED(n) __attribute__((aligned(n)))
#elif defined(_MSC_VER)
#define SM_ALIGNED(n) __declspec(align(n))
#else
#define SM_ALIGNED(n)
#endif
#endif /* SM_ALIGNED */

/*
 * SM_ALIGNAS(t): align a declaration to alignof(t).  Used on local
 * uint8_t[] scratch buffers that a chunk descriptor is built inside,
 * so the 8-byte descriptor words land aligned.  (Access is
 * unaligned-safe regardless; this is defense-in-depth.)  Every use in
 * the library aligns to __sm_bitvec_t, i.e. 8 bytes.
 */
#ifndef SM_ALIGNAS
#if defined(__GNUC__) || defined(__clang__)
#define SM_ALIGNAS(t) _Alignas(t)
#elif defined(_MSC_VER)
#define SM_ALIGNAS(t) __declspec(align(8))
#else
#define SM_ALIGNAS(t)
#endif
#endif /* SM_ALIGNAS */

/*
 * Symbol prefixing for embedding (Berkeley DB --with-uniquename
 * style).  A program that vendors sparsemap into a larger library
 * can rename every public symbol by defining SPARSEMAP_PREFIX before
 * including <sm.h>:
 *
 *	#define SPARSEMAP_PREFIX	myapp_
 *	#include <sm.h>
 *
 * turns sm_create() into myapp_sm_create(), sm_t into
 * myapp_sparsemap_t, and so on, at both declaration and call sites,
 * so two independently-vendored copies of sparsemap can coexist in
 * one address space without colliding at link time.  Only C
 * identifiers that become linker symbols (the public functions) and
 * the public type names are renamed; compile-time macros
 * (SM_IDX_MAX, the SM_VERSION_* values, enum constants) are
 * unaffected because they never reach the linker.  The serialized
 * wire format does not change.
 */
#ifdef SPARSEMAP_PREFIX
#define SM__CAT2(a, b) a##b
#define SM__CAT(a, b)  SM__CAT2(a, b)
#define SM__P(name)    SM__CAT(SPARSEMAP_PREFIX, name)

/* Public types (and the struct tag / deprecated noun constructor). */
#define sparsemap            SM__P(sparsemap)
#define sm_t                 SM__P(sm_t)
#define sm_allocator_t       SM__P(sm_allocator_t)
#define sm_cursor_t          SM__P(sm_cursor_t)
#define sm_membership_t      SM__P(sm_membership_t)
#define sm_stats_t           SM__P(sm_stats_t)
#define sm_subset_relation_t SM__P(sm_subset_relation_t)

/* Public functions. */
#define sm_add                      SM__P(sm_add)
#define sm_add_grow                 SM__P(sm_add_grow)
#define sm_add_grow_cursor          SM__P(sm_add_grow_cursor)
#define sm_add_many                 SM__P(sm_add_many)
#define sm_add_many_grow            SM__P(sm_add_many_grow)
#define sm_add_range                SM__P(sm_add_range)
#define sm_and                      SM__P(sm_and)
#define sm_andnot                   SM__P(sm_andnot)
#define sm_assign                   SM__P(sm_assign)
#define sm_capacity_remaining       SM__P(sm_capacity_remaining)
#define sm_cardinality              SM__P(sm_cardinality)
#define sm_clear                    SM__P(sm_clear)
#define sm_compare                  SM__P(sm_compare)
#define sm_contains                 SM__P(sm_contains)
#define sm_contains_cached          SM__P(sm_contains_cached)
#define sm_contains_many            SM__P(sm_contains_many)
#define sm_copy                     SM__P(sm_copy)
#define sm_create                   SM__P(sm_create)
#define sm_create_from_array        SM__P(sm_create_from_array)
#define sm_create_from_range        SM__P(sm_create_from_range)
#define sm_create_singleton         SM__P(sm_create_singleton)
#define sm_deserialize              SM__P(sm_deserialize)
#define sm_difference               SM__P(sm_difference)
#define sm_difference_cardinality   SM__P(sm_difference_cardinality)
#define sm_difference_inplace       SM__P(sm_difference_inplace)
#define sm_equals                   SM__P(sm_equals)
#define sm_extract_range            SM__P(sm_extract_range)
#define sm_fill_factor              SM__P(sm_fill_factor)
#define sm_flip_range               SM__P(sm_flip_range)
#define sm_free                     SM__P(sm_free)
#define sm_get_capacity             SM__P(sm_get_capacity)
#define sm_get_data                 SM__P(sm_get_data)
#define sm_get_size                 SM__P(sm_get_size)
#define sm_hash                     SM__P(sm_hash)
#define sm_init                     SM__P(sm_init)
#define sm_intersection             SM__P(sm_intersection)
#define sm_intersection_cardinality SM__P(sm_intersection_cardinality)
#define sm_intersection_inplace     SM__P(sm_intersection_inplace)
#define sm_is_empty                 SM__P(sm_is_empty)
#define sm_is_subset                SM__P(sm_is_subset)
#define sm_is_superset              SM__P(sm_is_superset)
#define sm_jaccard_index            SM__P(sm_jaccard_index)
#define sm_locator_build            SM__P(sm_locator_build)
#define sm_locator_contains         SM__P(sm_locator_contains)
#define sm_locator_free             SM__P(sm_locator_free)
#define sm_locator_rank             SM__P(sm_locator_rank)
#define sm_locator_select           SM__P(sm_locator_select)
#define sm_locator_t                SM__P(sm_locator_t)
#define sm_cursor_cached_t          SM__P(sm_cursor_cached_t)
#define sm_maximum                  SM__P(sm_maximum)
#define sm_membership               SM__P(sm_membership)
#define sm_minimum                  SM__P(sm_minimum)
#define sm_next_member              SM__P(sm_next_member)
#define sm_nonempty_difference      SM__P(sm_nonempty_difference)
#define sm_offset                   SM__P(sm_offset)
#define sm_open                     SM__P(sm_open)
#define sm_open_copy                SM__P(sm_open_copy)
#define sm_or                       SM__P(sm_or)
#define sm_overlap                  SM__P(sm_overlap)
#define sm_owned_copy               SM__P(sm_owned_copy)
#define sm_pop_first                SM__P(sm_pop_first)
#define sm_pop_last                 SM__P(sm_pop_last)
#define sm_prev_member              SM__P(sm_prev_member)
#define sm_rank                     SM__P(sm_rank)
#define sm_remove                   SM__P(sm_remove)
#define sm_remove_range             SM__P(sm_remove_range)
#define sm_scan                     SM__P(sm_scan)
#define sm_select                   SM__P(sm_select)
#define sm_serialize                SM__P(sm_serialize)
#define sm_serialized_size          SM__P(sm_serialized_size)
#define sm_set_allocator            SM__P(sm_set_allocator)
#define sm_set_data_size            SM__P(sm_set_data_size)
#define sm_shrink_to_fit            SM__P(sm_shrink_to_fit)
#define sm_singleton_member         SM__P(sm_singleton_member)
#define sm_span                     SM__P(sm_span)
#define sm_split                    SM__P(sm_split)
#define sm_statistics               SM__P(sm_statistics)
#define sm_subset_compare           SM__P(sm_subset_compare)
#define sm_to_array                 SM__P(sm_to_array)
#define sm_union                    SM__P(sm_union)
#define sm_union_cardinality        SM__P(sm_union_cardinality)
#define sm_union_inplace            SM__P(sm_union_inplace)
#define sm_validate                 SM__P(sm_validate)
#define sm_wrap                     SM__P(sm_wrap)
#define sm_xor                      SM__P(sm_xor)
#define sm_xor_cardinality          SM__P(sm_xor_cardinality)
#endif /* SPARSEMAP_PREFIX */

#if defined(__cplusplus)
extern "C" {
#endif

/** Library version (kept in sync with meson.build's project(version: ...)). */
#define SM_VERSION_STRING "5.4.0"
#define SM_VERSION_MAJOR  5
#define SM_VERSION_MINOR  4
#define SM_VERSION_PATCH  0

/** Handle to a sparsemap instance.
 *
 * By default this is an opaque type: allocate with sm_create() /
 * sm_wrap() and access only through the sm_* functions.  The struct
 * layout is private and may change between releases (the byte
 * capacity, used-byte count, and a lineage tag, none of which are
 * part of the serialized wire format).
 *
 * Define SM_EXPOSE_STRUCT before including <sm.h> to make the full
 * struct definition visible -- needed only to embed an sm_t by value
 * inside another structure (for example a shared-memory control
 * block).  Doing so opts out of the ABI-opacity guarantee: code that
 * embeds sm_t by value must be recompiled whenever the struct layout
 * changes.  The serialized format is unaffected either way.
 */
typedef struct sparsemap sm_t;

/** @brief Custom allocator hooks (process-global, CRoaring-style).
 *
 * Sparsemap allocates memory at construction time (sm_create /
 * sm_wrap / sm_owned_copy / sm_union / ...), at grow time
 * (sm_set_data_size, sm_*_inplace, sm_*_grow), and at free time.
 *
 * Embedders that need to route those allocations through a custom
 * allocator (PostgreSQL's palloc / pfree, an arena, a tracking
 * wrapper) install one process-wide with sm_set_allocator(), exactly
 * as CRoaring's roaring_init_memory_hook does.  There is no per-map
 * allocator: the hooks are global, and no allocator state is stored
 * in sm_t (keeping it to three words).
 *
 * Contract for the hook implementations:
 *
 *   - malloc(n): return at least `n` bytes of uninitialized memory,
 *     or NULL on failure.
 *   - realloc(p, n): grow or shrink an existing allocation; return
 *     the (possibly relocated) pointer or NULL on failure.  p == NULL
 *     behaves like malloc(n).
 *   - free(p): release an allocation made by malloc/realloc.  Must
 *     accept p == NULL as a no-op.
 *
 * Any individual function pointer may be NULL; sparsemap falls back
 * to the libc equivalent for that operation.  An all-zero struct
 * therefore means "use libc throughout", which is the default.
 */
typedef struct sm_allocator {
	void *(*malloc)(size_t n);
	void *(*realloc)(void *p, size_t n);
	void (*free)(void *p);
} sm_allocator_t;

/** @brief Set the process-wide allocator hooks.
 *
 * Affects every allocation sparsemap makes after the call.  Pass an
 * all-zero struct (e.g. `(sm_allocator_t){0}`) to reset to libc
 * malloc/realloc/free.  Not thread-safe; intended for one-shot
 * library initialization before any map is created, exactly like
 * CRoaring's roaring_init_memory_hook.  The struct is copied into a
 * file-static, so the caller's copy can go out of scope safely.
 */
void sm_set_allocator(sm_allocator_t a);

/*
 * Full struct definition.  Visible to the library's own translation
 * unit (which defines SM_INTERNAL before including this header) and
 * to consumers that explicitly opt in with SM_EXPOSE_STRUCT.  Kept in
 * one place so the embedded-copy use case cannot drift from the
 * library's own definition.
 *
 * sm_t is three machine words.  The allocation-lineage tag (how
 * m_data was provisioned) is folded into the low bits of m_capacity:
 * capacity is always rounded up to an 8-byte boundary, so its low 3
 * bits are free.  Use the internal __sm_cap() / __sm_kind() accessors
 * rather than touching m_capacity directly.  There is no per-map
 * allocator and no stored cursor; reads accelerate through a
 * caller-owned sm_cursor_t (see below).  Nothing here is serialized.
 */
#if defined(SM_INTERNAL) || defined(SM_EXPOSE_STRUCT)
struct SM_ALIGNED(8) sparsemap {
	size_t m_capacity;  /* (capacity & ~7) bytes; low 3 bits = lineage */
	size_t m_data_used; /* used size of m_data, in bytes */
	uint8_t *m_data;    /* the serialized bitmap data */
};
#endif

/** @brief Caller-owned read cursor for accelerated sequential lookups.
 *
 * A cursor caches the most-recently-located chunk so a SEQUENCE of
 * monotonically NON-DECREASING idx lookups on an UNMUTATED map resumes
 * the chunk walk from there instead of from chunk 0, turning an
 * otherwise O(N^2) scan into O(N).  Only sm_contains(), sm_next_member(),
 * and sm_prev_member() accept one.
 *
 * Contract:
 *   - Initialize with `sm_cursor_t c = SM_CURSOR_INIT;` before the loop.
 *   - Pass `&c` to consecutive lookups with non-decreasing idx.
 *   - ANY mutation of the map (sm_add / sm_remove / sm_assign /
 *     sm_set_data_size / sm_clear / sm_split / the *_inplace ops / ...)
 *     between two cursor uses invalidates the cursor.  Using a stale
 *     cursor afterwards is undefined behavior; reset it with
 *     SM_CURSOR_INIT.
 *   - Do not share one cursor across a mutation or across maps.
 *   - Passing NULL means "no acceleration" (walk from chunk 0); always
 *     safe.
 */
typedef struct sm_cursor {
	size_t offset;      /* byte offset of cached chunk; SIZE_MAX = invalid */
	uint64_t start_idx; /* cached chunk's start bit */
	size_t prev_offset; /* byte offset of the chunk immediately BEFORE
	                     * `offset`, or SIZE_MAX; a free left-neighbor
	                     * hint captured during the forward walk and used
	                     * to skip a head-walk in the coalescing path */
} sm_cursor_t;
#define SM_CURSOR_INIT { (size_t)-1, 0, (size_t)-1 }

/** @brief Caller-owned fixed 8-way MRU chunk cache for point lookups.
 *
 * Unlike sm_cursor_t (which caches ONE chunk and only helps a
 * monotonically non-decreasing scan), this caches the last
 * SM_CACHE_WAYS distinct chunks located, so a workload with a few hot
 * chunks probed in ANY order (clustered but not sorted) resolves most
 * lookups from the cache instead of walking from chunk 0.  The cache
 * is fixed-size and independent of the map's chunk count.
 *
 * Contract (identical to sm_cursor_t):
 *   - Initialize with `sm_cursor_cached_t c = SM_CURSOR_CACHED_INIT;`.
 *   - Pass `&c` to consecutive sm_contains_cached() calls.
 *   - ANY mutation of the map invalidates the cache; reset it with
 *     SM_CURSOR_CACHED_INIT before reusing.  A stale cache is undefined
 *     behavior (it caches byte offsets that a mutation can move).
 *   - Passing NULL means "no cache" (plain sm_contains); always safe.
 */
#define SM_CACHE_WAYS 8
typedef struct sm_cursor_cached {
	uint64_t start_idx[SM_CACHE_WAYS]; /* cached chunk start bits */
	uint64_t end_idx[SM_CACHE_WAYS];   /* start + capacity (exclusive top) */
	size_t offset[SM_CACHE_WAYS];      /* base-relative byte offset */
	uint8_t mru;                       /* next round-robin slot */
	uint8_t valid;                     /* bitmask of populated ways */
} sm_cursor_cached_t;
#define SM_CURSOR_CACHED_INIT { {0}, {0}, {0}, 0, 0 }

/** @brief Transient two-level sqrt(n) directory over a map's chunks.
 *
 * A locator is a caller-owned, read-only acceleration index built once
 * over an UNMUTATED map with sm_locator_build().  It samples every
 * stride-th chunk (stride ~= sqrt(chunk count)) into a superblock
 * directory, so contains / rank / select run in O(sqrt n) instead of
 * O(n) chunks.  Nothing here is serialized and sm_t is unchanged.
 *
 * Contract:
 *   - Build with sm_locator_build(map); free with sm_locator_free().
 *   - ANY mutation of the map invalidates the locator: REBUILD it.
 *   - A stale locator still returns CORRECT results (it detects the
 *     mismatch and falls back to the plain O(n) sm_contains / sm_rank /
 *     sm_select path) but loses the speedup.
 *   - rank/select for value == false fall back to the plain path
 *     (correct, not accelerated); the sqrt speedup is for value==true.
 *
 * The struct layout is exposed only so the type name resolves; treat
 * it as opaque and use it only through the sm_locator_* functions.
 */
typedef struct sm_locator {
	const sm_t *map;      /* the map this locator indexes */
	size_t count;         /* chunk count at build time */
	uint64_t first_start; /* first chunk start (staleness fingerprint) */
	uint64_t last_start;  /* last chunk start (staleness fingerprint) */
	size_t last_offset;   /* base-relative offset of the last chunk */
	size_t stride;        /* chunks per superblock (~sqrt(count)) */
	size_t n_sb;          /* number of superblock entries */
	uint64_t *sb_start;   /* [n_sb] start index of each stride-th chunk */
	size_t *sb_offset;    /* [n_sb] base-relative byte offset of it */
	size_t *sb_prefix;    /* [n_sb] cumulative SET bits BEFORE it */
} sm_locator_t;

/** Sentinel value returned when a lookup finds no matching bit. */
#define SM_IDX_MAX UINT64_MAX

/** Evaluates to true when \a x represents a valid (found) index. */
#define SM_FOUND(x) ((x) != SM_IDX_MAX)

/** Evaluates to true when \a x represents the not-found sentinel. */
#define SM_NOT_FOUND(x) ((x) == SM_IDX_MAX)

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/** @brief Allocate a heap-managed sparsemap with an internal buffer.
 *
 * Both the sm_t struct and its data buffer are allocated in a single
 * heap block.  Dispose with sm_free() (or libc free() for backward
 * compatibility, which is equivalent for this lineage).  The buffer can
 * later be grown via sm_set_data_size(map, NULL, new_size).
 *
 * @param[in] size  Buffer size in bytes (0 selects a 1024-byte default).
 * @returns A new sparsemap, or NULL on allocation failure.
 *
 * Example:
 * @code
 *   sm_t *map = sm_create(4096);
 *   sm_add(map, 42);
 *   assert(sm_contains(map, 42, NULL));
 *   sm_free(map);
 * @endcode
 */
sm_t *sm_create(size_t size);

/** @brief Deprecated alias for sm_create().
 *
 * Older callers used the noun-named sparsemap() constructor.  New code
 * should prefer the verb-named sm_create().  Retained for source
 * compatibility with existing consumers; slated for removal in a
 * future major release.
 */
sm_t *sparsemap(size_t size);

/** @brief Dispose of a sparsemap, regardless of allocation lineage.
 *
 * Frees both the struct and any library-owned data buffer.  For maps
 * created via sm_wrap(), sm_init(), or sm_open(),
 * the caller's data buffer is left untouched (the library does not own
 * it and never frees it).
 *
 * Calling sm_free(NULL) is a no-op.
 *
 * Note: maps allocated via sm_create() / sparsemap() are
 * historically disposable with libc free() because the struct and buffer
 * occupy a single allocation.  sm_free() works in that case too
 * and is the recommended call going forward because it also handles the
 * SM_OWNED_SPLIT lineage (used after a wrap-and-grow sequence).
 *
 * @param[in,out] map  The sparsemap to dispose, or NULL.
 */
void sm_free(sm_t *map);

/** @brief Create a deep copy of \a other.
 *
 * @param[in] other  The sparsemap to copy.  Must not be NULL.
 * @returns A new sparsemap with the same contents, or NULL on failure.
 */
sm_t *sm_copy(const sm_t *other);

/** @brief Return a guaranteed-owned, guaranteed-growable copy of any sparsemap.
 *
 * Regardless of \a map's allocation lineage, the result is allocated as
 * SM_OWNED_CONTIGUOUS (single calloc, struct + buffer in one block).  The
 * result can be safely grown via sm_set_data_size(NULL, ...) and
 * disposed with sm_free() or libc free().
 *
 * Use this when you have a sparsemap from somewhere (a library that hands
 * you a wrap'd map, a deserialized buffer, an aggregate of mixed lineages)
 * and you need a self-contained, modifiable copy.
 *
 * @param[in] map  The sparsemap to copy.  Must not be NULL.
 * @returns A new owned-contiguous sparsemap, or NULL on allocation failure.
 */
sm_t *sm_owned_copy(const sm_t *map);

/** @brief Allocate a sm_t that wraps a caller-provided buffer.
 *
 * The sm_t struct is heap-allocated, but the data buffer is owned by
 * the caller.  Dispose with sm_free() (which frees the struct only)
 * or with libc free() (equivalent).
 *
 * **Alignment requirement:** \a data must be aligned to at least 8 bytes
 * (the alignment of `uint64_t`).  Stack arrays should declare
 * `_Alignas(uint64_t) uint8_t buf[N];`; heap allocations from `malloc()`
 * are always sufficiently aligned.  On x86_64 / aarch64 / standard RISC-V
 * a misaligned buffer will work but with a perf penalty; on strict-
 * alignment cpus (ARMv5, some embedded) it will trap.
 *
 * Resizing via sm_set_data_size(map, NULL, larger) on a wrapped map
 * is supported: the library transparently allocates a fresh internal
 * buffer, copies the in-use prefix into it, and transitions the map's
 * lineage to owned-split.  The caller's original buffer is left untouched
 * and remains theirs to free.  The resulting map MUST be disposed with
 * sm_free() (libc free() will leak the new buffer).
 *
 * @param[in] data  Buffer for bitmap storage (stack or heap), 8-byte aligned.
 * @param[in] size  Size of \a data in bytes.
 * @returns A new sparsemap, or NULL on allocation failure.
 */
sm_t *sm_wrap(uint8_t *data, size_t size);

/** @brief Initialize a caller-allocated sm_t with a buffer.
 *
 * Use this when both the sm_t and its buffer are allocated by the
 * caller (e.g. on the stack).  The map is cleared to an empty state.
 *
 * @param[in,out] map   Pointer to an uninitialized sm_t.
 * @param[in]     data  Buffer for bitmap storage.
 * @param[in]     size  Size of \a data in bytes.
 *
 * Example:
 * @code
 *   sm_t map;
 *   uint8_t buf[1024];
 *   sm_init(&map, buf, sizeof(buf));
 *   sm_add(&map, 0);
 * @endcode
 */
void sm_init(sm_t *map, uint8_t *data, size_t size);

/** @brief Attach to an existing (serialized) sparsemap buffer.
 *
 * Unlike sm_init(), this does not clear the buffer.  It calculates the
 * used size from the buffer contents, making it suitable for deserializing a
 * previously-populated bitmap.
 *
 * @param[in,out] map   Pointer to an uninitialized sm_t.
 * @param[in]     data  Buffer containing serialized bitmap data.
 * @param[in]     size  Total capacity of \a data in bytes.
 */
void sm_open(sm_t *map, uint8_t *data, size_t size);

/** @brief Allocate a fresh map and deserialize raw on-disk bytes into it.
 *
 * Convenience for the common pattern:
 *
 *     sm_t *m = sm_create(n + slack);
 *     memcpy(sm_get_data(m), data, n);
 *     sm_open(m, sm_get_data(m), n + slack);
 *     // lineage ends up SM_WRAPPED; restore SM_OWNED_CONTIGUOUS
 *     // because the buffer is in fact contiguous with the struct.
 *
 * Returns an SM_OWNED_CONTIGUOUS map of capacity `n + slack` whose
 * first `n` bytes are a copy of `data`.  `slack` is grow-room for
 * subsequent insertions; pass 0 if you only intend to read from the
 * result.
 *
 * @param[in] data   Pointer to serialized bytes.
 * @param[in] n      Number of valid bytes at `data`.
 * @param[in] slack  Extra capacity bytes to allocate beyond `n`.
 * @returns A new owned-contiguous sparsemap, or NULL on alloc failure.
 */
sm_t *sm_open_copy(const uint8_t *data, size_t n, size_t slack);

/** @brief Reset the map to empty without freeing memory.
 *
 * After this call the map contains zero set bits but retains its buffer.
 *
 * @param[in,out] map  The sparsemap to clear.
 */
void sm_clear(sm_t *map);

/** @brief Resize the data buffer.
 *
 * Behaviour depends on \a data and the map's allocation lineage:
 *
 *   sm_set_data_size(map, NULL, new_size) -- library-managed
 *     resize.  Always succeeds (returning a possibly-relocated map
 *     pointer) or returns NULL on allocation failure.  Never silently
 *     no-ops.
 *
 *     For owned-contiguous maps the call may relocate the entire
 *     struct+buffer block; the caller MUST update all references to
 *     the returned pointer.
 *
 *     For owned-split maps only the data buffer is realloc'd; the
 *     struct address is stable.
 *
 *     For wrapped maps the result depends on direction:
 *       - new_size <= current capacity: m_capacity is updated in place,
 *         the caller's buffer is unchanged.
 *       - new_size >  current capacity: a new library-owned buffer is
 *         allocated and the in-use prefix copied into it.  Lineage
 *         transitions to owned-split, and the result MUST be disposed
 *         with sm_free().  The caller's original buffer is
 *         untouched.
 *
 *   sm_set_data_size(map, data, new_size) -- caller-supplied
 *     buffer.  The map is re-pointed to \a data.  The caller is
 *     responsible for copying any existing bits before the call.
 *     Lineage transitions to wrapped: the library will not realloc or
 *     free \a data on the caller's behalf.
 *
 * @param[in,out] map   The sparsemap to resize.  Must not be NULL.
 * @param[in]     data  New buffer, or NULL to let the library decide.
 * @param[in]     size  New buffer size in bytes.
 * @returns The (possibly relocated) sparsemap pointer on success,
 *          or NULL on allocation failure.
 */
sm_t *sm_set_data_size(sm_t *map, uint8_t *data, size_t size);

/* -------------------------------------------------------------------
 * Capacity and size
 * ------------------------------------------------------------------- */

/** @brief Estimate remaining buffer capacity as a percentage.
 *
 * Returns a value in [0.0, 100.0].  Because compression ratios change as
 * bits are added or removed, this estimate is non-monotonic -- it may
 * increase after a set() or decrease after an unset().
 *
 * @param[in] map  The sparsemap to query.
 * @returns Estimated percentage of unused buffer space.
 */
double sm_capacity_remaining(const sm_t *map);

/** @brief Return the total buffer capacity in bytes.
 *
 * This is the \a size value provided at construction, not the number of
 * indexable bits.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Buffer capacity in bytes.
 */
size_t sm_get_capacity(const sm_t *map);

/** @brief Return the number of buffer bytes currently in use.
 *
 * Useful for serialization: only the first sm_get_size() bytes of the
 * buffer returned by sm_get_data() need to be persisted.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Used byte count.
 */
size_t sm_get_size(sm_t *map);

/** @brief Return a pointer to the raw data buffer.
 *
 * The first sm_get_size() bytes contain the serialized bitmap; the
 * remainder (up to sm_get_capacity()) is unused.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Pointer to the data buffer.
 */
void *sm_get_data(const sm_t *map);

/* -------------------------------------------------------------------
 * Single-bit operations
 * ------------------------------------------------------------------- */

/** @brief Test whether the bit at \a idx is set.
 *
 * @param[in] map  The sparsemap to query.
 * @param[in] idx  0-based bit position.
 * @param[in] cur  Optional read cursor for sequential acceleration, or
 *                 NULL.  See sm_cursor_t.
 * @returns true if bit \a idx is 1, false if 0 or out of range.
 */
bool sm_contains(const sm_t *map, uint64_t idx, sm_cursor_t *cur);

/** @brief Test many bits in one left-to-right sweep (batched).
 *
 * Equivalent to calling sm_contains(map, idxs[i], NULL) for every i,
 * but done in a single O(chunks + n) pass instead of n independent
 * head-walks.
 *
 * @param[in]  map      The sparsemap to query (NULL -> all false).
 * @param[in]  idxs     Query indices, MUST be sorted ascending.
 * @param[out] results  results[i] receives membership of idxs[i].
 * @param[in]  n        Number of queries.
 *
 * The ascending-order requirement is a hard precondition (debug-asserted
 * under SPARSEMAP_DIAGNOSTIC); unsorted input yields unspecified but
 * memory-safe results.
 */
void sm_contains_many(const sm_t *map, const uint64_t *idxs, bool *results,
    size_t n);

/** @brief Test a bit using a caller-owned 8-way MRU chunk cache.
 *
 * Returns the same value as sm_contains(map, idx, NULL); the cache
 * accelerates repeated lookups into a small working set of chunks
 * probed in any order.  Pass NULL for \a cache to fall back to plain
 * sm_contains.  See sm_cursor_cached_t for the invalidation contract.
 */
bool sm_contains_cached(const sm_t *map, uint64_t idx,
    sm_cursor_cached_t *cache);

/** @brief Set or clear the bit at \a idx.
 *
 * Equivalent to `value ? sm_add(map, idx) : sm_remove(map, idx)`.
 *
 * @param[in,out] map    The sparsemap to modify.
 * @param[in]     idx    0-based bit position.
 * @param[in]     value  true to set, false to clear.
 * @returns \a idx on success, or SM_IDX_MAX with errno=ENOSPC.
 *
 * Example:
 * @code
 *   sm_assign(map, 100, true);   // set bit 100
 *   sm_assign(map, 100, false);  // clear bit 100
 * @endcode
 */
uint64_t sm_assign(sm_t *map, uint64_t idx, bool value);

/** @brief Set the bit at \a idx to 1.
 *
 * If the buffer is full, returns SM_IDX_MAX and sets errno to ENOSPC.
 * Grow the buffer with sm_set_data_size() and retry.
 *
 * Setting a bit may trigger chunk coalescing: if the new bit extends a
 * contiguous run of set bits across chunk boundaries, adjacent chunks may be
 * merged into a single RLE chunk.
 *
 * Note: sm_add takes no read cursor, so building a map by calling it in
 * an ascending loop is O(N^2) (each call rewalks the chunk list).
 * Callers building a map from many indices should use sm_add_many() /
 * sm_add_many_grow(), which thread an internal cursor for O(N)
 * construction.
 *
 * @param[in,out] map  The sparsemap to modify.
 * @param[in]     idx  0-based bit position to set.
 * @returns \a idx on success, or SM_IDX_MAX with errno=ENOSPC.
 *
 * Example:
 * @code
 *   uint64_t r = sm_add(map, 42);
 *   if (SM_NOT_FOUND(r)) {
 *       map = sm_set_data_size(map, NULL, new_size);
 *       sm_add(map, 42);
 *   }
 * @endcode
 */
uint64_t sm_add(sm_t *map, uint64_t idx);

/** @brief Add a bit, growing the map's buffer geometrically if needed.
 *
 * Convenience for the common pattern:
 *
 *     uint64_t rc = sm_add(m, idx);
 *     if (rc == SM_IDX_MAX) {
 *         sm_t *grown = sm_set_data_size(m, NULL,
 *                                                sm_get_capacity(m) * 2);
 *         if (!grown) { sm_free(m); return NULL; }
 *         m = grown;
 *         rc = sm_add(m, idx);
 *     }
 *
 * On ENOSPC, doubles the buffer (with a 4 KiB floor) and retries.
 * If the grow succeeds but the retry still ENOSPCs, returns
 * SM_IDX_MAX and leaves *map valid (and possibly grown).
 *
 * @param[in,out] map  Pointer to the map pointer.  Updated to the
 *                     possibly-relocated map after a grow.
 * @param[in]     idx  Bit to set.
 * @returns idx on success, or SM_IDX_MAX on allocation failure.
 */
uint64_t sm_add_grow(sm_t **map, uint64_t idx);

/** @brief Like sm_add_grow(), but threads a caller-owned cursor.
 *
 * Identical growth semantics to sm_add_grow(), but the point insert is
 * routed through the cursor-accelerated path so that an ascending
 * (append-pattern) bulk build stays O(N) instead of O(N log N): the
 * cursor caches the tail chunk between calls.  The cursor must be
 * initialized to SM_CURSOR_INIT before the first call and threaded
 * unchanged through the loop.
 *
 * When a grow relocates the buffer the cursor's cached byte offset is
 * stale, so this function resets @a cur to SM_CURSOR_INIT after a
 * successful grow before retrying; callers need not do anything.
 *
 * @param[in,out] map  Pointer to the map pointer.  Updated to the
 *                     possibly-relocated map after a grow.
 * @param[in]     idx  Bit to set.
 * @param[in,out] cur  Caller-owned cursor (SM_CURSOR_INIT before the
 *                     first call).  May be NULL to opt out of the
 *                     acceleration.
 * @returns idx on success, or SM_IDX_MAX on allocation failure.
 */
uint64_t sm_add_grow_cursor(sm_t **map, uint64_t idx, sm_cursor_t *cur);

/** @brief Clear the bit at \a idx (set to 0).
 *
 * Clearing a bit inside an RLE run causes the RLE chunk to be separated
 * into sparse and/or smaller RLE chunks.  This may temporarily increase
 * buffer usage even though a bit was removed.
 *
 * If the buffer is full (insufficient space for the new chunk layout),
 * returns SM_IDX_MAX and sets errno to ENOSPC.
 *
 * @param[in,out] map  The sparsemap to modify.
 * @param[in]     idx  0-based bit position to clear.
 * @returns \a idx on success, or SM_IDX_MAX with errno=ENOSPC.
 */
uint64_t sm_remove(sm_t *map, uint64_t idx);

/* -------------------------------------------------------------------
 * Aggregate queries
 * ------------------------------------------------------------------- */

/** @brief Count the total number of set bits (cardinality).
 *
 * Equivalent to `sm_rank(map, 0, SM_IDX_MAX, true)`.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Number of bits that are set to 1.
 */
size_t sm_cardinality(sm_t *map);

/** @brief Return the position of the first set bit (minimum).
 *
 * @param[in] map  The sparsemap to query.
 * @returns 0-based index of the lowest set bit, or 0 if the map is empty.
 */
uint64_t sm_minimum(const sm_t *map);

/** @brief Return the position of the last set bit (maximum).
 *
 * @param[in] map  The sparsemap to query.
 * @returns 0-based index of the highest set bit, or 0 if the map is empty.
 */
uint64_t sm_maximum(const sm_t *map);

/** @brief Return the fraction of bits that are set.
 *
 * Computed as cardinality / (maximum - minimum + 1).
 *
 * @param[in] map  The sparsemap to query.
 * @returns Fill factor in the range [0.0, 1.0].
 */
double sm_fill_factor(sm_t *map);

/* -------------------------------------------------------------------
 * Rank, select, and span
 * ------------------------------------------------------------------- */

/** @brief Count matching bits in the inclusive range [\a x, \a y].
 *
 * @param[in] map    The sparsemap to query.
 * @param[in] x      0-based start of range (inclusive).
 * @param[in] y      0-based end of range (inclusive).
 * @param[in] value  true to count set bits, false to count unset bits.
 * @returns Number of bits matching \a value in the range.
 *
 * Example:
 * @code
 *   // Count set bits in positions [100, 199]
 *   size_t n = sm_rank(map, 100, 199, true);
 * @endcode
 */
size_t sm_rank(sm_t *map, uint64_t x, uint64_t y, bool value);

/** @brief Find the position of the \a n'th matching bit (0-based).
 *
 * select(map, 0, true) returns the index of the first set bit.
 * select(map, 2, false) returns the index of the third unset bit.
 *
 * For RLE chunks this is O(1); for sparse chunks it scans bit vectors.
 *
 * @param[in] map    The sparsemap to query.
 * @param[in] n      Number of matching bits to skip (0 = first match).
 * @param[in] value  true to find set bits, false to find unset bits.
 * @returns 0-based index of the matching bit, or SM_IDX_MAX if
 *          fewer than n+1 matching bits exist.
 *
 * Example:
 * @code
 *   uint64_t first_set  = sm_select(map, 0, true);
 *   uint64_t third_zero = sm_select(map, 2, false);
 * @endcode
 */
uint64_t sm_select(sm_t *map, uint64_t n, bool value);

/** @brief Find the first contiguous run of \a len bits matching \a value.
 *
 * Searches forward from \a start for a span of at least \a len consecutive
 * bits that all match \a value.
 *
 * @param[in] map    The sparsemap to search.
 * @param[in] start  0-based position to begin searching.
 * @param[in] len    Required run length.
 * @param[in] value  true to find set bits, false to find unset bits.
 * @returns 0-based index of the first bit in the run, or SM_IDX_MAX
 *          if no such run exists.
 */
uint64_t sm_span(sm_t *map, uint64_t start, size_t len, bool value);

/* -------------------------------------------------------------------
 * sqrt(n) locator: transient O(sqrt n) contains / rank / select
 * ------------------------------------------------------------------- */

/** @brief Build a transient sqrt(n) locator over \a map.
 *
 * O(chunk count) one-pass build.  The returned locator accelerates
 * sm_locator_contains / _rank / _select to O(sqrt n).  See
 * sm_locator_t for the (re)build-on-mutation contract.
 *
 * @param[in] map  The sparsemap to index.
 * @returns A heap-allocated locator (free with sm_locator_free), or
 *          NULL on allocation failure or when \a map is NULL/empty.
 */
sm_locator_t *sm_locator_build(const sm_t *map);

/** @brief Release a locator built by sm_locator_build (NULL-safe). */
void sm_locator_free(sm_locator_t *loc);

/** @brief O(sqrt n) membership test; equals sm_contains(map, idx, NULL). */
bool sm_locator_contains(const sm_locator_t *loc, uint64_t idx);

/** @brief O(sqrt n) rank over inclusive [lo, hi]; equals sm_rank.
 *
 * For value == true this uses the superblock prefix directory.  For
 * value == false it falls back to the plain sm_rank path (correct, not
 * accelerated).
 */
size_t sm_locator_rank(const sm_locator_t *loc, uint64_t lo, uint64_t hi,
    bool value);

/** @brief O(sqrt n) select; equals sm_select(map, n, value).
 *
 * For value == true this uses the superblock prefix directory.  For
 * value == false it falls back to the plain sm_select path (correct,
 * not accelerated).
 */
uint64_t sm_locator_select(const sm_locator_t *loc, uint64_t n, bool value);

/* -------------------------------------------------------------------
 * Iteration
 * ------------------------------------------------------------------- */

/** @brief Invoke a callback for every set bit in the map.
 *
 * The callback receives batches of up to 64 indices at a time.  Indices are
 * delivered in ascending order.  Each index is an absolute 64-bit bit
 * position, so the callback array is `uint64_t`.
 *
 * @param[in] map      The sparsemap to scan.
 * @param[in] scanner  Callback invoked with (array_of_indices, count, aux).
 * @param[in] skip     Number of set bits to skip before invoking the callback.
 * @param[in] aux      Opaque pointer forwarded to \a scanner.
 *
 * Example:
 * @code
 *   void print_bits(uint64_t idx[], size_t n, void *aux) {
 *       for (size_t i = 0; i < n; i++)
 *           printf("%" PRIu64 "\n", idx[i]);
 *   }
 *   sm_scan(map, print_bits, 0, NULL);
 * @endcode
 */
void sm_scan(const sm_t *map,
    void (*scanner)(uint64_t vec[], size_t n, void *aux), size_t skip,
    void *aux);

/* -------------------------------------------------------------------
 * Bulk operations
 * ------------------------------------------------------------------- */

/** @brief Create a new sparsemap containing bits set in either \a a or \a b.
 *
 * The result is a newly allocated sparsemap whose set bits are exactly those
 * that appear in either input map (logical OR).  Neither input is modified.
 *
 * @param[in] a  First input sparsemap.
 * @param[in] b  Second input sparsemap.
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if both inputs are empty/NULL.
 */
sm_t *sm_union(const sm_t *a, const sm_t *b);

/** @brief Create a new sparsemap containing bits set in both \a a and \a b.
 *
 * The result is a newly allocated sparsemap whose set bits are exactly those
 * that appear in both input maps (logical AND).  Neither input is modified.
 *
 * @param[in] a  First input sparsemap.
 * @param[in] b  Second input sparsemap.
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if the result would be empty.
 */
sm_t *sm_intersection(const sm_t *a, const sm_t *b);

/** @brief Create a new sparsemap containing bits set in \a a but not in \a b.
 *
 * The result is a newly allocated sparsemap whose set bits are exactly those
 * that appear in \a a but not in \a b (logical AND NOT).  Neither input is
 * modified.
 *
 * @param[in] a  First input sparsemap (minuend).
 * @param[in] b  Second input sparsemap (subtrahend).
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if the result would be empty.
 */
sm_t *sm_difference(const sm_t *a, const sm_t *b);

/** @brief Split the map at \a idx, moving higher bits to \a other.
 *
 * After the split, \a map contains bits in [start, idx) and \a other
 * contains bits in [idx, end].  The \a other map must be empty on entry.
 *
 * When \a idx is SM_IDX_MAX, the map is split at the median set
 * bit, producing two halves of roughly equal cardinality.
 *
 * If the split crosses an RLE chunk, that chunk is first separated into
 * sparse/RLE pieces, then the split proceeds on the resulting sparse chunk.
 * Adjacent chunks that form contiguous runs are coalesced after the split.
 *
 * @param[in,out] map    Source map (retains [start, idx)).
 * @param[in]     idx    Split point, or SM_IDX_MAX for even split.
 * @param[in,out] other  Destination for [idx, end] (must be empty).
 * @returns The index at which the map was split, or SM_IDX_MAX with
 *          errno=ENOSPC if the buffer is too small.
 *
 * Example:
 * @code
 *   sm_t *left = sparsemap(4096);
 *   sm_t *right = sparsemap(4096);
 *   // populate left ...
 *   sm_split(left, SM_IDX_MAX, right);
 *   // left has the lower half, right has the upper half
 * @endcode
 */
uint64_t sm_split(sm_t *map, uint64_t idx, sm_t *other);

/** @brief Create a new sparsemap with all bits shifted by \a offset.
 *
 * Every set bit at position i in \a map appears at position i + offset in
 * the result.  Bits shifted below 0 are silently dropped (matching
 * CRoaring and PostgreSQL semantics).
 *
 * @param[in] map     The source sparsemap.
 * @param[in] offset  Signed shift amount (positive = right, negative = left).
 * @returns A newly allocated sparsemap (caller must free()), or NULL if all
 *          bits are shifted away or on allocation failure.
 */
sm_t *sm_offset(const sm_t *map, ssize_t offset);

/* -------------------------------------------------------------------
 * Predicates and comparisons
 * ------------------------------------------------------------------- */

/** @brief Test whether a sparsemap is empty (has no set bits).
 *
 * O(1) check via the chunk count, faster than `sm_cardinality(map) == 0`
 * which would walk every chunk.
 *
 * @param[in] map  The sparsemap to query.
 * @returns true if the map has no set bits, false otherwise.
 */
bool sm_is_empty(const sm_t *map);

/** @brief Test bit-set equality of two sparsemaps.
 *
 * Two maps are equal iff every bit set in one is also set in the other.
 * The on-disk representations need not be byte-identical: equality is
 * defined by content, not encoding (so an RLE chunk and an equivalent
 * sparse chunk encoding the same bits compare equal).
 *
 * @param[in] a  First sparsemap (may be NULL, treated as empty).
 * @param[in] b  Second sparsemap (may be NULL, treated as empty).
 * @returns true if a and b represent the same bit set.
 */
bool sm_equals(const sm_t *a, const sm_t *b);

/** @brief Test whether \a a's bits are a subset of \a b's bits.
 *
 * @param[in] a  Candidate subset (NULL is the empty set, always a subset).
 * @param[in] b  Candidate superset (NULL is the empty set).
 * @returns true if every bit set in \a a is also set in \a b.
 */
bool sm_is_subset(const sm_t *a, const sm_t *b);

/** @brief Test whether \a a's bits are a superset of \a b's bits.
 *
 * Equivalent to `sm_is_subset(b, a)` -- included as a named function
 * for readability of `sm_is_superset(haystack, needle)` style calls.
 *
 * @param[in] a  Candidate superset (NULL is the empty set).
 * @param[in] b  Candidate subset (NULL is the empty set, always a subset).
 * @returns true if every bit set in \a b is also set in \a a.
 */
bool sm_is_superset(const sm_t *a, const sm_t *b);

/** @brief Test whether two sparsemaps share at least one set bit.
 *
 * Short-circuits on first overlap; never allocates the intersection.
 *
 * @param[in] a  First sparsemap (NULL or empty produces false).
 * @param[in] b  Second sparsemap (NULL or empty produces false).
 * @returns true if a and b have any bit in common.
 */
bool sm_overlap(const sm_t *a, const sm_t *b);

/** @brief Membership classification of a sparsemap.
 *
 * Useful when callers want to special-case empty or singleton sets
 * without paying the full cost of `sm_cardinality`.  Stops at the
 * second set bit; never enumerates the rest.
 */
typedef enum {
	SM_EMPTY = 0,     /**< no bits set */
	SM_SINGLETON = 1, /**< exactly one bit set */
	SM_MULTIPLE = 2,  /**< two or more bits set */
} sm_membership_t;

/** @brief Classify a sparsemap as empty, singleton, or multi-element.
 *
 * @param[in] map  The sparsemap to classify (NULL is empty).
 * @returns SM_EMPTY, SM_SINGLETON, or SM_MULTIPLE.
 */
sm_membership_t sm_membership(const sm_t *map);

/** @brief Return the sole member of a singleton sparsemap.
 *
 * @param[in] map  The sparsemap to query.
 * @returns The 0-based index of the single set bit if `sm_membership(map)
 *          == SM_SINGLETON`, or SM_IDX_MAX otherwise (empty or multi).
 */
uint64_t sm_singleton_member(const sm_t *map);

/* -------------------------------------------------------------------
 * Member-by-member iteration
 * ------------------------------------------------------------------- */

/** @brief Find the lowest set bit at index > \a prev_idx.
 *
 * Standard idiom for forward iteration:
 * @code
 *   uint64_t i = SM_IDX_MAX;  // start sentinel
 *   sm_cursor_t c = SM_CURSOR_INIT;
 *   while ((i = sm_next_member(map, i, &c)) != SM_IDX_MAX) {
 *       // i is the next set bit
 *   }
 * @endcode
 *
 * Pass `SM_IDX_MAX` to start at the first set bit.
 *
 * @param[in] map       The sparsemap to scan.
 * @param[in] prev_idx  Lower exclusive bound (use SM_IDX_MAX for "start at 0").
 * @param[in] cur       Optional read cursor for sequential acceleration,
 *                      or NULL.  See sm_cursor_t.
 * @returns The next set bit index, or SM_IDX_MAX if none.
 */
uint64_t sm_next_member(const sm_t *map, uint64_t prev_idx, sm_cursor_t *cur);

/** @brief Find the highest set bit at index < \a prev_idx.
 *
 * Standard idiom for reverse iteration:
 * @code
 *   uint64_t i = SM_IDX_MAX;  // start past-the-end
 *   while ((i = sm_prev_member(map, i, NULL)) != SM_IDX_MAX) {
 *       // i is the previous set bit
 *   }
 * @endcode
 *
 * @param[in] map       The sparsemap to scan.
 * @param[in] prev_idx  Upper exclusive bound (use SM_IDX_MAX for "start at end").
 * @param[in] cur       Optional read cursor (currently unused for reverse
 *                      iteration; accepted for API symmetry), or NULL.
 * @returns The previous set bit index, or SM_IDX_MAX if none.
 */
uint64_t sm_prev_member(const sm_t *map, uint64_t prev_idx, sm_cursor_t *cur);

/* -------------------------------------------------------------------
 * Cardinality without allocation
 *
 * These compute |a OP b| without materializing the result.  Useful in
 * hot paths where the caller only wants the size, not the bits.
 * ------------------------------------------------------------------- */

/** @brief Compute the cardinality of (a union b) without allocating it. */
size_t sm_union_cardinality(const sm_t *a, const sm_t *b);

/** @brief Compute the cardinality of (a intersect b) without allocating it. */
size_t sm_intersection_cardinality(const sm_t *a, const sm_t *b);

/** @brief Compute |a \ b| without allocating the difference. */
size_t sm_difference_cardinality(const sm_t *a, const sm_t *b);

/** @brief Test whether `a \ b` has any set bits, without allocating.
 *
 * Equivalent to `sm_difference_cardinality(a, b) > 0` but with
 * short-circuit on first non-overlap.  Mirrors PostgreSQL's
 * `bms_nonempty_difference`.
 */
bool sm_nonempty_difference(const sm_t *a, const sm_t *b);

/** @brief Jaccard similarity index: |a intersect b| / |a union b|.
 *
 * @returns A value in [0.0, 1.0].  Returns 0.0 if both maps are empty
 *          (the standard convention for the indeterminate 0/0 case).
 */
double sm_jaccard_index(const sm_t *a, const sm_t *b);

/* -------------------------------------------------------------------
 * Bulk add and array conversion
 * ------------------------------------------------------------------- */

/** @brief Add N indices from an array.
 *
 * Equivalent to a loop over `sm_add(map, arr[i])`, but sorts a private
 * copy of `arr` ascending first, so the cost is O(n log n + n)
 * regardless of caller order.  (A raw unsorted loop is O(n^2): the
 * bulk path threads an internal cursor that only accelerates ascending
 * inserts, so random-order inserts each do a full chunk walk + byte-shift.)  The
 * caller's array is const and untouched.
 *
 * @param[in,out] map  Destination.
 * @param[in]     arr  Array of indices (any order).
 * @param[in]     n    Length of `arr`.
 * @returns true if every add succeeded; false if a scratch allocation
 *          failed or any add returned SPARSEMAP_IDX_MAX (capacity
 *          exhausted -- use sm_add_many_grow to grow instead).
 */
bool sm_add_many(sm_t *map, const uint64_t *arr, size_t n);

/** @brief Add N indices, growing the buffer as needed.
 *
 * Like sm_add_many but takes `sm_t **` and uses sm_add_grow, so the
 * buffer is realloc'd geometrically on ENOSPC instead of failing.
 * Sorts a private copy of `arr` ascending first (same O(n log n + n)
 * rationale).  This is the bulk-build entry point for callers that
 * accumulate an unbounded index stream.
 *
 * @param[in,out] map  Destination (may be realloc'd; *map is updated).
 * @param[in]     arr  Array of indices (any order).
 * @param[in]     n    Length of `arr`.
 * @returns true on success; false if a scratch allocation failed or
 *          sm_add_grow exhausted its grow retries.
 */
bool sm_add_many_grow(sm_t **map, const uint64_t *arr, size_t n);

/** @brief Materialize all set bits as a uint64_t array.
 *
 * Two-pass: pass NULL for `out` to size, then allocate and pass the
 * buffer.  Or pass a buffer of `*n_out` capacity; on return, `*n_out`
 * is the number actually written.
 *
 * @param[in]     map    Source.
 * @param[out]    out    Caller-allocated buffer (or NULL to query size).
 * @param[in,out] n_out  In: capacity of `out`.  Out: number written.
 */
void sm_to_array(const sm_t *map, uint64_t *out, size_t *n_out);

/* -------------------------------------------------------------------
 * Range manipulation and symmetric difference
 * ------------------------------------------------------------------- */

/** @brief Set every bit in `[lo, hi)`.
 *
 * Equivalent to looping `sm_add(map, i)` for i in [lo, hi).
 * Implementation is currently the naive loop; a chunk-aware fast
 * path may land in a future release.
 *
 * @param[in,out] map  Destination.
 * @param[in]     lo   Inclusive lower bound.
 * @param[in]     hi   Exclusive upper bound (lo == hi is a no-op).
 * @returns true if every bit was added; false if any add returned
 *          SPARSEMAP_IDX_MAX (capacity exhausted).
 */
bool sm_add_range(sm_t *map, uint64_t lo, uint64_t hi);

/** @brief Clear every bit in `[lo, hi)`.
 *
 * @param[in,out] map  Destination.
 * @param[in]     lo   Inclusive lower bound.
 * @param[in]     hi   Exclusive upper bound.
 * @returns true if every bit was cleared; false if any remove failed.
 */
bool sm_remove_range(sm_t *map, uint64_t lo, uint64_t hi);

/** @brief Extract a range of bits as a new sparsemap.
 *
 * Returns a newly allocated owned-contiguous sparsemap containing
 * exactly the bits set in \a map within `[lo, hi)`.  The bit indices
 * are preserved (no shift); the result `r` satisfies
 * `sm_contains(r, i) == sm_contains(map, i)` for `i in [lo, hi)` and
 * `sm_contains(r, i) == false` for `i` outside that range.
 *
 * Equivalent in semantics to:
 *
 *     result = sm_intersection(map, sm_create_from_range(lo, hi));
 *
 * but avoids the second allocation by extracting directly.
 *
 * @returns A new sparsemap, or NULL if the result would be empty or
 *          on allocation failure.
 */
sm_t *sm_extract_range(const sm_t *map, uint64_t lo, uint64_t hi);

/** @brief Symmetric difference: bits set in exactly one of \a a, \a b.
 *
 * Returns a newly allocated owned-contiguous sparsemap.
 */
sm_t *sm_xor(const sm_t *a, const sm_t *b);

/** @brief Synonym for sm_union (logical OR). */
sm_t *sm_or(const sm_t *a, const sm_t *b);

/** @brief Synonym for sm_intersection (logical AND). */
sm_t *sm_and(const sm_t *a, const sm_t *b);

/** @brief Synonym for sm_difference (logical AND-NOT: bits in a but not b). */
sm_t *sm_andnot(const sm_t *a, const sm_t *b);

/** @brief XOR cardinality without allocation.
 *
 * Equivalent to `sm_cardinality(sm_xor(a,b))` but doesn't materialize
 * the result.
 */
size_t sm_xor_cardinality(const sm_t *a, const sm_t *b);

/* -------------------------------------------------------------------
 * Constructors
 * ------------------------------------------------------------------- */

/** @brief Create a sparsemap containing exactly the bit at `idx`.
 *
 * Convenience wrapper around `sm_create()` + `sm_add()`.  Mirrors
 * PostgreSQL's `bms_make_singleton`.
 *
 * @param[in] idx  The single bit to set.
 * @returns A new owned-contiguous sparsemap, or NULL on alloc failure.
 */
sm_t *sm_create_singleton(uint64_t idx);

/** @brief Create a sparsemap containing every bit in `[lo, hi)`.
 *
 * @param[in] lo  Inclusive lower bound.
 * @param[in] hi  Exclusive upper bound.
 * @returns A new owned-contiguous sparsemap, or NULL on alloc failure.
 *          Empty range produces an empty map (not NULL).
 */
sm_t *sm_create_from_range(uint64_t lo, uint64_t hi);

/** @brief Create a sparsemap from an array of indices.
 *
 * @param[in] arr  Array of indices.
 * @param[in] n    Length of `arr`.
 * @returns A new owned-contiguous sparsemap, or NULL on alloc failure.
 */
sm_t *sm_create_from_array(const uint64_t *arr, size_t n);

/* -------------------------------------------------------------------
 * Hashing and comparison
 * ------------------------------------------------------------------- */

/** @brief Stable content-based hash of the bit set.
 *
 * Two maps that compare equal under sm_equals() always hash to the
 * same value, regardless of internal RLE-vs-sparse encoding choices.
 */
uint64_t sm_hash(const sm_t *map);

/** @brief Three-way compare for ordering bitmaps.
 *
 * Lexicographic order on the bit sequence (sorted ascending).  Suitable
 * for sorting an array of bitmaps deterministically.  Mirrors
 * PostgreSQL's `bms_compare`.
 *
 * @returns Negative, zero, or positive following the standard convention.
 */
int sm_compare(const sm_t *a, const sm_t *b);

/** @brief Subset-relation between two sparsemaps. */
typedef enum {
	SM_REL_EQUAL = 0,     /**< a == b */
	SM_REL_SUBSET_A = 1,  /**< a is a strict subset of b */
	SM_REL_SUBSET_B = 2,  /**< b is a strict subset of a */
	SM_REL_DIFFERENT = 3, /**< neither is a subset of the other */
} sm_subset_relation_t;

/** @brief Classify the subset relationship between \a a and \a b.
 *
 * Mirrors PostgreSQL's `bms_subset_compare`.  More efficient than
 * calling `sm_is_subset` twice when the caller needs the full picture.
 */
sm_subset_relation_t sm_subset_compare(const sm_t *a, const sm_t *b);

/* -------------------------------------------------------------------
 * Destructive iteration
 * ------------------------------------------------------------------- */

/** @brief Find the lowest set bit, clear it, and return it.
 *
 * Useful for worklist algorithms.  Mirrors PostgreSQL's
 * `bms_first_member`.
 *
 * @returns The lowest set bit's index, or SM_IDX_MAX if the map was empty.
 */
uint64_t sm_pop_first(sm_t *map);

/** @brief Find the highest set bit, clear it, and return it.
 *
 * The reverse of sm_pop_first.  Useful for stack-style worklist
 * algorithms.  Returns SM_IDX_MAX if the map is empty.
 */
uint64_t sm_pop_last(sm_t *map);

/* -------------------------------------------------------------------
 * In-place set operations
 *
 * These mutate `dst` instead of allocating a new result.  The return
 * value is `dst` itself when no growth was needed, or a new pointer
 * if `dst` had to be relocated (the wrap-and-grow promotion case).
 * Caller idiom:
 *
 *     dst = sm_union_inplace(dst, src);
 *
 * Mirrors PostgreSQL's `bms_add_members` / `bms_int_members` /
 * `bms_del_members` and CRoaring's `_inplace` variants.
 * ------------------------------------------------------------------- */

/** @brief In-place union: `dst := dst U src`.
 *
 * @returns The (possibly relocated) dst.  NULL on alloc failure.
 */
sm_t *sm_union_inplace(sm_t *dst, const sm_t *src);

/** @brief In-place intersection: `dst := dst INT src`.
 *
 * Result always shrinks or stays same; never reallocates.
 */
sm_t *sm_intersection_inplace(sm_t *dst, const sm_t *src);

/** @brief In-place difference: `dst := dst \ src`.
 *
 * Result always shrinks or stays same; never reallocates.
 */
sm_t *sm_difference_inplace(sm_t *dst, const sm_t *src);

/* -------------------------------------------------------------------
 * Range complement
 * ------------------------------------------------------------------- */

/** @brief Complement every bit in `[lo, hi)`: set bits become unset and vice versa.
 *
 * In-place.  Naive implementation: O(hi-lo) sm_assign calls.
 *
 * @returns true on success, false if the buffer was too small to grow.
 */
bool sm_flip_range(sm_t *map, uint64_t lo, uint64_t hi);

/* -------------------------------------------------------------------
 * Maintenance and introspection
 * ------------------------------------------------------------------- */

/** @brief Runtime self-check of a sparsemap's internal consistency.
 *
 * Verifies (without `SPARSEMAP_DIAGNOSTIC`):
 *   - chunk count matches the actual number of chunks reachable
 *     by walking the buffer
 *   - each chunk's claimed size fits within m_data_used
 *   - chunk start offsets are monotonically increasing
 *   - sum of chunk sizes + SM_SIZEOF_OVERHEAD == m_data_used
 *
 * Useful as an after-deserialize sanity check.
 *
 * @returns true if the map is internally consistent.
 */
bool sm_validate(const sm_t *map);

/** @brief Statistics about a sparsemap's internal layout.
 *
 * Useful for understanding compression effectiveness or diagnosing
 * unexpectedly-large maps.
 */
typedef struct sm_stats {
	size_t chunks_total;      /**< total chunks */
	size_t chunks_rle;        /**< chunks using RLE encoding */
	size_t chunks_sparse;     /**< chunks using sparse encoding */
	size_t bytes_used;        /**< sm_get_size(map) */
	size_t bytes_capacity;    /**< sm_get_capacity(map) */
	uint64_t bits_set;        /**< sm_cardinality(map) */
	uint64_t bits_in_rle;     /**< bits set within RLE chunks */
	uint64_t bits_in_sparse;  /**< bits set within sparse chunks */
	double bytes_per_set_bit; /**< bytes_used / bits_set */
} sm_stats_t;

/** @brief Fill an sm_stats_t with introspection data. */
void sm_statistics(const sm_t *map, sm_stats_t *stats);

/** @brief Realloc the data buffer down to exactly `m_data_used` bytes.
 *
 * Useful after a sequence of removals.  Owned-contiguous and
 * owned-split lineages only; wrap'd maps are rejected (no library
 * ownership of the buffer to shrink).
 *
 * @returns The (possibly relocated) map pointer, or NULL on alloc failure.
 */
sm_t *sm_shrink_to_fit(sm_t *map);

/* -------------------------------------------------------------------
 * Portable serialization
 *
 * Format (16 bytes header + body):
 *
 *   uint32_t magic     = 0x736d3130 ("sm10")  -- versions <2
 *   uint8_t  version   = 1
 *   uint8_t  flags     = 0x01 if little-endian, 0x00 if big-endian
 *   uint16_t reserved  = 0 (must be ignored on read)
 *   uint64_t cardinality           -- size hint for callers
 *   <body: existing internal m_data layout, in source endian>
 *
 * Cross-endian deserialization is not yet supported; sm_deserialize
 * returns NULL if the source endian doesn't match the host.
 * ------------------------------------------------------------------- */

/** @brief Compute the buffer size needed to serialize \a map.
 *
 * @returns Number of bytes that sm_serialize will write.
 */
size_t sm_serialized_size(const sm_t *map);

/** @brief Serialize \a map into \a out (`sm_serialized_size` bytes).
 *
 * @param[in]  map       Source map.
 * @param[out] out       Caller-allocated buffer of at least sm_serialized_size bytes.
 * @param[in]  out_size  Capacity of \a out.
 * @returns Number of bytes written, or 0 on error.
 */
size_t sm_serialize(const sm_t *map, uint8_t *out, size_t out_size);

/** @brief Deserialize a previously-serialized buffer into a fresh map.
 *
 * Bounded-safe: validates the header magic, version, endianness, and
 * that each chunk's claimed size fits in the remaining buffer.
 * Returns NULL on any malformed input rather than crashing.
 *
 * @param[in] in   Source buffer.
 * @param[in] n    Source buffer size.
 * @returns A new owned-contiguous sparsemap, or NULL on error.
 */
sm_t *sm_deserialize(const uint8_t *in, size_t n);

#if defined(__cplusplus)
}
#endif

#endif /* !defined(SPARSEMAP_H) */

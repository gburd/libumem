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

/*
 * Sparsemap
 *
 * This is an implementation for a sparse, compressed bitmap. It is resizable
 * and mutable, with reasonable performance for random access modifications
 * and lookups.
 *
 * The implementation is separated into tiers.
 *
 * Tier 0 (lowest): bits are stored in a sm_bitvec_t (uint64_t).
 *
 * Tier 1 (middle): multiple sm_bitvec_t are managed in a chunk map. The chunk
 *    map only stores those sm_bitvec_t that have a mixed payload of bits (i.e.
 *    some bits are 1, some are 0). As soon as ALL bits in a sm_bitvec_t are
 *    identical, this sm_bitvec_t is no longer stored, it is compressed.
 *
 *    The chunk maps store additional flags (2 bit) for each sm_bitvec_t in an
 *    additional word (same size as the sm_bitvec_t itself).
 *
 *     00 11 22 33
 *     ^-- descriptor for sm_bitvec_t 1
 *        ^-- descriptor for sm_bitvec_t 2
 *           ^-- descriptor for sm_bitvec_t 3
 *              ^-- descriptor for sm_bitvec_t 4
 *
 *    Those flags (*) can have one of the following values:
 *
 *     00   The sm_bitvec_t is all zero -> sm_bitvec_t is not stored
 *     11   The sm_bitvec_t is all one -> sm_bitvec_t is not stored
 *     10   The sm_bitvec_t contains a bitmap -> sm_bitvec_t is stored
 *     01   The sm_bitvec_t is not used (**)
 *
 *    The serialized size of a chunk map in memory therefore is at least
 *    one sm_bitvec_t for the flags, and (optionally) additional sm_bitvec_ts
 *    if they are required.
 *
 *    (*) The code comments often use the Erlang format for binary
 *    representation, i.e. 2#10 for (binary) 01.
 *
 *    (**) This flag is set to reduce the capacity of a chunk map.
 *
 * Tier 2 (highest): the Sparsemap manages multiple chunk maps. Each chunk
 *    has its own offset (relative to the offset of the Sparsemap). In
 *    addition, the Sparsemap manages the memory of the chunk maps, and
 *    is able to grow or shrink that memory as required.
 */
#ifndef SPARSEMAP_H
#define SPARSEMAP_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * The public interface for a sparse bit-mapped index, a "sparse map".
 *
 * |sm_idx_t| is the user's numerical data type which is mapped to a single bit
 * in the bitmap. Usually this is uint32_t or uint64_t.  |sm_bitvec_t| is the
 * storage type for a bit vector used by the __sm_chunk_t internal maps.
 * Usually this is an uint64_t.
 */

typedef struct sparsemap sparsemap_t;
typedef size_t sparsemap_idx_t;
#define SPARSEMAP_IDX_MAX SIZE_MAX
#define SPARSEMAP_FOUND(x) ((x) != SPARSEMAP_IDX_MAX)
#define SPARSEMAP_NOT_FOUND(x) ((x) == SPARSEMAP_IDX_MAX)
typedef uint32_t sm_idx_t;
typedef uint64_t sm_bitvec_t;

/** @brief Allocate a new, empty sparsemap_t with a buffer of \b size on the
 * heap to use for storage of bitmap data.
 *
 * The buffer used for the bitmap is allocated in the same heap allocation as
 * the structure, this means that you only need to call free() on the returned
 * object to free all resources.  Using this method allows you to grow the
 * buffer size by calling #sparsemap_set_data_size().  This function calls
 * #sparsemap_init().
 *
 * @param[in] size The starting size of the buffer used for the bitmap, default
 * is 1024 bytes.
 * @returns The newly allocated sparsemap reference.
 */
sparsemap_t *sparsemap(size_t size);

/** @brief Allocate a new, copy of the \b other sparsemap_t.
 *
 * @param[in] other The sparsemap to copy.
 */
sparsemap_t *sparsemap_copy(sparsemap_t *other);

/** @brief Allocate a new, empty sparsemap_t that references (wraps) the buffer
 * \b data of \b size bytes to use for storage of bitmap data.
 *
 * This function allocates a new sparsemap_t but not the buffer which is
 * provided by the caller as \b data which can be allocated on the stack or
 * heap.  Caller is responsible for calling free() on the returned heap object
 * and releasing the memory used for \b data.  Resizing the buffer is only
 * supported when the heap object for the map includes the buffer and the
 * \b data offset supplied is relative to the object (see #sparsemap()).
 *
 * @param[in] data A heap or stack memory buffer of \b size for use storing
 * bitmap data.
 * @param[in] size The size of the buffer \b data used for the bitmap.
 * @returns The newly allocated sparsemap reference.
 */
sparsemap_t *sparsemap_wrap(uint8_t *data, size_t size);

/** @brief Initialize an existing sparsemap_t by assigning \b data of \b size
 * bytes for storage of bitmap data.
 *
 * Given the address of an existing \b map allocated on the stack or heap this
 * function will initialize the data structure and use the provided \b data of
 * \b size for bitmap data.  Caller is responsible for all memory management.
 * Resizing the buffer is not directly supported, you
 * may resize it and call #sparsemap_set_data_size() and then ensure that should
 * the address of the object changed you need to update it by calling #sparsemap_
 * m_data field.
 *
 * @param[in] map The sparsemap reference.
 * @param[in] data A heap or stack memory buffer of \b size for use storing
 * bitmap data.
 * @param[in] size The size of the buffer \b data used for the bitmap.
 */
void sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size);

/** @brief Opens, without initializing, an existing sparsemap contained within
 * the specified buffer.
 *
 * Given the address of an existing \b map this function will assign to the
 * provided data structure \b data of \b size for bitmap data.  Caller is
 * responsible for all memory management.  Use this when as a way to
 * "deserialize" bytes and make them ready for use as a bitmap.
 *
 * @param[in] map The sparsemap reference.
 * @param[in] data A heap or stack memory buffer of \b size for use storing
 * bitmap data.
 * @param[in] size The size of the buffer \b data used for the bitmap.
 */
void sparsemap_open(sparsemap_t *map, uint8_t *data, size_t size);

/** @brief Resets values and empties the buffer making it ready to accept new
 *  data but does not free the memory.
 *
 * @param[in] map The sparsemap reference.
 */
void sparsemap_clear(sparsemap_t *map);

/** @brief Update the size of the buffer \b data used for storing the bitmap.
 *
 * When called with \b data NULL on a \b map that was created with #sparsemap()
 * this function will reallocate the storage for both the map and data possibly
 * changing the address of the map itself so it is important for the caller to
 * update all references to this map to the address returned in this scenario.
 * Access to stale references will result in memory violations and program
 * termination.  Caller is not required to free() the old address, only the new
 * one should it have changed.  This uses #realloc() under the covers, all
 * caveats apply here as well.
 *
 * When called referencing a \b map that was allocate by the caller this
 * function will only update the values within the data structure.
 *
 * @param[in] map The sparsemap reference.
 * @param[in] size The desired size of the buffer \b data used for the bitmap.
 * @returns The -- potentially changed -- sparsemap reference, or NULL should a
 * #realloc() fail (\b ENOMEM)
 * @note The resizing of caller supplied allocated objects is not yet fully
 * supported.
 */
sparsemap_t *sparsemap_set_data_size(sparsemap_t *map, uint8_t *data, size_t size);

/** @brief Calculate remaining capacity, approaches 0 when full.
 *
 * Provides an estimate in the range [0.0, 100.0] of the remaining capacity of
 * the buffer storing bitmap data.  This can change up or down as more data
 * is added/removed due to the method for compressed representation, do not
 * expect a smooth progression either direction.  This is a rough estimate only
 * and may also jump in value after seemingly indiscriminate changes to the map.
 *
 * @param[in] map The sparsemap reference.
 * @returns an estimate for remaining capacity that approaches 0.0 when full or
 * 100.0 when empty
 */
double sparsemap_capacity_remaining(sparsemap_t *map);

/** @brief Returns the capacity of the underlying byte array in bytes.
 *
 * Specifically, this returns the byte \b size provided for the underlying
 * buffer used to store bitmap data.
 *
 * @param[in] map The sparsemap reference.
 * @returns byte size of the buffer used for storing bitmap data
 */
size_t sparsemap_get_capacity(sparsemap_t *map);

/** @brief Returns the value of a bit at index \b idx, either true for "set" (1)
 * or \b false for "unset" (0).
 *
 * @param[in] map The sparsemap reference.
 * @param[in] idx The 0-based offset into the bitmap index to examine.
 * @returns either true or false
 */
bool sparsemap_is_set(sparsemap_t *map, sparsemap_idx_t idx);

/** @brief Sets the bit at index \b idx to \b value.
 *
 * A sparsemap has a fixed size buffer with a capacity that can be exhausted by
 * when calling this function.  In such cases the return value is not equal to
 * the provided \b idx and errno is set to ENOSPC.  In such situations it is
 * possible to grow the data size and retry the set() operation under certain
 * circumstances (see #sparsemap() and #sparsemap_set_data_size()).
 *
 * @param[in] map The sparsemap reference.
 * @param[in] idx The 0-based offset into the bitmap index to modify.
 * @returns the \b idx supplied on success or SPARSEMAP_IDX_MAX on error
 * with \b errno set to ENOSPC when the map is full.
 */
sparsemap_idx_t sparsemap_set(sparsemap_t *map, sparsemap_idx_t idx, bool value);

/** @brief Returns the byte size of the data buffer that has been used thus far.
 *
 * @param[in] map The sparsemap reference.
 * @returns the byte size of the data buffer that has been used thus far
 */
size_t sparsemap_get_size(sparsemap_t *map);

/** @brief Returns a pointer to the data buffer used for the map.
 *
 * @param[in] map The sparsemap reference.
 * @returns a pointer to the data buffer used for the map
 */
void *sparsemap_get_data(sparsemap_t *map);

/** @brief Returns the number of elements in the map.
 *
 * @param[in] map The sparsemap reference.
 * @returns the number of elements in the map
 */
size_t sparsemap_count(sparsemap_t *map);

/** @brief Returns the offset of the first bit set in the map.
 *
 * This is the same as the value of the first set bit in the
 * map.
 *
 * @param[in] map The sparsemap reference.
 * @returns the offset of the first bit set in the map
 */
sparsemap_idx_t sparsemap_get_starting_offset(sparsemap_t *map);

/** @brief Returns the offset of the last bit set in the map.
 *
 * This is the same as the value of the last bit set in the
 * map.
 *
 * @param[in] map The sparsemap reference.
 * @returns the offset of the index bit set in the map
 */
sparsemap_idx_t sparsemap_get_ending_offset(sparsemap_t *map);

/** @brief Returns the percent of bits set in the map.
 *
 * @param[in] map The sparsemap reference.
 * @returns the percent of bits set.
 */
double sparsemap_fill_factor(sparsemap_t *map);

/** @brief Provides a method for a callback function to examine every bit set in
 * the index.
 *
 * This decompresses the whole bitmap and invokes #scanner() passing an array
 * of the positions of set bits in order from 0 index to the end of the map.
 *
 * @param[in] map The sparsemap reference.
 * @param[in] skip Start the scan after \b skip position in the map.
 * @param[in] aux Auxiliary information passed to the scanner.
 */
void sparsemap_scan(sparsemap_t *map, void (*scanner)(sm_idx_t vec[], size_t n, void *aux), size_t skip, void *aux);

/** @brief Merges the values from \b source into \b destination, \b source is unchanged.
 *
 * Efficiently adds all set bits from \b source into \b destination.
 *
 * @param[in] destination The sparsemap reference into which we will merge \b source.
 * @param[in] source The bitmap to merge into \b destination.
 * @returns 0 on success, or sets errno to ENOSPC and returns the amount of
 * additional space required to successfully merge the maps.
 */
int sparsemap_merge(sparsemap_t *destination, sparsemap_t *source);

/** @brief Splits the bitmap by assigning all bits starting at \b offset to the
 * \b other bitmap while removing them from \b map.
 *
 * The \b other bitmap is expected to be empty.
 *
 * @param[in] map The sparsemap reference.
 * @param[in] offset The 0-based offset into the bitmap at which to split, if
 * set to SPARSEMAP_IDX_MAX then the bits will be evenly split.
 * @param[in] other The bitmap into which we place the split.
 * @returns the offset at which the map was split
 */
sparsemap_idx_t sparsemap_split(sparsemap_t *map, sparsemap_idx_t offset, sparsemap_t *other);

/** @brief Finds the index of the \b n'th bit set to \b value.
 *
 * Locates the \b n'th bit either set, \b value is true, or unset, \b value is
 * false, from the start of the bitmap.
 * So, if your bit pattern is: ```1101 1110 1010 1101 1011 1110 1110 1111``` and
 * you request the first set bit the result is `0` (meaning the 1st bit in the
 * map which is index 0 because this is 0-based indexing).  The first unset bit
 * is `2` (or the third bit in the pattern).  When n is 3 and value is true the
 * result would be `3` (the fourth bit, or the third set bit which is at index
 * 3 when 0-based).
 *
 * @param[in] map The sparsemap reference.
 * @param[in] n Specifies how many bits to ignore (when n=2 return the position
 * of the third matching bit).
 * @param[in] value Determines if the search is to examine set (true) or unset
 * (false) bits in the bitmap index.
 * @returns the 0-based index of the located bit position within the map; when
 * not found either SPARSEMAP_IDX_MAX.
 */
sparsemap_idx_t sparsemap_select(sparsemap_t *map, sparsemap_idx_t n, bool value);

/** @brief Counts the bits matching \b value in the provided range, [\b x, \b
 * y].
 *
 * Counts the set, \b value is true, or unset, \b value is false, bits starting
 * at the \b idx'th bit (0-based) in the range [\b x, \b y] (inclusive on either
 * end).  If range is [0, 0] this examines 1 bit, the first one in the map, and
 * returns 1 if value is true and the bit was set.
 *
 * @param[in] map The sparsemap reference.
 * @param[in] x 0-based start of the inclusive range to examine.
 * @param[in] y 0-based end of the inclusive range to examine.
 * @param[in] value Determines if the scan is to count the set (true) or unset
 * (false) bits in the range.
 * @returns the count of bits found within the range that match the \b value
 */
size_t sparsemap_rank(sparsemap_t *map, size_t x, size_t y, bool value);

/** @brief Locates the first contiguous set of bits of \b len starting at \b idx
 * matching \b value in the bitmap.
 *
 * @param[in] map The sparsemap reference.
 * @param[in] start 0-based start of search within the bitmap.
 * @param[in] len The length of contiguous bits we're seeking.
 * @param[in] value Determines if the scan is to find all set (true) or unset
 * (false) bits of \b len.
 * @returns the index of the first bit matching the criteria; when not found
 * found SPARSEMAP_IDX_MAX
 */
size_t sparsemap_span(sparsemap_t *map, sparsemap_idx_t start, size_t len, bool value);

#if defined(__cplusplus)
}
#endif

#endif /* !defined(SPARSEMAP_H) */

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

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <popcount.h>
#include <sparsemap.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SPARSEMAP_DIAGNOSTIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __sm_diag(format, ...) __sm_diag_(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#pragma GCC diagnostic pop
void __attribute__((format(printf, 4, 5))) __sm_diag_(const char *file, int line, const char *func, const char *format, ...)
{
  va_list args;
  fprintf(stderr, "%s:%d:%s(): ", file, line, func);
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

#define __sm_assert(expr) \
  if (!(expr))            \
  fprintf(stderr, "%s:%d:%s(): assertion failed! %s\n", __FILE__, __LINE__, __func__, #expr)

#define __sm_when_diag(expr) \
  if (1)                     \
  expr
#else
#define __sm_diag(file, line, func, format, ...) ((void)0)
#define __sm_assert(expr) ((void)0)
#define __sm_when_diag(expr) \
  if (0)                     \
  expr
#endif

#define IS_8_BYTE_ALIGNED(addr) (((uintptr_t)(addr)&0x7) == 0)

enum __SM_CHUNK_INFO {
  /* metadata overhead: 4 bytes for __sm_chunk_t count */
  SM_SIZEOF_OVERHEAD = sizeof(uint32_t),

  /* number of bits that can be stored in a sm_bitvec_t */
  SM_BITS_PER_VECTOR = (sizeof(sm_bitvec_t) * 8),

  /* number of flags that can be stored in a single index byte */
  SM_FLAGS_PER_INDEX_BYTE = 4,

  /* number of flags that can be stored in the index */
  SM_FLAGS_PER_INDEX = (sizeof(sm_bitvec_t) * SM_FLAGS_PER_INDEX_BYTE),

  /* maximum capacity of a __sm_chunk_t (in bits) */
  SM_CHUNK_MAX_CAPACITY = (SM_BITS_PER_VECTOR * SM_FLAGS_PER_INDEX),

  /* sm_bitvec_t payload is all zeros (2#00) */
  SM_PAYLOAD_ZEROS = 0,

  /* sm_bitvec_t payload is all ones (2#11) */
  SM_PAYLOAD_ONES = 3,

  /* sm_bitvec_t payload is mixed (2#10) */
  SM_PAYLOAD_MIXED = 2,

  /* sm_bitvec_t is not used (2#01) */
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

#define SM_CHUNK_GET_FLAGS(from, at) ((((from)) & ((sm_bitvec_t)SM_FLAG_MASK << ((at)*2))) >> ((at)*2))

typedef struct {
  sm_bitvec_t *m_data;
} __sm_chunk_t;

struct __attribute__((aligned(8))) sparsemap {
  size_t m_capacity;  /* The total size of m_data */
  size_t m_data_used; /* The used size of m_data */
  uint8_t *m_data;    /* The serialized bitmap data */
};

/**
 * Calculates the number of sm_bitvec_ts required by a single byte with flags
 * (in m_data[0]).
 */
static size_t
__sm_chunk_calc_vector_size(uint8_t b)
{
  // clang-format off
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
  // clang-format on
  return (size_t)lookup[b];
}

/** @brief Returns the position of a sm_bitvec_t in m_data.
 *
 * Each chunk has a set of bitvec that are sometimes abbreviated due to
 * compression (e.g. when a bitvec is all zeros or ones there is no need
 * to store anything, so no wasted space).
 *
 * @param[in] chunk The chunk in question.
 * @param[in] bv The index of the vector to find in the chunk.
 * @returns the position of a sm_bitvec_t in m_data
 */
static size_t
__sm_chunk_get_position(__sm_chunk_t *chunk, size_t bv)
{
  /* Handle 4 indices (1 byte) at a time. */
  size_t num_bytes = bv / ((size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR);

  size_t position = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < num_bytes; i++, p++) {
    position += __sm_chunk_calc_vector_size(*p);
  }

  bv -= num_bytes * SM_FLAGS_PER_INDEX_BYTE;
  for (size_t i = 0; i < bv; i++) {
    size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, i);
    if (flags == SM_PAYLOAD_MIXED) {
      position++;
    }
  }

  return position;
}

/** @brief Initialize __sm_chunk_t with provided data.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] data The memory to use within this chunk.
 */
static inline void
__sm_chunk_init(__sm_chunk_t *chunk, uint8_t *data)
{
  chunk->m_data = (sm_bitvec_t *)data;
}

/** @brief Examines the chunk to determine its current capacity.
 *
 * @param[in] chunk The chunk in question.
 * @returns the maximum capacity in bytes of this __sm_chunk_t
 */
static size_t
__sm_chunk_get_capacity(__sm_chunk_t *chunk)
{
  size_t capacity = SM_CHUNK_MAX_CAPACITY;

  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (!*p || *p == 0xff) {
      continue;
    }
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        capacity -= SM_BITS_PER_VECTOR;
      }
    }
  }
  return capacity;
}

/** @brief Reduces the capacity of this chunk.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] capacity The reduced capacity in bytes to assign to the chunk,
 * must be less than SM_CHUNK_MAX_CAPACITY.
 */
static void
__sm_chunk_reduce_capacity(__sm_chunk_t *chunk, size_t capacity)
{
  __sm_assert(capacity % SM_BITS_PER_VECTOR == 0);
  __sm_assert(capacity <= SM_CHUNK_MAX_CAPACITY);

  if (capacity >= SM_CHUNK_MAX_CAPACITY) {
    return;
  }

  size_t reduced = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (ssize_t i = sizeof(sm_bitvec_t) - 1; i >= 0; i--) {
    for (int j = SM_FLAGS_PER_INDEX_BYTE - 1; j >= 0; j--) {
      p[i] &= ~((sm_bitvec_t)SM_PAYLOAD_ONES << (j * 2));
      p[i] |= ((sm_bitvec_t)SM_PAYLOAD_NONE << (j * 2));
      reduced += SM_BITS_PER_VECTOR;
      if (capacity + reduced == SM_CHUNK_MAX_CAPACITY) {
        __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
        return;
      }
    }
  }
  __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
}

static void
__sm_chunk_increase_capacity(__sm_chunk_t *chunk, size_t capacity)
{
  __sm_assert(capacity % SM_BITS_PER_VECTOR == 0);
  __sm_assert(capacity <= SM_CHUNK_MAX_CAPACITY);
  __sm_assert(capacity > __sm_chunk_get_capacity(chunk));

  size_t initial_capacity = __sm_chunk_get_capacity(chunk);
  if (capacity <= initial_capacity || capacity > SM_CHUNK_MAX_CAPACITY) {
    return;
  }

  size_t increased = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (!*p || *p == 0xff) {
      continue;
    }
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        *p &= ~((sm_bitvec_t)SM_PAYLOAD_ONES << (j * 2));
        *p |= ((sm_bitvec_t)SM_PAYLOAD_ZEROS << (j * 2));
        increased += SM_BITS_PER_VECTOR;
        if (increased + initial_capacity == capacity) {
          __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
          return;
        }
      }
    }
  }
  __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
}

/** @brief Examines the chunk to determine if it is empty.
 *
 * @param[in] chunk The chunk in question.
 * @returns true if this __sm_chunk_t is empty
 */
static bool
__sm_chunk_is_empty(__sm_chunk_t *chunk)
{
  if (chunk->m_data[0] != 0) {
    /* A chunk is considered empty if all flags are SM_PAYLOAD_ZERO or _NONE. */
    register uint8_t *p = (uint8_t *)chunk->m_data;
    for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
      if (*p) {
        for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
          size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
          if (flags != SM_PAYLOAD_NONE && flags != SM_PAYLOAD_ZEROS) {
            return false;
          }
        }
      }
    }
  }
  /* The __sm_chunk_t is empty if all flags (in m_data[0]) are zero. */
  return true;
}

/** @brief Examines the chunk to determine its size.
 *
 * @param[in] chunk The chunk in question.
 * @returns the size of the data buffer, in bytes.
 */
static size_t
__sm_chunk_get_size(__sm_chunk_t *chunk)
{
  /* At least one sm_bitvec_t is required for the flags (m_data[0]) */
  size_t size = sizeof(sm_bitvec_t);
  /* Use a lookup table for each byte of the flags */
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    size += sizeof(sm_bitvec_t) * __sm_chunk_calc_vector_size(*p);
  }

  return size;
}

/** @brief Examines the chunk at \b idx to determine that bit's state (set,
 * or unset).
 *
 * @param[in] chunk The chunk in question.
 * @param[in] idx The 0-based index into this chunk to examine.
 * @returns the value of a bit at index \b idx
 */
static bool
__sm_chunk_is_set(__sm_chunk_t *chunk, size_t idx)
{
  /* in which sm_bitvec_t is |idx| stored? */
  size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  /* now retrieve the flags of that sm_bitvec_t */
  size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, bv);
  switch (flags) {
  case SM_PAYLOAD_ZEROS:
  case SM_PAYLOAD_NONE:
    return false;
  case SM_PAYLOAD_ONES:
    return true;
  default:
    __sm_assert(flags == SM_PAYLOAD_MIXED);
    /* FALLTHROUGH */
  }

  /* get the sm_bitvec_t at |bv| */
  sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, bv)];
  /* and finally check the bit in that sm_bitvec_t */
  return (w & ((sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR))) > 0;
}

/** @brief Assigns a state to a bit in the chunk (set or unset).
 *
 * Sets the value of a bit at index \b idx. Then updates position \b pos to the
 * position of the sm_bitvec_t which is inserted/deleted and \b fill - the value
 * of the fill word (used when growing).
 *
 * @param[in] chunk The chunk in question.
 * @param[in] idx The 0-based index into this chunk to mutate.
 * @param[in] value The new state for the \b idx'th bit.
 * @param[in,out] pos The position of the sm_bitvec_t inserted/deleted within the chunk.
 * @param[in,out] fill The value of the fill word (when growing).
 * @param[in] retired When not retried, grow the chunk by a bitvec.
 * @returns \b SM_NEEDS_TO_GROW, \b SM_NEEDS_TO_SHRINK, or \b SM_OK
 * @note, the caller MUST to perform the relevant actions and call set() again,
 * this time with \b retried = true.
 */
static int
__sm_chunk_set(__sm_chunk_t *chunk, size_t idx, bool value, size_t *pos, sm_bitvec_t *fill, bool retried)
{
  /* In which sm_bitvec_t is |idx| stored? */
  size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  /* Now retrieve the flags of that sm_bitvec_t. */
  size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, bv);
  assert(flags != SM_PAYLOAD_NONE);
  if (flags == SM_PAYLOAD_ZEROS) {
    /* Easy - set bit to 0 in a sm_bitvec_t of zeroes. */
    if (value == false) {
      *pos = 0;
      *fill = 0;
      return SM_OK;
    }
    /* The sparsemap must grow this __sm_chunk_t by one additional sm_bitvec_t,
       then try again. */
    if (!retried) {
      *pos = 1 + __sm_chunk_get_position(chunk, bv);
      *fill = 0;
      return SM_NEEDS_TO_GROW;
    }
    /* New flags are 2#10 meaning SM_PAYLOAD_MIXED. Currently, flags are set
       to 2#00, so 2#00 | 2#10 = 2#10. */
    *chunk->m_data |= ((sm_bitvec_t)SM_PAYLOAD_MIXED << (bv * 2));
    /* FALLTHROUGH */
  } else if (flags == SM_PAYLOAD_ONES) {
    /* Easy - set bit to 1 in a sm_bitvec_t of ones. */
    if (value == true) {
      *pos = 0;
      *fill = 0;
      return SM_OK;
    }
    /* The sparsemap must grow this __sm_chunk_t by one additional sm_bitvec_t,
       then try again. */
    if (!retried) {
      *pos = 1 + __sm_chunk_get_position(chunk, bv);
      *fill = (sm_bitvec_t)-1;
      return SM_NEEDS_TO_GROW;
    }
    /* New flags are 2#10 meaning SM_PAYLOAD_MIXED. Currently, flags are
       set to 2#11, so 2#11 ^ 2#01 = 2#10. */
    chunk->m_data[0] ^= ((sm_bitvec_t)SM_PAYLOAD_NONE << (bv * 2));
    /* FALLTHROUGH */
  }

  /* Now flip the bit. */
  size_t position = 1 + __sm_chunk_get_position(chunk, bv);
  sm_bitvec_t w = chunk->m_data[position];
  if (value) {
    w |= (sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR);
  } else {
    w &= ~((sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR));
  }

  /* If this sm_bitvec_t is now all zeroes or ones then we can remove it. */
  if (w == 0) {
    chunk->m_data[0] &= ~((sm_bitvec_t)SM_PAYLOAD_ONES << (bv * 2));
    *pos = position;
    *fill = 0;
    return SM_NEEDS_TO_SHRINK;
  }
  if (w == (sm_bitvec_t)-1) {
    chunk->m_data[0] |= (sm_bitvec_t)SM_PAYLOAD_ONES << (bv * 2);
    *pos = position;
    *fill = 0;
    return SM_NEEDS_TO_SHRINK;
  }

  chunk->m_data[position] = w;
  *pos = 0;
  *fill = 0;
  return SM_OK;
}

/** @brief Finds the index of the \b n'th bit after \b offset bits with \b
 * value.
 *
 * Scans the \b chunk until after \b offset bits (of any value) have
 * passed and then begins counting the bits that match \b value looking
 * for the \b n'th bit.  It may not be in this chunk, when it is offset is set.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] value Informs what we're seeking, a set or unset bit's position.
 * @param offset[in,out] Sets \b offset to n if the n'th bit was found
 * in this __sm_chunk_t, or reduced value of \b n bits observed the search up
 * to a maximum of SM_BITS_PER_VECTOR.
 * @returns the 0-based index of the n'th set bit when found, otherwise
 * SM_BITS_PER_VECTOR
 */
static size_t
__sm_chunk_select(__sm_chunk_t *chunk, size_t n, ssize_t *offset, bool value)
{
  size_t ret = 0;
  register uint8_t *p;

  p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (*p == 0 && value) {
      ret += (size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        if (value == true) {
          ret += SM_BITS_PER_VECTOR;
          continue;
        } else {
          if (n > SM_BITS_PER_VECTOR) {
            n -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          *offset = -1;
          return ret + n;
        }
      }
      if (flags == SM_PAYLOAD_ONES) {
        if (value == true) {
          if (n > SM_BITS_PER_VECTOR) {
            n -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          *offset = -1;
          return ret + n;
        } else {
          ret += SM_BITS_PER_VECTOR;
          continue;
        }
      }
      if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (value) {
            if (w & ((sm_bitvec_t)1 << k)) {
              if (n == 0) {
                *offset = -1;
                return ret;
              }
              n--;
            }
            ret++;
          } else {
            if (!(w & ((sm_bitvec_t)1 << k))) {
              if (n == 0) {
                *offset = -1;
                return ret;
              }
              n--;
            }
            ret++;
          }
        }
      }
    }
  }
  *offset = n;
  return ret;
}

/** @brief Counts the bits matching \b value in the range [0, \b idx]
 * inclusive after ignoring the first \b offset bits in the chunk.
 *
 * Scans the \b chunk until after \b offset bits (of any value) have
 * passed and then begins counting the bits that match \b value. The
 * result should never be greater than \b idx + 1 maxing out at
 * SM_BITS_PER_VECTOR.  A range of [0, 0] will count 1 bit at \b offset
 * + 1 in this chunk.  A range of [0, 9] will count 10 bits, starting
 * with the 0th and ending with the 9th and return at most a count of
 * 10.
 *
 * @param[in] chunk The chunk in question.
 * @param[in,out] begin Decreases \b offset by the number of bits ignored,
 * at most by SM_BITS_PER_VECTOR.
 * @param[in] end The ending value of the range (inclusive) to count.
 * @param[out] pos_in_chunk The position of the last bit examined in this chunk,
 * always
 * <= SM_BITS_PER_VECTOR, used when counting unset bits that fall within this
 * chunk's range but after the last set bit.
 * @param[out] last_bitvec The last sm_bitvec_t, masked and shifted, so as to be able
 * to examine the bits used in the last portion of the ranking as a way to
 * skip forward during a #span() operation.
 * @param[in] value Informs what we're seeking, set or unset bits.
 * @returns the count of the bits matching \b value within the range.
 */
static size_t
__sm_chunk_rank(__sm_chunk_t *chunk, size_t *begin, size_t end, size_t *pos_in_chunk, sm_bitvec_t *last_bitvec, bool value)
{
  size_t ret = 0;

  *pos_in_chunk = 0;

  /* A chunk can only hold at most SM_CHUNK_MAX_CAPACITY bits, so if
     begin is larger than that, we're basically done. */
  if (*begin >= SM_CHUNK_MAX_CAPACITY) {
    *pos_in_chunk = SM_CHUNK_MAX_CAPACITY;
    *begin -= SM_CHUNK_MAX_CAPACITY;
    return 0;
  }

  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        *last_bitvec = 0;
        if (end >= SM_BITS_PER_VECTOR) {
          *pos_in_chunk += SM_BITS_PER_VECTOR;
          end -= SM_BITS_PER_VECTOR;
          if (*begin >= SM_BITS_PER_VECTOR) {
            *begin = *begin - SM_BITS_PER_VECTOR;
          } else {
            if (value == false) {
              ret += SM_BITS_PER_VECTOR - *begin;
            }
            *begin = 0;
          }
        } else {
          *pos_in_chunk += end + 1;
          if (value == false) {
            if (*begin > end) {
              *begin = *begin - end;
            } else {
              ret += end + 1 - *begin;
              *begin = 0;
              return ret;
            }
          } else {
            return ret;
          }
        }
      } else if (flags == SM_PAYLOAD_ONES) {
        *last_bitvec = UINT64_MAX;
        if (end >= SM_BITS_PER_VECTOR) {
          *pos_in_chunk += SM_BITS_PER_VECTOR;
          end -= SM_BITS_PER_VECTOR;
          if (*begin >= SM_BITS_PER_VECTOR) {
            *begin = *begin - SM_BITS_PER_VECTOR;
          } else {
            if (value == true) {
              ret += SM_BITS_PER_VECTOR - *begin;
            }
            *begin = 0;
          }
        } else {
          *pos_in_chunk += end + 1;
          if (value == true) {
            if (*begin > end) {
              *begin = *begin - end;
            } else {
              ret += end + 1 - *begin;
              *begin = 0;
              return ret;
            }
          } else {
            return ret;
          }
        }
      } else if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        if (end >= SM_BITS_PER_VECTOR) {
          *pos_in_chunk += SM_BITS_PER_VECTOR;
          end -= SM_BITS_PER_VECTOR;
          uint64_t mask = *begin == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (*begin >= 64 ? 64 : *begin)));
          sm_bitvec_t mw;
          if (value == true) {
            mw = w & mask;
          } else {
            mw = ~w & mask;
          }
          size_t pc = popcountll(mw);
          ret += pc;
          *begin = (*begin > SM_BITS_PER_VECTOR) ? *begin - SM_BITS_PER_VECTOR : 0;
        } else {
          *pos_in_chunk += end + 1;
          sm_bitvec_t mw;
          uint64_t mask;
          uint64_t end_mask = (end == 63) ? UINT64_MAX : ((uint64_t)1 << (end + 1)) - 1;
          uint64_t begin_mask = *begin == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (*begin >= 64 ? 64 : *begin)));
          /* To count the set bits we need to mask off the portion of the vector that we need
             to count then call popcount().  So, let's create a mask for the range between
             begin and end inclusive [*begin, end]. */
          mask = end_mask & begin_mask;
          if (value) {
            mw = w & mask;
          } else {
            mw = ~w & mask;
          }
          int pc = popcountll(mw);
          ret += pc;
          *last_bitvec = mw >> ((*begin > 63) ? 63 : *begin);
          *begin = *begin > end ? *begin - end + 1 : 0;
          return ret;
        }
      }
    }
  }
  return ret;
}

/** @brief Calls \b scanner with sm_bitmap_t for each vector in this chunk.
 *
 * Decompresses the whole chunk into separate bitmaps then calls visitor's
 * \b #operator() function for all bits that are set.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] start Starting offset
 * @param[in] scanner Callback function which receives an array of indices (with
 * bits set to 1), the size of the array and an auxiliary pointer provided by
 * the caller.
 * @param[in] skip The number of bits to skip in the beginning.
 * @returns the number of (set) bits that were passed to the scanner
 */
static size_t
__sm_chunk_scan(__sm_chunk_t *chunk, sm_idx_t start, void (*scanner)(sm_idx_t[], size_t, void *aux), size_t skip, void *aux)
{
  size_t ret = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  sm_idx_t buffer[SM_BITS_PER_VECTOR];
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (*p == 0) {
      /* Skip chunks that are all zeroes. */
      skip -= skip > SM_BITS_PER_VECTOR ? SM_BITS_PER_VECTOR : skip;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE || flags == SM_PAYLOAD_ZEROS) {
        /* Skip when all zeroes. */
        skip -= skip > SM_BITS_PER_VECTOR ? SM_BITS_PER_VECTOR : skip;
      } else if (flags == SM_PAYLOAD_ONES) {
        if (skip) {
          if (skip >= SM_BITS_PER_VECTOR) {
            skip -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          size_t n = 0;
          for (size_t b = 0; b < SM_BITS_PER_VECTOR; b++) {
            buffer[n++] = start + ret + b;
          }
          scanner(&buffer[0], n, aux);
          ret += n;
          skip = 0;
        } else {
          for (size_t b = 0; b < SM_BITS_PER_VECTOR; b++) {
            buffer[b] = start + ret + b;
          }
          scanner(&buffer[0], SM_BITS_PER_VECTOR, aux);
          ret += SM_BITS_PER_VECTOR;
        }
      } else if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        size_t n = 0;
        if (skip) {
          if (skip >= SM_BITS_PER_VECTOR) {
            skip -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          for (int b = 0; b < SM_BITS_PER_VECTOR; b++) {
            if (skip > 0) {
              skip--;
              continue;
            }
            if (w & ((sm_bitvec_t)1 << b)) {
              buffer[n++] = start + ret + b;
              ret++;
            }
          }
        } else {
          for (int b = 0; b < SM_BITS_PER_VECTOR; b++) {
            if (w & ((sm_bitvec_t)1 << b)) {
              buffer[n++] = start + ret + b;
            }
          }
          ret += n;
        }
        __sm_assert(n > 0);
        scanner(&buffer[0], n, aux);
      }
    }
  }
  return ret;
}

/** @brief Provides the number of chunks currently in the map.
 *
 * @param[in] chunk  The sparsemap_t in question.
 * @returns the number of chunks in the sparsemap
 */
static size_t
__sm_get_chunk_count(sparsemap_t *map)
{
  return *(uint32_t *)&map->m_data[0];
}

/** @brief Encapsulates the method to find the starting address of a chunk's
 * data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] offset The offset in bytes for the desired chunk.
 * @returns the data for the specified \b offset
 */
static inline uint8_t *
__sm_get_chunk_data(sparsemap_t *map, size_t offset)
{
  return &map->m_data[SM_SIZEOF_OVERHEAD + offset];
}

/** @brief Encapsulates the method to find the address of the first unused byte
 * in \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @returns a pointer after the end of the used data
 */
static uint8_t *
__sm_get_chunk_end(sparsemap_t *map)
{
  uint8_t *p = __sm_get_chunk_data(map, 0);
  size_t count = __sm_get_chunk_count(map);
  for (size_t i = 0; i < count; i++) {
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  return p;
}

/** @brief Provides the byte size amount of \b m_data consumed.
 *
 * @param[in] map The sparsemap_t in question.
 * @returns the used size in the data buffer
 */
static size_t
__sm_get_size_impl(sparsemap_t *map)
{
  uint8_t *start = __sm_get_chunk_data(map, 0);
  uint8_t *p = start;

  size_t count = __sm_get_chunk_count(map);
  for (size_t i = 0; i < count; i++) {
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  return SM_SIZEOF_OVERHEAD + p - start;
}

/** @brief Aligns to SM_BITS_PER_VECTOR a given index \b idx.
 *
 * @param[in] idx The index to align.
 * @returns the aligned offset (aligned to sm_bitvec_t capacity).
 */
static sm_idx_t
__sm_get_vector_aligned_offset(size_t idx)
{
  const size_t capacity = SM_BITS_PER_VECTOR;
  return (idx / capacity) * capacity;
}

/** @brief Aligns to SM_CHUNK_CAPACITY a given index \b idx.
 *
 * @param[in] idx The index to align.
 * @returns the aligned offset (aligned to __sm_chunk_t capacity)
 */
static sm_idx_t
__sm_get_chunk_aligned_offset(size_t idx)
{
  const size_t capacity = SM_CHUNK_MAX_CAPACITY;
  return (idx / capacity) * capacity;
}

/** @brief Provides the byte offset of a chunk at index \b idx.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] idx The index of the chunk to locate.
 * @returns the byte offset of a __sm_chunk_t in m_data, or -1 if there
 * are no chunks.
 */
static ssize_t
__sm_get_chunk_offset(sparsemap_t *map, sparsemap_idx_t idx)
{
  size_t count = __sm_get_chunk_count(map);
  if (count == 0) {
    return -1;
  }

  uint8_t *start = __sm_get_chunk_data(map, 0);
  uint8_t *p = start;

  for (sparsemap_idx_t i = 0; i < count - 1; i++) {
    sm_idx_t s = *(sm_idx_t *)p;
    __sm_assert(s == __sm_get_vector_aligned_offset(s));
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p + sizeof(sm_idx_t));
    if (s >= idx || idx < s + __sm_chunk_get_capacity(&chunk)) {
      break;
    }
    p += sizeof(sm_idx_t) + __sm_chunk_get_size(&chunk);
  }

  return (ssize_t)(p - start);
}

/** @brief Sets the number of __sm_chunk_t's.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] new_count The new number of chunks in the map.
 */
static void
__sm_set_chunk_count(sparsemap_t *map, size_t new_count)
{
  *(uint32_t *)&map->m_data[0] = (uint32_t)new_count;
}

/** @brief Appends raw data at the end of used portion of \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] buffer The bytes to copy into \b m_data.
 * @param[in] buffer_size The size of the byte array \b buffer to copy.
 */
static void
__sm_append_data(sparsemap_t *map, uint8_t *buffer, size_t buffer_size)
{
  __sm_assert(map->m_data_used + buffer_size <= map->m_capacity);

  memcpy(&map->m_data[map->m_data_used], buffer, buffer_size);
  map->m_data_used += buffer_size;
}

/** @brief Inserts data at \b offset in the middle of \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] offset The offset in bytes into \b m_data to place the buffer.
 * @param[in] buffer The bytes to copy into \b m_data.
 * @param[in] buffer_size The size of the byte array \b buffer to copy.
 */
void
__sm_insert_data(sparsemap_t *map, size_t offset, uint8_t *buffer, size_t buffer_size)
{
  __sm_assert(map->m_data_used + buffer_size <= map->m_capacity);

  uint8_t *p = __sm_get_chunk_data(map, offset);
  memmove(p + buffer_size, p, map->m_data_used - offset);
  memcpy(p, buffer, buffer_size);
  map->m_data_used += buffer_size;
}

/** @brief Removes data from \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] offset The offset in bytes into \b m_data at which to excise data.
 * @param[in] gap_size The size of the excision.
 */
static void
__sm_remove_data(sparsemap_t *map, size_t offset, size_t gap_size)
{
  __sm_assert(map->m_data_used >= gap_size);
  uint8_t *p = __sm_get_chunk_data(map, offset);
  memmove(p, p + gap_size, map->m_data_used - offset - gap_size);
  map->m_data_used -= gap_size;
}

/** @brief Merges into the chunk at \b offset all set bits from \b src.
 *
 * @param[in] map The map the chunk belongs too.
 * @param[in] offset The offset of the first bit in the chunk to be merged.
 * @todo merge at the vector level not offset
 */
void
__sm_merge_chunk(sparsemap_t *map, sparsemap_idx_t src_start, sparsemap_idx_t dst_start, sparsemap_idx_t capacity, __sm_chunk_t *dst_chunk,
  __sm_chunk_t *src_chunk)
{
  ssize_t delta = src_start - dst_start;
  for (sparsemap_idx_t j = 0; j < capacity; j++) {
    ssize_t offset = __sm_get_chunk_offset(map, src_start + j);
    if (__sm_chunk_is_set(src_chunk, j) && !__sm_chunk_is_set(dst_chunk, j + delta)) {
      size_t position;
      sm_bitvec_t fill;
      switch (__sm_chunk_set(dst_chunk, j + delta, true, &position, &fill, false)) {
      case SM_NEEDS_TO_GROW:
        offset += sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t);
        __sm_insert_data(map, offset, (uint8_t *)&fill, sizeof(sm_bitvec_t));
        __sm_chunk_set(dst_chunk, j + delta, true, &position, &fill, true);
        break;
      case SM_NEEDS_TO_SHRINK:
        if (__sm_chunk_is_empty(src_chunk)) {
          __sm_assert(position == 1);
          __sm_remove_data(map, offset, sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2);
          __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
        } else {
          offset += sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t);
          __sm_remove_data(map, offset, sizeof(sm_bitvec_t));
        }
        break;
      case SM_OK:
      default:
        break;
      }
    }
  }
}

/*
 * The following is the "Sparsemap" implementation, it uses chunks (code above)
 * and is the public API for this compressed bitmap representation.
 */

void
sparsemap_clear(sparsemap_t *map)
{
  if (map == NULL) {
    return;
  }
  memset(map->m_data, 0, map->m_capacity);
  map->m_data_used = SM_SIZEOF_OVERHEAD;
  __sm_set_chunk_count(map, 0);
}

sparsemap_t *
sparsemap(size_t size)
{
  if (size == 0) {
    size = 1024;
  }

  size_t data_size = (size * sizeof(uint8_t));

  /* Ensure that m_data is 8-byte aligned. */
  size_t total_size = sizeof(sparsemap_t) + data_size;
  size_t padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
  total_size += padding;

  sparsemap_t *map = (sparsemap_t *)calloc(1, total_size);
  if (map) {
    uint8_t *data = (uint8_t *)(((uintptr_t)map + sizeof(sparsemap_t)) & ~(uintptr_t)7);
    sparsemap_init(map, data, size);
    __sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(map->m_data)); });
  }
  return map;
}

sparsemap_t *
sparsemap_copy(sparsemap_t *other)
{
  size_t cap = sparsemap_get_capacity(other);
  sparsemap_t *map = sparsemap(cap);
  if (map) {
    map->m_capacity = other->m_capacity;
    map->m_data_used = other->m_data_used;
    memcpy(map->m_data, other->m_data, cap);
  }
  return map;
}

sparsemap_t *
sparsemap_wrap(uint8_t *data, size_t size)
{
  sparsemap_t *map = (sparsemap_t *)calloc(1, sizeof(sparsemap_t));
  if (map) {
    map->m_data = data;
    map->m_data_used = 0;
    map->m_capacity = size;
  }
  return map;
}

void
sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size)
{
  map->m_data = data;
  map->m_data_used = 0;
  map->m_capacity = size;
  sparsemap_clear(map);
}

void
sparsemap_open(sparsemap_t *map, uint8_t *data, size_t size)
{
  map->m_data = data;
  map->m_data_used = __sm_get_size_impl(map);
  map->m_capacity = size;
}

sparsemap_t *
sparsemap_set_data_size(sparsemap_t *map, uint8_t *data, size_t size)
{
  size_t data_size = (size * sizeof(uint8_t));

  /* If this sparsemap was allocated by the sparsemap() API and we're not handed
     a new data, it's up to us to resize it. */
  if (data == NULL && (uintptr_t)map->m_data == (uintptr_t)map + sizeof(sparsemap_t) && size > map->m_capacity) {

    /* Ensure that m_data is 8-byte aligned. */
    size_t total_size = sizeof(sparsemap_t) + data_size;
    size_t padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
    total_size += padding;

    sparsemap_t *m = (sparsemap_t *)realloc(map, total_size);
    if (!m) {
      return NULL;
    }
    memset(((uint8_t *)m) + sizeof(sparsemap_t) + (m->m_capacity * sizeof(uint8_t)), 0, size - m->m_capacity + padding);
    m->m_capacity = data_size;
    m->m_data = (uint8_t *)(((uintptr_t)m + sizeof(sparsemap_t)) & ~(uintptr_t)7);
    __sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(m->m_data)); }) return m;
  } else {
    /* NOTE: It is up to the caller to realloc their buffer and provide it here
       for reassignment. */
    if (data != NULL && data != map->m_data) {
      map->m_data = data;
    }
    map->m_capacity = size;
    return map;
  }
}

double
sparsemap_capacity_remaining(sparsemap_t *map)
{
  if (map->m_data_used >= map->m_capacity) {
    return 0;
  }
  if (map->m_capacity == 0) {
    return 100.0;
  }
  return 100 - (((double)map->m_data_used / (double)map->m_capacity) * 100);
}

size_t
sparsemap_get_capacity(sparsemap_t *map)
{
  return map->m_capacity;
}

bool
sparsemap_is_set(sparsemap_t *map, sparsemap_idx_t idx)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Get the __sm_chunk_t which manages this index */
  ssize_t offset = __sm_get_chunk_offset(map, idx);

  /* No __sm_chunk_t's available -> the bit is not set */
  if (offset == -1) {
    return false;
  }

  /* Otherwise load the __sm_chunk_t */
  uint8_t *p = __sm_get_chunk_data(map, offset);
  sm_idx_t start = *(sm_idx_t *)p;
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p + sizeof(sm_idx_t));

  /* Determine if the bit is out of bounds of the __sm_chunk_t; if yes then
  the bit is not set. */
  if (idx < start || (unsigned long)idx - start >= __sm_chunk_get_capacity(&chunk)) {
    return false;
  }

  /* Otherwise ask the __sm_chunk_t whether the bit is set. */
  return __sm_chunk_is_set(&chunk, idx - start);
}

sparsemap_idx_t
sparsemap_set(sparsemap_t *map, sparsemap_idx_t idx, bool value)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Get the __sm_chunk_t which manages this index */
  ssize_t offset = __sm_get_chunk_offset(map, idx);
  bool dont_grow = false;
  if (map->m_data_used + sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2 > map->m_capacity) {
    errno = ENOSPC;
    return SPARSEMAP_IDX_MAX;
  }

  /* If there are no __sm_chunk_t and the bit is set to zero then return
     immediately; otherwise create an initial __sm_chunk_t. */
  if (offset == -1) {
    if (value == false) {
      return idx;
    }

    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    __sm_append_data(map, &buf[0], sizeof(buf));

    uint8_t *p = __sm_get_chunk_data(map, 0);
    *(sm_idx_t *)p = __sm_get_chunk_aligned_offset(idx);

    __sm_set_chunk_count(map, 1);

    /* We already inserted an additional sm_bitvec_t; given that has happened
       there is no need to grow the vector even further. */
    dont_grow = true;
    offset = 0;
  }

  /* Load the __sm_chunk_t */
  uint8_t *p = __sm_get_chunk_data(map, offset);
  sm_idx_t start = *(sm_idx_t *)p;
  __sm_assert(start == __sm_get_vector_aligned_offset(start));

  /* The new index is smaller than the first __sm_chunk_t: create a new
     __sm_chunk_t and insert it at the front. */
  if (idx < start) {
    if (value == false) {
      /* nothing to do */
      return idx;
    }

    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    __sm_insert_data(map, offset, &buf[0], sizeof(buf));

    size_t aligned_idx = __sm_get_chunk_aligned_offset(idx);
    if (start - aligned_idx < SM_CHUNK_MAX_CAPACITY) {
      __sm_chunk_t chunk;
      __sm_chunk_init(&chunk, p + sizeof(sm_idx_t));
      __sm_chunk_reduce_capacity(&chunk, start - aligned_idx);
    }
    *(sm_idx_t *)p = start = aligned_idx;

    /* We just added another chunk! */
    __sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

    /* We already inserted an additional sm_bitvec_t; later on there
      is no need to grow the vector even further. */
    dont_grow = true;
  }

  /* A __sm_chunk_t exists, but the new index exceeds its capacities: create
     a new __sm_chunk_t and insert it after the current one. */
  else {
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p + sizeof(sm_idx_t));
    if (idx - start >= (sparsemap_idx_t)__sm_chunk_get_capacity(&chunk)) {
      if (value == false) {
        /* nothing to do */
        return idx;
      }

      size_t size = __sm_chunk_get_size(&chunk);
      offset += (sizeof(sm_idx_t) + size);
      p += sizeof(sm_idx_t) + size;

      uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
      __sm_insert_data(map, offset, &buf[0], sizeof(buf));

      start += __sm_chunk_get_capacity(&chunk);
      if ((sparsemap_idx_t)start + SM_CHUNK_MAX_CAPACITY <= idx) {
        start = __sm_get_chunk_aligned_offset(idx);
      }
      *(sm_idx_t *)p = start;
      __sm_assert(start == __sm_get_vector_aligned_offset(start));

      /* We just added another chunk! */
      __sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

      /* We already inserted an additional sm_bitvec_t; later on there
         is no need to grow the vector even further. */
      dont_grow = true;
    }
  }

  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p + sizeof(sm_idx_t));

  /* Now update the __sm_chunk_t. */
  size_t position;
  sm_bitvec_t fill;
  int code = __sm_chunk_set(&chunk, idx - start, value, &position, &fill, false);
  switch (code) {
  case SM_OK:
    break;
  case SM_NEEDS_TO_GROW:
    if (!dont_grow) {
      offset += (sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t));
      __sm_insert_data(map, offset, (uint8_t *)&fill, sizeof(sm_bitvec_t));
    }
    __sm_chunk_set(&chunk, idx - start, value, &position, &fill, true);
    break;
  case SM_NEEDS_TO_SHRINK:
    /* If the __sm_chunk_t is empty then remove it. */
    if (__sm_chunk_is_empty(&chunk)) {
      __sm_assert(position == 1);
      __sm_remove_data(map, offset, sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2);
      __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
    } else {
      offset += (sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t));
      __sm_remove_data(map, offset, sizeof(sm_bitvec_t));
    }
    break;
  default:
    __sm_assert(!"shouldn't be here");
#ifdef DEBUG
    abort();
#endif
    break;
  }
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  return idx;
}

sparsemap_idx_t
sparsemap_get_starting_offset(sparsemap_t *map)
{
  sparsemap_idx_t offset = 0;
  size_t count = __sm_get_chunk_count(map);
  if (count == 0) {
    return 0;
  }
  uint8_t *p = __sm_get_chunk_data(map, 0);
  sparsemap_idx_t relative_position = *(sm_idx_t *)p;
  p += sizeof(sm_idx_t);
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p);
  for (size_t m = 0; m < sizeof(sm_bitvec_t); m++, p++) {
    for (int n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      } else if (flags == SM_PAYLOAD_ZEROS) {
        relative_position += SM_BITS_PER_VECTOR;
      } else if (flags == SM_PAYLOAD_ONES) {
        offset = relative_position;
        goto done;
      } else if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = chunk.m_data[1 + __sm_chunk_get_position(&chunk, m * SM_FLAGS_PER_INDEX_BYTE + n)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (w & ((sm_bitvec_t)1 << k)) {
            offset = relative_position + k;
            goto done;
          }
        }
        relative_position += SM_BITS_PER_VECTOR;
      }
    }
  }
done:;
  return offset;
}

sparsemap_idx_t
sparsemap_get_ending_offset(sparsemap_t *map)
{
  sparsemap_idx_t offset = 0;
  size_t count = __sm_get_chunk_count(map);
  if (count == 0) {
    return 0;
  }
  uint8_t *p = __sm_get_chunk_data(map, 0);
  for (size_t i = 0; i < count - 1; i++) {
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  sm_idx_t start = *(sm_idx_t *)p;
  p += sizeof(sm_idx_t);
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p);
  sparsemap_idx_t relative_position = start;
  for (size_t m = 0; m < sizeof(sm_bitvec_t); m++, p++) {
    for (int n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      } else if (flags == SM_PAYLOAD_ZEROS) {
        relative_position += SM_BITS_PER_VECTOR;
      } else if (flags == SM_PAYLOAD_ONES) {
        relative_position += SM_BITS_PER_VECTOR;
        offset = relative_position;
      } else if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = chunk.m_data[1 + __sm_chunk_get_position(&chunk, m * SM_FLAGS_PER_INDEX_BYTE + n)];
        int idx = 0;
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (w & ((sm_bitvec_t)1 << k)) {
            idx = k;
          }
        }
        offset = relative_position + idx;
        relative_position += SM_BITS_PER_VECTOR;
      }
    }
  }
  return offset;
}

double
sparsemap_fill_factor(sparsemap_t *map)
{
  size_t rank = sparsemap_rank(map, 0, SPARSEMAP_IDX_MAX, true);
  sparsemap_idx_t end = sparsemap_get_ending_offset(map);
  return (double)rank / (double)end * 100.0;
}

void *
sparsemap_get_data(sparsemap_t *map)
{
  return map->m_data;
}

size_t
sparsemap_get_size(sparsemap_t *map)
{
  if (map->m_data_used) {
    size_t size = __sm_get_size_impl(map);
    if (size != map->m_data_used) {
      map->m_data_used = size;
    }
    __sm_when_diag({ __sm_assert(map->m_data_used == __sm_get_size_impl(map)); });
    return map->m_data_used;
  }
  return map->m_data_used = __sm_get_size_impl(map);
}

size_t
sparsemap_count(sparsemap_t *map)
{
  return sparsemap_rank(map, 0, SPARSEMAP_IDX_MAX, true);
}

void
sparsemap_scan(sparsemap_t *map, void (*scanner)(sm_idx_t[], size_t, void *aux), size_t skip, void *aux)
{
  uint8_t *p = __sm_get_chunk_data(map, 0);
  size_t count = __sm_get_chunk_count(map);

  for (size_t i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)p;
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    size_t skipped = __sm_chunk_scan(&chunk, start, scanner, skip, aux);
    if (skip) {
      assert(skip >= skipped);
      skip -= skipped;
    }
    p += __sm_chunk_get_size(&chunk);
  }
}

int
sparsemap_merge(sparsemap_t *destination, sparsemap_t *source)
{
  uint8_t *src, *dst;
  size_t src_count = __sm_get_chunk_count(source);
  sparsemap_idx_t dst_ending_offset = sparsemap_get_ending_offset(destination);

  if (src_count == 0) {
    return 0;
  }

  ssize_t remaining_capacity = destination->m_capacity - destination->m_data_used -
    (source->m_data_used + src_count * (sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2));

  /* Estimate worst-case overhead required for merge. */
  if (remaining_capacity <= 0) {
    errno = ENOSPC;
    return -remaining_capacity;
  }

  src = __sm_get_chunk_data(source, 0);
  while (src_count) {
    sm_idx_t src_start = *(sm_idx_t *)src;
    __sm_chunk_t src_chunk;
    __sm_chunk_init(&src_chunk, src + sizeof(sm_idx_t));
    size_t src_capacity = __sm_chunk_get_capacity(&src_chunk);
    ssize_t dst_offset = __sm_get_chunk_offset(destination, src_start);
    if (dst_offset >= 0) {
      dst = __sm_get_chunk_data(destination, dst_offset);
      sm_idx_t dst_start = *(sm_idx_t *)dst;
      __sm_chunk_t dst_chunk;
      __sm_chunk_init(&dst_chunk, dst + sizeof(sm_idx_t));
      size_t dst_capacity = __sm_chunk_get_capacity(&dst_chunk);

      /* Try to expand the capacity if there's room before the start of the next chunk. */
      if (src_start == dst_start && dst_capacity < src_capacity) {
        ssize_t nxt_offset = __sm_get_chunk_offset(destination, dst_start + dst_capacity + 1);
        uint8_t *nxt_dst = __sm_get_chunk_data(destination, nxt_offset);
        sm_idx_t nxt_dst_start = *(sm_idx_t *)nxt_dst;
        if (nxt_dst_start > dst_start + src_capacity) {
          __sm_chunk_increase_capacity(&dst_chunk, src_capacity);
          dst_capacity = __sm_chunk_get_capacity(&dst_chunk);
        }
      }

      /* Source chunk precedes next destination chunk. */
      if ((src_start + src_capacity) <= dst_start) {
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        ssize_t offset = __sm_get_chunk_offset(destination, dst_start);
        __sm_insert_data(destination, offset, src, sizeof(sm_idx_t) + src_size);
        /* Update the chunk count and data_used. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);
        src_count--;
        src += sizeof(sm_idx_t) + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      /* Source chunk follows next destination chunk. */
      if (src_start >= (dst_start + dst_capacity)) {
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        if (dst_offset == __sm_get_chunk_offset(destination, SPARSEMAP_IDX_MAX)) {
          __sm_append_data(destination, src, sizeof(sm_idx_t) + src_size);
        } else {
          ssize_t offset = __sm_get_chunk_offset(destination, src_start);
          __sm_insert_data(destination, offset, src, sizeof(sm_idx_t) + src_size);
        }
        /* Update the chunk count and data_used. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);
        src_count--;
        src += sizeof(sm_idx_t) + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      /* Source and destination and a perfect overlapping pair. */
      if (src_start == dst_start && src_capacity == dst_capacity) {
        __sm_merge_chunk(destination, src_start, dst_start, dst_capacity, &dst_chunk, &src_chunk);
        src_count--;
        src += sizeof(sm_idx_t) + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      /* Non-uniform overlapping chunks. */
      if (dst_start < src_start || (dst_start == src_start && dst_capacity != src_capacity)) {
        size_t src_end = src_start + src_capacity;
        size_t dst_end = dst_start + dst_capacity;
        size_t overlap = src_end > dst_end ? src_capacity - (src_end - dst_end) : src_capacity;
        __sm_merge_chunk(destination, src_start, dst_start, overlap, &dst_chunk, &src_chunk);
        for (size_t n = src_start + overlap; n <= src_end; n++) {
          if (sparsemap_is_set(source, n)) {
            sparsemap_set(destination, n, true);
          }
        }
        src_count--;
        src += sizeof(sm_idx_t) + __sm_chunk_get_size(&src_chunk);
        continue;
      }
    } else {
      if (src_start >= dst_ending_offset) {
        /* Starting offset is after destination chunks, so append data. */
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        __sm_append_data(destination, src, sizeof(sm_idx_t) + src_size);

        /* Update the chunk count and data_used. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);

        src_count--;
        src += sizeof(sm_idx_t) + __sm_chunk_get_size(&src_chunk);
        continue;
      } else {
        /* Source chunk precedes next destination chunk. */
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        ssize_t offset = __sm_get_chunk_offset(destination, src_start);
        __sm_insert_data(destination, offset, src, sizeof(sm_idx_t) + src_size);

        /* Update the chunk count and data_used. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);

        src_count--;
        src += sizeof(sm_idx_t) + __sm_chunk_get_size(&src_chunk);
        continue;
      }
    }
  }
  return 0;
}

sparsemap_idx_t
sparsemap_split(sparsemap_t *map, sparsemap_idx_t offset, sparsemap_t *other)
{
  if (!(offset == SPARSEMAP_IDX_MAX) && offset >= sparsemap_get_ending_offset(map)) {
    return 0;
  }

  if (offset == SPARSEMAP_IDX_MAX) {
    sparsemap_idx_t begin = sparsemap_get_starting_offset(map);
    sparsemap_idx_t end = sparsemap_get_ending_offset(map);
    if (begin != end) {
      size_t count = sparsemap_rank(map, begin, end, true);
      offset = sparsemap_select(map, count / 2, true);
    } else {
      return SPARSEMAP_IDX_MAX;
    }
  }

  /* |dst| points to the destination buffer */
  uint8_t *dst = __sm_get_chunk_end(other);

  /* |src| points to the source-chunk */
  uint8_t *src = __sm_get_chunk_data(map, 0);

  bool in_middle = false;
  uint8_t *prev = src;
  size_t i, count = __sm_get_chunk_count(map);
  for (i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)src;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, src + sizeof(sm_idx_t));
    if (start == offset) {
      break;
    }
    if (start + __sm_chunk_get_capacity(&chunk) > (unsigned long)offset) {
      in_middle = true;
      break;
    }
    if (start > offset) {
      src = prev;
      i--;
      break;
    }

    prev = src;
    src += sizeof(sm_idx_t) + __sm_chunk_get_size(&chunk);
  }
  if (i == count) {
    __sm_assert(sparsemap_get_size(map) > SM_SIZEOF_OVERHEAD);
    __sm_assert(sparsemap_get_size(other) > SM_SIZEOF_OVERHEAD);
    return offset;
  }

  /* Now copy all the remaining chunks. */
  int moved = 0;

  /* If |offset| is in the middle of a chunk then this chunk has to be split */
  if (in_middle) {
    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    memcpy(dst, &buf[0], sizeof(buf));

    *(sm_idx_t *)dst = __sm_get_vector_aligned_offset(offset);
    dst += sizeof(sm_idx_t);

    /* the |other| sparsemap_t now has one additional chunk */
    __sm_set_chunk_count(other, __sm_get_chunk_count(other) + 1);
    if (other->m_data_used != 0) {
      other->m_data_used += sizeof(sm_idx_t) + sizeof(sm_bitvec_t);
    }

    sm_idx_t start = *(sm_idx_t *)src;
    src += sizeof(sm_idx_t);
    __sm_chunk_t s_chunk;
    __sm_chunk_init(&s_chunk, src);
    size_t capacity = __sm_chunk_get_capacity(&s_chunk);

    __sm_chunk_t d_chunk;
    __sm_chunk_init(&d_chunk, dst);
    __sm_chunk_reduce_capacity(&d_chunk, __sm_get_vector_aligned_offset(capacity - (offset % capacity)));

    /* Now copy the bits. */
    sparsemap_idx_t b = __sm_get_vector_aligned_offset(offset % capacity);
    for (size_t j = start; j < capacity + start; j++) {
      if (j >= offset) {
        if (__sm_chunk_is_set(&s_chunk, j - start)) {
          sparsemap_set(other, j, true);
          if (j >= b && (j - b) % capacity < SM_BITS_PER_VECTOR) {
            sparsemap_set(map, j, false);
          }
        }
      }
    }

    src += __sm_chunk_get_size(&s_chunk);
    size_t dsize = __sm_chunk_get_size(&d_chunk);
    dst += dsize;
    i++;

    /* Reduce the capacity of the source-chunk effectively erases bits. */
    size_t r = __sm_get_vector_aligned_offset(((offset - start) % capacity) + SM_BITS_PER_VECTOR);
    __sm_chunk_reduce_capacity(&s_chunk, r);
  }

  /* Now continue with all remaining chunks. */
  for (; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)src;
    src += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, src);
    size_t s = __sm_chunk_get_size(&chunk);

    *(sm_idx_t *)dst = start;
    dst += sizeof(sm_idx_t);
    memcpy(dst, src, s);
    src += s;
    dst += s;

    moved++;
  }

  /* Force new calculation. */
  other->m_data_used = 0;
  map->m_data_used = 0;

  /* Update the Chunk counters. */
  __sm_set_chunk_count(map, __sm_get_chunk_count(map) - moved);
  __sm_set_chunk_count(other, __sm_get_chunk_count(other) + moved);

  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  __sm_assert(sparsemap_get_size(other) > SM_SIZEOF_OVERHEAD);

  return offset;
}

sparsemap_idx_t
sparsemap_select(sparsemap_t *map, sparsemap_idx_t n, bool value)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  sm_idx_t start;
  size_t count = __sm_get_chunk_count(map);

  if (count == 0 && value == false) {
    return n;
  }

  uint8_t *p = __sm_get_chunk_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    start = *(sm_idx_t *)p;
    /* Start of this chunk is greater than n meaning there are a set of 0s
       before the first 1 sufficient to consume n. */
    if (value == false && i == 0 && start > n) {
      return n;
    }
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);

    ssize_t new_n = n;
    size_t index = __sm_chunk_select(&chunk, n, &new_n, value);
    if (new_n == -1) {
      return start + index;
    }
    n = new_n;

    p += __sm_chunk_get_size(&chunk);
  }
  return SPARSEMAP_IDX_MAX;
}

static size_t
__sm_rank_vec(sparsemap_t *map, size_t begin, size_t end, bool value, sm_bitvec_t *vec)
{
  assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  size_t amt, gap, pos = 0, result = 0, prev = 0, count, len = end - begin + 1;
  uint8_t *p;

  if (begin > end) {
    return 0;
  }

  count = __sm_get_chunk_count(map);

  if (count == 0) {
    if (value == false) {
      /* The count/rank of unset bits in an empty map is inf, so what you requested is the answer. */
      return len;
    }
  }

  p = __sm_get_chunk_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)p;
    /* [prev, start + pos), prev is the last bit examined 0-based. */
    if (i == 0) {
      gap = start;
    } else {
      if (prev + SM_CHUNK_MAX_CAPACITY == start) {
        gap = 0;
      } else {
        gap = start - (prev + pos);
      }
    }
    /* Start of this chunk is greater than the end of the desired range. */
    if (start > end) {
      if (value == true) {
        /* We're counting set bits and this chunk starts after the range
           [begin, end], we're done. */
        return result;
      } else {
        if (i == 0) {
          /* We're counting unset bits and the first chunk starts after the
             range meaning everything proceeding this chunk was zero and should
             be counted, also we're done. */
          result += (end - begin) + 1;
          return result;
        } else {
          /* We're counting unset bits and some chunk starts after the range, so
             we've counted enough, we're done. */
          if (pos > end) {
            return result;
          } else {
            if (end - pos < gap) {
              result += end - pos;
              return result;
            } else {
              result += gap;
              return result;
            }
          }
        }
      }
    } else {
      /* The range and this chunk overlap. */
      if (value == false) {
        if (begin > gap) {
          begin -= gap;
        } else {
          result += gap - begin;
          begin = 0;
        }
      } else {
        if (begin >= gap) {
          begin -= gap;
        }
      }
    }
    prev = start;
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);

    /* Count all the set/unset inside this chunk. */
    amt = __sm_chunk_rank(&chunk, &begin, end - start, &pos, vec, value);
    result += amt;
    p += __sm_chunk_get_size(&chunk);
  }
  /* Count any additional unset bits that fall outside the last chunk but
     within the range. */
  if (value == false) {
    size_t last = prev - 1 + pos;
    if (end > last) {
      result += end - last - begin;
    }
  }
  return result;
}

size_t
sparsemap_rank(sparsemap_t *map, size_t begin, size_t end, bool value)
{
  sm_bitvec_t vec;
  return __sm_rank_vec(map, begin, end, value, &vec);
}

size_t
sparsemap_span(sparsemap_t *map, sparsemap_idx_t idx, size_t len, bool value)
{
  size_t rank, nth;
  sm_bitvec_t vec = 0;
  sparsemap_idx_t offset;

  /* When skipping forward to `idx` offset in the map we can determine how
     many selects we can avoid by taking the rank of the range and starting
     at that bit. */
  nth = (idx == 0) ? 0 : sparsemap_rank(map, 0, idx - 1, value);
  if (SPARSEMAP_NOT_FOUND(nth)) {
    return nth;
  }
  /* Find the first bit that matches value, then... */
  offset = sparsemap_select(map, nth, value);
  do {
    /* See if the rank of the bits in the range starting at offset is equal
       to the desired amount. */
    rank = (len == 1) ? 1 : __sm_rank_vec(map, offset, offset + len - 1, value, &vec);
    if (rank >= len) {
      /* We've found what we're looking for, return the index of the first
         bit in the range. */
      break;
    }
    /* Now we try to jump forward as much as possible before we look for a
       new match. We do this by counting the remaining bits in the returned
       vec from the call to rank_vec(). */
    int amt = 1;
    if (vec > 0) {
      /* The returned vec had som set bits, let's move forward in the map as much
         as possible (max: 64 bit positions). */
      int max = len > SM_BITS_PER_VECTOR ? SM_BITS_PER_VECTOR : len;
      while (amt < max && (vec & 1 << amt)) {
        amt++;
      }
    }
    nth += amt;
    offset = sparsemap_select(map, nth, value);
  } while (SPARSEMAP_FOUND(offset));

  return offset;
}

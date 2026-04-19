#define _POSIX_C_SOURCE 199309L
#define X86_INTRIN

#include <assert.h>
#include <pthread.h> // If using threads
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __x86_64__ // Check if running on x86_64 architecture
#ifdef X86_INTRIN
#include <errno.h>
#include <x86intrin.h>
#endif
#endif

#include "common.h"
#include "sparsemap.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(...)                                                \
  do {                                                             \
    fprintf(stderr, "%s:%d:%s(): ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, __VA_ARGS__);                                  \
  } while (0)
#pragma GCC diagnostic pop

uint64_t
tsc(void)
{
#ifdef __x86_64__ // Check if running on x86_64 architecture
#ifdef X86_INTRIN
  return __rdtsc();
#else
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
#endif
#ifdef __arm__ // Check if compiling for ARM architecture
  uint64_t result;
  __asm__ volatile("mrs %0, pmccntr_el0" : "=r"(result));
  return result;
}
#endif
#endif
return 0;
}

/* Debug helpers for sparsemap testing */
#ifdef SPARSEMAP_TESTING
char *QCC_showSparsemap(void *value, int len);
char *QCC_showChunk(void *value, int len);
#endif

// get microsecond timestamp
uint64_t
msts()
{
#ifdef _SC_MONOTONIC_CLOCK
  struct timespec ts;
  if (sysconf(_SC_MONOTONIC_CLOCK) > 0) {
    /* A monotonic clock presents */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
      return (uint64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    else
      return 0;
  }
  return 0;
#else
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0)
    return (uint64_t)(tv.tv_sec * 1000000 + tv.tv_usec);
  else
    return 0;
#endif
}

double
nsts(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    perror("clock_gettime");
    return -1.0; // Return -1.0 on error
  }
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

int __xorshift32_state = 0;

// Xorshift algorithm for PRNG
uint32_t
xorshift32(void)
{
  uint32_t x = __xorshift32_state;
  if (x == 0) {
    x = 123456789;
  }
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  __xorshift32_state = x;
  return x;
}

void
xorshift32_seed(void)
{
  __xorshift32_state = XORSHIFT_SEED_VALUE;
}

void
shuffle(int *array, size_t n)
{
  for (size_t i = n - 1; i > 0; --i) {
    size_t j = xorshift32() % (i + 1);
    if (i != j) {
      array[i] ^= array[j];
      array[j] ^= array[i];
      array[i] ^= array[j];
    }
  }
}

static int
compare_ints(const void *a, const void *b)
{
  return *(const int *)a - *(const int *)b;
}

// Check if there's already a sequence of 'r' sequential integers
int
has_sequential_set(int a[], int l, int r)
{
  // Start with a count of 1 for the first number
  int count = 1;
  for (int i = 1; i < l; ++i) {
    // Check if the current and previous elements are sequential
    if (a[i] - a[i - 1] == 1) {
      count++;
      if (count >= r) {
        // Found a sequential set of length 'r' starting at 'i'
        return i;
      }
    } else {
      // Reset count if the sequence breaks
      count = 1;
    }
  }
  // No sequential set of length 'r' found
  return -1;
}

// Function to ensure an array contains a set of 'r' sequential integers
int
ensure_sequential_set(int a[], int l, int r)
{
  if (!a || l == 0 || r < 1 || r > l - 1) {
    return 0;
  }

  // Sort the array to check for existing sequences
  qsort(a, l, sizeof(int), compare_ints);

  // Check if a sequential set of length 'r' already exists
  int offset = has_sequential_set(a, l, r);
  if (offset >= 0) {
    return offset; // Sequence already exists, no modification needed
  }

  // Find the minimum and maximum values in the array
  int min_value = a[0];
  int max_value = a[l - 1];

  // Generate a random value between min_value and max_value
  int value = random_uint32() % (max_value - min_value - r + 1);
  // Generate a random location between 0 and l - r
  int d = l - r - 1;
  offset = d == 0 ? 0 : random_uint32() % d;

  // Adjust the array to include a sequential set of 'r' integers at the random offset
  for (int i = 0; i < r; ++i) {
    a[i + offset] = value + i;
  }
  return value;
}

void
print_array(int *array, int l)
{
  int a[l];
  memcpy(a, array, sizeof(int) * l);
  qsort(a, l, sizeof(int), compare_ints);

  fprintf(stderr, "int a[] = {");
  for (int i = 0; i < l; i++) {
    fprintf(stderr, "%d", a[i]);
    if (i != l - 1) {
      fprintf(stderr, ", ");
    }
  }
  fprintf(stderr, "};\n");
}

bool
has_span(sparsemap_t *map, int *array, int l, int n)
{
  if (n == 0 || l == 0 || n > l) {
    return false;
  }

  int sorted[l];
  memcpy(sorted, array, sizeof(int) * l);
  qsort(sorted, l, sizeof(int), compare_ints);

  for (int i = 0; i <= l - n; i++) {
    if (sorted[i] + n - 1 == sorted[i + n - 1]) {
      for (int j = 0; j < n; j++) {
        size_t pos = sorted[j + i];
        bool set = sparsemap_is_set(map, pos);
        assert(set);
      }
      __diag("Found span: [%d, %d], length: %d\n", sorted[i], sorted[i + n - 1], n);
      return true;
    }
  }

  return false;
}

bool
is_span(int *array, int n, int x, int l)
{
  if (n == 0 || l < 0) {
    return false;
  }

  int a[n];
  memcpy(a, array, sizeof(int) * n);
  qsort(a, n, sizeof(int), compare_ints);

  // Iterate through the array to find a span starting at x of length l
  for (int i = 0; i < n; i++) {
    if (a[i] == x) {
      // Check if the span can fit in the array
      if (i + l - 1 < n && a[i + l - 1] == x + l - 1) {
        return true; // Found the span
      }
    }
  }
  return false; // Span not found
}

void
print_spans(int *array, int n)
{
  int a[n];
  size_t start = 0, end = 0;

  if (n == 0) {
    fprintf(stderr, "Array is empty\n");
    return;
  }

  memcpy(a, array, sizeof(int) * n);
  qsort(a, n, sizeof(int), compare_ints);

  for (int i = 1; i < n; i++) {
    if (a[i] == a[i - 1] + 1) {
      end = i; // Extend the span
    } else {
      // Print the current span
      if (start == end) {
        fprintf(stderr, "[%d] ", a[start]);
      } else {
        fprintf(stderr, "[%d, %d] ", a[start], a[end]);
      }
      // Move to the next span
      start = i;
      end = i;
    }
  }

  // Print the last span if needed
  if (start == end) {
    fprintf(stderr, "[%d]\n", a[start]);
  } else {
    fprintf(stderr, "[%d, %d]\n", a[start], a[end]);
  }
}

bool
is_set(const int array[], int bit)
{
  for (int i = 0; i < 1024; i++) {
    if (array[i] == bit) {
      return true;
    }
  }
  return false;
}

int
whats_set_uint64(uint64_t number, int pos[64])
{
  int length = 0;

  for (int i = 0; i < 64; i++) {
    if (number & ((uint64_t)1 << i)) {
      pos[length++] = i;
    }
  }

  return length;
}

/** @brief Fills an array with unique random values between 0 and max_value.
 *
 * @param[in] a The array to fill.
 * @param[in] l The length of the array to fill.
 * @param[in] max_value The maximum value for the random numbers.
 */
void
setup_test_array(int a[], int l, int max_value)
{

  // Create a set to store the unique values.
  int unique_values[max_value + 1];
  for (int i = 0; i <= max_value; ++i) {
    unique_values[i] = 0;
  }

  // Keep generating random numbers until we have l unique values.
  int count = 0;
  while (count < l) {
    int random_number = random_uint32() % (max_value + 1);
    if (unique_values[random_number] == 0) {
      unique_values[random_number] = 1;
      a[count] = random_number;
      count++;
    }
  }
}

void
bitmap_from_uint32(sparsemap_t *map, uint32_t number)
{
  for (int i = 0; i < 32; i++) {
    bool bit = number & (1 << i);
    sparsemap_assign(map, i, bit);
  }
}

uint32_t
rank_uint64(uint64_t number, int n, int p)
{
  if (p < n || p > 63) {
    return 0;
  }

  /* Create a mask for the range between n and p.
     This works by shifting 1 to the left (p+1) times, subtracting 1 to have
     a sequence of p 1's, then shifting n times to the left to position it
     starting at n. Finally, subtracting (1 << n) - 1 removes the bits below
     n from the mask. */
  uint64_t mask = ((uint64_t)1 << (p + 1)) - 1 - (((uint64_t)1 << n) - 1);

  /* Apply the mask and count the set bits in the result. */
  uint64_t maskedNumber = number & mask;

  /* Count the bits set in maskedNumber. */
  uint32_t count = 0;
  while (maskedNumber) {
    count += maskedNumber & 1;
    maskedNumber >>= 1;
  }

  return count;
}

void
print_bits(char *name, uint64_t value)
{
  if (name) {
    printf("%s\t", name);
  }
  for (int i = 63; i >= 0; i--) {
    printf("%lu", (value >> i) & 1);
    if (i % 8 == 0) {
      printf(" "); // Add space for better readability
    }
  }
  printf("\n");
}

void
sm_bitmap_from_uint64(sparsemap_t *map, int offset, uint64_t number)
{
  for (int i = offset; i < 64; i++) {
    bool bit = number & ((uint64_t)1 << i);
    sparsemap_assign(map, i, bit);
  }
}

sparsemap_idx_t
sm_add_span(sparsemap_t *map, int map_size, int span_length)
{
  int attempts = map_size / span_length;
  sparsemap_idx_t placed_at;
  do {
    placed_at = random_uint32() % (map_size - span_length - 1);
    if (sm_occupied(map, placed_at, span_length, true)) {
      attempts--;
    } else {
      break;
    }
  } while (attempts);
  for (sparsemap_idx_t i = placed_at; i < placed_at + span_length; i++) {
    if (sparsemap_set(map, i) != i) {
      return placed_at;
    }
  }
  return placed_at;
}

void
sm_whats_set(sparsemap_t *map, int off, int len)
{
  printf("what's set in the range [%d, %d): ", off, off + len);
  for (int i = off; i < off + len; i++) {
    if (sparsemap_is_set(map, i)) {
      printf("%d ", i);
    }
  }
  printf("\n");
}

bool
sm_is_span(sparsemap_t *map, sparsemap_idx_t m, int len, bool value)
{
  for (sparsemap_idx_t i = m; i < m + len; i++) {
    if (sparsemap_is_set(map, i) != value) {
      return false;
    }
  }
  return true;
}

bool
sm_occupied(sparsemap_t *map, sparsemap_idx_t m, int len, bool value)
{
  for (sparsemap_idx_t i = m; i < (sparsemap_idx_t)len; i++) {
    if (sparsemap_is_set(map, i) == value) {
      return true;
    }
  }
  return false;
}

char *
bytes_as(double bytes, char *s, size_t size)
{
  const char *units[] = { "b", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB" };
  size_t i = 0;

  while (bytes >= 1024 && i < sizeof(units) / sizeof(units[0]) - 1) {
    bytes /= 1024;
    i++;
  }

  snprintf(s, size, "%.2f %s", bytes, units[i]);
  return s;
}


#include <sparsemap.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(...)                                                \
  do {                                                             \
    fprintf(stderr, "%s:%d:%s(): ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, __VA_ARGS__);                                  \
  } while (0)
#pragma GCC diagnostic pop

#ifdef MUNIT_VERSION
#define random_uint32 munit_rand_uint32
#define logf(...) munit_logf(MUNIT_LOG_INFO, __VA_ARGS__)
#else
#define random_uint32 xorshift32
#define logf(...) fprintf(stderr, __VA_ARGS__)
#endif

/* Stable seeds make for stable "random" sequences for repeatable tests. */
#ifdef STABLE_SEED
#define XORSHIFT_SEED_VALUE (8675309)
#else
#define XORSHIFT_SEED_VALUE ((unsigned int)time(NULL) ^ getpid())
#endif

uint64_t tsc(void);
double tsc_ticks_to_ns(uint64_t tsc_ticks);
double nsts(void);

void xorshift32_seed(void);
uint32_t xorshift32(void);

void print_array(int *array, int l);
void print_spans(int *array, int n);

bool is_span(int *array, int n, int x, int l);
bool is_set(const int array[], int bit);
bool has_span(sparsemap_t *map, int *array, int l, int n);
int is_unique(int a[], int l, int value);

void setup_test_array(int a[], int l, int max_value);
void shuffle(int *array, size_t n);
int ensure_sequential_set(int a[], int l, int r);
sparsemap_idx_t sm_add_span(sparsemap_t *map, int map_size, int span_length);

void print_bits(char *name, uint64_t value);

void bitmap_from_uint32(sparsemap_t *map, uint32_t number);
void sm_bitmap_from_uint64(sparsemap_t *map, int offset, uint64_t number);
uint32_t rank_uint64(uint64_t number, int n, int p);
int whats_set_uint64(uint64_t number, int bitPositions[64]);

void sm_whats_set(sparsemap_t *map, int off, int len);

bool sm_is_span(sparsemap_t *map, sparsemap_idx_t m, int len, bool value);
bool sm_occupied(sparsemap_t *map, sparsemap_idx_t m, int len, bool value);

char *bytes_as(double bytes, char *s, size_t size);

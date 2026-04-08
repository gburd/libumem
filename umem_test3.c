#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_MALLOC_H) && defined(HAVE_MALLINFO)
#include <malloc.h>
#endif

/*
 * These symbols are defined in libumem but not exported via umem.h.
 * We access them to verify PTC (per-thread cache) state.
 */
extern int umem_ptc_enabled;
extern const int umem_genasm_supported;

static void minfo(void)
{
#if defined(HAVE_MALLOC_H)
#if defined(HAVE_MALLINFO2)
  /* Use mallinfo2() which returns size_t instead of int (no overflow) */
  struct mallinfo2 mi;
  mi = mallinfo2();
  printf(" fordblks = %zu\n", mi.fordblks);
#elif defined(HAVE_MALLINFO)
  /* Fall back to deprecated mallinfo() if mallinfo2() not available */
  struct mallinfo mi;
  mi = mallinfo();
  printf(" fordblks = %d\n", mi.fordblks);
#endif
#if defined(HAVE_MALLOC_STATS)
  malloc_stats();
#endif
  printf("\n");
#endif
}

int
main (void)
{
  char *p;
  int i;

  minfo();
  p = malloc(10);
  free(p);
  minfo();

  /*
   * After malloc/free, umem is initialized.  Check PTC state.
   * When libumem_malloc is preloaded or linked, malloc() goes
   * through umem's malloc replacement, which may use the PTC
   * genasm fast path.
   */
  printf("PTC: genasm_supported=%d, ptc_enabled=%d\n",
      umem_genasm_supported, umem_ptc_enabled);

  /*
   * Exercise various allocation sizes through malloc/free to
   * cover multiple PTC cache buckets.  The PTC fast path handles
   * allocations up to umem_ptc_size bytes.
   */
  {
    static const size_t sizes[] = {
      1, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    };
    int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    printf("Testing %d allocation sizes via malloc/free...\n", nsizes);

    for (i = 0; i < nsizes; i++) {
      p = malloc(sizes[i]);
      if (p == NULL) {
        fprintf(stderr, "malloc(%zu) failed\n", sizes[i]);
        return EXIT_FAILURE;
      }
      memset(p, 0xAB, sizes[i]);
      free(p);
    }

    /* Rapid alloc/free cycle to exercise PTC reuse. */
    printf("Testing rapid alloc/free cycle (1000 iterations)...\n");
    for (i = 0; i < 1000; i++) {
      p = malloc(64);
      if (p == NULL) {
        fprintf(stderr, "malloc(64) failed at iteration %d\n", i);
        return EXIT_FAILURE;
      }
      memset(p, (unsigned char)i, 64);
      free(p);
    }
  }

  printf("malloc replacement PTC test passed\n");

  return EXIT_SUCCESS;
}

/* vim:ts=2:sw=2:et:
 */

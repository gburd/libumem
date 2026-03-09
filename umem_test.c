#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "umem.h"

/*
 * These symbols are defined in libumem but not exported via umem.h.
 * We access them to verify PTC (per-thread cache) state.
 */
extern int umem_ptc_enabled;
extern const int umem_genasm_supported;

int main(void)
{
  char *foo;
  int ptc_state;

  /* Perform a basic allocation to ensure umem is initialized. */
  foo = umem_alloc(32, UMEM_DEFAULT);
  if (foo == NULL) {
    fprintf(stderr, "umem_alloc failed\n");
    return EXIT_FAILURE;
  }

  strcpy(foo, "hello there");

  printf("Hello %s\n", foo);

  umem_free(foo, 32);

  /* Check PTC status after umem is initialized. */
  ptc_state = umem_ptc_enabled;
  printf("PTC: genasm_supported=%d, ptc_enabled=%d\n",
      umem_genasm_supported, ptc_state);

  /*
   * If the architecture supports genasm, PTC should be enabled
   * (unless explicitly disabled via UMEM_OPTIONS=perthread_cache=0
   * or debug mode).  Report the status for diagnostics.
   */
  if (umem_genasm_supported && !ptc_state) {
    printf("PTC: NOTE - genasm supported but PTC not enabled "
        "(debug mode or perthread_cache=0?)\n");
  }

  /*
   * Verify alloc/free still works correctly after PTC state check.
   * This catches any corruption from PTC initialization.
   */
  foo = umem_alloc(64, UMEM_DEFAULT);
  if (foo == NULL) {
    fprintf(stderr, "post-PTC-check umem_alloc failed\n");
    return EXIT_FAILURE;
  }
  memset(foo, 'A', 63);
  foo[63] = '\0';
  if (strlen(foo) != 63) {
    fprintf(stderr, "memory corruption detected\n");
    return EXIT_FAILURE;
  }
  umem_free(foo, 64);

  printf("basic PTC integration test passed\n");

  return EXIT_SUCCESS;
}


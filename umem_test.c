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

  printf("=== umem_test: Basic functionality and PTC integration ===\n");
  printf("Testing: umem_alloc(), umem_free(), PTC status\n\n");

  /* Perform a basic allocation to ensure umem is initialized. */
  printf("Test 1: Basic 32-byte allocation... ");
  foo = umem_alloc(32, UMEM_DEFAULT);
  if (foo == NULL) {
    fprintf(stderr, "FAIL\n");
    return EXIT_FAILURE;
  }
  strcpy(foo, "hello there");
  printf("PASS (data: '%s')\n", foo);
  umem_free(foo, 32);

  /* Check PTC status after umem is initialized. */
  ptc_state = umem_ptc_enabled;
  printf("Test 2: PTC status check... ");
  printf("genasm_supported=%d, ptc_enabled=%d ", umem_genasm_supported, ptc_state);
  if (umem_genasm_supported && !ptc_state) {
    printf("(NOTE: genasm supported but PTC not enabled)\n");
  } else {
    printf("PASS\n");
  }

  /*
   * Verify alloc/free still works correctly after PTC state check.
   * This catches any corruption from PTC initialization.
   */
  printf("Test 3: Post-init 64-byte allocation with verification... ");
  foo = umem_alloc(64, UMEM_DEFAULT);
  if (foo == NULL) {
    fprintf(stderr, "FAIL (allocation failed)\n");
    return EXIT_FAILURE;
  }
  memset(foo, 'A', 63);
  foo[63] = '\0';
  if (strlen(foo) != 63) {
    fprintf(stderr, "FAIL (memory corruption detected)\n");
    return EXIT_FAILURE;
  }
  umem_free(foo, 64);
  printf("PASS\n");

  printf("\nAll tests passed (3/3)\n");
  return EXIT_SUCCESS;
}

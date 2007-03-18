#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main (void)
{
  char *p;

  p = malloc(10);
  free(p);

  return EXIT_SUCCESS;
}

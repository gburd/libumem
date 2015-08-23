#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "umem.h"

extern void umem_startup(caddr_t, size_t, size_t, caddr_t, caddr_t);

int main(int argc, char *argv[])
{
  char *foo;

  umem_startup(NULL, 0, 0, NULL, NULL);

  foo = umem_alloc(32, UMEM_DEFAULT);

  strcpy(foo, "hello there");

  printf("Hello %s\n", foo);

  umem_free(foo, 32);

  return EXIT_SUCCESS;
}


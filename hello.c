#include <stdio.h>
#include <stdlib.h>

int main() {
  char *ptr = (char *)malloc(4096);
  snprintf(ptr, 4096, "%s", "Hello, World!");
  printf("%s 0x%p\n", ptr, ptr);
  return 0;
}

/*
 * Local variables:
 * tab-width:4
 * compile-command: "gcc -fpic -Wall -Werror -Ofast -march=native -mtune=native hello.c -o hello"
 * End:
 */


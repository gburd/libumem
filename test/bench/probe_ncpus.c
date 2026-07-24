#include <stdio.h>
#include <umem.h>
extern int umem_max_ncpus;
int main(void) {
	void *p = umem_alloc(16, 0);
	umem_free(p, 16);
	printf("umem_max_ncpus=%d\n", umem_max_ncpus);
	return 0;
}

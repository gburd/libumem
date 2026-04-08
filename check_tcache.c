/*
 * Check tcache status
 */
#include <stdio.h>
#include <umem.h>

extern int umem_tcache_enabled;
extern int umem_ptc_enabled;

int main(void) {
    void *p = umem_alloc(64, UMEM_DEFAULT);  // Force umem init

    printf("umem_tcache_enabled = %d\n", umem_tcache_enabled);
    printf("umem_ptc_enabled = %d\n", umem_ptc_enabled);

    umem_free(p, 64);
    return 0;
}

#include <stdio.h>
#include <umem.h>
#include "umem_impl.h"
#include "umem_ptc.h"
extern int8_t umem_ptc_bin_table[];
extern umem_cache_t *umem_alloc_table[];
#ifndef UMEM_ALIGN_SHIFT
#define UMEM_ALIGN_SHIFT 4
#endif
int main(void) {
	void *p = umem_alloc(176, 0); umem_free(p, 176);
	for (size_t s = 160; s <= 200; s += 8) {
		int idx = (int)((s - 1) >> UMEM_ALIGN_SHIFT);
		umem_cache_t *cp = umem_alloc_table[idx];
		printf("size=%zu idx=%d cache=%s bufsize=%zu bin_table=%d\n",
		    s, idx, cp ? cp->cache_name : "NULL",
		    cp ? cp->cache_bufsize : 0, (int)umem_ptc_bin_table[idx]);
	}
	return 0;
}

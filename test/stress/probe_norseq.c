/*
 * P8.5b norseq escape-hatch probe.  Allocates a batch larger than a PTC
 * magazine and frees it, forcing PTC-magazine misses so the routed rseq
 * layer fires when it is enabled.  Prints umem_dump_contention on stdout;
 * the accompanying test_norseq.sh runs this twice -- once with
 * UMEM_OPTIONS=norseq (expects rseq_enabled=0 and rseq_alloc column all 0)
 * and once without (expects rseq_enabled=1 and a nonzero rseq_alloc) -- so
 * the runtime off switch is regression-covered.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include "umem.h"

#define BATCH 4096	/* > any magazine (max magsize 255) */
#define SZ    64

int
main(void)
{
	void **buf = malloc(BATCH * sizeof(*buf));
	if (buf == NULL)
		return (2);
	/* A few rounds so the layer is well past its arming threshold. */
	for (int r = 0; r < 64; r++) {
		for (int i = 0; i < BATCH; i++)
			buf[i] = umem_alloc(SZ, UMEM_DEFAULT);
		for (int i = 0; i < BATCH; i++)
			if (buf[i]) *(char *)buf[i] = (char)i;
		for (int i = 0; i < BATCH; i++)
			if (buf[i]) umem_free(buf[i], SZ);
	}
	free(buf);
	umem_dump_contention(stdout);
	return (0);
}

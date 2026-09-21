/*
 * RSS/retention observation for the P1.4/P1.5 reclaim changes.  NOT a
 * benchmark and not a performance claim: it reports RSS and per-arena
 * retained bytes at fixed points in one single-threaded sequence, so the
 * effect of excluding metadata-bearing slabs from page discard and of
 * draining slabs at destroy is visible as numbers rather than asserted.
 *
 * Reported per phase: RSS (VmRSS from /proc/self/status) and, for the
 * private arenas, vmem_size(VMEM_ALLOC).
 */
#include "umem_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kb = -1;
	if (f == NULL) return -1;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
	fclose(f);
	return kb;
}

static void report(const char *phase, vmem_t *vmp)
{
	printf("%-28s RSS=%ld kB", phase, rss_kb());
	if (vmp != NULL)
		printf("  arena_alloc=%lu B",
		    (unsigned long)vmem_size(vmp, VMEM_ALLOC));
	printf("\n");
	fflush(stdout);
}

static void update_pass(void)
{
	umem_st_update_thr = thr_self();
	umem_cache_applyall(umem_cache_update);
	umem_st_update_thr = 0;
}

int main(void)
{
	enum { N = 20000, SZ = 512, SPAN = 64 * 1024 * 1024 };
	void **p = malloc(N * sizeof(void *));
	void *base;
	vmem_t *vmp;
	umem_cache_t *cp;
	int i;

	if (p == NULL) return 1;
	if (posix_memalign(&base, pagesize, SPAN) != 0) return 1;
	vmp = vmem_create("rss_probe", base, SPAN, pagesize,
	    NULL, NULL, NULL, 0, VM_NOSLEEP);
	if (vmp == NULL) return 1;
	cp = umem_cache_create("rss_probe_cache", SZ, 0,
	    NULL, NULL, NULL, NULL, vmp, UMC_NOMAGAZINE);
	if (cp == NULL) return 1;

	printf("cache: bufsize=%d slabsize=%lu flags=0x%x hash=%d buftag=%d "
	    "pagesize=%lu\n", SZ, (unsigned long)cp->cache_slabsize,
	    cp->cache_flags, !!(cp->cache_flags & UMF_HASH),
	    !!(cp->cache_flags & UMF_BUFTAG), (unsigned long)pagesize);
	printf("reclaim_enabled=%u reclaim_delay=%u reap_interval=%u\n",
	    umem_reclaim_enabled, umem_reclaim_delay, umem_reap_interval);

	report("0 baseline", vmp);
	for (i = 0; i < N; i++) {
		p[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (p[i] != NULL) memset(p[i], 0xa5, SZ);
	}
	report("1 after alloc", vmp);
	for (i = 0; i < N; i++)
		if (p[i] != NULL) umem_cache_free(cp, p[i]);
	report("2 after free (retained)", vmp);
	update_pass();
	report("3 after 1 update pass", vmp);
	for (i = 0; i < 8; i++) update_pass();
	report("4 after 8 more passes", vmp);
	umem_cache_destroy(cp);
	report("5 after cache destroy", vmp);
	vmem_destroy(vmp);
	free(base);
	free(p);
	return 0;
}

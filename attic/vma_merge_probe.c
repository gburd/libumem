/*
 * vma_merge_probe: which anonymous-mapping operations merge VMAs on Linux?
 *
 * The heap-ceiling fix depends on this, so measure it rather than assume it.
 * Each case reserves address space the way vmem_mmap_top_alloc() does
 * (PROT_NONE | MAP_NORESERVE), commits it the way vmem_mmap_alloc() does
 * (mmap MAP_FIXED RW, or mprotect), touches it, and reports the VMA delta.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static long vmas(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char l[512]; long n = 0;
	while (fgets(l, sizeof l, f)) n++;
	fclose(f); return n;
}

static void *reserve(size_t sz)
{
	void *p = mmap(0, sz, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return p;
}

static void commit_mmap(char *p, size_t sz, int touch)
{
	if (mmap(p, sz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0) == MAP_FAILED)
	{ perror("mmap fixed"); exit(1); }
	if (touch) p[0] = 1;
}

static void commit_mprotect(char *p, size_t sz, int touch)
{
	if (mprotect(p, sz, PROT_READ|PROT_WRITE)) { perror("mprotect"); exit(1); }
	if (touch) p[0] = 1;
}

/* n reservations of rsz each, committed in csz pieces descending (slab order) */
static void run(const char *name, int n, size_t rsz, size_t csz, int touch, int ascending,
    void (*commit)(char *, size_t, int))
{
	long v0 = vmas();
	char **r = calloc(n, sizeof *r);
	int adjacent = 1;
	for (int i = 0; i < n; i++) {
		r[i] = reserve(rsz);
		if (i && r[i] + rsz != r[i-1] && r[i-1] + rsz != r[i]) adjacent = 0;
		if (ascending)
			for (size_t o = 0; o < rsz; o += csz) commit(r[i] + o, csz, touch);
		else
			for (size_t o = rsz; o > 0; o -= csz) commit(r[i] + o - csz, csz, touch);
	}
	long v1 = vmas();
	printf("%-48s reservations=%d x %zuK commit=%zuK %s %s adjacent=%d  VMAs +%ld\n",
	    name, n, rsz>>10, csz>>10, touch ? "touch" : "notouch",
	    ascending ? "asc" : "desc", adjacent, v1 - v0);
	for (int i = 0; i < n; i++) munmap(r[i], rsz);
}

int main(void)
{
	/* What the current tree does: 128K reservations, 4K MAP_FIXED, descending */
	run("current: 128K resv, 4K mmap desc",          16, 128<<10, 4<<10, 1, 0, commit_mmap);
	run("current: 128K resv, 4K mmap asc",           16, 128<<10, 4<<10, 1, 1, commit_mmap);
	run("current: 128K resv, 4K mmap desc notouch",  16, 128<<10, 4<<10, 0, 0, commit_mmap);
	/* Bigger slab, same reservation granularity */
	run("64K slab: 64K resv, 64K mmap",              16, 64<<10, 64<<10, 1, 0, commit_mmap);
	run("64K slab: 128K resv, 64K mmap",             16, 128<<10, 64<<10, 1, 0, commit_mmap);
	/* Big reservations, small commits */
	run("1M resv, 4K mmap desc",                      4, 1<<20, 4<<10, 1, 0, commit_mmap);
	run("1M resv, 64K mmap desc",                     4, 1<<20, 64<<10, 1, 0, commit_mmap);
	run("1M resv, 64K mmap asc",                      4, 1<<20, 64<<10, 1, 1, commit_mmap);
	run("16M resv, 64K mmap desc",                    2, 16<<20, 64<<10, 1, 0, commit_mmap);
	/* mprotect instead of MAP_FIXED */
	run("1M resv, 64K mprotect desc",                 4, 1<<20, 64<<10, 1, 0, commit_mprotect);
	run("128K resv, 4K mprotect desc",               16, 128<<10, 4<<10, 1, 0, commit_mprotect);
	run("64K resv, 64K mprotect",                    16, 64<<10, 64<<10, 1, 0, commit_mprotect);
	return 0;
}

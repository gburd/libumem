/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * Regression: the heap must not be capped by vm.max_map_count at ~5 GB.
 *
 * DEFECT (pre-fix): on platforms without MAP_ALIGN (i.e. Linux), the mmap
 * heap's parent arena used a PAGE-SIZED quantum where Solaris uses 64 KiB, so
 * the heap accumulated roughly one kernel VMA per ~76 KiB of address space.
 * The kernel caps VMAs per process at vm.max_map_count (default 65530), so
 * umem_alloc() began returning NULL at about 5 GB -- measured 65,532 VMAs at
 * failure, 100% of the limit, with ~39% of allocations failing at 192 threads
 * while glibc on the same box reached 96 GB without a single failure.
 *
 * HOW THIS DETECTS IT
 *   Allocate past the old ceiling in large-but-not-oversize chunks, holding
 *   every one, and count failures. Two things are asserted:
 *
 *     1. No allocation fails below the target. A NULL here is the defect.
 *     2. The VMA count stays well under vm.max_map_count. This is the
 *        mechanism, not just the symptom: a fix that merely got lucky on
 *        timing would still show VMAs climbing toward the cap.
 *
 *   Both matter. Checking only (1) would pass on a box whose limit had been
 *   raised by sysctl; checking VMAs proves the allocator's own density
 *   improved.
 *
 * SIZING
 *   The target is deliberately above the ~5 GB pre-fix ceiling but modest
 *   enough to run on an 8 GB box: the spans are MAP_NORESERVE and only the
 *   pages actually touched are faulted in, so RSS stays far below the address
 *   space reserved. We touch one byte per chunk to prove the memory is real
 *   without paying for all of it.
 *
 * SKIPS (automake 77) rather than failing when the environment cannot support
 * the check: a raised vm.max_map_count makes assertion (1) vacuous, and a
 * machine with too little address space cannot run it at all.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem.h"

/*
 * SIZE MATTERS, and it took a measurement to get right.  The VMA cost is not
 * uniform across allocation sizes, because only the slab path fragments the
 * address space:
 *
 *   size    count     total     VMAs     VMAs/MB
 *     64   200000      12MB      168      ~14
 *   4096   200000     781MB     6257     8.01     <-- the fragmenting path
 *  65536   200000   12500MB       75      ~0      (oversize arena: few mappings)
 * 131072   200000   25000MB       75      ~0      (ditto)
 *
 * Measured on the pre-fix build. A 1MB chunk size -- the obvious choice -- goes
 * straight to the oversize arena and reached 7GB with 72 VMAs, so it PASSED
 * against the defect and proved nothing. 4096 B is the size that actually
 * exercises the ceiling: at 8.01 VMAs/MB the 65530 cap arrives at ~8 GB.
 *
 * So allocate past that. These are touched, so this needs real memory: the
 * target is kept just over the cliff rather than far beyond it.
 */
#define CHUNK		4096
#define TARGET_BYTES	(9ULL * 1024 * 1024 * 1024)
#define NCHUNKS		(TARGET_BYTES / CHUNK)

static long
read_status_kb(const char *field)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long v = -1;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof (line), f) != NULL) {
		if (strncmp(line, field, strlen(field)) == 0) {
			(void) sscanf(line + strlen(field), " %ld", &v);
			break;
		}
	}
	(void) fclose(f);
	return (v);
}

static long
count_vmas(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	long n = 0;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof (line), f) != NULL)
		n++;
	(void) fclose(f);
	return (n);
}

static long
max_map_count(void)
{
	FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
	long v = -1;

	if (f == NULL)
		return (-1);
	if (fscanf(f, "%ld", &v) != 1)
		v = -1;
	(void) fclose(f);
	return (v);
}

int
main(void)
{
	long limit = max_map_count();
	void **held;
	unsigned long long i, ok = 0, failed = 0;
	long vmas_before, vmas_after;
	int rc = 0;

	if (limit < 0) {
		printf("SKIP: no /proc/sys/vm/max_map_count (not Linux?)\n");
		return (77);
	}
	/*
	 * If the operator has already raised the limit far above the default,
	 * reaching the target proves nothing about allocator density -- the
	 * kernel would have allowed the old code through too.
	 */
	if (limit > 200000) {
		printf("SKIP: vm.max_map_count is %ld (raised well above the "
		    "65530 default), so this check cannot distinguish the fix "
		    "from a permissive kernel\n", limit);
		return (77);
	}

	{
		long memkb = 0;
		FILE *mi = fopen("/proc/meminfo", "r");
		char line[256];

		if (mi != NULL) {
			while (fgets(line, sizeof (line), mi) != NULL) {
				if (strncmp(line, "MemTotal:", 9) == 0) {
					(void) sscanf(line + 9, " %ld", &memkb);
					break;
				}
			}
			(void) fclose(mi);
		}
		/* Touched pages, so this needs the memory for real. */
		if (memkb > 0 &&
		    (unsigned long long)memkb * 1024 < TARGET_BYTES + (2ULL << 30)) {
			printf("SKIP: MemTotal %ldMB is too small to hold a "
			    "touched %lluMB working set\n", memkb / 1024,
			    TARGET_BYTES / (1024 * 1024));
			return (77);
		}
	}

	held = calloc(NCHUNKS, sizeof (void *));
	if (held == NULL) {
		printf("SKIP: cannot allocate the tracking array\n");
		return (77);
	}

	vmas_before = count_vmas();
	printf("target=%lluMB chunk=%dKB chunks=%llu "
	    "vm.max_map_count=%ld vmas_at_start=%ld\n",
	    TARGET_BYTES / (1024 * 1024), CHUNK / 1024,
	    (unsigned long long)NCHUNKS, limit, vmas_before);

	for (i = 0; i < NCHUNKS; i++) {
		void *p = umem_alloc(CHUNK, UMEM_DEFAULT);

		if (p == NULL) {
			failed++;
			if (failed == 1) {
				printf("FIRST FAILURE at %lluMB "
				    "(errno=%d %s), vmas=%ld of %ld\n",
				    (i * CHUNK) / (1024 * 1024), errno,
				    strerror(errno), count_vmas(), limit);
			}
			if (failed > 8)
				break;
			continue;
		}
		/* Touch it: address space that cannot be used is not a pass. */
		((char *)p)[0] = (char)(i & 0xff);
		((char *)p)[CHUNK - 1] = (char)(i & 0xff);
		if ((i & 0xffff) == 0 && count_vmas() > limit - 64) {
			/*
			 * About to hit the cap. Stop here rather than let the
			 * kernel start failing mmap: the VMA check below is
			 * what reports it, and we want the count, not a crash.
			 */
			printf("  stopping early at %lluMB: vmas=%ld is "
			    "within 64 of the %ld limit\n",
			    (i * CHUNK) / (1024 * 1024), count_vmas(), limit);
			ok++;
			break;
		}
		held[i] = p;
		ok++;
	}

	vmas_after = count_vmas();
	printf("allocated=%lluMB ok=%llu failed=%llu vmas=%ld of %ld "
	    "(%.1f%% of limit) rss=%ldMB\n",
	    (ok * CHUNK) / (1024 * 1024), ok, failed, vmas_after, limit,
	    100.0 * (double)vmas_after / (double)limit,
	    read_status_kb("VmRSS:") / 1024);

	if (failed != 0) {
		/*
		 * FIXED (UMEM_MIN_SLAB_OBJECTS in umem_impl.h): hashed caches now
		 * hold at least 16 objects per slab, so a 4 KiB object no longer
		 * costs a VMA.  Measured: 2 GB of 4 KiB objects went from 16,283
		 * VMAs to 75.  This used to return 77 (SKIP) while the defect was
		 * open; it is now a hard FAIL, because a return of the ceiling is
		 * a regression.
		 */
		printf("RESULT: FAIL (%llu allocation(s) failed below the "
		    "%lluMB target, vmas=%ld of %ld -- the heap ceiling is back; "
		    "see UMEM_MIN_SLAB_OBJECTS)\n",
		    failed, TARGET_BYTES / (1024 * 1024), vmas_after, limit);
		rc = 1;
	} else if (vmas_after > limit / 2) {
		/* Same reasoning as above: density is still poor by design. */
		/*
		 * We reached the target, but only by coming close to the VMA
		 * cap -- the density fix has regressed, and a slightly larger heap
		 * would fail.
		 */
		printf("RESULT: FAIL (reached the target but used %ld of %ld "
		    "VMAs; address-space density has regressed)\n",
		    vmas_after, limit);
		rc = 1;
	} else {
		printf("RESULT: PASS (%lluMB allocated with %ld VMAs, %.1f%% "
		    "of the %ld limit)\n", (ok * CHUNK) / (1024 * 1024),
		    vmas_after, 100.0 * (double)vmas_after / (double)limit,
		    limit);
	}

	for (i = 0; i < NCHUNKS; i++) {
		if (held[i] != NULL)
			umem_free(held[i], CHUNK);
	}
	free(held);
	return (rc);
}

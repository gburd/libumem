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
 * heap_density: measure address-space density and RSS for a fixed workload.
 *
 *   heap_density <size|mix> <total_bytes> [maps_out]
 *
 * Allocates <total_bytes> worth of <size>-byte objects (or cycles the
 * umem_alloc size classes when <size> is "mix"), touches each, and prints
 * the VMA count, VmRSS and VmHWM.  With [maps_out], copies /proc/self/maps
 * there at the peak so the mapping structure can be inspected offline.
 *
 * This is the before/after instrument for the heap-ceiling fix
 * (docs/results/2026-09-22-umem-heap-ceiling-vma.md): the headline number is
 * VMAs at 2 GB of 4 KiB objects; the small-heap RSS check uses a few MB.
 * Not a test; nothing here passes or fails.
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

static const size_t mix_sizes[] = {
	16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
	1536, 2048, 3072, 4096, 8192, 16384, 32768, 65536,
};

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
count_vmas(const char *copy_to)
{
	FILE *f = fopen("/proc/self/maps", "r");
	FILE *o = copy_to ? fopen(copy_to, "w") : NULL;
	char line[512];
	long n = 0;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof (line), f) != NULL) {
		n++;
		if (o != NULL)
			fputs(line, o);
	}
	(void) fclose(f);
	if (o != NULL)
		(void) fclose(o);
	return (n);
}

int
main(int argc, char **argv)
{
	size_t size = 0, total, done = 0;
	unsigned long long n = 0, failed = 0;
	int mix;
	const char *maps_out = argc > 3 ? argv[3] : NULL;
	struct { void *p; size_t sz; } *held;
	size_t cap, i;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <size|mix> <total_bytes> [maps_out]\n",
		    argv[0]);
		return (2);
	}
	mix = strcmp(argv[1], "mix") == 0;
	if (!mix)
		size = strtoull(argv[1], NULL, 0);
	total = strtoull(argv[2], NULL, 0);

	cap = mix ? total / 16 : total / size + 1;
	held = calloc(cap, sizeof (*held));
	if (held == NULL) {
		perror("calloc");
		return (2);
	}

	long vmas0 = count_vmas(NULL);
	long rss0 = read_status_kb("VmRSS:");

	for (i = 0; i < cap && done < total; i++) {
		size_t sz = mix ?
		    mix_sizes[i % (sizeof (mix_sizes) / sizeof (mix_sizes[0]))] :
		    size;
		void *p = umem_alloc(sz, UMEM_DEFAULT);

		if (p == NULL) {
			if (failed++ == 0)
				printf("FIRST FAILURE at %zuMB errno=%d %s\n",
				    done >> 20, errno, strerror(errno));
			if (failed > 8)
				break;
			continue;
		}
		((char *)p)[0] = 1;
		((char *)p)[sz - 1] = 1;
		held[i].p = p;
		held[i].sz = sz;
		done += sz;
		n++;
	}

	long vmas = count_vmas(maps_out);
	printf("size=%s total=%zuMB objects=%llu failed=%llu "
	    "vmas=%ld (start %ld) rss=%ldKB (start %ldKB) hwm=%ldKB "
	    "vmas_per_mb=%.2f rss_overhead=%.1f%%\n",
	    argv[1], done >> 20, n, failed, vmas, vmas0,
	    read_status_kb("VmRSS:"), rss0, read_status_kb("VmHWM:"),
	    done ? (double)(vmas - vmas0) / ((double)done / (1 << 20)) : 0.0,
	    done ? 100.0 * ((double)(read_status_kb("VmRSS:") - rss0) * 1024 -
	    (double)done) / (double)done : 0.0);

	for (i = 0; i < cap; i++)
		if (held[i].p != NULL)
			umem_free(held[i].p, held[i].sz);
	free(held);
	return (failed ? 1 : 0);
}

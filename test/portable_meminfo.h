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
 * portable_meminfo.h -- RSS and VMA (mapping) counting for the libumem test
 * suite, across Linux, illumos/Solaris, and the BSDs.
 *
 * The suite was written against Linux /proc (/proc/self/status for VmRSS,
 * /proc/self/maps for the mapping list).  Neither exists in that form on
 * illumos: there RSS is psinfo_t.pr_rssize (KiB) in /proc/self/psinfo, and
 * the mapping list is an array of prmap_t in /proc/self/map.  getrusage()'s
 * ru_maxrss is NOT a usable fallback on illumos -- it is left 0 there
 * (verified on SunOS 5.11, 2026-10-17).  Header-only so each test picks it up
 * without a new library object.
 *
 *   pm_rss_kb()     resident set size in KiB, or -1 if unavailable.
 *   pm_vma_count()  number of address-space mappings, or -1 if unavailable.
 */

#ifndef UMEM_TEST_PORTABLE_MEMINFO_H
#define	UMEM_TEST_PORTABLE_MEMINFO_H

#include <stdio.h>
#include <stddef.h>

#if defined(__sun) || defined(__illumos__)
#include <fcntl.h>
#include <unistd.h>
#include <procfs.h>		/* psinfo_t, prmap_t */
#endif

static inline long
pm_rss_kb(void)
{
#if defined(__linux__)
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kb = -1;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof (line), f) != NULL)
		if (sscanf(line, "VmRSS: %ld", &kb) == 1)
			break;
	(void) fclose(f);
	return (kb);
#elif defined(__sun) || defined(__illumos__)
	int fd = open("/proc/self/psinfo", O_RDONLY);
	psinfo_t ps;
	long kb = -1;

	if (fd < 0)
		return (-1);
	if (read(fd, &ps, sizeof (ps)) == (ssize_t)sizeof (ps))
		kb = (long)ps.pr_rssize;	/* already in KiB */
	(void) close(fd);
	return (kb);
#else
	return (-1);
#endif
}

static inline long
pm_vma_count(void)
{
#if defined(__linux__)
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	long n = 0;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof (line), f) != NULL)
		n++;
	(void) fclose(f);
	return (n);
#elif defined(__sun) || defined(__illumos__)
	int fd = open("/proc/self/map", O_RDONLY);
	off_t sz;
	long n = -1;

	if (fd < 0)
		return (-1);
	/* /proc/self/map is an array of prmap_t, one per mapping. */
	sz = lseek(fd, 0, SEEK_END);
	if (sz >= 0)
		n = (long)(sz / (off_t)sizeof (prmap_t));
	(void) close(fd);
	return (n);
#else
	return (-1);
#endif
}

#endif /* UMEM_TEST_PORTABLE_MEMINFO_H */

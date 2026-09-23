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
 * Shared bits for the hard-limit probes (docs/plans Phase 6).
 *
 * Every probe is built twice from one source: once against libumem's API
 * (default) and once as a plain glibc binary (-DLIM_GLIBC), so the same
 * program answers "does glibc share this limit".  ALLOC/FREE hide the
 * difference; umem_free() needs the size, so every probe tracks sizes.
 *
 * These are measurement instruments, never in TESTS -- most take minutes
 * and gigabytes.
 */
#ifndef _LIMITS_H
#define	_LIMITS_H

#ifndef _GNU_SOURCE
#define	_GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef LIM_GLIBC
#define	LIM_NAME	"glibc"
#define	ALLOC(sz)	malloc(sz)
#define	FREE(p, sz)	free(p)
#else
#include "umem.h"
#define	LIM_NAME	"umem"
#define	ALLOC(sz)	umem_alloc((sz), UMEM_DEFAULT)
#define	FREE(p, sz)	umem_free((p), (sz))
#endif

static inline uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

/* /proc/self/status field in kB -> bytes.  0 if unreadable. */
static inline uint64_t
proc_status_kb(const char *key)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	uint64_t v = 0;
	size_t kl = strlen(key);

	if (f == NULL)
		return (0);
	while (fgets(line, sizeof (line), f) != NULL) {
		if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
			v = strtoull(line + kl + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
	return (v * 1024);
}

static inline uint64_t rss_bytes(void) { return (proc_status_kb("VmRSS")); }

static inline unsigned long
count_vmas(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	unsigned long n = 0;

	if (f == NULL)
		return (0);
	while (fgets(line, sizeof (line), f) != NULL)
		n++;
	fclose(f);
	return (n);
}

#define	MB(x)	((double)(x) / (1024.0 * 1024.0))

#endif	/* _LIMITS_H */

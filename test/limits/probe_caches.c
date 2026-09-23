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
 * Phase 6 probe: CACHE COUNT (and fork with many caches, item 6b).
 *
 * Create N umem_cache_t.  Each costs UMEM_CACHE_SIZE(umem_max_ncpus) from
 * umem_cache_arena (cache_cpu[] is sized by umem_max_ncpus, rounded to a
 * power of two -- 256 on a 192-CPU box, 128 B each), two mmap()ed depot
 * arrays of umem_max_ncpus umem_maglist_t (64 B each), one mmap()ed
 * cache_rseq array, and a 64-slot hash table.  Reports bytes/cache and the
 * VMA count (three mmap() calls per cache = three VMAs per cache, so
 * vm.max_map_count 65530 / 3 is a hard ceiling on cache count).
 *
 * Then measures what the update thread's umem_cache_applyall() walk costs the
 * allocation path: with N caches live, time allocations for 25 s (two update
 * intervals) and report the worst stall.  applyall holds umem_cache_lock for
 * the whole walk; the allocation fast path does not take that lock, but
 * umem_cache_update() takes each cache's cache_lock in turn.
 *
 * Then fork() with N caches: umem_lockup() walks every cache and takes
 * (ncpus + 2 + 2*depot_ncpus + 1) mutexes per cache.
 *
 * umem-only: glibc has no cache API.  Usage: probe_caches <ncaches> [--fork]
 */
#ifndef _GNU_SOURCE
#define	_GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "umem.h"

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static uint64_t
rss_bytes(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	uint64_t v = 0;
	if (f == NULL) return (0);
	while (fgets(line, sizeof (line), f))
		if (strncmp(line, "VmRSS:", 6) == 0) { v = strtoull(line + 6, NULL, 10); break; }
	fclose(f);
	return (v * 1024);
}

static unsigned long
count_vmas(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	unsigned long n = 0;
	if (f == NULL) return (0);
	while (fgets(line, sizeof (line), f)) n++;
	fclose(f);
	return (n);
}

int
main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 10000;
	int do_fork = argc > 2 && strcmp(argv[2], "--fork") == 0;
	umem_cache_t **caches = calloc(n, sizeof (umem_cache_t *));
	void **objs = calloc(n, sizeof (void *));
	int i, created = 0;
	uint64_t t0, rss0, worst = 0;
	char name[32];

	{ void *p = umem_alloc(64, UMEM_DEFAULT); umem_free(p, 64); }
	rss0 = rss_bytes();
	printf("umem caches n=%d ncpus_online=%ld rss0=%.1fMB vmas0=%lu\n", n,
	    sysconf(_SC_NPROCESSORS_ONLN), rss0 / 1048576.0, count_vmas());

	t0 = now_ns();
	for (i = 0; i < n; i++) {
		snprintf(name, sizeof (name), "lim%d", i);
		uint64_t a = now_ns();
		caches[i] = umem_cache_create(name, 64 + (i % 16) * 8, 0,
		    NULL, NULL, NULL, NULL, NULL, 0);
		uint64_t d = now_ns() - a;
		if (d > worst) worst = d;
		if (caches[i] == NULL) {
			printf("umem_cache_create FAIL at %d: errno=%d (%s) "
			    "rss=%.1fMB vmas=%lu\n", i, errno, strerror(errno),
			    rss_bytes() / 1048576.0, count_vmas());
			break;
		}
		created++;
		/* one live object per cache so the cache is non-trivial */
		objs[i] = umem_cache_alloc(caches[i], UMEM_DEFAULT);
	}
	uint64_t rss1 = rss_bytes();
	printf("created %d in %.2fs (worst create %.1fms): rss=%.1fMB delta=%.1fMB "
	    "per_cache=%.1fKB vmas=%lu (%.2f/cache)\n", created,
	    (now_ns() - t0) / 1e9, worst / 1e6, rss1 / 1048576.0,
	    (rss1 - rss0) / 1048576.0, (double)(rss1 - rss0) / created / 1024.0,
	    count_vmas(), (double)count_vmas() / created);
	fflush(stdout);
	if (created == 0)
		return (1);

	/*
	 * Allocation stalls while the update thread walks `created` caches.
	 * umem_reap_interval is 10 s; sample for 25 s to be sure two passes
	 * land inside the window.
	 */
	worst = 0;
	uint64_t nstall = 0, probes = 0;
	uint64_t end = now_ns() + 25ULL * 1000000000ULL;
	while (now_ns() < end) {
		uint64_t a = now_ns();
		void *p = umem_alloc(64, UMEM_DEFAULT);
		uint64_t d = now_ns() - a;
		if (p) umem_free(p, 64);
		if (d > worst) worst = d;
		if (d > 1000000) nstall++;
		probes++;
		if ((probes & 0xfff) == 0) usleep(50);
	}
	printf("25s alloc probe with %d caches: %llu probes, worst %.1fms, stalls>1ms %llu\n",
	    created, (unsigned long long)probes, worst / 1e6,
	    (unsigned long long)nstall);
	/* the same, but creating a cache while the walk may be running */
	worst = 0; end = now_ns() + 12ULL * 1000000000ULL;
	int extra = 0;
	while (now_ns() < end) {
		snprintf(name, sizeof (name), "x%d", extra);
		uint64_t a = now_ns();
		umem_cache_t *c = umem_cache_create(name, 64, 0, NULL, NULL,
		    NULL, NULL, NULL, 0);
		uint64_t d = now_ns() - a;
		if (d > worst) worst = d;
		if (c) { umem_cache_destroy(c); extra++; }
		usleep(1000);
	}
	printf("12s create/destroy probe: %d cycles, worst create %.1fms\n",
	    extra, worst / 1e6);
	fflush(stdout);

	if (do_fork) {
		int k;
		for (k = 0; k < 3; k++) {
			uint64_t a = now_ns();
			pid_t pid = fork();
			uint64_t d = now_ns() - a;
			if (pid == 0) {
				/* child: allocate a little, report, exit */
				void *p = umem_alloc(64, UMEM_DEFAULT);
				umem_free(p, 64);
				_exit(0);
			}
			int st;
			waitpid(pid, &st, 0);
			printf("fork #%d with %d caches: parent fork() %.2fms, child rc=%d\n",
			    k, created, d / 1e6, WEXITSTATUS(st));
		}
	}

	/* destroy cost */
	t0 = now_ns(); worst = 0;
	for (i = 0; i < created; i++) {
		if (objs[i]) umem_cache_free(caches[i], objs[i]);
		uint64_t a = now_ns();
		umem_cache_destroy(caches[i]);
		uint64_t d = now_ns() - a;
		if (d > worst) worst = d;
	}
	printf("destroyed %d in %.2fs (worst %.1fms): rss=%.1fMB vmas=%lu\n",
	    created, (now_ns() - t0) / 1e9, worst / 1e6,
	    rss_bytes() / 1048576.0, count_vmas());
	return (0);
}

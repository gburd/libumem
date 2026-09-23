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
 * Phase 6 probe: KERNEL KNOBS.  Allocate until failure under a resource cap
 * and report exactly what the caller saw: NULL with which errno, or abort,
 * or (worst) a pointer the process then faults on.
 *
 *   probe_rlimit as   <cap_mb>   RLIMIT_AS  set in-process before allocating
 *   probe_rlimit data <cap_mb>   RLIMIT_DATA (Linux >= 4.7 counts mmap too)
 *   probe_rlimit none <target_mb> no cap; for vm.overcommit_memory=2 and
 *                                vm.max_map_count runs, where the operator
 *                                sets the sysctl before running.
 *
 * Size mix: 4 KiB slab objects (the VMA-heavy path) and 1 MiB oversize, so
 * both the slab span import and the oversize import meet the cap.  Every
 * returned pointer is written, so a "successful" allocation the kernel will
 * not back becomes a visible SIGSEGV/SIGBUS instead of a silent lie.
 *
 * Exit status: 0 = clean NULL with errno set; 3 = NULL with errno==0
 * (a failure reported as success); a signal death is the third answer.
 */
#include "limits.h"
#include <sys/resource.h>
#include <signal.h>

static volatile uint64_t last_ok;

static void
on_sig(int s)
{
	char buf[128];
	int n = snprintf(buf, sizeof (buf),
	    "SIGNAL %d after %llu MB successfully allocated and touched\n", s,
	    (unsigned long long)last_ok);
	write(1, buf, n);
	_exit(128 + s);
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "as";
	uint64_t cap_mb = argc > 2 ? strtoull(argv[2], NULL, 10) : 1024;
	struct rlimit rl;
	uint64_t total = 0, i;
	void *p;

	signal(SIGSEGV, on_sig);
	signal(SIGBUS, on_sig);
	signal(SIGABRT, on_sig);

	if (strcmp(mode, "as") == 0 || strcmp(mode, "data") == 0) {
		int res = strcmp(mode, "as") == 0 ? RLIMIT_AS : RLIMIT_DATA;
		rl.rlim_cur = rl.rlim_max = cap_mb << 20;
		if (setrlimit(res, &rl) != 0) {
			printf("setrlimit: %s\n", strerror(errno));
			return (2);
		}
	}
	{ FILE *f = fopen("/proc/sys/vm/overcommit_memory", "r"); int oc = -1;
	  FILE *g = fopen("/proc/sys/vm/max_map_count", "r"); long mm = -1;
	  if (f) { fscanf(f, "%d", &oc); fclose(f); }
	  if (g) { fscanf(g, "%ld", &mm); fclose(g); }
	  printf("%s rlimit mode=%s cap=%lluMB overcommit_memory=%d max_map_count=%ld\n",
	      LIM_NAME, mode, (unsigned long long)cap_mb, oc, mm); }
	fflush(stdout);

	/* Phase A: 4 KiB objects until failure or cap (touched). */
	errno = 0;
	for (i = 0; ; i++) {
		errno = 0;
		p = ALLOC(4096);
		if (p == NULL) {
			int e = errno;
			printf("4K path: NULL after %llu MB, errno=%d (%s), vmas=%lu rss=%.0fMB\n",
			    (unsigned long long)(total >> 20), e, strerror(e),
			    count_vmas(), MB(rss_bytes()));
			if (e == 0) return (3);
			break;
		}
		memset(p, 1, 4096);
		total += 4096;
		last_ok = total >> 20;
		if (strcmp(mode, "none") == 0 && total >= (cap_mb << 20)) {
			printf("4K path: reached %llu MB with no failure, vmas=%lu\n",
			    (unsigned long long)(total >> 20), count_vmas());
			break;
		}
	}
	fflush(stdout);
	/* Phase B: 1 MiB oversize objects until failure (touched). */
	uint64_t total_b = 0;
	for (i = 0; ; i++) {
		errno = 0;
		p = ALLOC(1 << 20);
		if (p == NULL) {
			int e = errno;
			printf("1M path: NULL after %llu more MB, errno=%d (%s), vmas=%lu rss=%.0fMB\n",
			    (unsigned long long)(total_b >> 20), e, strerror(e),
			    count_vmas(), MB(rss_bytes()));
			if (e == 0) return (3);
			break;
		}
		memset(p, 1, 1 << 20);
		total_b += 1 << 20;
		last_ok = (total + total_b) >> 20;
		if (strcmp(mode, "none") == 0 && total_b >= (cap_mb << 20) / 2) {
			printf("1M path: reached %llu MB with no failure, vmas=%lu\n",
			    (unsigned long long)(total_b >> 20), count_vmas());
			break;
		}
	}
	/* Phase C: after failure, can we still allocate something small? */
	errno = 0;
	p = ALLOC(64);
	printf("post-failure 64B alloc: %s (errno=%d)\n", p ? "ok" : "NULL", errno);
	if (p) FREE(p, 64);
	return (0);
}

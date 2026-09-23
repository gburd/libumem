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
 * P6.9 regression: a forked child must have a working update thread.
 *
 * Threads do not survive fork(), and umem_do_release(as_child=1) zeroes
 * umem_update_thr to record that.  Nothing recreated it, so a forked child's
 * periodic maintenance -- depot reaping, slab reclaim, hash rescale, magazine
 * resize -- was dead until the child called umem_reap() or failed an
 * allocation.  For a pre-fork server that is every worker for its whole life.
 *
 * WHAT THIS TESTS: the same thing test_reclaim_returns tests, but in a child.
 * The parent initialises the library (so it has an update thread), forks; the
 * CHILD builds and frees a 64 MB heap of 4 KiB objects and watches its own
 * RSS.  With reap_interval=1,reclaim_delay=2 (documented tunables, set by the
 * .sh wrapper) the freed heap must come back within the window.  The child
 * does not call umem_reap().
 *
 *   PASS: child RSS falls within 30 % of the heap's size above its baseline.
 *   FAIL: it does not.  Pre-fix the child has one thread and RSS is flat.
 *
 * The parent also confirms the child has two tasks, which is the direct
 * observation; the RSS arm is the consequence a user would see.
 */

#include "umem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>

#define	OBJ		4096
#define	HEAP		(64UL * 1024 * 1024)
#define	NOBJ		(HEAP / OBJ)
#define	WINDOW_S	20
#define	PASS_FRACTION	0.30

static long
rss_kb(void)
{
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
}

static int
ntasks(pid_t pid)
{
	char path[64];
	DIR *d;
	int n = 0;

	(void) snprintf(path, sizeof (path), "/proc/%d/task", (int)pid);
	d = opendir(path);
	if (d == NULL)
		return (-1);
	while (readdir(d) != NULL)
		n++;
	(void) closedir(d);
	return (n - 2);	/* . and .. */
}

static int
child_body(void)
{
	void **held;
	unsigned long i;
	long rss_base, rss_freed, rss_now, target_kb, heap_kb;
	int t;

	held = malloc(NOBJ * sizeof (void *));
	if (held == NULL)
		return (77);

	rss_base = rss_kb();
	for (i = 0; i < NOBJ; i++) {
		held[i] = umem_alloc(OBJ, UMEM_DEFAULT);
		if (held[i] == NULL) {
			printf("  child: SKIP umem_alloc failed at %lu\n", i);
			return (77);
		}
		memset(held[i], 0x5a, OBJ);
	}
	for (i = 0; i < NOBJ; i++)
		umem_free(held[i], OBJ);
	free(held);
	rss_freed = rss_kb();

	heap_kb = rss_freed - rss_base;
	target_kb = rss_base + (long)(heap_kb * PASS_FRACTION);
	printf("  child: tasks=%d base=%ldMB after_free=%ldMB target<=%ldMB\n",
	    ntasks(getpid()), rss_base / 1024, rss_freed / 1024,
	    target_kb / 1024);

	for (t = 1; t <= WINDOW_S; t++) {
		void *p = umem_alloc(32, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, 32);
		sleep(1);
		rss_now = rss_kb();
		if (rss_now <= target_kb) {
			printf("  child: RSS %ldMB at t=%ds -- returned\n",
			    rss_now / 1024, t);
			return (0);
		}
	}
	printf("  child: RSS still %ldMB after %ds -- NOT returned\n",
	    rss_kb() / 1024, WINDOW_S);
	return (1);
}

int
main(void)
{
	pid_t pid;
	int status, parent_tasks;
	void *warm;

	/* Parent: initialise the library so it has an update thread. */
	warm = umem_alloc(64, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, 64);
	parent_tasks = ntasks(getpid());
	printf("parent: tasks=%d\n", parent_tasks);
	if (parent_tasks < 2) {
		printf("SKIP: parent has no update thread; P6.8 fix absent, "
		    "cannot test the child\n");
		return (77);
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return (77);
	}
	if (pid == 0)
		_exit(child_body());

	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
		printf("RESULT: FAIL (child did not exit normally)\n");
		return (1);
	}
	switch (WEXITSTATUS(status)) {
	case 0:
		printf("RESULT: PASS (forked child returned its freed heap "
		    "without umem_reap(): it has an update thread)\n");
		return (0);
	case 77:
		printf("RESULT: SKIP\n");
		return (77);
	default:
		printf("RESULT: FAIL (forked child did not return freed "
		    "memory -- no update thread in the child; see P6.9)\n");
		return (1);
	}
}

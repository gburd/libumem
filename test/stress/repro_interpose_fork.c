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
 * Regression: the malloc interposer must participate in fork().
 *
 * DEFECT (pre-fix): malloc_interpose.c owns two mutexes that ORDINARY free()
 * and realloc() acquire on every call -- the static-buffer lock and the
 * libc-pointer-tracking lock -- and neither had any fork handling.  A child
 * therefore could inherit one of them already held by a thread that does not
 * exist in the child, and the child's very next free() would block forever.
 *
 * The allocator's own locks were covered by umem_fork.c; these were not,
 * because they live in a different shared object.
 *
 * HOW THIS REPRODUCES IT
 *   Several threads hammer malloc/free/realloc so that the interposer's locks
 *   are held a large fraction of the time.  The main thread forks repeatedly.
 *   Each child immediately does the one thing that needs those locks -- a
 *   free() and a realloc() -- and then _exit()s with a success code.  If the
 *   child inherited a held lock it never reaches _exit and the parent's
 *   waitpid times out.
 *
 *   The window is small per fork, so this forks many times.  A clean run is
 *   not proof on its own; the pre-fix log in docs/results/ shows the hang.
 *
 * WHY _exit AND NOT exit: exit() runs atexit handlers and stdio teardown,
 * which allocate; that would muddy which call blocked.  _exit is the narrow
 * probe.
 *
 * ONLY MEANINGFUL UNDER LD_PRELOAD=libumem_malloc.so.  Without the
 * interposer, malloc/free are libc's and this test exercises libc's own
 * fork safety instead, so it reports SKIP.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NTHREADS   6
#define NFORKS     300
#define CHILD_WAIT_MS 5000

static volatile int stop_workers;

/* Keep the interposer's locks busy: realloc and free both classify the
 * pointer, which takes the tracking lock; small/odd sizes also exercise the
 * bootstrap/static paths. */
static void *
worker(void *arg)
{
	unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1u;

	while (!stop_workers) {
		size_t n = 8 + (rand_r(&seed) % 512);
		void *p = malloc(n);
		if (p == NULL)
			continue;
		memset(p, 0xA5, n);
		void *q = realloc(p, n * 2 + 8);
		if (q == NULL) {
			free(p);
			continue;
		}
		memset(q, 0x5A, n * 2 + 8);
		free(q);

		void *c = calloc(1, 16 + (rand_r(&seed) % 128));
		if (c != NULL)
			free(c);
	}
	return (NULL);
}

/* Is the umem interposer actually in front of us? */
static int
interposed(void)
{
	/* libumem_malloc.so sets this in its constructor. */
	extern int umem_malloc_is_interposing __attribute__((weak));
	return (&umem_malloc_is_interposing != NULL &&
	    umem_malloc_is_interposing != 0);
}

static int
wait_with_timeout(pid_t pid, int timeout_ms)
{
	int waited = 0;

	for (;;) {
		int status;
		pid_t r = waitpid(pid, &status, WNOHANG);

		if (r == pid) {
			if (WIFEXITED(status))
				return (WEXITSTATUS(status));
			return (-2);		/* signalled */
		}
		if (r < 0)
			return (-3);		/* waitpid error */
		if (waited >= timeout_ms)
			return (-1);		/* TIMED OUT: the hang */

		struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
		(void) nanosleep(&ts, NULL);
		waited += 2;
	}
}

int
main(void)
{
	pthread_t th[NTHREADS];
	int i;
	int hangs = 0, signals = 0, bad_exit = 0;

	if (!interposed()) {
		printf("SKIP: not running under LD_PRELOAD=libumem_malloc.so "
		    "(nothing to test: the interposer's fork handling is the "
		    "subject)\n");
		return (77);			/* automake SKIP */
	}

	for (i = 0; i < NTHREADS; i++) {
		if (pthread_create(&th[i], NULL, worker,
		    (void *)(uintptr_t)(i + 1)) != 0) {
			fprintf(stderr, "pthread_create failed: %s\n",
			    strerror(errno));
			return (2);
		}
	}

	/* Let the workers get going so the locks are genuinely contended. */
	struct timespec warm = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
	(void) nanosleep(&warm, NULL);

	for (i = 0; i < NFORKS; i++) {
		/* Hold a tracked allocation across the fork so the child has
		 * something real to free. */
		void *keep = malloc(96);
		if (keep == NULL)
			continue;
		memset(keep, 0x33, 96);

		pid_t pid = fork();
		if (pid < 0) {
			free(keep);
			continue;
		}
		if (pid == 0) {
			/* CHILD: exactly the operations that need the
			 * interposer's locks. */
			free(keep);
			void *p = malloc(64);
			if (p != NULL) {
				void *q = realloc(p, 256);
				if (q != NULL)
					free(q);
				else
					free(p);
			}
			_exit(0);
		}

		int rc = wait_with_timeout(pid, CHILD_WAIT_MS);
		free(keep);

		if (rc == -1) {
			hangs++;
			fprintf(stderr, "FAIL: child %d (fork %d) did not "
			    "finish within %d ms -- inherited a held "
			    "interposer lock\n", (int)pid, i, CHILD_WAIT_MS);
			(void) kill(pid, SIGKILL);
			(void) waitpid(pid, NULL, 0);
			break;			/* one hang is the verdict */
		} else if (rc == -2) {
			signals++;
		} else if (rc != 0) {
			bad_exit++;
		}
	}

	stop_workers = 1;
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);

	printf("forks=%d hangs=%d signalled=%d bad_exit=%d\n",
	    NFORKS, hangs, signals, bad_exit);

	if (hangs != 0) {
		printf("RESULT: FAIL (child deadlocked on an inherited "
		    "interposer lock)\n");
		return (1);
	}
	if (signals != 0 || bad_exit != 0) {
		printf("RESULT: FAIL (child died abnormally)\n");
		return (1);
	}
	printf("RESULT: PASS (every child completed free/realloc after "
	    "fork)\n");
	return (0);
}

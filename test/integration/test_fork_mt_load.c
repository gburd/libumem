/*
 * Regression test for P1.2 (fork lock order) in
 * docs/plans/2026-09-21-production-readiness.md.
 *
 * WHAT THIS CATCHES THAT umem_ptc_fork_test DOES NOT
 *
 * umem_ptc_fork_test forks from an effectively single-threaded parent, so no
 * other thread can be holding an allocator lock when the pthread_atfork
 * prepare handler runs.  The defect is an ABBA deadlock that requires a
 * concurrent allocator:
 *
 *   forking thread (umem_lockup_cache, pre-fix order):
 *       holds cp->cache_full/cache_empty/cache_depot_*[i].ml_lock
 *       blocks acquiring cp->cache_cpu[k].cc_lock
 *   allocating thread (_umem_cache_alloc / _umem_cache_free / *_batch):
 *       holds cp->cache_cpu[k].cc_lock
 *       blocks in umem_depot_alloc/umem_depot_free on an ml_lock
 *
 * So the parent wedges inside fork() and never returns.  To make it happen we
 * need (a) several threads in the cc_lock + depot path at once and (b) forks
 * landing in that window repeatedly.
 *
 * Forcing depot traffic: objects are allocated by one thread and freed by a
 * different one.  A magazine therefore cannot be recycled inside one CPU
 * cache; it has to be pushed to / popped from the depot, which is exactly the
 * umem_depot_alloc()/umem_depot_free() call made while cc_lock is held.  Half
 * of the size classes used are above umem_ptc_maxsize (2048) so they bypass
 * the per-thread cache entirely and always go through cc_lock.
 *
 * Failure mode is a hang, so this test carries its own watchdog thread: on
 * deadline expiry it writes a diagnosis with write(2) (the process may hold
 * every allocator lock at that point, so nothing here may allocate) and
 * _exit(1)s.  A hang is a FAIL, never an indefinite wait.
 */

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "umem.h"

/* Mixed classes: <=2048 are PTC-eligible, >2048 always take cc_lock. */
static const size_t sizes[] = { 32, 96, 256, 1024, 3072, 8192, 16384 };
#define NSIZES	(sizeof (sizes) / sizeof (sizes[0]))

#define RING_CAP	2048

struct entry {
	void *p;
	size_t sz;
};

/*
 * Hand-off ring: producers push, consumers pop.  Storage is static so ring
 * operations never call the allocator under test.
 */
static struct entry ring[RING_CAP];
static size_t ring_head, ring_tail, ring_count;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int stop_flag;
static volatile int watchdog_deadline_s = 120;

static int nthreads = 8;
static int nforks = 300;
static int child_wait_s = 15;

static unsigned long long total_allocs, total_frees;	/* diagnostic only */

static int
ring_push(void *p, size_t sz)
{
	int ok = 0;

	(void) pthread_mutex_lock(&ring_lock);
	if (ring_count < RING_CAP) {
		ring[ring_head].p = p;
		ring[ring_head].sz = sz;
		ring_head = (ring_head + 1) % RING_CAP;
		ring_count++;
		ok = 1;
	}
	(void) pthread_mutex_unlock(&ring_lock);
	return (ok);
}

static int
ring_pop(struct entry *out)
{
	int ok = 0;

	(void) pthread_mutex_lock(&ring_lock);
	if (ring_count > 0) {
		*out = ring[ring_tail];
		ring_tail = (ring_tail + 1) % RING_CAP;
		ring_count--;
		ok = 1;
	}
	(void) pthread_mutex_unlock(&ring_lock);
	return (ok);
}

struct worker_arg {
	int id;
	int producer;
};

static void *
worker(void *arg)
{
	struct worker_arg *wa = arg;
	unsigned seed = (unsigned)(wa->id * 2654435761u) | 1u;
	unsigned long long allocs = 0, frees = 0;

	while (!stop_flag) {
		int i;

		if (wa->producer) {
			for (i = 0; i < 64 && !stop_flag; i++) {
				size_t sz = sizes[(seed >> 8) % NSIZES];
				void *p;

				seed = seed * 1103515245u + 12345u;
				p = umem_alloc(sz, UMEM_DEFAULT);
				if (p == NULL)
					continue;
				allocs++;
				*(volatile char *)p = (char)i;
				/*
				 * Hand it to another thread so the magazine
				 * must travel through the depot.  If the ring
				 * is full, free it here rather than leak.
				 */
				if (!ring_push(p, sz)) {
					umem_free(p, sz);
					frees++;
				}
			}
		} else {
			for (i = 0; i < 64 && !stop_flag; i++) {
				struct entry e;

				if (ring_pop(&e)) {
					*(volatile char *)e.p = (char)i;
					umem_free(e.p, e.sz);
					frees++;
				} else {
					/* Keep the CPU cache hot anyway. */
					size_t sz = sizes[(seed >> 8) % NSIZES];
					void *p;

					seed = seed * 1103515245u + 12345u;
					p = umem_alloc(sz, UMEM_DEFAULT);
					if (p != NULL) {
						allocs++;
						umem_free(p, sz);
						frees++;
					}
				}
			}
		}
	}

	(void) pthread_mutex_lock(&ring_lock);
	total_allocs += allocs;
	total_frees += frees;
	(void) pthread_mutex_unlock(&ring_lock);
	return (NULL);
}

/*
 * Watchdog.  Async-signal-safe only: when the ABBA deadlock fires, the main
 * thread is inside fork() holding every allocator lock, so printf() (which
 * can allocate) would wedge this thread too.
 */
static void *
watchdog(void *arg)
{
	int elapsed = 0;
	char buf[160];
	int n;

	(void) arg;
	while (elapsed < watchdog_deadline_s) {
		struct timespec ts = { 1, 0 };

		(void) nanosleep(&ts, NULL);
		if (stop_flag > 1)		/* normal completion */
			return (NULL);
		elapsed++;
	}

	n = snprintf(buf, sizeof (buf),
	    "FAIL: deadline %ds exceeded -- fork()/alloc deadlock "
	    "(pid %ld; attach with: gdb -p %ld -batch -ex 'thread apply all bt')\n",
	    watchdog_deadline_s, (long)getpid(), (long)getpid());
	if (n > 0)
		(void) !write(2, buf, (size_t)n);
	_exit(1);
	return (NULL);
}

/* Child side: prove the inherited lock state is usable. */
static void
child_body(void)
{
	size_t i;

	for (i = 0; i < NSIZES; i++) {
		void *p = umem_alloc(sizes[i], UMEM_DEFAULT);
		if (p == NULL)
			_exit(2);
		*(volatile char *)p = 1;
		umem_free(p, sizes[i]);
	}
	_exit(0);
}

/* waitpid with a deadline: a hung child must FAIL, not block forever. */
static int
wait_child(pid_t pid, int timeout_s)
{
	int waited_ms = 0;

	for (;;) {
		int status = 0;
		pid_t r = waitpid(pid, &status, WNOHANG);

		if (r == pid) {
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
				return (0);
			fprintf(stderr, "FAIL: child %ld bad status 0x%x\n",
			    (long)pid, (unsigned)status);
			return (-1);
		}
		if (r < 0 && errno != EINTR) {
			fprintf(stderr, "FAIL: waitpid: %s\n",
			    strerror(errno));
			return (-1);
		}
		if (waited_ms >= timeout_s * 1000) {
			fprintf(stderr,
			    "FAIL: child %ld did not exit within %ds "
			    "(inherited a held allocator lock)\n",
			    (long)pid, timeout_s);
			(void) kill(pid, SIGKILL);
			(void) waitpid(pid, NULL, 0);
			return (-1);
		}
		{
			struct timespec ts = { 0, 2 * 1000 * 1000 };
			(void) nanosleep(&ts, NULL);
			waited_ms += 2;
		}
	}
}

static int
env_int(const char *name, int dflt)
{
	const char *v = getenv(name);
	int n;

	if (v == NULL || *v == '\0')
		return (dflt);
	n = atoi(v);
	return (n > 0 ? n : dflt);
}

int
main(void)
{
	pthread_t *tids;
	struct worker_arg *args;
	pthread_t wd;
	int i, rc = 0;
	int forks_done = 0;

	nthreads = env_int("FORK_MT_THREADS", 8);
	nforks = env_int("FORK_MT_FORKS", 300);
	watchdog_deadline_s = env_int("FORK_MT_DEADLINE", 120);
	child_wait_s = env_int("FORK_MT_CHILD_WAIT", 15);
	if (nthreads < 2)
		nthreads = 2;

	printf("fork-under-load: pid=%ld threads=%d forks=%d deadline=%ds\n",
	    (long)getpid(), nthreads, nforks, watchdog_deadline_s);
	(void) fflush(stdout);

	/* Allocated before the load starts, with libc, on purpose. */
	tids = calloc((size_t)nthreads, sizeof (*tids));
	args = calloc((size_t)nthreads, sizeof (*args));
	if (tids == NULL || args == NULL) {
		fprintf(stderr, "FAIL: out of memory\n");
		return (1);
	}

	if (pthread_create(&wd, NULL, watchdog, NULL) != 0) {
		fprintf(stderr, "FAIL: watchdog thread\n");
		return (1);
	}

	for (i = 0; i < nthreads; i++) {
		args[i].id = i;
		args[i].producer = (i % 2) == 0;
		if (pthread_create(&tids[i], NULL, worker, &args[i]) != 0) {
			fprintf(stderr, "FAIL: worker %d\n", i);
			return (1);
		}
	}

	/* Let the depot fill so forks land in the cc_lock->ml_lock window. */
	{
		struct timespec ts = { 0, 300 * 1000 * 1000 };
		(void) nanosleep(&ts, NULL);
	}

	for (i = 0; i < nforks && rc == 0; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			fprintf(stderr, "FAIL: fork: %s\n", strerror(errno));
			rc = 1;
			break;
		}
		if (pid == 0)
			child_body();	/* does not return */

		if (wait_child(pid, child_wait_s) != 0)
			rc = 1;
		else
			forks_done++;
	}

	stop_flag = 1;
	for (i = 0; i < nthreads; i++)
		(void) pthread_join(tids[i], NULL);
	stop_flag = 2;			/* retire the watchdog */

	/* Drain whatever the producers left behind. */
	for (;;) {
		struct entry e;

		if (!ring_pop(&e))
			break;
		umem_free(e.p, e.sz);
		total_frees++;
	}

	printf("%s: %d/%d forks completed, %llu allocs, %llu frees\n",
	    rc == 0 ? "PASS" : "FAIL", forks_done, nforks,
	    total_allocs, total_frees);
	return (rc);
}

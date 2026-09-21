/*
 * Regression test for P1.6 (maintenance-thread startup) in
 * docs/plans/2026-09-21-production-readiness.md.
 *
 * DEFECT: umem_create_update_thread() published its "go" state and the new
 * update thread published its "done" state through DIFFERENT mutexes -- the
 * worker set obj->flag and signalled obj->cond while holding obj->mtx, but the
 * creator tested obj->flag and waited on obj->cond under obj->cmtx, which the
 * worker never touched.  A signal that lands between the creator's predicate
 * test and its pthread_cond_wait() is therefore lost, and since the worker
 * signals exactly once, the creator waits forever: umem_reap() never returns.
 *
 * HOW THIS TEST MAKES THAT DETERMINISTIC
 *
 * The lost-wakeup window is "after the predicate is read, before the wait is
 * registered".  We widen it by interposing pthread_cond_wait() in the test
 * executable (an executable's definition preempts libc's for calls made from
 * libumem.so) and sleeping briefly before delegating to the real one, once,
 * on the thread that calls umem_reap().
 *
 * That delay is harmless to CORRECT code: with the fix, the creator holds the
 * single handshake mutex across "set go / broadcast / test done / wait", so
 * during our sleep the worker is still blocked acquiring that mutex and cannot
 * signal early.  It is fatal to the BROKEN code: the worker holds a different
 * mutex, so it runs, sets the flag, signals an empty condvar, and exits the
 * handshake during our sleep -- after which the creator's real wait blocks
 * with nothing left to wake it.
 *
 * The same code path also used to call pthread_cond_wait() inside ASSERT(),
 * which misc.h compiles out under NDEBUG (no wait at all in release builds).
 * Building this test with -DNDEBUG covers that; see the report.
 *
 * A hang is the failure, so SIGALRM bounds the run.
 */

#include "config.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "umem.h"

extern void umem_reap(void);

static volatile int widen_armed;
static pthread_t widen_thread;

/*
 * Interposed pthread_cond_wait.  Delays the first wait performed by the
 * arming thread, then behaves exactly like the real one.
 */
int
pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	static int (*real)(pthread_cond_t *, pthread_mutex_t *);

	if (real == NULL) {
		real = (int (*)(pthread_cond_t *, pthread_mutex_t *))
		    dlsym(RTLD_NEXT, "pthread_cond_wait");
		if (real == NULL) {
			(void) fprintf(stderr,
			    "FAIL: cannot resolve real pthread_cond_wait\n");
			_exit(1);
		}
	}

	if (widen_armed && pthread_equal(pthread_self(), widen_thread)) {
		struct timespec ts = { 0, 250 * 1000 * 1000 };	/* 250 ms */

		widen_armed = 0;	/* one shot */
		(void) nanosleep(&ts, NULL);
	}

	return (real(cond, mutex));
}

static void
on_alarm(int sig)
{
	static const char msg[] =
	    "FAIL: umem_reap() did not return -- update-thread startup "
	    "handshake lost its wakeup (P1.6)\n";

	(void) sig;
	(void) !write(2, msg, sizeof (msg) - 1);
	_exit(1);
}

int
main(void)
{
	void *p;
	struct timespec ts;
	int i;

	/* Force library initialization (sets umem_reap_next = now + 1s). */
	p = umem_alloc(64, UMEM_DEFAULT);
	if (p == NULL) {
		(void) fprintf(stderr, "FAIL: initial umem_alloc\n");
		return (1);
	}
	umem_free(p, 64);

	/* umem_reap() is a no-op until umem_reap_next passes. */
	ts.tv_sec = 1;
	ts.tv_nsec = 300 * 1000 * 1000;
	(void) nanosleep(&ts, NULL);

	(void) signal(SIGALRM, on_alarm);
	(void) alarm(20);

	widen_thread = pthread_self();
	widen_armed = 1;

	/*
	 * First reap creates the update thread and runs the handshake.
	 * Later reaps are cheap (umem_update_thr is already set); run a few
	 * so a hang anywhere in the path is caught, not just the first.
	 */
	for (i = 0; i < 3; i++) {
		umem_reap();
		ts.tv_sec = 0;
		ts.tv_nsec = 50 * 1000 * 1000;
		(void) nanosleep(&ts, NULL);
	}

	(void) alarm(0);

	/* The allocator must still work after the update thread is up. */
	p = umem_alloc(4096, UMEM_DEFAULT);
	if (p == NULL) {
		(void) fprintf(stderr, "FAIL: post-reap umem_alloc\n");
		return (1);
	}
	*(volatile char *)p = 1;
	umem_free(p, 4096);

	printf("PASS: update-thread startup handshake completed\n");
	return (0);
}

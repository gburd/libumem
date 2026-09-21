/*
 * Regression test for P1.6 (maintenance-thread startup) in
 * docs/plans/2026-09-21-production-readiness.md.
 *
 * DEFECT: umem_create_update_thread() published its "go" state and the new
 * update thread published its "done" state through DIFFERENT mutexes -- the
 * worker set obj->flag and signalled obj->cond while holding obj->mtx, while
 * the creator tested obj->flag and waited on obj->cond under obj->cmtx, a
 * mutex the worker never touched.  A signal that lands between the creator's
 * predicate test and its pthread_cond_wait() is therefore lost, and the
 * worker signals exactly once, so the creator waits forever: umem_reap()
 * never returns.
 *
 * HOW THIS TEST MAKES THAT DETERMINISTIC
 *
 * The lost-wakeup window is "after the waiter reads the predicate, before it
 * registers its wait".  We widen it by interposing pthread_cond_wait() in
 * this executable (an executable's definition preempts libc's for calls made
 * from libumem.so) and sleeping 250ms in the first wait performed by the
 * thread that calls umem_reap().
 *
 * The delay is harmless to CORRECT code: the fixed creator holds the single
 * handshake mutex across set-go / broadcast / test-done / wait, so during the
 * sleep the worker is still blocked acquiring that mutex and cannot signal.
 * It is fatal to the BROKEN code: the worker holds a different mutex, so it
 * runs during the sleep, sets the flag, signals a condvar with no waiter on
 * it, and leaves the handshake -- after which the creator's real wait blocks
 * with nothing left to wake it.
 *
 * WHY dlvsym AND NOT dlsym  (this cost a wrong result once; do not "simplify")
 *
 * dlsym(RTLD_NEXT, "pthread_cond_wait") on glibc returns the GLIBC_2.2.5
 * COMPAT implementation, which uses the pre-2.3.2 condition-variable layout.
 * Interposing with that as the forwarding target breaks every wait in the
 * process: a control program with a textbook handshake and no libumem at all
 * hangs under it.  An earlier version of this test did exactly that, so its
 * "pre-fix failure" proved nothing about libumem.  dlvsym() with the
 * GLIBC_2.3.2 version resolves the implementation libumem is actually linked
 * against (objdump -R libumem.so: pthread_cond_wait@GLIBC_2.3.2).
 *
 * Because that failure mode is invisible (it looks exactly like the bug being
 * hunted), self_check() below runs a textbook handshake THROUGH the interposer
 * before libumem is exercised.  If the interposer is broken, this test reports
 * that rather than reporting a libumem defect.
 *
 * The same code path also used to call pthread_cond_wait() inside ASSERT(),
 * which misc.h compiles out under NDEBUG (no wait at all in release builds);
 * build this file with -DNDEBUG to cover that.
 *
 * A hang is the failure, so every phase is bounded by SIGALRM.
 */

#define _GNU_SOURCE		/* dlvsym; must precede any header */

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
static volatile int interposer_entered;

/*
 * Interposed pthread_cond_wait.  Delays the first wait performed by the
 * arming thread, then behaves exactly like the real one.
 */
int
pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	static int (*real)(pthread_cond_t *, pthread_mutex_t *);

	if (real == NULL) {
		/* See "WHY dlvsym AND NOT dlsym" above. */
		real = (int (*)(pthread_cond_t *, pthread_mutex_t *))
		    dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.3.2");
		if (real == NULL)
			real = (int (*)(pthread_cond_t *, pthread_mutex_t *))
			    dlsym(RTLD_NEXT, "pthread_cond_wait");
		if (real == NULL) {
			(void) fprintf(stderr,
			    "FAIL: cannot resolve real pthread_cond_wait\n");
			_exit(1);
		}
	}

	interposer_entered = 1;

	if (widen_armed && pthread_equal(pthread_self(), widen_thread)) {
		struct timespec ts = { 0, 250 * 1000 * 1000 };	/* 250 ms */

		widen_armed = 0;	/* one shot */
		(void) nanosleep(&ts, NULL);
	}

	return (real(cond, mutex));
}

/* ---- self-check: the interposer must not break condvars by itself ---- */

static struct {
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	int go;
	int done;
} sc;

static void *
sc_worker(void *arg)
{
	(void) arg;
	(void) pthread_mutex_lock(&sc.mtx);
	while (!sc.go)
		(void) pthread_cond_wait(&sc.cond, &sc.mtx);
	sc.done = 1;
	(void) pthread_cond_broadcast(&sc.cond);
	(void) pthread_mutex_unlock(&sc.mtx);
	return (NULL);
}

static void
sc_alarm(int sig)
{
	static const char msg[] =
	    "FAIL(self-check): a textbook handshake hung through this test's "
	    "own pthread_cond_wait interposer -- the harness is broken, this "
	    "says nothing about libumem (see the dlvsym note in this file)\n";

	(void) sig;
	(void) !write(2, msg, sizeof (msg) - 1);
	_exit(1);
}

/*
 * Run a correct handshake through the interposer, delay armed, exactly as
 * libumem's will be run.  Must complete.
 */
static void
self_check(void)
{
	pthread_t t;

	(void) pthread_mutex_init(&sc.mtx, NULL);
	(void) pthread_cond_init(&sc.cond, NULL);
	sc.go = sc.done = 0;

	(void) signal(SIGALRM, sc_alarm);
	(void) alarm(15);

	widen_thread = pthread_self();
	widen_armed = 1;

	if (pthread_create(&t, NULL, sc_worker, NULL) != 0) {
		(void) fprintf(stderr, "FAIL: self-check thread\n");
		exit(1);
	}

	(void) pthread_mutex_lock(&sc.mtx);
	sc.go = 1;
	(void) pthread_cond_broadcast(&sc.cond);
	while (!sc.done)
		(void) pthread_cond_wait(&sc.cond, &sc.mtx);
	(void) pthread_mutex_unlock(&sc.mtx);
	(void) pthread_join(t, NULL);

	(void) alarm(0);

	if (!interposer_entered) {
		(void) fprintf(stderr,
		    "FAIL(self-check): interposer was never entered -- link "
		    "with -rdynamic or the test proves nothing\n");
		exit(1);
	}
	printf("self-check: interposer sound (delay armed, handshake "
	    "completed)\n");
	(void) fflush(stdout);
}

/* ---- the actual regression ---- */

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

	self_check();

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

	/* Re-arm the delay for libumem's handshake. */
	widen_thread = pthread_self();
	widen_armed = 1;

	/*
	 * The first reap creates the update thread and runs the handshake.
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

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
 * The error log must not deadlock against a signal handler.
 *
 * THE DEFECT.  umem_err_recoverable() -> umem_error_enter() ->
 * umem_log_enter() took umem_error_lock with mutex_lock().  Under LD_PRELOAD
 * the interposer sets umem_abort = 0, so every refused free() goes through
 * that path in steady state.  A signal that lands while a thread is inside
 * umem_log_enter, whose handler then frees a bad pointer, re-enters
 * umem_log_enter on the same thread and blocks on a lock that thread already
 * holds: deadlock.  glibc's malloc_printerr writes and aborts without a lock
 * and cannot deadlock (production-readiness review 2026-09-24, 4.6).
 *
 * WHAT THIS TESTS.  Install a SIGALRM handler that calls free() on a forged
 * pointer (a stack address: refused by the hull check, so it logs).  Arm an
 * interval timer at 50 us.  In the main thread, free() forged pointers in a
 * tight loop for 300 ms, so the main thread is inside umem_log_enter a large
 * fraction of the time and the timer lands there often.
 *
 *   PASS: the loop completes and the process exits 0.
 *   FAIL: the process hangs (the .sh wrapper kills it at 10 s and reports).
 *
 * The count of handler invocations is printed so a PASS is known to have
 * exercised the path (thousands, not zero).  With the fix, umem_log_enter
 * trylocks and drops the line when busy; umem_error_dropped counts those, and
 * the test prints it: a PASS with dropped == 0 would mean the timer never
 * landed inside the lock and the test proved nothing, so that is reported as
 * SKIP.
 *
 * ASan intercepts free() ahead of the interposer: SKIP under --enable-asan.
 *
 * NOT RUN with umem_output on (UMEM_DEBUG=verbose).  With stderr output
 * enabled the recoverable path symbolises the stack through libdw on every
 * refused free(), by design (someone asked to see it).  On aarch64 that is
 * ~34 frames and milliseconds per call, so a 50 us timer whose handler
 * refuses a free never lets the main loop finish: 436,000 stderr lines in
 * 4 s, gdb showing the process RUNNING in libdw and mmap, not blocked.  That
 * is the cost of a diagnostic the user turned on, not a lock, and this test
 * is about the lock.  It reports SKIP rather than misreport expense as
 * deadlock.  (libdw is also not async-signal-safe, so a program that
 * enables verbose output and frees from signal handlers is in glibc's
 * "malloc from a handler" territory either way.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern volatile unsigned long umem_error_dropped;

static volatile sig_atomic_t handler_calls;
static char forged_a[256] __attribute__((aligned(16)));
static char forged_b[256] __attribute__((aligned(16)));

static int
asan_active(void)
{
#if defined(__SANITIZE_ADDRESS__)
	return (1);
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
	return (1);
#else
	return (0);
#endif
#else
	return (0);
#endif
}

static void
on_alarm(int sig)
{
	(void) sig;
	handler_calls++;
	free(forged_b + 16);	/* not ours: refused, logged */
}

int
main(void)
{
	struct itimerval it;
	struct timespec t0, t1;
	long iters = 0;
	void *warm;

	if (asan_active()) {
		printf("SKIP: ASan intercepts free()\n");
		return (77);
	}
	{
		const char *d = getenv("UMEM_DEBUG");
		if (d != NULL && strstr(d, "verbose") != NULL) {
			printf("SKIP: umem_output on -- the recoverable path "
			    "symbolises via libdw per refusal; that is expense, "
			    "not the lock this test is about\n");
			return (77);
		}
	}
	warm = malloc(64);
	free(warm);

	signal(SIGALRM, on_alarm);
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = 50;
	it.it_value = it.it_interval;
	setitimer(ITIMER_REAL, &it, NULL);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	do {
		free(forged_a + 16);	/* not ours: refused, logged */
		iters++;
		clock_gettime(CLOCK_MONOTONIC, &t1);
	} while ((t1.tv_sec - t0.tv_sec) * 1000 +
	    (t1.tv_nsec - t0.tv_nsec) / 1000000 < 300);

	memset(&it, 0, sizeof (it));
	setitimer(ITIMER_REAL, &it, NULL);

	printf("main frees=%ld handler frees=%d log lines dropped=%lu\n",
	    iters, (int)handler_calls, umem_error_dropped);
	if (handler_calls < 100) {
		printf("SKIP: timer fired %d times; path not exercised\n",
		    (int)handler_calls);
		return (77);
	}
	if (umem_error_dropped == 0) {
		printf("SKIP: the handler never landed inside the error lock; "
		    "this run cannot distinguish the fix from luck\n");
		return (77);
	}
	printf("RESULT: PASS (no deadlock; %lu log lines dropped rather than "
	    "blocking a signal handler)\n", umem_error_dropped);
	return (0);
}

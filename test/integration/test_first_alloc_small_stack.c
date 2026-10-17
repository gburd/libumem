/*
 * test_first_alloc_small_stack -- libumem's FIRST allocation must work on a
 * thread with a small stack, as glibc's and jemalloc's do.
 *
 *   test_first_alloc_small_stack api    first call is umem_alloc()
 *   test_first_alloc_small_stack malloc first call is malloc() (run it with
 *                                       LD_PRELOAD=libumem_malloc.so; the .sh
 *                                       driver does both)
 *
 * The process makes no allocation before the thread runs, so the thread's
 * first call runs umem_init() on the thread's stack.  The thread is created
 * with a 16 KiB stack, or PTHREAD_STACK_MIN if that is larger (aarch64 glibc:
 * 128 KiB, for 64 KiB pages).  The first call then runs with only
 * FIRST_ALLOC_BUDGET bytes of stack left: a VLA uses up the rest.  The budget
 * is what makes the test mean the same thing on both arches, since arm64
 * cannot create a 16 KiB thread; and it models a first allocation made deep
 * in a call chain.
 *
 *   PASS (0): the first allocation returned non-NULL and the process survived.
 *   FAIL: a SIGSEGV on the guard page (rc 139), which is how it fails.
 *   SKIP (77): pthread_getattr_np could not report the stack bounds.
 *
 * Pre-fix (init_lib.c umem_get_max_ncpus with a 64 KiB on-stack buffer),
 * umem_init used 66-69 KB of stack on both arches and this test SEGVs on
 * both.  glibc malloc's first call used <= 3.4 KB on the same boxes and
 * jemalloc's <= 3.2 KB (docs/results/2026-09-27-arm-dlopen-stack-crash.md).
 *
 * FIRST_ALLOC_BUDGET is 8 KiB, about what a 16 KiB x86 thread has left
 * after its TLS and guard (7.7 KB measured).  glibc and jemalloc first calls
 * fit in it with room to spare.  An x86 16 KiB thread gets less than the
 * budget, so there under() is skipped and the thread's own size is the test.
 *
 * A second, TIGHTER budget is selectable with a numeric third argument
 * (bytes), e.g. "api 4096": a first umem_alloc() with only ~4 KiB of usable
 * stack below the alloc frame.  glibc survives 4 KiB.
 *
 * A depth-CAP mode is selectable with "cap:<bytes>" as the third argument,
 * e.g. "api cap:3800": run the first allocation on a large (default) stack,
 * paint below sp and scan for the deepest touched byte (the method of
 * probe_first_alloc_stack.c), and assert the peak <= cap.  This is an
 * enforced upper bound on first-alloc stack use, green now and red if a
 * regression deepens it.
 *
 * O7 (dropping the eager umem_stacktrace_init() from umem_init) is
 * arch-specific, measured on Debian 13 (docs/results/r8stack/):
 *   arm64:  first-alloc peak 5.36 KB -> 3.57 KB.  Now survives a 4 KiB stack
 *           where it SEGV'd before.  cap 3800 B: PASS post-fix, FAIL pre-fix.
 *   x86_64: first-alloc peak ~5.2 KB, UNCHANGED by O7.  The x86 floor is not
 *           the stacktrace warm-up but dlopen(NULL) (~3.4 KB) + ncpus/numa
 *           probing (~3.5 KB) + umem_init's own frames; O7 removed a frame
 *           that was not the x86 peak.  cap 5400 B: PASS now, red if the x86
 *           first-alloc depth grows (e.g. eager warm re-added, or a new deep
 *           sub-path).  Only arm64 meets the 4 KiB target; chasing the x86
 *           floor (moving dlopen/numa off the first-alloc path) is a separate
 *           follow-up, not done here.
 */
#include <pthread.h>
#include <limits.h>		/* PTHREAD_STACK_MIN (illumos puts it here) */
/* illumos spells glibc's pthread_getattr_np as pthread_attr_get_np. */
#if !defined(__GLIBC__) && (defined(__sun) || defined(__illumos__))
#define pthread_getattr_np pthread_attr_get_np
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <umem.h>

/*
 * PTHREAD_STACK_MIN is in <limits.h> on illumos and only visible with the
 * right feature macro; some platforms omit it entirely.  Fall back to a safe
 * floor so this test compiles everywhere (found by the UltraSPARC/illumos
 * hardware run, docs/results/r12/sparc-illumos-hardware-run.md).
 */
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif

#define	FIRST_ALLOC_BUDGET	8192
#define	SMALL_STACK		(16 * 1024)

static size_t budget = FIRST_ALLOC_BUDGET;
static size_t cap;		/* > 0: depth-cap mode; assert depth <= cap */
static size_t measured_depth;
static int use_malloc;
static void *result;
static size_t usable;
static int ready_before;	/* umem_ready just before the first call */

/*
 * libumem's init state (umem_impl.h: 1 = STARTUP, i.e. umem_init not yet
 * run).  Read to report WHERE umem_init ran: under LD_PRELOAD, libc can call
 * malloc on the main thread before this thread exists (pthread_create's DTV,
 * for one), and then the malloc arm is a survival check, not a first-init
 * check.  The driver prints which one it was.
 */
extern int umem_ready;

static __attribute__((noinline)) void
first_alloc(void)
{
	result = use_malloc ? malloc(64) : umem_alloc(64, UMEM_DEFAULT);
}

static __attribute__((noinline)) void
paint(volatile unsigned char *lo, volatile unsigned char *hi)
{
	while (lo < hi)
		*lo++ = 0xA5;
}

/* Leave about FIRST_ALLOC_BUDGET bytes of stack below this frame's callee. */
static __attribute__((noinline)) void
under(size_t burn)
{
	volatile unsigned char pad[burn];

	pad[0] = pad[burn - 1] = 0;
	first_alloc();
	__asm__ __volatile__("" : : "r"(pad) : "memory");
}

static void *
thr(void *arg)
{
	pthread_attr_t a;
	void *lo;
	size_t size;
	unsigned char *sp = __builtin_frame_address(0);

	(void) arg;
	ready_before = umem_ready;
	if (pthread_getattr_np(pthread_self(), &a) != 0)
		return (NULL);
	(void) pthread_attr_getstack(&a, &lo, &size);
	(void) pthread_attr_destroy(&a);
	usable = (size_t)(sp - (unsigned char *)lo);

	if (cap > 0) {
		/*
		 * Depth-cap mode: measure the peak stack the first allocation
		 * uses (paint below sp with 0xA5, run it, scan for the lowest
		 * overwritten byte -- same method as probe_first_alloc_stack.c)
		 * and assert it is <= cap.  Runs on the thread's own (large)
		 * stack, no VLA.  The paint floor is 4 KiB above the stack low
		 * end (the guard), like the probe.
		 */
		volatile unsigned char *pfloor = (unsigned char *)lo + 4096;
		volatile unsigned char *p;

		if (sp - 1024 > pfloor)
			paint(pfloor, sp - 1024);
		first_alloc();
		if (sp - 1024 > pfloor) {
			for (p = pfloor; p < sp && *p == 0xA5; p++)
				;
			measured_depth = (size_t)(sp - p);
		}
		return (NULL);
	}

	/* 512 B: this frame, under()'s frame, and the call. */
	if (usable > budget + 512)
		under(usable - budget - 512);
	else if (budget < FIRST_ALLOC_BUDGET) {
		/*
		 * A tight budget the platform floor cannot honour (arm64's
		 * 128 KiB min thread still leaves > budget usable, so this
		 * only bites if TLS/guard eat the difference): SKIP rather
		 * than run the alloc with more stack than the arm intends.
		 */
		usable = 0;
	} else
		first_alloc();
	return (NULL);
}

int
main(int argc, char **argv)
{
	pthread_attr_t a;
	pthread_t t;
	size_t stk = SMALL_STACK;
	int rc;

	use_malloc = argc > 1 && strcmp(argv[1], "malloc") == 0;
	if (argc > 2) {
		if (strncmp(argv[2], "cap:", 4) == 0)
			cap = (size_t)strtoul(argv[2] + 4, NULL, 0);
		else {
			unsigned long b = strtoul(argv[2], NULL, 0);
			if (b >= 512)
				budget = (size_t)b;
		}
	}
	if (stk < (size_t)PTHREAD_STACK_MIN)
		stk = (size_t)PTHREAD_STACK_MIN;

	(void) pthread_attr_init(&a);
	/*
	 * Depth-cap mode measures the peak stack of the first allocation, so it
	 * needs a large stack to paint into: use the platform default attr, not
	 * SMALL_STACK.
	 */
	if (cap == 0 && (rc = pthread_attr_setstacksize(&a, stk)) != 0) {
		fprintf(stderr, "FAIL: setstacksize(%zu) rc=%d\n", stk, rc);
		return (1);
	}
	if ((rc = pthread_create(&t, &a, thr, NULL)) != 0) {
		fprintf(stderr, "FAIL: pthread_create rc=%d\n", rc);
		return (1);
	}
	(void) pthread_join(t, NULL);

	if (cap > 0) {
		if (measured_depth == 0) {
			printf("SKIP: could not measure first-alloc depth\n");
			return (77);
		}
		printf("%s: first %s() peak stack %zu B (cap %zu B); umem_init %s\n",
		    measured_depth <= cap ? "PASS" : "FAIL",
		    use_malloc ? "malloc" : "umem_alloc", measured_depth, cap,
		    ready_before == 1 ? (umem_ready == 1 ?
		    "never ran (not umem's malloc)" : "ran on this thread") :
		    "had already run before this thread");
		return (measured_depth <= cap ? 0 : 1);
	}

	if (usable == 0) {
		printf("SKIP: no stack bounds, or tight budget below platform "
		    "floor (usable stack could not be burned to %zu B)\n",
		    budget);
		return (77);
	}
	printf("%s: first %s() on a %zu B thread with %zu B budget "
	    "(usable %zu): %s; umem_init %s\n", result != NULL ? "PASS" : "FAIL",
	    use_malloc ? "malloc" : "umem_alloc", stk, budget,
	    usable, result != NULL ? "ok" : "NULL",
	    ready_before == 1 ? (umem_ready == 1 ? "never ran (not umem's malloc)"
	    : "ran on this thread") : "had already run before this thread");
	return (result != NULL ? 0 : 1);
}

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
 * stack below the alloc frame.  glibc survives 4 KiB; umem's first call at
 * v3.x cost 5.2-5.4 KB (docs/results/2026-09-27-arm-dlopen-stack-crash.md
 * "Residual"), so this arm SEGVs on umem before O7 (the eager
 * umem_stacktrace_init in umem_init) and passes after.  If the platform's
 * pthread stack floor plus TLS/guard leaves less than the budget usable,
 * under() is skipped and the arm SKIPs (77) rather than falsely failing.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <umem.h>

#define	FIRST_ALLOC_BUDGET	8192
#define	SMALL_STACK		(16 * 1024)

static size_t budget = FIRST_ALLOC_BUDGET;
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
		unsigned long b = strtoul(argv[2], NULL, 0);
		if (b >= 512)
			budget = (size_t)b;
	}
	if (stk < (size_t)PTHREAD_STACK_MIN)
		stk = (size_t)PTHREAD_STACK_MIN;

	(void) pthread_attr_init(&a);
	if ((rc = pthread_attr_setstacksize(&a, stk)) != 0) {
		fprintf(stderr, "FAIL: setstacksize(%zu) rc=%d\n", stk, rc);
		return (1);
	}
	if ((rc = pthread_create(&t, &a, thr, NULL)) != 0) {
		fprintf(stderr, "FAIL: pthread_create rc=%d\n", rc);
		return (1);
	}
	(void) pthread_join(t, NULL);

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

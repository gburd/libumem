/*
 * probe_first_alloc_stack -- evidence tool for
 * docs/results/2026-09-27-arm-dlopen-stack-crash.md.  Not a test; built by
 * hand in the EC2 job (see the doc for the exact command).
 *
 *   probe <mode> <stack_kib>      mode: umem | malloc | dlopen | ncpus | none
 *                                 stack_kib 0 = default pthread attr
 *
 * Runs <mode>'s operation ONCE, as the first thing a fresh pthread does, and
 * prints:
 *   usable = bytes between the thread's sp and the lowest usable stack address
 *            (pthread_getattr_np), i.e. what the thread really has to spend.
 *   depth  = peak stack bytes the operation used below the caller's sp,
 *            measured by painting the stack with 0xA5 and scanning for the
 *            lowest overwritten byte.
 * The process must not allocate before the thread runs, so nothing is printed
 * until after the join.
 *
 * "umem" calls umem_alloc(64) directly (the bench_main -a umem path);
 * "malloc" calls malloc(64), i.e. glibc, or whatever LD_PRELOAD supplies;
 * "dlopen" is dlopen(NULL, RTLD_LAZY)+dlclose, what envvar.c does;
 * "ncpus" is umem_get_max_ncpus() via dlsym (init_lib.c).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *umem_alloc(size_t, int);

static const char *mode;
static size_t usable, depth;
static void *result;

static __attribute__((noinline)) void
paint(volatile unsigned char *lo, volatile unsigned char *hi)
{
	while (lo < hi)
		*lo++ = 0xA5;
}

static __attribute__((noinline)) void
op(void)
{
	if (strcmp(mode, "umem") == 0) {
		result = umem_alloc(64, 0);
	} else if (strcmp(mode, "malloc") == 0) {
		result = malloc(64);
	} else if (strcmp(mode, "dlopen") == 0) {
		void *h = dlopen(NULL, RTLD_LAZY);
		result = h;
		if (h != NULL)
			(void) dlclose(h);
	} else if (strcmp(mode, "ncpus") == 0) {
		int (*f)(void) = (int (*)(void))dlsym(RTLD_DEFAULT,
		    "umem_get_max_ncpus");
		result = f != NULL ? (void *)(intptr_t)f() : NULL;
	}
}

static void *
thr(void *arg)
{
	pthread_attr_t a;
	void *addr;
	size_t size;
	unsigned char *sp = __builtin_frame_address(0);
	unsigned char *lo, *p;
	int measure = arg != NULL;

	(void) pthread_getattr_np(pthread_self(), &a);
	(void) pthread_attr_getstack(&a, &addr, &size);
	(void) pthread_attr_destroy(&a);
	usable = (size_t)(sp - (unsigned char *)addr);

	lo = (unsigned char *)addr + 4096;
	if (measure && sp - 1024 > lo)
		paint(lo, sp - 1024);
	op();
	if (measure && sp - 1024 > lo) {
		for (p = lo; p < sp && *p == 0xA5; p++)
			;
		depth = (size_t)(sp - p);
	}
	return (NULL);
}

int
main(int argc, char **argv)
{
	pthread_attr_t a;
	pthread_t t;
	size_t kib;
	int rc, measure;

	if (argc < 3) {
		fprintf(stderr, "usage: %s mode stack_kib [measure]\n", argv[0]);
		return (2);
	}
	mode = argv[1];
	kib = strtoul(argv[2], NULL, 0);
	measure = argc > 3;

	(void) pthread_attr_init(&a);
	if (kib != 0 && (rc = pthread_attr_setstacksize(&a, kib * 1024)) != 0) {
		fprintf(stderr, "setstacksize(%zu KiB) rc=%d (PTHREAD_STACK_MIN=%ld)\n",
		    kib, rc, (long)PTHREAD_STACK_MIN);
		return (3);
	}
	if ((rc = pthread_create(&t, &a, thr, measure ? (void *)1 : NULL)) != 0) {
		fprintf(stderr, "pthread_create rc=%d\n", rc);
		return (4);
	}
	(void) pthread_join(t, NULL);
	printf("mode=%s stack_kib=%zu usable=%zu depth=%s%zu result=%s\n",
	    mode, kib, usable, measure ? "" : "n/a:", depth,
	    result != NULL || strcmp(mode, "none") == 0 ? "ok" : "NULL");
	return (0);
}

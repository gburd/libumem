/*
 * probe_first_alloc_stack -- evidence tool for
 * docs/results/2026-09-27-arm-dlopen-stack-crash.md.  Not a test; built by
 * hand in the EC2 job (see the doc for the exact command).
 *
 *   probe <mode> <stack_kib> [m]  mode: umem | malloc | dlopen | ncpus |
 *                                       stinit | numa | none
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
static unsigned char *thr_sp, *thr_lo;

/*
 * PROBE_NEIGHBOUR=1 (build with -DPROBE_NB, which adds 64 KiB of static TLS to
 * EVERY thread and so shifts every "usable" figure by 64 KiB): create a second, default-attr thread AFTER the probe
 * thread (so its mapping lies directly below the probe thread's guard page),
 * fill a TLS array and a stack array with a canary, run the probe op, then
 * have the neighbour count how many canary bytes changed.  Shows whether an
 * over-deep frame in the probe thread wrote into another thread's memory.
 */
#ifdef PROBE_NB
#define	NB_N	(64 * 1024)
#else
#define	NB_N	1	/* keep the default build's static TLS unperturbed */
#endif
static __thread unsigned char nb_tls[NB_N];
static pthread_barrier_t nb_go, nb_done;
static long nb_tls_bad = -1, nb_stk_bad = -1;

static void *
neighbour(void *arg)
{
	volatile unsigned char stk[NB_N];
	long i, t = 0, k = 0;

	(void) arg;
	for (i = 0; i < NB_N; i++)
		nb_tls[i] = stk[i] = 0x5A;
	(void) pthread_barrier_wait(&nb_go);	/* probe op may run now */
	(void) pthread_barrier_wait(&nb_done);	/* probe op finished */
	for (i = 0; i < NB_N; i++) {
		t += nb_tls[i] != 0x5A;
		k += stk[i] != 0x5A;
	}
	nb_tls_bad = t;
	nb_stk_bad = k;
	return (NULL);
}

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
	} else if (strcmp(mode, "stinit") == 0 || strcmp(mode, "numa") == 0) {
		/* umem_stacktrace_init (backtrace warm + libdw) / umem_numa_init */
		int (*f)(void) = (int (*)(void))dlsym(RTLD_DEFAULT,
		    mode[0] == 's' ? "umem_stacktrace_init" : "umem_numa_init");
		result = f != NULL ? (void *)(intptr_t)(f() + 100) : NULL;
	} else if (strcmp(mode, "ncpus") == 0) {
		int (*f)(void) = (int (*)(void))dlsym(RTLD_DEFAULT,
		    "umem_get_max_ncpus");
		result = f != NULL ? (void *)(intptr_t)f() : NULL;
	}
}

/*
 * PROBE_LEAVE=<bytes>: run op() with only about <bytes> of stack left, by
 * calling it from under a VLA that uses up the rest.  Models "first
 * allocation made deep in a call chain", and gives arm64 (whose minimum
 * thread stack is 128 KiB) the same budget a 16 KiB x86 thread has.
 */
static __attribute__((noinline)) void
op_under(size_t burn)
{
	volatile unsigned char pad[burn];

	pad[0] = pad[burn - 1] = 0;
	op();
	__asm__ __volatile__("" : : "r"(pad) : "memory");
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
	thr_sp = sp;
	thr_lo = addr;

	if (getenv("PROBE_NEIGHBOUR") != NULL)
		(void) pthread_barrier_wait(&nb_go);
	lo = (unsigned char *)addr + 4096;
	if (measure && sp - 1024 > lo)
		paint(lo, sp - 1024);
	if (getenv("PROBE_LEAVE") != NULL &&
	    usable > strtoul(getenv("PROBE_LEAVE"), NULL, 0) + 512)
		op_under(usable - strtoul(getenv("PROBE_LEAVE"), NULL, 0) - 512);
	else
		op();
	if (measure && sp - 1024 > lo) {
		for (p = lo; p < sp && *p == 0xA5; p++)
			;
		depth = (size_t)(sp - p);
	}
	if (getenv("PROBE_NEIGHBOUR") != NULL)
		(void) pthread_barrier_wait(&nb_done);
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
	if (getenv("PROBE_NEIGHBOUR") != NULL) {
		(void) pthread_barrier_init(&nb_go, NULL, 2);
		(void) pthread_barrier_init(&nb_done, NULL, 2);
	}
	if ((rc = pthread_create(&t, &a, thr, measure ? (void *)1 : NULL)) != 0) {
		fprintf(stderr, "pthread_create rc=%d\n", rc);
		return (4);
	}
	if (getenv("PROBE_NEIGHBOUR") != NULL) {
		pthread_t nb;
		/* must be created after t: mmap places it below t's guard */
		(void) pthread_create(&nb, NULL, neighbour, NULL);
		(void) pthread_join(t, NULL);
		(void) pthread_join(nb, NULL);
		printf("neighbour: tls canary bytes changed=%ld, stack canary "
		    "bytes changed=%ld\n", nb_tls_bad, nb_stk_bad);
	} else {
		(void) pthread_join(t, NULL);
	}
	printf("mode=%s stack_kib=%zu usable=%zu depth=%s%zu result=%s",
	    mode, kib, usable, measure ? "" : "n/a:", depth,
	    result != NULL || strcmp(mode, "none") == 0 ? "ok" : "NULL");
	if (strcmp(mode, "ncpus") == 0)
		printf(" ncpus=%ld", (long)(intptr_t)result);
	{
		uint32_t *mx = dlsym(RTLD_DEFAULT, "umem_max_ncpus");
		if (mx != NULL && strcmp(mode, "umem") == 0)
			printf(" umem_max_ncpus=%u", *mx);
	}
	printf("\n");
	/*
	 * PROBE_WHERE=<bytes>: name the mapping that holds thr_sp - bytes, to
	 * show where a frame deeper than the thread's stack actually wrote.
	 */
	if (getenv("PROBE_WHERE") != NULL) {
		uintptr_t want = (uintptr_t)thr_sp -
		    strtoul(getenv("PROBE_WHERE"), NULL, 0);
		char line[512];
		FILE *f = fopen("/proc/self/maps", "r");

		printf("thread stack lo=%p sp=%p; target=%#lx\n",
		    (void *)thr_lo, (void *)thr_sp, (unsigned long)want);
		while (f != NULL && fgets(line, sizeof (line), f) != NULL) {
			unsigned long a, b;
			if (sscanf(line, "%lx-%lx", &a, &b) == 2 &&
			    ((want >= a && want < b) ||
			    ((uintptr_t)thr_lo >= a && (uintptr_t)thr_lo <= b)))
				printf("  maps: %s", line);
		}
		if (f != NULL)
			fclose(f);
	}
	return (0);
}

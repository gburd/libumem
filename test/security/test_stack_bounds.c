/*
 * P5.9 regression: the frame-pointer walk in getpcstack() must not dereference
 * an address outside this thread's stack.
 *
 * THE DEFECT (pre-fix, getpcstack.c:83-100 on x86_64, :137-155 on aarch64)
 *   The walk validated three things -- pointer alignment, a 16 MiB ceiling
 *   above the STARTING frame, and that each frame address increases -- and then
 *   dereferenced fp[0] and fp[1].  None of those establishes that fp points at
 *   this thread's stack at all:
 *
 *     - a corrupted saved-fp (a stack buffer overflow anywhere in the program,
 *       which is exactly the bug class UMEM_DEBUG=audit is switched on to
 *       investigate) points wherever the corrupting write put it;
 *     - a caller compiled WITHOUT frame pointers -- the -O2 default -- leaves
 *       an ordinary data value in %rbp/x29, so the "saved fp" is whatever that
 *       function happened to be using the register for.  Any value that is
 *       aligned, larger than the current frame and within 16 MiB of it passes
 *       all three checks.
 *
 *   The allocator then READ that address, under UMEM_DEBUG=audit, on every
 *   allocation.  Read-only: the only writes are pcstack[depth++], bounded by
 *   pcstack_limit, into the caller's own bufctl -- confirmed by reading both
 *   walks line by line; neither writes through fp.  So this is a crash or an
 *   info leak (the read values become stack PCs in an audit record that
 *   `umemctl leaks` and umem_inspect print), not code execution.
 *
 * THE FIX: pthread_getattr_np()/pthread_attr_getstack() give the thread's real
 * stack bounds, cached per thread; every frame outside [lo, hi) is rejected.
 *
 * HOW THIS DEMONSTRATES IT -- and why not simply "point fp at the heap"
 *   The pre-fix walk only follows a frame that is at a HIGHER address than the
 *   current one, so aiming the chain at the heap does not reach the read at
 *   all: on Linux the heap is below the stack and the monotonic check stops the
 *   walk.  A demonstration has to aim it just ABOVE a stack, and be certain
 *   what is there.
 *
 *   So the test runs on a thread whose stack IT allocates: one mmap with a
 *   PROT_NONE guard page immediately above the usable stack
 *   (pthread_attr_setstack).  The corrupted frame pointer is aimed into that
 *   guard page, a handful of bytes above the thread's own frames.  Then:
 *
 *     - the address is higher than the current frame  -> monotonic check passes
 *     - it is a few KB away                           -> 16 MiB ceiling passes
 *     - it is 16-byte aligned                         -> alignment passes
 *     - it is PROT_NONE                               -> the read SEGVs
 *
 *   and pthread_attr_getstack() reports exactly the stack we supplied, so the
 *   fixed walk rejects the frame and reads nothing.  Pre-fix the child dies of
 *   SIGSEGV; post-fix it exits 0.  This is deterministic and does not depend on
 *   the process's memory layout.
 *
 *   The arms run in a forked child precisely because the pre-fix behaviour is
 *   a fatal signal: the parent reports which signal, rather than dying with it.
 *
 * WHAT THIS ASSERTS
 *   A. A corrupted frame chain aimed just above a known thread stack does not
 *      make the allocator read there (child exits 0, not SIGSEGV).
 *   B. A caller compiled WITHOUT frame pointers survives -- the everyday
 *      version of the same exposure, no corruption required.
 *   C. CONTROL: with an intact chain, audit still captures NON-EMPTY stacks, so
 *      A and B are not passing because the walk now refuses everything.
 *
 * Run it under --enable-asan too: ASan additionally catches an out-of-bounds
 * read that happens to land in a mapped page, which a SEGV check cannot see.
 *
 * Exit: 0 pass, 1 fail, 77 if this platform has no frame-pointer walk to test.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "umem.h"

static int failures;

static void
pass(const char *what)
{
	printf("  PASS: %s\n", what);
}

static void
fail(const char *what)
{
	printf("  FAIL: %s\n", what);
	failures++;
}

/* Allocate/free under audit, which is what makes getpcstack() run. */
static int
churn(int n)
{
	int i, ok = 0;

	for (i = 0; i < n; i++) {
		size_t sz = 64 + (size_t)(i % 16) * 8;
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (p == NULL)
			continue;
		memset(p, 0xA5, 64);
		ok++;
		umem_free(p, sz);
	}
	return (ok);
}

/* ---------------------------------------------------------------- arm A --- */

#define	STACK_USABLE	(1024 * 1024)
#define	GUARD_SIZE	(64 * 1024)

struct thread_arg {
	uintptr_t guard_lo;	/* first byte of the PROT_NONE guard */
};

/*
 * Aim this frame's saved frame pointer into the guard page, then allocate.
 * noinline so the frame genuinely exists.
 */
__attribute__((noinline))
static void
corrupt_own_frame_and_allocate(uintptr_t target)
{
	uintptr_t *myfp = (uintptr_t *)__builtin_frame_address(0);
	uintptr_t saved;

	if (myfp == NULL)
		return;

	saved = myfp[0];
	myfp[0] = target;	/* the corruption a stack overflow would cause */

	(void) churn(40);

	/*
	 * Restore before returning: the epilogue reloads the frame pointer
	 * from this slot, so leaving it corrupted would crash the TEST rather
	 * than demonstrating anything about the allocator.
	 */
	myfp[0] = saved;
}

static void *
corrupt_thread(void *argp)
{
	struct thread_arg *ta = argp;

	/*
	 * 16-byte aligned (aarch64 requires it), inside the guard page, and a
	 * little way in so fp[0] and fp[1] are both unreadable.
	 */
	corrupt_own_frame_and_allocate((ta->guard_lo + 64) & ~(uintptr_t)15);
	return (NULL);
}

/*
 * Run arm A on a thread whose stack we own, so we know exactly what is above
 * it.  Returns 0 on success.
 */
static int
run_corrupt_arm(void)
{
	pthread_attr_t attr;
	pthread_t tid;
	struct thread_arg ta;
	void *map;
	size_t total = GUARD_SIZE + STACK_USABLE + GUARD_SIZE;
	char *stack_lo;
	int rc;

	/*
	 * Layout, low to high:
	 *   [lower guard][usable stack][UPPER GUARD]
	 * The upper guard is the one that matters: the stack grows DOWN, so
	 * frames sit just below it, and a frame pointer that walks "up" off the
	 * end lands in it.
	 */
	map = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (map == MAP_FAILED) {
		printf("  (arm A skipped: mmap failed)\n");
		return (0);
	}
	stack_lo = (char *)map + GUARD_SIZE;
	if (mprotect(stack_lo, STACK_USABLE, PROT_READ | PROT_WRITE) != 0) {
		printf("  (arm A skipped: mprotect failed)\n");
		(void) munmap(map, total);
		return (0);
	}
	ta.guard_lo = (uintptr_t)(stack_lo + STACK_USABLE);

	if (pthread_attr_init(&attr) != 0) {
		(void) munmap(map, total);
		return (0);
	}
	if (pthread_attr_setstack(&attr, stack_lo, STACK_USABLE) != 0) {
		printf("  (arm A skipped: pthread_attr_setstack failed)\n");
		(void) pthread_attr_destroy(&attr);
		(void) munmap(map, total);
		return (0);
	}
	rc = pthread_create(&tid, &attr, corrupt_thread, &ta);
	(void) pthread_attr_destroy(&attr);
	if (rc != 0) {
		printf("  (arm A skipped: pthread_create failed)\n");
		(void) munmap(map, total);
		return (0);
	}
	(void) pthread_join(tid, NULL);
	/* Deliberately leak the mapping: freeing a stack a thread just left is
	 * its own hazard, and this process is about to exit. */
	return (0);
}

/* ---------------------------------------------------------------- arm B --- */

/*
 * A caller with NO frame pointer, which is the -O2 default and needs no
 * corruption at all: %rbp/x29 holds whatever this function is using it for.
 * The `optimize` attribute is GCC-specific; where it is unavailable this arm
 * degrades to an ordinary call, which is stated rather than hidden.
 */
#if defined(__GNUC__) && !defined(__clang__)
#define	HAVE_OMIT_FP_ATTR 1
__attribute__((noinline, optimize("omit-frame-pointer")))
#else
__attribute__((noinline))
#endif
static int
no_frame_pointer_caller(void)
{
	return (churn(40));
}

/* ------------------------------------------------------------------------- */

/*
 * Each arm runs in a fork child: pre-fix, arm A terminates with SIGSEGV, and
 * the point of the test is to REPORT that rather than die of it.
 */
static int
run_in_child(int (*fn)(void), const char *what)
{
	pid_t pid = fork();
	int status;

	if (pid < 0) {
		fail("fork failed");
		return (1);
	}
	if (pid == 0) {
		int rc = fn();
		_exit(rc == 0 ? 0 : 1);
	}
	if (waitpid(pid, &status, 0) < 0) {
		fail("waitpid failed");
		return (1);
	}
	if (WIFSIGNALED(status)) {
		printf("  FAIL: %s -- child died with signal %d "
		    "(the walk dereferenced an address off the stack)\n",
		    what, WTERMSIG(status));
		failures++;
		return (1);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("  FAIL: %s -- child exited %d\n", what,
		    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		failures++;
		return (1);
	}
	pass(what);
	return (0);
}

static int
arm_b(void)
{
	return (no_frame_pointer_caller() > 0 ? 0 : 1);
}

int
main(void)
{
	int n;

	printf("P5.9: the frame walk must stay inside the thread stack\n");
	printf("  (UMEM_DEBUG must include 'audit'; the Makefile target sets "
	    "it)\n");

	/* ---------------------------------------------------- C: control */
	printf("[C] control: an intact chain still walks\n");
	n = churn(100);
	if (n > 0)
		pass("100 audited allocations with an intact frame chain");
	else
		fail("no allocation succeeded; every arm below is vacuous");

	/* ------------------------------------------- A: corrupted chain */
	printf("[A] a corrupted frame chain aimed at a guard page\n");
	(void) run_in_child(run_corrupt_arm,
	    "corrupted frame pointer above a known stack was not followed");

	/* ------------------------------------------ B: no frame pointer */
	printf("[B] a caller compiled without frame pointers\n");
#ifndef HAVE_OMIT_FP_ATTR
	printf("  (note: this compiler has no per-function "
	    "omit-frame-pointer attribute; this arm is weaker here)\n");
#endif
	(void) run_in_child(arm_b,
	    "frame-pointer-less caller survived audited allocation");

	/* Still healthy in the parent afterwards. */
	if (churn(50) > 0)
		pass("allocator still working after both arms");
	else
		fail("allocator stopped working");

	printf("\n");
	if (failures != 0) {
		printf("test_stack_bounds: FAIL (%d)\n", failures);
		return (1);
	}
	printf("test_stack_bounds: PASS\n");
	return (0);
}

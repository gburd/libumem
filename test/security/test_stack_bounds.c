/*
 * P5.9 regression: the frame-pointer walk in getpcstack() must not dereference
 * an address outside this thread's stack.
 *
 * THE DEFECT (pre-fix, getpcstack.c:83-100 on x86_64, :137-155 on aarch64)
 *   The walk validated three things -- pointer alignment, a 16 MiB ceiling
 *   above the STARTING frame, and that each frame address increases -- and then
 *   dereferenced fp[0] and fp[1].  None of those establishes that fp points at
 *   this thread's stack:
 *
 *     - a corrupted saved-fp (a stack buffer overflow anywhere in the program,
 *       which is the bug class UMEM_DEBUG=audit is switched on to investigate)
 *       points wherever the corrupting write put it;
 *     - a caller compiled WITHOUT frame pointers leaves an ordinary data value
 *       in %rbp/x29, so the "saved fp" is whatever that function was using the
 *       register for.  Any value that is aligned, higher than the current frame
 *       and within 16 MiB of it passed all three checks.
 *
 *   The allocator then READ that address.  Read-only: the only writes are
 *   pcstack[depth++], bounded by pcstack_limit, into the caller's own bufctl --
 *   confirmed by reading both walks line by line; neither writes through fp.
 *   So this is a crash or an info leak (the values become stack PCs in an audit
 *   record that `umemctl leaks` and umem_inspect print), not code execution.
 *
 * THE FIX: pthread_getattr_np()/pthread_attr_getstack() give the thread's real
 * stack bounds, cached per thread; every frame outside [lo, hi) is rejected.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS TEST CALLS getpcstack() DIRECTLY AND NOT THROUGH umem_alloc()
 *
 *   The first version of this test corrupted its own frame and then allocated
 *   under UMEM_DEBUG=audit, on the assumption that the walk would climb from
 *   inside the allocator up into the corrupted frame.  IT DOES NOT, and the
 *   test passed even with the fix reverted -- i.e. it was vacuous.  Measured
 *   cause, on the build this repo produces:
 *
 *     - the library is compiled -O2 with no -fno-omit-frame-pointer
 *       (CFLAGS = "-g -O2 -std=c17 ..."), and objdump confirms _umem_alloc
 *       opens with `push %r15`, i.e. it keeps no frame pointer;
 *     - so a walk that starts inside umem_alloc terminates after ~2 frames,
 *       before ever reaching the caller's frame.  Measured: depth 2 through
 *       umem_alloc, depth 7 from an -fno-omit-frame-pointer caller.
 *
 *   Two consequences, both stated rather than papered over:
 *
 *     1. The reachable exposure is NARROWER than "the -O2 default makes this
 *        worse": in the default build getpcstack barely walks, so audit
 *        records are ~2 frames deep whatever the caller does.  The
 *        arbitrary-read exposure needs a build whose LIBRARY keeps frame
 *        pointers -- --enable-asan does, since it adds
 *        -fno-omit-frame-pointer -- or a chain that does.
 *     2. A regression must therefore drive getpcstack() the way that build
 *        drives it: called from a frame-pointer-having caller, which is
 *        exactly what this file is compiled as (see the Makefile.am entry).
 *
 *   So this test calls getpcstack() directly.  That is the function under
 *   test, it is the entry point UMEM_AUDIT uses, and it is the only way to
 *   reach the walk deterministically in every build.
 * ---------------------------------------------------------------------------
 *
 * HOW ARM A IS DETERMINISTIC
 *   The corrupted frame must be aimed somewhere we KNOW is unreadable, and
 *   above the current frame (the walk only climbs).  So the arm runs on a
 *   thread whose stack the test allocates: one mmap with a PROT_NONE guard page
 *   immediately above the usable stack, installed with pthread_attr_setstack.
 *   The corrupted frame pointer is aimed a few bytes into that guard page:
 *
 *     - higher than the current frame  -> the monotonic check passes
 *     - a few KB away                  -> the 16 MiB ceiling passes
 *     - 16-byte aligned                -> the alignment check passes
 *     - PROT_NONE                      -> the read SEGVs
 *
 *   and pthread_attr_getstack() reports exactly the stack we supplied, so the
 *   fixed walk rejects the frame and reads nothing.
 *
 *   The arms run in a fork child because the pre-fix behaviour is a fatal
 *   signal: the parent reports which signal rather than dying with it.
 *
 * WHAT THIS ASSERTS
 *   A. A frame pointer aimed into a guard page just above a known thread stack
 *      is NOT dereferenced (child exits 0; pre-fix it dies of SIGSEGV).
 *   B. The walk still refuses it when the aim is further up, past the mapping
 *      entirely -- the "wandered off the top" case the 16 MiB ceiling was
 *      originally written for, which must keep working.
 *   C. CONTROL: an uncorrupted chain still produces a NON-ZERO depth on the
 *      same thread, so A and B are not passing because the walk now refuses
 *      everything.  This is the vacuity guard, and it is the check that would
 *      have caught the first version of this test.
 *
 * PRE-FIX DEMONSTRATION: with the bounds consultation removed from
 * getpcstack.c, arm A's child dies with SIGSEGV.  Verified on x86_64 AND
 * aarch64 via scripts/ec2/p5_6_9_prefix_demo.sh: control arm depth 3, then
 * "child died with signal 11 (the walk dereferenced an address off the stack)".
 * Instrumenting the walk in-library prints
 *   DBG fp=0x7fb6f91c5040 hb=1 lo=0x7fb6f90c5000 hi=0x7fb6f91c5000 ok=0
 * post-fix -- the frame aimed into the guard page is rejected (ok=0) and never
 * dereferenced.
 *
 * A NOTE ON WHAT THIS TEST DOES AND DOES NOT COVER.  It exercises the walk in
 * the configuration where the defect is REACHABLE: getpcstack() entered from a
 * caller that keeps frame pointers.  In a default -O2 build the allocator's own
 * frames have no frame pointer, so a walk entered through umem_alloc() stops
 * after ~2 frames and cannot reach a corrupted caller frame at all.  That makes
 * the default build largely insulated from P5.9 by accident -- and also means
 * UMEM_DEBUG=audit records are ~2 frames deep there, which is a separate,
 * user-facing limitation recorded in umem_debugging.7.
 *
 * Run it under --enable-asan as well: ASan catches an out-of-bounds read that
 * lands in a mapped page, which a SEGV check cannot see.
 *
 * Exit: 0 pass, 1 fail, 77 no frame-pointer walk on this platform.
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

/*
 * getpcstack() is the function under test.  It is internal (misc.h), not part
 * of the installed API, so declare it here rather than pulling in a private
 * header the installed-header check would then have to care about.
 */
extern int getpcstack(uintptr_t *pcstack, int pcstack_limit, int check_signal);

/*
 * Only the two Linux frame-pointer walks are what P5.9 is about.  Elsewhere
 * getpcstack() is either the Solaris implementation (which has always had real
 * stack bounds) or the backtrace(3)/dummy fallback (no frame walk at all), so
 * there is nothing here to demonstrate and a PASS would be meaningless.
 */
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64) || \
    defined(__i386) || defined(__aarch64__) || defined(__arm64__))
#define	P59_APPLICABLE 1
#endif

#ifndef P59_APPLICABLE

int
main(void)
{
	printf("SKIP: no frame-pointer walk on this platform "
	    "(getpcstack uses bounds-checked or backtrace(3) paths)\n");
	return (77);
}

#else

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

#define	STACK_USABLE	(1024 * 1024)
#define	GUARD_SIZE	(64 * 1024)

/* What the child thread should do with its frame pointer. */
enum aim {
	AIM_NONE,		/* leave it alone: the control arm */
	AIM_GUARD,		/* just above the stack, inside the guard */
	AIM_PAST_MAPPING	/* well past the mapping entirely */
};

struct thread_arg {
	enum aim	aim;
	uintptr_t	guard_lo;	/* first byte of the upper guard */
	int		depth;		/* out: what getpcstack returned */
};

/*
 * Aim a frame pointer at `target` and walk from a frame BELOW it.
 *
 * WHY TWO FUNCTIONS, and why the corrupted slot is not this frame's own:
 *   The obvious shape -- overwrite __builtin_frame_address(0)[0], call
 *   getpcstack(), put it back -- is not safe to compile at -O2.  The compiler
 *   is entitled to keep the frame pointer in a register across the call, to
 *   reorder the restore, or to use the slot itself; the test then corrupts its
 *   own return path and dies of SIGSEGV in its epilogue, which looks exactly
 *   like the defect it is supposed to be detecting.  It did: an -O0 in this
 *   file's CFLAGS was overridden by the -O2 that CFLAGS appends later on the
 *   command line, and both arms SEGVed with the FIX IN PLACE while
 *   instrumentation proved the walk itself had correctly rejected the frame
 *   (ok=0) and returned.
 *
 *   So the corruption goes in a frame that has already stopped executing:
 *   victim_frame() records the address of its own frame-pointer slot and
 *   returns; the caller then writes `target` into that slot and walks from a
 *   DEEPER frame, so the walk climbs through the poisoned link while no live
 *   function depends on it.  Nothing this test executes afterwards reads the
 *   slot, so the arm cannot self-inflict a crash and any SIGSEGV is the
 *   allocator's.
 *
 * Returns the depth getpcstack() reported.
 */
struct walk_ctx {
	uintptr_t	*slot;		/* frame-pointer slot to poison */
	uintptr_t	target;		/* what to poison it with */
	int		depth;		/* out */
};

/*
 * Establish a frame, hand its frame-pointer slot up, and return.  The frame is
 * dead from here on, which is what makes poisoning it safe.
 */
__attribute__((noinline))
static void
victim_frame(struct walk_ctx *ctx, void (*next)(struct walk_ctx *))
{
	ctx->slot = (uintptr_t *)__builtin_frame_address(0);
	next(ctx);
}

/* Called from inside victim_frame: poison the caller's slot, then walk. */
__attribute__((noinline))
static void
poison_and_walk(struct walk_ctx *ctx)
{
	uintptr_t pcstack[32];
	uintptr_t saved = ctx->slot[0];

	if (ctx->target != 0)
		ctx->slot[0] = ctx->target;

	/*
	 * The walk starts in THIS frame and climbs into victim_frame's, whose
	 * saved-fp link now points at `target`.
	 */
	ctx->depth = getpcstack(pcstack, 20, 0);

	/* Restore before victim_frame's epilogue runs. */
	ctx->slot[0] = saved;
}

static int
walk_with_frame_aimed_at(uintptr_t target)
{
	struct walk_ctx ctx;

	ctx.slot = NULL;
	ctx.target = target;
	ctx.depth = -1;
	victim_frame(&ctx, poison_and_walk);
	return (ctx.depth);
}

static void *
corrupt_thread(void *argp)
{
	struct thread_arg *ta = argp;
	uintptr_t target = 0;

	switch (ta->aim) {
	case AIM_NONE:
		target = 0;
		break;
	case AIM_GUARD:
		/*
		 * A little way into the guard page, 16-byte aligned (aarch64
		 * requires it), so both fp[0] and fp[1] are unreadable.
		 */
		target = (ta->guard_lo + 64) & ~(uintptr_t)15;
		break;
	case AIM_PAST_MAPPING:
		/* Past the guard too: unmapped, still within 16 MiB. */
		target = (ta->guard_lo + GUARD_SIZE + 4096) & ~(uintptr_t)15;
		break;
	}

	ta->depth = walk_with_frame_aimed_at(target);
	return (NULL);
}

/*
 * Run one arm on a thread whose stack we allocate, so we know exactly what lies
 * above it.  Returns the depth getpcstack reported, or -1 if the arm could not
 * be set up.
 */
static int
run_on_own_stack(enum aim aim)
{
	pthread_attr_t attr;
	pthread_t tid;
	struct thread_arg ta;
	void *map;
	size_t total = GUARD_SIZE + STACK_USABLE + GUARD_SIZE;
	char *stack_lo;
	int rc;

	/*
	 * Layout, low to high:  [guard][usable stack][UPPER GUARD]
	 * The upper guard is the one that matters: the stack grows DOWN, so
	 * frames sit just below it and a frame pointer walking "up" off the end
	 * lands in it.
	 */
	map = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (map == MAP_FAILED)
		return (-1);
	stack_lo = (char *)map + GUARD_SIZE;
	if (mprotect(stack_lo, STACK_USABLE, PROT_READ | PROT_WRITE) != 0) {
		(void) munmap(map, total);
		return (-1);
	}

	ta.aim = aim;
	ta.guard_lo = (uintptr_t)(stack_lo + STACK_USABLE);
	ta.depth = -1;

	if (pthread_attr_init(&attr) != 0) {
		(void) munmap(map, total);
		return (-1);
	}
	if (pthread_attr_setstack(&attr, stack_lo, STACK_USABLE) != 0) {
		(void) pthread_attr_destroy(&attr);
		(void) munmap(map, total);
		return (-1);
	}
	rc = pthread_create(&tid, &attr, corrupt_thread, &ta);
	(void) pthread_attr_destroy(&attr);
	if (rc != 0) {
		(void) munmap(map, total);
		return (-1);
	}
	(void) pthread_join(tid, NULL);
	/* The mapping is deliberately leaked: unmapping a stack a thread has
	 * just left is its own hazard, and this process is about to exit. */
	return (ta.depth);
}

/*
 * Each arm runs in a fork child: pre-fix, the corrupted arms terminate with
 * SIGSEGV, and the point is to REPORT that rather than die of it.
 *
 * The child's exit status carries the depth (clamped), so the parent can apply
 * the vacuity check without sharing memory.
 */
static int
child_status_for(enum aim aim, int *depth_out, int *signalled)
{
	pid_t pid = fork();
	int status;

	*depth_out = -1;
	*signalled = 0;

	if (pid < 0)
		return (-1);
	if (pid == 0) {
		int d = run_on_own_stack(aim);
		if (d < 0)
			_exit(100);		/* setup failed */
		if (d > 99)
			d = 99;
		_exit(d);
	}
	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	if (WIFSIGNALED(status)) {
		*signalled = WTERMSIG(status);
		return (0);
	}
	if (!WIFEXITED(status))
		return (-1);
	if (WEXITSTATUS(status) == 100)
		return (-1);		/* setup failed in the child */
	*depth_out = WEXITSTATUS(status);
	return (0);
}

static void
run_arm(enum aim aim, const char *what)
{
	int depth, sig;

	if (child_status_for(aim, &depth, &sig) != 0) {
		printf("  (skipped: %s -- could not set up the arm)\n", what);
		return;
	}
	if (sig != 0) {
		printf("  FAIL: %s -- child died with signal %d "
		    "(the walk dereferenced an address off the stack)\n",
		    what, sig);
		failures++;
		return;
	}
	printf("  PASS: %s (walk returned depth %d and read nothing "
	    "off-stack)\n", what, depth);
}

int
main(void)
{
	int control_depth, sig;

	printf("P5.9: the frame walk must stay inside the thread stack\n");

	/*
	 * ---------------------------------------------------- C: control
	 *
	 * THE VACUITY GUARD, and the reason this test looks the way it does:
	 * the first version of it drove the walk through umem_alloc(), where
	 * the library's own -O2 frames carry no frame pointer, so the walk
	 * stopped after ~2 frames and never reached the corruption.  It passed
	 * with the fix reverted.  If the walk cannot produce a multi-frame
	 * chain from an uncorrupted caller here, every arm below is vacuous and
	 * this test says so instead of reporting green.
	 */
	printf("[C] control: an uncorrupted chain walks more than one frame\n");
	if (child_status_for(AIM_NONE, &control_depth, &sig) != 0 || sig != 0) {
		fail("the control arm could not run; the arms below would be "
		    "vacuous");
		printf("\ntest_stack_bounds: FAIL (%d)\n", failures);
		return (1);
	}
	if (control_depth >= 2) {
		printf("  PASS: uncorrupted walk returned depth %d\n",
		    control_depth);
	} else {
		printf("  FAIL: uncorrupted walk returned depth %d -- the walk "
		    "is not reaching the frames this test corrupts, so arms A "
		    "and B prove nothing\n", control_depth);
		failures++;
		printf("\ntest_stack_bounds: FAIL (%d)\n", failures);
		return (1);
	}

	/* ------------------------------------------- A: aimed at a guard */
	printf("[A] a frame pointer aimed into the guard page above the "
	    "stack\n");
	run_arm(AIM_GUARD,
	    "corrupted frame pointer in a guard page was not dereferenced");

	/* ------------------------------------ B: aimed past the mapping */
	printf("[B] a frame pointer aimed past the mapping entirely\n");
	run_arm(AIM_PAST_MAPPING,
	    "frame pointer past the whole mapping was not dereferenced");

	printf("\n");
	if (failures != 0) {
		printf("test_stack_bounds: FAIL (%d)\n", failures);
		return (1);
	}
	printf("test_stack_bounds: PASS\n");
	return (0);
}

#endif /* P59_APPLICABLE */

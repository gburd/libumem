/*
 * test/unit/test_hook_contracts.c -- enforce the umem_hooks.h concurrency
 * contract (Phase 3 item 3 of docs/plans/2026-09-21-production-readiness.md).
 *
 * Each test corresponds to a labelled clause of the CONCURRENCY CONTRACT
 * block in umem_hooks.h and fails if the clause is violated.
 *
 * PRE-FIX BEHAVIOUR (all four failed before the fix):
 *
 *   L1  unregister_drains: tracking read hook_active, called the user
 *       callback OUTSIDE the registry lock, and then came back to touch the
 *       hook again for statistics.  Another thread could unregister and free
 *       the hook in that window, while umem_hooks.3 explicitly permitted
 *       freeing on unregister return.  Here the hook lives in a heap block
 *       that the unregistering thread poisons and frees the instant
 *       unregister returns, so a surviving in-flight caller writes freed
 *       memory -- caught by the poison check, and by ASan when built with it.
 *
 *   L2  walk_reentrant: umem_hook_walk() invoked its callback with the
 *       registry mutex held, so registering, unregistering, or dumping from
 *       inside a walk callback deadlocked (self-deadlock on a non-recursive
 *       mutex).  Pre-fix this test HANGS; the watchdog turns the hang into a
 *       failure rather than an infinite CI run.
 *
 *   L4  walk_stops: a nonzero callback return did not stop iteration,
 *       contradicting umem_hooks.3.
 *
 *   L1  unregister_is_idempotent: unregister of a never-registered hook.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem_hooks.h"

static int failures;

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
			    __FILE__, __LINE__, (msg));			\
			failures++;					\
		}							\
	} while (0)

/* ------------------------------------------------------------------ */
/* Watchdog: a contract violation in this file shows up as a deadlock, */
/* and a hung test is indistinguishable from a slow one in CI.         */
/* ------------------------------------------------------------------ */

static void *
watchdog(void *arg)
{
	int secs = *(int *)arg;
	sleep((unsigned)secs);
	fprintf(stderr,
	    "FAIL: watchdog fired after %ds -- a hook operation deadlocked "
	    "(see contract L2 in umem_hooks.h)\n", secs);
	fflush(stderr);
	_exit(1);
}

static void
watchdog_start(int secs)
{
	static int s;
	pthread_t t;
	s = secs;
	if (pthread_create(&t, NULL, watchdog, &s) == 0)
		(void) pthread_detach(t);
}

/* ------------------------------------------------------------------ */
/* L1: umem_hook_unregister() drains in-flight callbacks.             */
/* ------------------------------------------------------------------ */

/*
 * The hook lives inside this wrapper.  After unregister returns, the owner
 * poisons the whole wrapper and frees it.  Any thread still inside the hook
 * then either writes freed memory (ASan/valgrind catch it) or observes the
 * poison, which we detect explicitly so the test also fails in a plain build.
 */
struct owned_hook {
	umem_hook_t hook;
	volatile int poisoned;
	volatile int in_callback;	/* callbacks currently executing */
};

#define POISON_BYTE 0xa5

static volatile int l1_stop;
static volatile int l1_poison_observed;
static volatile unsigned long l1_track_calls;
static struct owned_hook *volatile l1_current;

static void *
l1_alloc(size_t size, void *arg)
{
	struct owned_hook *oh = arg;

	/*
	 * Widen the window the pre-fix code had: the callback runs with no
	 * lock held, and unregister used to be free to complete here.
	 */
	__sync_fetch_and_add(&oh->in_callback, 1);
	if (oh->poisoned)
		l1_poison_observed = 1;
	sched_yield();
	if (oh->poisoned)
		l1_poison_observed = 1;
	__sync_fetch_and_sub(&oh->in_callback, 1);

	(void) size;
	return ((void *)(uintptr_t)0x1);	/* never dereferenced */
}

static void
l1_free(void *ptr, void *arg)
{
	(void) ptr; (void) arg;
}

/* Hammer track_alloc on whatever hook is currently published. */
static void *
l1_tracker(void *unused)
{
	(void) unused;
	while (!l1_stop) {
		struct owned_hook *oh = l1_current;
		if (oh == NULL)
			continue;
		(void) umem_hook_track_alloc(&oh->hook, 64);
		__sync_fetch_and_add(&l1_track_calls, 1);
	}
	return (NULL);
}

static void
test_unregister_drains(void)
{
	enum { NTHREADS = 4, NROUNDS = 400 };
	pthread_t th[NTHREADS];
	int i;

	l1_stop = 0;
	l1_poison_observed = 0;
	l1_track_calls = 0;
	l1_current = NULL;

	for (i = 0; i < NTHREADS; i++)
		CHECK(pthread_create(&th[i], NULL, l1_tracker, NULL) == 0,
		    "pthread_create");

	for (i = 0; i < NROUNDS; i++) {
		struct owned_hook *oh = calloc(1, sizeof (*oh));
		CHECK(oh != NULL, "calloc");
		if (oh == NULL)
			break;
		oh->hook.hook_name = "drain_test";
		oh->hook.hook_alloc = l1_alloc;
		oh->hook.hook_free = l1_free;
		oh->hook.hook_arg = oh;

		CHECK(umem_hook_register(&oh->hook) == 0, "register");
		l1_current = oh;
		/* Let the tracker threads get inside the callback. */
		sched_yield();

		umem_hook_unregister(&oh->hook);
		l1_current = NULL;

		/*
		 * Contract L1: no thread can be inside the hook now.  If any
		 * is, it sees the poison and/or writes freed memory.
		 */
		CHECK(oh->in_callback == 0,
		    "L1 violated: a callback was still running after "
		    "umem_hook_unregister() returned");
		oh->poisoned = 1;
		memset((char *)oh + sizeof (oh->hook), POISON_BYTE,
		    sizeof (*oh) - sizeof (oh->hook));
		oh->poisoned = 1;
		free(oh);
	}

	l1_stop = 1;
	for (i = 0; i < NTHREADS; i++)
		(void) pthread_join(th[i], NULL);

	CHECK(!l1_poison_observed,
	    "L1 violated: a hook callback ran against a freed hook");
	CHECK(l1_track_calls > 0,
	    "test is vacuous: no tracked allocations executed at all");
	printf("  L1 unregister drains in-flight callbacks: %lu tracked "
	    "calls across %d rounds\n", l1_track_calls, NROUNDS);
}

/* ------------------------------------------------------------------ */
/* L2: no user code under the registry lock.                          */
/* ------------------------------------------------------------------ */

static void *dummy_alloc(size_t s, void *a) { (void)s; (void)a; return (NULL); }
static void dummy_free(void *p, void *a) { (void)p; (void)a; }

static umem_hook_t l2_inner = {
	.hook_name = "l2_inner",
	.hook_alloc = dummy_alloc,
	.hook_free = dummy_free,
};

static int l2_did_register;
static int l2_did_find;
static int l2_did_track;

static int
l2_walk_cb(umem_hook_t *hook, void *arg)
{
	(void) hook; (void) arg;

	/*
	 * Contract L2: all of this is legal from inside a walk callback.
	 * Pre-fix every one of these self-deadlocked on hook_list_lock.
	 */
	if (umem_hook_register(&l2_inner) == 0)
		l2_did_register = 1;
	if (umem_hook_find("l2_outer") != NULL)
		l2_did_find = 1;
	(void) umem_hook_track_alloc(&l2_inner, 16);
	l2_did_track = 1;
	umem_hook_unregister(&l2_inner);
	return (0);
}

static void
test_walk_reentrant(void)
{
	umem_hook_t outer = {
		.hook_name = "l2_outer",
		.hook_alloc = dummy_alloc,
		.hook_free = dummy_free,
	};

	CHECK(umem_hook_register(&outer) == 0, "register outer");
	CHECK(umem_hook_walk(l2_walk_cb, NULL) == 0, "walk");
	CHECK(l2_did_register, "L2: register from a walk callback failed");
	CHECK(l2_did_find, "L2: find from a walk callback failed");
	CHECK(l2_did_track, "L2: track from a walk callback failed");
	umem_hook_unregister(&outer);
	printf("  L2 walk callbacks may re-enter the hook API: ok\n");
}

/* ------------------------------------------------------------------ */
/* L4: a nonzero walk callback stops iteration.                       */
/* ------------------------------------------------------------------ */

static int
l4_stop_cb(umem_hook_t *hook, void *arg)
{
	(void) hook;
	(*(int *)arg)++;
	return (1);
}

static void
test_walk_stops(void)
{
	umem_hook_t a = { .hook_name = "l4_a", .hook_alloc = dummy_alloc };
	umem_hook_t b = { .hook_name = "l4_b", .hook_alloc = dummy_alloc };
	umem_hook_t c = { .hook_name = "l4_c", .hook_alloc = dummy_alloc };
	int visited = 0;

	CHECK(umem_hook_register(&a) == 0, "register a");
	CHECK(umem_hook_register(&b) == 0, "register b");
	CHECK(umem_hook_register(&c) == 0, "register c");

	CHECK(umem_hook_walk(l4_stop_cb, &visited) == -1,
	    "walk should report -1 when a callback stops it");
	CHECK(visited == 1,
	    "L4 violated: a nonzero callback return did not stop the walk");

	umem_hook_unregister(&a);
	umem_hook_unregister(&b);
	umem_hook_unregister(&c);
	printf("  L4 nonzero callback stops the walk: visited %d\n", visited);
}

/* ------------------------------------------------------------------ */
/* L1: unregister is safe on an unregistered / doubly-unregistered    */
/*     hook, since callers rely on it to decide when to free.         */
/* ------------------------------------------------------------------ */

static void
test_unregister_idempotent(void)
{
	umem_hook_t h = { .hook_name = "l1_idem", .hook_alloc = dummy_alloc };

	umem_hook_unregister(&h);		/* never registered */
	CHECK(umem_hook_register(&h) == 0, "register");
	umem_hook_unregister(&h);
	umem_hook_unregister(&h);		/* again */
	CHECK(h.hook_active == 0, "hook_active should be 0 after unregister");
	umem_hook_unregister(NULL);		/* NULL */
	printf("  L1 unregister is idempotent: ok\n");
}

int
main(void)
{
	watchdog_start(120);

	printf("umem_hooks contract tests\n");
	test_unregister_idempotent();
	test_walk_stops();
	test_walk_reentrant();
	test_unregister_drains();

	if (failures != 0) {
		printf("\n%d contract violation(s)\n", failures);
		return (1);
	}
	printf("\nall hook contract tests passed\n");
	return (0);
}

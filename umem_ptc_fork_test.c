/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Fork safety tests for libumem with per-thread caching (PTC).
 *
 * These tests verify that libumem's pthread_atfork handlers correctly
 * manage lock state across fork(2), ensuring that both parent and child
 * processes can continue to allocate and free memory without deadlock
 * or corruption.
 *
 * Test cases:
 *   1. Pre-fork with active cache: parent populates caches, forks,
 *      child verifies it can allocate and free.
 *   2. Post-fork independent allocation: both parent and child allocate
 *      and free memory independently after fork.
 *   3. Multiple children: parent forks several children, each allocates
 *      independently with no cross-contamination.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "umem.h"

#define SMALL_SIZE	32
#define MEDIUM_SIZE	128
#define LARGE_SIZE	512
#define N_ALLOCS	64
#define N_CHILDREN	4
#define MAGIC_BYTE_PARENT	0xAA
#define MAGIC_BYTE_CHILD	0xBB

static int test_num = 0;
static int tests_failed = 0;

static void
pass(const char *name)
{
	test_num++;
	printf("ok %d - %s\n", test_num, name);
}

static void
fail(const char *name)
{
	test_num++;
	tests_failed++;
	printf("not ok %d - %s\n", test_num, name);
}

static void
check(int condition, const char *name)
{
	if (condition)
		pass(name);
	else
		fail(name);
}

/*
 * Fill a buffer with a pattern and verify it.
 */
static void
fill_buf(void *buf, size_t size, unsigned char pattern)
{
	memset(buf, pattern, size);
}

static int
verify_buf(void *buf, size_t size, unsigned char pattern)
{
	unsigned char *p = (unsigned char *)buf;
	size_t i;

	for (i = 0; i < size; i++) {
		if (p[i] != pattern)
			return (0);
	}
	return (1);
}

/*
 * Populate umem caches by allocating and freeing a bunch of buffers.
 * This exercises the magazine layer and, if PTC is active, the
 * per-thread cache.
 */
static void
warm_caches(void)
{
	void *bufs[N_ALLOCS];
	int i;

	for (i = 0; i < N_ALLOCS; i++)
		bufs[i] = umem_alloc(SMALL_SIZE, UMEM_DEFAULT);

	for (i = 0; i < N_ALLOCS; i++) {
		if (bufs[i] != NULL)
			umem_free(bufs[i], SMALL_SIZE);
	}
}

/*
 * Allocate, fill, verify, and free a set of buffers at various sizes.
 * Returns 0 on success, -1 on failure.
 */
static int
alloc_verify_free(unsigned char pattern)
{
	static const size_t sizes[] = { SMALL_SIZE, MEDIUM_SIZE, LARGE_SIZE };
	int nsizes = sizeof (sizes) / sizeof (sizes[0]);
	void *bufs[N_ALLOCS];
	int i, j;

	for (j = 0; j < nsizes; j++) {
		for (i = 0; i < N_ALLOCS; i++) {
			bufs[i] = umem_alloc(sizes[j], UMEM_DEFAULT);
			if (bufs[i] == NULL)
				return (-1);
			fill_buf(bufs[i], sizes[j], pattern);
		}

		for (i = 0; i < N_ALLOCS; i++) {
			if (!verify_buf(bufs[i], sizes[j], pattern))
				return (-1);
			umem_free(bufs[i], sizes[j]);
		}
	}
	return (0);
}

/*
 * Wait for a child process and return its exit status.
 * Returns -1 if waitpid fails or the child was killed by a signal.
 */
static int
wait_for_child(pid_t pid)
{
	int status;

	if (waitpid(pid, &status, 0) != pid)
		return (-1);

	if (WIFEXITED(status))
		return (WEXITSTATUS(status));

	return (-1);
}

/*
 * Test 1: Pre-fork with active PTC
 *
 * Parent warms up caches (populating per-thread and magazine caches),
 * then forks. The child verifies it can allocate, use, and free memory
 * without deadlock or corruption.
 */
static void
test_prefork_active_ptc(void)
{
	pid_t pid;

	warm_caches();

	/* Also hold some live allocations across fork */
	void *parent_buf = umem_alloc(MEDIUM_SIZE, UMEM_DEFAULT);
	check(parent_buf != NULL, "pre-fork: parent allocation before fork");
	fill_buf(parent_buf, MEDIUM_SIZE, MAGIC_BYTE_PARENT);

	pid = fork();
	check(pid >= 0, "pre-fork: fork succeeded");

	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child process */
		int rc = 0;

		/*
		 * The parent's buffer was duplicated into our address space.
		 * Verify we can read it (the data should match).
		 */
		if (!verify_buf(parent_buf, MEDIUM_SIZE, MAGIC_BYTE_PARENT))
			rc = 1;

		/*
		 * Now do fresh allocations in the child. This exercises
		 * the post-fork lock release path and any PTC state reset.
		 */
		if (alloc_verify_free(MAGIC_BYTE_CHILD) != 0)
			rc = 1;

		/*
		 * Free the inherited buffer in the child.
		 * This should not corrupt the parent's heap since we are
		 * in a separate address space after fork.
		 */
		umem_free(parent_buf, MEDIUM_SIZE);

		_exit(rc);
	}

	/* Parent continues */
	check(verify_buf(parent_buf, MEDIUM_SIZE, MAGIC_BYTE_PARENT),
	    "pre-fork: parent buffer intact after fork");

	int child_rc = wait_for_child(pid);
	check(child_rc == 0, "pre-fork: child alloc/free succeeded");

	umem_free(parent_buf, MEDIUM_SIZE);
}

/*
 * Test 2: Post-fork independent allocation
 *
 * Fork first, then have both parent and child allocate independently.
 * Verify that allocations in one process do not affect the other.
 */
static void
test_postfork_independent(void)
{
	pid_t pid;

	pid = fork();
	check(pid >= 0, "post-fork: fork succeeded");

	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: allocate, verify, free */
		int rc = alloc_verify_free(MAGIC_BYTE_CHILD);
		_exit(rc != 0 ? 1 : 0);
	}

	/* Parent: allocate, verify, free in parallel with child */
	int parent_ok = (alloc_verify_free(MAGIC_BYTE_PARENT) == 0);
	check(parent_ok, "post-fork: parent independent alloc succeeded");

	int child_rc = wait_for_child(pid);
	check(child_rc == 0, "post-fork: child independent alloc succeeded");
}

/*
 * Test 3: Multiple children with no cross-contamination
 *
 * Parent warms caches, holds live buffers, then forks N_CHILDREN
 * children. Each child:
 *   - verifies inherited buffers
 *   - does its own allocations with a unique pattern
 *   - frees everything and exits
 *
 * Parent waits for all children and verifies its own buffers are intact.
 */
static void
test_multiple_children(void)
{
	pid_t pids[N_CHILDREN];
	void *parent_bufs[N_ALLOCS];
	int i;

	warm_caches();

	/* Allocate and fill buffers in parent */
	for (i = 0; i < N_ALLOCS; i++) {
		parent_bufs[i] = umem_alloc(SMALL_SIZE, UMEM_DEFAULT);
		if (parent_bufs[i] == NULL)
			break;
		fill_buf(parent_bufs[i], SMALL_SIZE, MAGIC_BYTE_PARENT);
	}
	check(i == N_ALLOCS, "multi-child: parent allocated all buffers");

	/* Fork multiple children */
	for (i = 0; i < N_CHILDREN; i++) {
		pids[i] = fork();
		if (pids[i] < 0) {
			fail("multi-child: fork failed");
			/* Reap any already-forked children */
			int j;
			for (j = 0; j < i; j++)
				wait_for_child(pids[j]);
			goto cleanup;
		}

		if (pids[i] == 0) {
			/* Child i */
			int rc = 0;
			unsigned char child_pattern =
			    (unsigned char)(0xC0 + i);
			int k;

			/* Verify inherited parent buffers */
			for (k = 0; k < N_ALLOCS; k++) {
				if (!verify_buf(parent_bufs[k], SMALL_SIZE,
				    MAGIC_BYTE_PARENT)) {
					rc = 1;
					break;
				}
			}

			/* Allocate with child-unique pattern */
			void *child_bufs[N_ALLOCS];
			for (k = 0; k < N_ALLOCS; k++) {
				child_bufs[k] = umem_alloc(MEDIUM_SIZE,
				    UMEM_DEFAULT);
				if (child_bufs[k] == NULL) {
					rc = 1;
					break;
				}
				fill_buf(child_bufs[k], MEDIUM_SIZE,
				    child_pattern);
			}

			/* Verify child buffers */
			for (k = 0; k < N_ALLOCS; k++) {
				if (child_bufs[k] == NULL)
					break;
				if (!verify_buf(child_bufs[k], MEDIUM_SIZE,
				    child_pattern)) {
					rc = 1;
				}
				umem_free(child_bufs[k], MEDIUM_SIZE);
			}

			/* Free inherited buffers */
			for (k = 0; k < N_ALLOCS; k++)
				umem_free(parent_bufs[k], SMALL_SIZE);

			_exit(rc);
		}
	}

	/* Parent: verify our buffers are still intact */
	{
		int all_ok = 1;
		for (i = 0; i < N_ALLOCS; i++) {
			if (!verify_buf(parent_bufs[i], SMALL_SIZE,
			    MAGIC_BYTE_PARENT)) {
				all_ok = 0;
				break;
			}
		}
		check(all_ok,
		    "multi-child: parent buffers intact after all forks");
	}

	/* Wait for all children */
	{
		int all_children_ok = 1;
		for (i = 0; i < N_CHILDREN; i++) {
			int rc = wait_for_child(pids[i]);
			if (rc != 0)
				all_children_ok = 0;
		}
		check(all_children_ok,
		    "multi-child: all children completed without error");
	}

cleanup:
	for (i = 0; i < N_ALLOCS; i++) {
		if (parent_bufs[i] != NULL)
			umem_free(parent_bufs[i], SMALL_SIZE);
	}
}

/*
 * Test 4: Fork after umem_cache_create
 *
 * Create a custom umem_cache, allocate from it, fork, then verify
 * that both parent and child can use the cache independently.
 */
static int
cache_constructor(void *buf, void *ignored, int flags)
{
	(void) ignored;
	(void) flags;
	memset(buf, 0, 64);
	return (0);
}

static void
cache_destructor(void *buf, void *ignored)
{
	(void) buf;
	(void) ignored;
}

static void
test_fork_with_cache(void)
{
	umem_cache_t *cp;
	void *obj;
	pid_t pid;

	cp = umem_cache_create("fork_test_cache", 64, 0,
	    cache_constructor, cache_destructor, NULL, NULL, NULL, 0);
	check(cp != NULL, "cache-fork: cache created");

	if (cp == NULL)
		return;

	/* Warm up the cache */
	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	check(obj != NULL, "cache-fork: parent alloc from cache");
	if (obj != NULL) {
		fill_buf(obj, 64, MAGIC_BYTE_PARENT);
		umem_cache_free(cp, obj);
	}

	/* Allocate an object to hold across fork */
	obj = umem_cache_alloc(cp, UMEM_DEFAULT);
	check(obj != NULL, "cache-fork: parent holds object across fork");
	if (obj != NULL)
		fill_buf(obj, 64, MAGIC_BYTE_PARENT);

	pid = fork();
	check(pid >= 0, "cache-fork: fork succeeded");

	if (pid < 0) {
		if (obj != NULL)
			umem_cache_free(cp, obj);
		umem_cache_destroy(cp);
		return;
	}

	if (pid == 0) {
		/* Child */
		int rc = 0;
		void *child_obj;

		/* Verify inherited object */
		if (obj != NULL && !verify_buf(obj, 64, MAGIC_BYTE_PARENT))
			rc = 1;

		/* Allocate from the cache in the child */
		child_obj = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (child_obj == NULL) {
			rc = 1;
		} else {
			fill_buf(child_obj, 64, MAGIC_BYTE_CHILD);
			if (!verify_buf(child_obj, 64, MAGIC_BYTE_CHILD))
				rc = 1;
			umem_cache_free(cp, child_obj);
		}

		if (obj != NULL)
			umem_cache_free(cp, obj);

		/*
		 * Do not call umem_cache_destroy in child -- the cache
		 * is shared state from the fork and destroying it here
		 * would be invalid (the parent still references it).
		 * Just exit; process teardown will clean up.
		 */
		_exit(rc);
	}

	/* Parent */
	check(obj != NULL && verify_buf(obj, 64, MAGIC_BYTE_PARENT),
	    "cache-fork: parent object intact after fork");

	int child_rc = wait_for_child(pid);
	check(child_rc == 0,
	    "cache-fork: child cache alloc/free succeeded");

	if (obj != NULL)
		umem_cache_free(cp, obj);
	umem_cache_destroy(cp);
}

/*
 * Test 5: Rapid fork-and-allocate stress
 *
 * Rapidly fork children that each do a small number of allocations.
 * This stresses the atfork lock/unlock path under quick succession.
 */
static void
test_rapid_fork_stress(void)
{
	int i;
	pid_t pids[N_CHILDREN * 2];
	int nchildren = N_CHILDREN * 2;
	int all_ok = 1;

	warm_caches();

	for (i = 0; i < nchildren; i++) {
		pids[i] = fork();
		if (pids[i] < 0) {
			fail("rapid-fork: fork failed");
			nchildren = i;
			break;
		}

		if (pids[i] == 0) {
			/* Child: quick alloc/free cycle */
			void *p = umem_alloc(SMALL_SIZE, UMEM_DEFAULT);
			if (p == NULL)
				_exit(1);
			fill_buf(p, SMALL_SIZE, (unsigned char)i);
			if (!verify_buf(p, SMALL_SIZE, (unsigned char)i))
				_exit(1);
			umem_free(p, SMALL_SIZE);

			/* Second allocation test */
			p = umem_alloc(MEDIUM_SIZE, UMEM_DEFAULT);
			if (p == NULL)
				_exit(1);
			fill_buf(p, MEDIUM_SIZE, (unsigned char)(i + 0x80));
			if (!verify_buf(p, MEDIUM_SIZE,
			    (unsigned char)(i + 0x80)))
				_exit(1);
			umem_free(p, MEDIUM_SIZE);

			_exit(0);
		}
	}

	/* Parent: verify no allocator deadlock by doing our own allocs */
	{
		void *p = umem_alloc(LARGE_SIZE, UMEM_DEFAULT);
		check(p != NULL,
		    "rapid-fork: parent can still allocate during reaps");
		if (p != NULL)
			umem_free(p, LARGE_SIZE);
	}

	for (i = 0; i < nchildren; i++) {
		int rc = wait_for_child(pids[i]);
		if (rc != 0)
			all_ok = 0;
	}
	check(all_ok, "rapid-fork: all children completed successfully");
}

int
main(void)
{
	printf("1..18\n");

	test_prefork_active_ptc();
	test_postfork_independent();
	test_multiple_children();
	test_fork_with_cache();
	test_rapid_fork_stress();

	if (tests_failed > 0) {
		printf("# %d test(s) FAILED\n", tests_failed);
		return (EXIT_FAILURE);
	}

	printf("# All tests passed\n");
	return (EXIT_SUCCESS);
}

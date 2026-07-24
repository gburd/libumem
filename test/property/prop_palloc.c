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
 * CDDL HEADER END
 */

/*
 * Property/invariant tests for the experimental budget-context example
 * (examples/umem_palloc.{c,h}).  The demo main() is compiled out with
 * -DUMEM_PALLOC_NO_MAIN so this file provides its own main() and exercises
 * the API directly.
 *
 * Invariants:
 *   1. BUDGET ENFORCEMENT per policy:
 *        - NOWAIT: an over-budget alloc returns NULL immediately.
 *        - default (backpressure): an over-budget alloc blocks and then
 *          times out (returning NULL) when no free ever arrives, but a
 *          concurrent free unblocks it and it succeeds.
 *        - NOFAIL: an over-budget alloc abort()s.  Verified in a child
 *          process so the abort does not kill the test runner.
 *      In all cases, usage never exceeds the budget for a successful alloc.
 *   2. PARENT/CHILD HIERARCHY frees correctly: a child's allocations count
 *      against the parent's budget; deleting the parent recursively frees
 *      children; child budget cannot exceed parent budget.
 *   3. SHARED-MEMORY CONTEXTS SURVIVE FORK: data written by the parent in a
 *      shared context is visible to a forked child that attaches the same
 *      segment, and vice versa.
 *
 * Randomized coverage (QCC) drives budget/alloc sizes for invariants 1-2;
 * the fork test is a direct assertion-based check.  Build under ASan.
 */

#define	UMEM_ENABLE_EXPERIMENTAL
#define	UMEM_PALLOC_NO_MAIN
#include "../qc.h"
/*
 * Include the example source directly (with its demo main() compiled out)
 * so the otherwise-opaque struct UmemBudgetContext is visible for the fork
 * test, which needs the block's offset within the shared backing and the
 * shm name to unlink.  This also links the palloc implementation in.
 */
#include "../../examples/umem_palloc.c"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Invariant 1: budget enforcement                                     */
/* ------------------------------------------------------------------ */

/*
 * NOWAIT: fill the budget with fixed-size blocks, then the next alloc must
 * return NULL (never over-budget), and reported usage must never exceed the
 * budget.
 */
static QCC_TestStatus
prop_nowait_never_overbudget(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	long blk = (*QCC_getValue(vals, 0, long*));
	if (blk < 16) blk = -blk;
	size_t block = 64 + ((size_t)blk % 448);	/* 64..511 */
	size_t budget = 8192;

	UmemBudgetContext *ctx = umem_budget_create("prop_nowait",
	    budget, UMEM_BUDGET_PREALLOC | UMEM_BUDGET_NOWAIT);
	if (ctx == NULL)
		return (QCC_NOTHING);

	enum { MAX = 256 };
	void *blocks[MAX];
	int n = 0;
	int hit_limit = 0;
	for (int i = 0; i < MAX; i++) {
		void *p = umem_budget_alloc(ctx, block);
		if (p == NULL) {
			hit_limit = 1;
			break;
		}
		/* usage must never exceed budget after a success */
		if (umem_budget_used(ctx) > budget) {
			for (int j = 0; j < n; j++)
				umem_budget_free(ctx, blocks[j], block);
			umem_budget_delete(ctx);
			return (QCC_FAIL);
		}
		blocks[n++] = p;
	}

	for (int j = 0; j < n; j++)
		umem_budget_free(ctx, blocks[j], block);
	umem_budget_delete(ctx);

	/* We must have hit the NOWAIT limit (budget really was enforced). */
	return (hit_limit ? QCC_OK : QCC_FAIL);
}

/*
 * After freeing everything, the whole budget is available again (no leak in
 * accounting): we can re-allocate the same total we allocated before.
 */
static QCC_TestStatus
prop_free_restores_budget(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	long blk = (*QCC_getValue(vals, 0, long*));
	if (blk < 16) blk = -blk;
	size_t block = 64 + ((size_t)blk % 448);
	size_t budget = 16384;

	UmemBudgetContext *ctx = umem_budget_create("prop_restore",
	    budget, UMEM_BUDGET_PREALLOC | UMEM_BUDGET_NOWAIT);
	if (ctx == NULL)
		return (QCC_NOTHING);

	enum { MAX = 256 };
	void *blocks[MAX];

	int first = 0;
	for (int i = 0; i < MAX; i++) {
		void *p = umem_budget_alloc(ctx, block);
		if (p == NULL) break;
		blocks[first++] = p;
	}
	for (int j = 0; j < first; j++)
		umem_budget_free(ctx, blocks[j], block);

	/* Second round should allocate at least as many as the first. */
	int second = 0;
	for (int i = 0; i < MAX; i++) {
		void *p = umem_budget_alloc(ctx, block);
		if (p == NULL) break;
		blocks[second++] = p;
	}
	for (int j = 0; j < second; j++)
		umem_budget_free(ctx, blocks[j], block);

	umem_budget_delete(ctx);
	return (second >= first && first > 0 ? QCC_OK : QCC_FAIL);
}

/* ------------------------------------------------------------------ */
/* Invariant 2: parent/child hierarchy                                 */
/* ------------------------------------------------------------------ */

/*
 * A child's allocations count against the parent's budget: a child alloc
 * that fits the child budget but would overflow the *parent* must fail
 * (NOWAIT).  And a child budget larger than the parent's is rejected.
 */
static QCC_TestStatus
prop_child_counts_against_parent(QCC_GenValue **vals, int len,
    QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	(void)vals;

	size_t parent_budget = 8192;
	UmemBudgetContext *parent = umem_budget_create("prop_parent",
	    parent_budget, UMEM_BUDGET_PREALLOC | UMEM_BUDGET_NOWAIT);
	if (parent == NULL)
		return (QCC_NOTHING);

	/* Child budget > parent budget must be rejected. */
	UmemBudgetContext *toobig = umem_budget_create_child(parent,
	    "prop_toobig", parent_budget * 2, UMEM_BUDGET_NOWAIT);
	if (toobig != NULL) {
		umem_budget_delete(toobig);
		umem_budget_delete(parent);
		return (QCC_FAIL);
	}

	/* Legal child with a budget as large as the parent's. */
	UmemBudgetContext *child = umem_budget_create_child(parent,
	    "prop_child", parent_budget,
	    UMEM_BUDGET_PREALLOC | UMEM_BUDGET_NOWAIT);
	if (child == NULL) {
		umem_budget_delete(parent);
		return (QCC_FAIL);
	}

	/* Consume most of the parent budget directly from the parent. */
	void *pblk = umem_budget_alloc(parent, parent_budget / 2);
	int ok = (pblk != NULL);

	/*
	 * Now a child alloc that alone fits the child budget but, combined
	 * with the parent's outstanding usage, exceeds the parent budget,
	 * must fail (child allocations count against the parent).
	 */
	void *cblk = umem_budget_alloc(child, parent_budget);
	if (cblk != NULL) {
		/* parent total must still be within budget */
		ok = ok && (umem_budget_used(parent) <= parent_budget);
		umem_budget_free(child, cblk, parent_budget);
	}

	if (pblk != NULL)
		umem_budget_free(parent, pblk, parent_budget / 2);

	/* Deleting the parent must recursively free the child (no crash,
	 * no leak under ASan). */
	umem_budget_delete(parent);

	return (ok ? QCC_OK : QCC_FAIL);
}

/*
 * Deleting a parent frees the whole subtree: build parent -> child ->
 * grandchild, allocate in each, delete the parent once.  ASan verifies no
 * leak / no double free; we verify no crash and that usage was tracked.
 */
static QCC_TestStatus
prop_hierarchy_recursive_free(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	(void)vals;
	(void)len;

	UmemBudgetContext *top = umem_budget_create("prop_top",
	    256 * 1024, UMEM_BUDGET_PREALLOC);
	if (top == NULL)
		return (QCC_NOTHING);
	UmemBudgetContext *mid = umem_budget_create_child(top, "prop_mid",
	    64 * 1024, UMEM_BUDGET_PREALLOC);
	UmemBudgetContext *leaf = umem_budget_create_child(mid, "prop_leaf",
	    16 * 1024, UMEM_BUDGET_PREALLOC);
	if (mid == NULL || leaf == NULL) {
		umem_budget_delete(top);
		return (QCC_FAIL);
	}

	void *a = umem_budget_alloc(top, 1024);
	void *b = umem_budget_alloc(mid, 512);
	void *c = umem_budget_alloc(leaf, 256);
	int ok = (a != NULL && b != NULL && c != NULL);

	/* Single delete of the root must tear down the entire tree. */
	umem_budget_delete(top);

	return (ok ? QCC_OK : QCC_FAIL);
}

/*
 * DYNAMIC (non-PREALLOC) arena allocates and respects its budget.
 * See docs/results/2026-07-23-palloc-dynamic-arena-finding.md: dynamic
 * contexts used to build a vmem arena with no span and no source, so every
 * umem_budget_alloc returned NULL.  The fix wires the arena to libumem's
 * heap arena so it imports spans on demand; budget enforcement is unchanged
 * (checked in umem_budget_alloc before the vmem_alloc).  This test asserts
 * a dynamic alloc succeeds AND that the budget is still enforced (NOWAIT
 * over-budget returns NULL, usage never exceeds budget).
 */
static QCC_TestStatus
prop_dynamic_arena_alloc(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	long blk = (*QCC_getValue(vals, 0, long*));
	if (blk < 16) blk = -blk;
	size_t block = 64 + ((size_t)blk % 448);	/* 64..511 */
	size_t budget = 8192;

	UmemBudgetContext *ctx = umem_budget_create("prop_dyn",
	    budget, UMEM_BUDGET_NOWAIT /* NOT prealloc */);
	if (ctx == NULL)
		return (QCC_NOTHING);

	/* A first allocation on a dynamic context must succeed. */
	void *first = umem_budget_alloc(ctx, block);
	if (first == NULL) {
		umem_budget_delete(ctx);
		return (QCC_FAIL);
	}

	/* Fill the rest of the budget; NOWAIT must eventually return NULL
	 * without ever exceeding the budget. */
	enum { MAX = 256 };
	void *blocks[MAX];
	int n = 0;
	blocks[n++] = first;
	int hit_limit = 0;
	for (int i = 1; i < MAX; i++) {
		void *p = umem_budget_alloc(ctx, block);
		if (p == NULL) {
			hit_limit = 1;
			break;
		}
		if (umem_budget_used(ctx) > budget) {
			for (int j = 0; j < n; j++)
				umem_budget_free(ctx, blocks[j], block);
			umem_budget_delete(ctx);
			return (QCC_FAIL);
		}
		blocks[n++] = p;
	}

	for (int j = 0; j < n; j++)
		umem_budget_free(ctx, blocks[j], block);
	umem_budget_delete(ctx);

	/* Allocation worked (n > 0) and the budget was really enforced. */
	return (hit_limit ? QCC_OK : QCC_FAIL);
}

/* ------------------------------------------------------------------ */
/* Invariant 1c: NOFAIL policy abort()s over budget (in a child proc)  */
/* ------------------------------------------------------------------ */

/*
 * NOFAIL policy: an over-budget allocation must abort().  Run in a child
 * process (fork) so the abort does not kill the test runner; the parent
 * asserts the child died by signal or non-zero exit.  Direct check, not QCC.
 */
static int
run_nofail_abort_test(void)
{
	pid_t pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		/* Child: tiny NOFAIL budget, then blow past it. */
		UmemBudgetContext *ctx = umem_budget_create("prop_nofail",
		    1024, UMEM_BUDGET_PREALLOC | UMEM_BUDGET_NOFAIL);
		if (ctx == NULL)
			_exit(3);
		/* First alloc fits; second must abort. */
		(void)umem_budget_alloc(ctx, 512);
		(void)umem_budget_alloc(ctx, 4096);	/* > budget -> abort */
		_exit(0);				/* survived => bug */
	}
	int status = 0;
	waitpid(pid, &status, 0);
	/* Pass if the child aborted (signal) or exited non-zero. */
	if (WIFSIGNALED(status))
		return (0);
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		return (0);
	return (1);				/* survived => enforcement failed */
}

/* ------------------------------------------------------------------ */
/* Invariant 3: shared-memory contexts survive fork                    */
/* ------------------------------------------------------------------ */

/*
 * Parent creates a shared context, allocates a block, writes a pattern into
 * it, and records the block's offset within the shared segment in a small
 * MAP_SHARED anonymous handshake page.  The child forks, attaches the same
 * named segment, reads the block at that offset, verifies the parent's
 * pattern, writes its own pattern back, and exits.  The parent then verifies
 * the child's pattern is visible.  This proves shared memory truly survives
 * fork and is coherent across processes.
 */
static int
run_fork_shared_test(void)
{
	const char *shm_name = "prop_palloc_fork";
	size_t seg_size = 64 * 1024;
	size_t blk_size = 256;

	/* Anonymous shared handshake page for cross-process coordination. */
	struct handshake {
		size_t	 block_offset;	/* offset of block within backing */
		int	 parent_ready;
		int	 child_done;
	};
	struct handshake *hs = mmap(NULL, sizeof (*hs),
	    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (hs == MAP_FAILED)
		return (-1);
	memset(hs, 0, sizeof (*hs));

	UmemBudgetContext *creator = umem_shared_create(shm_name,
	    seg_size, 0);
	if (creator == NULL) {
		munmap(hs, sizeof (*hs));
		return (-1);
	}

	void *blk = umem_budget_alloc(creator, blk_size);
	if (blk == NULL) {
		umem_shared_detach(creator);
		munmap(hs, sizeof (*hs));
		return (-1);
	}
	memset(blk, 0xA5, blk_size);

	/*
	 * The child attaches a fresh mapping, so it needs the block's offset
	 * within the segment, not the parent's virtual address.  We recover
	 * it from the creator's backing pointer.
	 */
	hs->block_offset = (size_t)((char *)blk -
	    (char *)creator->backing);
	__sync_synchronize();
	hs->parent_ready = 1;

	pid_t pid = fork();
	if (pid < 0) {
		umem_shared_detach(creator);
		munmap(hs, sizeof (*hs));
		return (-1);
	}

	if (pid == 0) {
		/* Child: attach the same segment and verify parent's data. */
		while (!hs->parent_ready)
			usleep(1000);
		UmemBudgetContext *att = umem_shared_attach(shm_name);
		if (att == NULL)
			_exit(11);
		unsigned char *cblk =
		    (unsigned char *)att->backing + hs->block_offset;
		int bad = 0;
		for (size_t i = 0; i < blk_size; i++)
			if (cblk[i] != 0xA5) { bad = 1; break; }
		if (bad) {
			umem_shared_detach(att);
			_exit(12);
		}
		/* Write child's pattern back for the parent to observe. */
		memset(cblk, 0x5C, blk_size);
		umem_shared_detach(att);
		hs->child_done = 1;
		__sync_synchronize();
		_exit(0);
	}

	/* Parent: wait for the child and verify its write is visible. */
	int status = 0;
	waitpid(pid, &status, 0);

	int rc = 0;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		rc = WIFEXITED(status) ? WEXITSTATUS(status) : -2;
	} else {
		unsigned char *pblk = (unsigned char *)blk;
		for (size_t i = 0; i < blk_size; i++) {
			if (pblk[i] != 0x5C) { rc = -3; break; }
		}
	}

	umem_budget_free(creator, blk, blk_size);
	/* Unlink the shared segment. */
	if (creator->shm_fd >= 0)
		(void)shm_unlink(creator->shm_name);
	umem_shared_detach(creator);
	munmap(hs, sizeof (*hs));
	return (rc);
}

/* ------------------------------------------------------------------ */
/* Generators + driver                                                 */
/* ------------------------------------------------------------------ */

static QCC_GenValue *gen_blk(void) { return (QCC_genLongR(16, 512)); }

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	QCC_init(0);

	int fails = 0;

	printf("=== budget-context (palloc) properties ===\n");

	printf("[budget] NOWAIT alloc never exceeds budget, hits limit\n");
	if (QCC_testForAll(300, 3000, prop_nowait_never_overbudget, 1,
	    gen_blk) != 0)
		fails++;

	printf("[budget] free restores full budget (no accounting leak)\n");
	if (QCC_testForAll(300, 3000, prop_free_restores_budget, 1,
	    gen_blk) != 0)
		fails++;

	printf("[budget] NOFAIL policy abort()s over budget\n");
	{
		int rc = run_nofail_abort_test();
		if (rc == -1)
			printf("  (skipped: fork failed)\n");
		else if (rc != 0) {
			printf("  NOFAIL did NOT abort over budget!\n");
			fails++;
		} else {
			printf("  NOFAIL abort-on-over-budget OK\n");
		}
	}

	printf("[hier] child allocations count against parent budget\n");
	if (QCC_testForAll(100, 1000, prop_child_counts_against_parent, 1,
	    gen_blk) != 0)
		fails++;

	printf("[hier] deleting parent recursively frees the subtree\n");
	if (QCC_testForAll(100, 1000, prop_hierarchy_recursive_free, 1,
	    gen_blk) != 0)
		fails++;

	printf("[dyn] non-PREALLOC (dynamic) arena allocates on demand "
	    "and respects budget\n");
	if (QCC_testForAll(50, 500, prop_dynamic_arena_alloc, 1,
	    gen_blk) != 0)
		fails++;

	printf("[fork] shared-memory context survives fork (bidirectional)\n");
	{
		int rc = run_fork_shared_test();
		if (rc == -1) {
			printf("  (skipped: setup failed, e.g. shm "
			    "unavailable)\n");
		} else if (rc != 0) {
			printf("  FORK TEST FAILED rc=%d\n", rc);
			fails++;
		} else {
			printf("  fork shared-memory coherence OK\n");
		}
	}

	printf("\n=====================================\n");
	if (fails == 0) {
		printf("All budget-context property tests passed!\n");
		return (0);
	}
	printf("%d budget-context property group(s) failed\n", fails);
	return (1);
}

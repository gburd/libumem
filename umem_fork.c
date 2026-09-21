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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include "config.h"
#include "umem_base.h"
#include "vmem_base.h"

#ifndef _WIN32
#include <unistd.h>

/*
 * The following functions are for pre- and post-fork1(2) handling.
 *
 * THE ONE TRUE LOCK ORDER.  Every lock below is acquired in this order, by
 * the fork handlers and by ordinary allocation paths alike:
 *
 *   1. umem_init_lock
 *   2. vmem locks (vmem_lockup(): vmem_list_lock, vmem_nosleep_lock, each
 *      arena's vm_lock, vmem_segfree_lock; then vmem_sbrk_lockup():
 *      sbrk_lock, then sbrk_faillock)
 *   3. umem_cache_lock
 *   4. umem_update_lock
 *   5. umem_flags_lock
 *   6. per umem_cache_t, in this order:
 *        a. cache_cpu[*].cc_lock          (ascending CPU index)
 *        b. depot maglist locks: cache_full.ml_lock, cache_empty.ml_lock,
 *           then cache_depot_full[i].ml_lock / cache_depot_empty[i].ml_lock
 *           (ascending stripe index)
 *        c. cache_lock                    (slab layer)
 *   7. log headers: lh_cpu[*].clh_lock (ascending), then lh_lock
 *
 * 6a before 6b before 6c is dictated by the allocation paths, so it is not
 * negotiable here:
 *
 *   - _umem_cache_alloc() takes ccp->cc_lock and then, still holding it,
 *     calls umem_depot_alloc()/umem_depot_free(), which BLOCK on ml_lock:
 *     the local-stripe pop and the global fallback both use the blocking
 *     umem_depot_pop(), and umem_depot_push() is unconditionally blocking.
 *     _umem_cache_free() and both *_batch() variants do the same.  So
 *     cc_lock is always ABOVE ml_lock.  (The trylock-based cross-CPU steal
 *     does not change this; only the remote-stripe scan is non-blocking.)
 *   - umem_depot_alloc() -> umem_depot_destroy_stale() -> umem_slab_free()
 *     takes cache_lock while cc_lock and no ml_lock are held, so cache_lock
 *     is below both.  Nothing in the allocator takes cc_lock or ml_lock
 *     while holding cache_lock -- that is the documented contract on
 *     umem_depot_alloc()/umem_depot_free().
 *
 * This is also the Solaris/illumos lineage: the original umem_lockup_cache()
 * took the per-CPU cc_locks first, then the depot lock, then cache_lock.  It
 * agrees with the hierarchy documented under "Lock Ordering" in umem.c.
 *
 * A previous version of this file acquired cache_lock -> ml_locks ->
 * cc_locks, with a comment claiming that matched normal operation.  It did
 * not: it was the exact reverse of 6a/6b, so a forking thread holding
 * ml_lock and waiting for cc_lock deadlocked against an allocating thread
 * holding cc_lock and waiting for ml_lock (ABBA).  Reproduced by
 * test/integration/test_fork_mt_load.c.
 */

static void
umem_lockup_cache(umem_cache_t *cp)
{
	int idx;
	int ncpus = cp->cache_cpu_mask + 1;

	/* See THE ONE TRUE LOCK ORDER above: 6a, then 6b, then 6c. */
	for (idx = 0; idx < ncpus; idx++)
		(void) mutex_lock(&cp->cache_cpu[idx].cc_lock);

	(void) mutex_lock(&cp->cache_full.ml_lock);
	(void) mutex_lock(&cp->cache_empty.ml_lock);
	for (idx = 0; idx < cp->cache_depot_ncpus; idx++) {
		(void) mutex_lock(&cp->cache_depot_full[idx].ml_lock);
		(void) mutex_lock(&cp->cache_depot_empty[idx].ml_lock);
	}

	(void) mutex_lock(&cp->cache_lock);
}

static void
umem_release_cache(umem_cache_t *cp)
{
	int idx;
	int ncpus = cp->cache_cpu_mask + 1;

	/* Release in reverse of acquisition order */
	(void) mutex_unlock(&cp->cache_lock);

	for (idx = cp->cache_depot_ncpus - 1; idx >= 0; idx--) {
		(void) mutex_unlock(&cp->cache_depot_empty[idx].ml_lock);
		(void) mutex_unlock(&cp->cache_depot_full[idx].ml_lock);
	}
	(void) mutex_unlock(&cp->cache_empty.ml_lock);
	(void) mutex_unlock(&cp->cache_full.ml_lock);

	for (idx = ncpus - 1; idx >= 0; idx--)
		(void) mutex_unlock(&cp->cache_cpu[idx].cc_lock);
}

static void
umem_lockup_log_header(umem_log_header_t *lhp)
{
	int idx;
	if (lhp == NULL)
		return;
	for (idx = 0; idx < umem_max_ncpus; idx++)
		(void) mutex_lock(&lhp->lh_cpu[idx].clh_lock);

	(void) mutex_lock(&lhp->lh_lock);
}

static void
umem_release_log_header(umem_log_header_t *lhp)
{
	int idx;
	if (lhp == NULL)
		return;

	(void) mutex_unlock(&lhp->lh_lock);

	for (idx = 0; idx < umem_max_ncpus; idx++)
		(void) mutex_unlock(&lhp->lh_cpu[idx].clh_lock);
}

static void
umem_lockup(void)
{
	umem_cache_t *cp;

	(void) mutex_lock(&umem_init_lock);
	/*
	 * If another thread is busy initializing the library, we must
	 * wait for it to complete (by calling umem_init()) before allowing
	 * the fork() to proceed.
	 */
	if (umem_ready == UMEM_READY_INITING && umem_init_thr != thr_self()) {
		(void) mutex_unlock(&umem_init_lock);
		(void) umem_init();
		(void) mutex_lock(&umem_init_lock);
	}

	vmem_lockup();
	vmem_sbrk_lockup();

	(void) mutex_lock(&umem_cache_lock);
	(void) mutex_lock(&umem_update_lock);
	(void) mutex_lock(&umem_flags_lock);

	umem_lockup_cache(&umem_null_cache);
	for (cp = umem_null_cache.cache_prev; cp != &umem_null_cache;
	    cp = cp->cache_prev)
		umem_lockup_cache(cp);

	umem_lockup_log_header(umem_transaction_log);
	umem_lockup_log_header(umem_content_log);
	umem_lockup_log_header(umem_failure_log);
	umem_lockup_log_header(umem_slab_log);

	(void) cond_broadcast(&umem_update_cv);

}

static void
umem_do_release(int as_child)
{
	umem_cache_t *cp;
	int cleanup_update = 0;

	/*
	 * Clean up the update state if we are the child process and
	 * another thread was processing updates.
	 */
	if (as_child) {
		if (umem_update_thr != thr_self()) {
			umem_update_thr = 0;
			cleanup_update = 1;
		}
		if (umem_st_update_thr != thr_self()) {
			umem_st_update_thr = 0;
			cleanup_update = 1;
		}
	}

	if (cleanup_update) {
		umem_reaping = UMEM_REAP_DONE;

		for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
		    cp = cp->cache_next) {
			if (cp->cache_uflags & UMU_NOTIFY)
				cp->cache_uflags &= ~UMU_NOTIFY;

			/*
			 * If the cache is active, we just re-add it to
			 * the update list.  This will re-do any active
			 * updates on the cache, but that won't break
			 * anything.
			 *
			 * The worst that can happen is a cache has
			 * its magazines rescaled twice, instead of once.
			 */
			if (cp->cache_uflags & UMU_ACTIVE) {
				umem_cache_t *cnext, *cprev;

				ASSERT(cp->cache_unext == NULL &&
				    cp->cache_uprev == NULL);

				cp->cache_uflags &= ~UMU_ACTIVE;
				cp->cache_unext = cnext = &umem_null_cache;
				cp->cache_uprev = cprev =
				    umem_null_cache.cache_uprev;
				cnext->cache_uprev = cp;
				cprev->cache_unext = cp;
			}
		}
	}

	umem_release_log_header(umem_slab_log);
	umem_release_log_header(umem_failure_log);
	umem_release_log_header(umem_content_log);
	umem_release_log_header(umem_transaction_log);

	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next)
		umem_release_cache(cp);
	umem_release_cache(&umem_null_cache);

	(void) mutex_unlock(&umem_flags_lock);
	(void) mutex_unlock(&umem_update_lock);
	(void) mutex_unlock(&umem_cache_lock);

	vmem_sbrk_release();
	vmem_release();

	(void) mutex_unlock(&umem_init_lock);
}

static void
umem_release(void)
{
	umem_do_release(0);
}

static void
umem_release_child(void)
{
	umem_do_release(1);
}
#endif

void
umem_forkhandler_init(void)
{
#ifndef _WIN32
	/*
	 * There is no way to unregister these atfork functions,
	 * but we don't need to.  The dynamic linker and libc take
	 * care of unregistering them if/when the library is unloaded.
	 */
	(void) pthread_atfork(umem_lockup, umem_release, umem_release_child);
#endif
}

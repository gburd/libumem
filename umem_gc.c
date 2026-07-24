/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * umem_gc.c - Shenandoah-style garbage collector core engine
 *
 * Provides automatic memory management on top of libumem's slab allocator.
 * Each GC allocation prepends a gc_header for tracking. All GC objects are
 * linked in a global intrusive list protected by a mutex. Collection is
 * triggered when allocated bytes exceed a threshold (2x survived bytes).
 *
 * GC phases: IDLE -> MARK -> REMARK -> SWEEP -> FINALIZE -> IDLE
 *
 * The mark and root-scanning logic is provided by the root scanner (Task #2).
 * This file provides the allocation/free paths, object tracking, phase state
 * machine, size-classed GC caches, thread registration, and statistics.
 */

#include "config.h"

#include <umem.h>
#include <umem_impl.h>
#include "umem_base.h"
#include "umem_gc.h"
#include "umem_gc_roots.h"
#include "umem_sparsemap.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <time.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* ----------------------------------------------------------------
 * GC size classes
 *
 * We create dedicated umem caches for GC allocations. Each cache
 * handles objects of a specific size class (including the gc_header).
 * Allocations larger than the biggest size class fall through to
 * umem_alloc() directly.
 * ---------------------------------------------------------------- */

#define	GC_NUM_SIZE_CLASSES	8

/*
 * Size classes for GC objects (user size + gc_header overhead).
 * These are the total allocation sizes passed to the backing umem cache.
 */
static const size_t gc_size_classes[GC_NUM_SIZE_CLASSES] = {
	64,	/* user: up to ~24 bytes (64 - sizeof(gc_header)) */
	128,	/* user: up to ~88 bytes */
	256,	/* user: up to ~216 bytes */
	512,	/* user: up to ~472 bytes */
	1024,	/* user: up to ~984 bytes */
	2048,	/* user: up to ~2008 bytes */
	4096,	/* user: up to ~4056 bytes */
	8192	/* user: up to ~8152 bytes */
};

#define	GC_MAX_CACHED_SIZE	8192	/* total size including header */

static umem_cache_t *gc_caches[GC_NUM_SIZE_CLASSES];

/* ----------------------------------------------------------------
 * Global GC state
 * ---------------------------------------------------------------- */

/* Initialization state */
static _Atomic int gc_initialized;
static pthread_mutex_t gc_init_lock = PTHREAD_MUTEX_INITIALIZER;

/* GC phase */
static _Atomic umem_gc_phase_t gc_phase = GC_PHASE_IDLE;

/* Mark value: incremented each GC cycle */
static _Atomic uint32_t gc_mark_value = 1;

/* Global object list (doubly-linked, protected by gc_objects_lock) */
static pthread_mutex_t gc_objects_lock = PTHREAD_MUTEX_INITIALIZER;
static umem_gc_header_t gc_objects_sentinel;

/* Thread list (protected by gc_threads_lock) */
static pthread_mutex_t gc_threads_lock = PTHREAD_MUTEX_INITIALIZER;
static umem_gc_thread_t *gc_thread_list;
static pthread_key_t gc_thread_key;

/* GC threshold and statistics (atomics for lock-free reads) */
static _Atomic size_t gc_bytes_allocated;
static _Atomic size_t gc_bytes_survived;
static _Atomic size_t gc_threshold;
static _Atomic size_t gc_heap_size;
static _Atomic size_t gc_total_allocated;
static _Atomic uint64_t gc_collections;
static _Atomic uint64_t gc_objects_freed;
static _Atomic uint64_t gc_finalizers_run;

/* Minimum threshold to avoid thrashing on small heaps */
#define	GC_MIN_THRESHOLD	(256 * 1024)	/* 256 KB */

/* Default threshold multiplier: collect when allocated > 2x survived */
#define	GC_THRESHOLD_MULTIPLIER	2

/* Collection lock (only one collection at a time) */
static pthread_mutex_t gc_collect_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Phase rwlock: prevents mark phase from starting while a realloc
 * is pinning an object. Reallocs take a read lock (concurrent with
 * each other), mark phase takes a write lock (exclusive).
 */
static pthread_rwlock_t gc_phase_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* Page-level sparsemap for O(1) GC pointer lookup */
static umem_sparsemap_t *gc_pagemap;

/* Finalizer recursion guard: prevents GC_MALLOC during finalization */
static __thread int gc_in_finalizer;

/* Maximum finalizer iterations before timeout */
#define	GC_FINALIZER_MAX_ITER	10000

/* ----------------------------------------------------------------
 * Stop-the-world (STW) mechanism
 *
 * Uses SIGUSR2 to suspend all registered GC threads during root
 * scanning. Signal handler saves registers and blocks on a per-thread
 * semaphore until the collector releases it.
 * ---------------------------------------------------------------- */

#define	GC_SUSPEND_SIGNAL	SIGUSR2
#define	GC_STW_TIMEOUT_SEC	5

static pthread_t gc_stw_coordinator;
static _Atomic int gc_stw_suspended_count;
static _Atomic int gc_stw_target_count;
static int gc_stw_installed;	/* signal handler installed */

/*
 * Get the thread registry from umem_gc_roots.c.
 * We need direct access for STW signal delivery.
 */
extern umem_gc_thread_info_t umem_gc_threads[];
extern int umem_gc_nthreads;
extern mutex_t umem_gc_threads_lock;

/*
 * Authoritative "a stop-the-world is in progress" flag, owned by the
 * collector.  It is set true in gc_stop_the_world() and cleared in
 * gc_resume_the_world(), both while the collector holds
 * umem_gc_threads_lock.  A suspend-signal handler parks (and spins) ONLY
 * while this flag is set; a signal that is delivered late -- after the
 * collector already resumed -- observes the flag clear and returns at
 * once instead of parking forever.  That is what prevents a worker
 * whose SIGUSR2 arrives after the final collection from spinning
 * indefinitely.  volatile sig_atomic_t is async-signal-safe to read.
 */
static volatile sig_atomic_t gc_stw_active;

static void
gc_suspend_handler(int signo)
{
	int i;
	pthread_t self = pthread_self();
	volatile int stack_anchor;

	(void)signo;

	/*
	 * If no stop-the-world is in progress this is a stale/late signal
	 * from a collection that has already resumed; do nothing.
	 */
	if (!gc_stw_active)
		return;

	/* Find our thread info */
	for (i = 0; i < UMEM_GC_MAX_THREADS; i++) {
		if (umem_gc_threads[i].gcti_registered &&
		    pthread_equal(umem_gc_threads[i].gcti_thread, self)) {
			/*
			 * Spill all callee-saved registers to a scannable
			 * buffer and record the live stack pointer BEFORE
			 * acknowledging that we have parked.  A callee-saved
			 * register may hold the only pointer to a live object;
			 * the collector scans gcti_regs and [gcti_sp, base) so
			 * those roots are never lost.  sigsetjmp is
			 * async-signal-safe.
			 */
			(void) sigsetjmp(umem_gc_threads[i].gcti_regs, 0);
			umem_gc_threads[i].gcti_sp = (void *)&stack_anchor;

			/*
			 * ACK: publish gcti_suspended=1.  The collector waits
			 * until every thread it signalled has this set before
			 * it begins marking, so a live pointer held only in
			 * this thread's registers/stack is captured.  The
			 * seq_cst fence in fetch_add publishes gcti_sp and
			 * gcti_regs to the collector's acquire loads.
			 */
			umem_gc_threads[i].gcti_suspended = 1;
			atomic_fetch_add(&gc_stw_suspended_count, 1);

			/*
			 * Spin until the collector ends the pause.  We avoid
			 * sem_wait here because it is not async-signal-safe on
			 * all platforms (notably FreeBSD).
			 */
			while (gc_stw_active)
				;

			umem_gc_threads[i].gcti_suspended = 0;
			return;
		}
	}

	/* Not a registered GC thread: ignore */
}

static int
gc_install_stw_signal(void)
{
	struct sigaction sa;

	if (gc_stw_installed)
		return (0);

	(void) memset(&sa, 0, sizeof (sa));
	sa.sa_handler = gc_suspend_handler;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);

	if (sigaction(GC_SUSPEND_SIGNAL, &sa, NULL) != 0)
		return (-1);

	gc_stw_installed = 1;
	return (0);
}

static void gc_resume_the_world(void);

/*
 * Suspend all registered GC threads except the caller.
 * Returns 0 on success, -1 on timeout.
 */
static int
gc_stop_the_world(void)
{
	int i;
	int target = 0;
	struct timespec deadline;
	static int signalled[UMEM_GC_MAX_THREADS];
	int nsignalled = 0;

	gc_stw_coordinator = pthread_self();

	/*
	 * Hold umem_gc_threads_lock only across signal -> wait-for-park,
	 * then release it BEFORE marking.  Holding it is required so the
	 * target set is stable while we choose whom to signal and wait for
	 * acks: no thread can register/unregister mid-handshake, so the ack
	 * target is exact and a signalled thread cannot vanish before
	 * acking.  Once every signalled thread has parked it is frozen in
	 * the suspend handler spinning on gc_stw_active and CANNOT touch the
	 * registry, so the parked set stays sound with the lock dropped.
	 *
	 * Releasing before the (potentially long) mark is essential: a
	 * mutator calling umem_gc_register_thread()/unregister must not
	 * block for the whole pause.  A thread that registers after we
	 * release simply was not part of this snapshot; it is brand-new
	 * (nothing rooted yet) and, if the Phase-3 scan happens to walk it,
	 * it is not suspended and gets a conservative full-stack scan.
	 * Because the lock is dropped here, the Phase-3 root scan re-locks
	 * normally (umem_gc_collect passes threads_locked = 0).
	 */
	(void) mutex_lock(&umem_gc_threads_lock);

	/*
	 * Clear per-thread ack flags, then announce the pause.  Ordering
	 * matters: gc_stw_active must be visible as true before the first
	 * signal is delivered, so a handler cannot observe it false and
	 * skip parking.
	 */
	atomic_store(&gc_stw_suspended_count, 0);
	for (i = 0; i < UMEM_GC_MAX_THREADS; i++)
		umem_gc_threads[i].gcti_suspended = 0;
	gc_stw_active = 1;
	atomic_thread_fence(memory_order_seq_cst);

	/* Signal all other registered threads */
	for (i = 0; i < UMEM_GC_MAX_THREADS; i++) {
		if (!umem_gc_threads[i].gcti_registered)
			continue;
		if (pthread_equal(umem_gc_threads[i].gcti_thread,
		    gc_stw_coordinator))
			continue;
		if (pthread_kill(umem_gc_threads[i].gcti_thread,
		    GC_SUSPEND_SIGNAL) == 0) {
			signalled[nsignalled++] = i;
			target++;
		}
	}

	atomic_store(&gc_stw_target_count, target);

	if (target == 0) {
		/* Nobody to park: drop the lock and mark unlocked. */
		(void) mutex_unlock(&umem_gc_threads_lock);
		return (0);
	}

	/*
	 * Wait until every signalled thread has parked (published
	 * gcti_suspended = 1).  Checking the per-thread flags -- not just a
	 * shared counter -- is immune to a stale signal from a prior cycle
	 * inflating the count: only a thread that actually parked in THIS
	 * pause sets its flag (the flags were cleared above under the lock,
	 * and no thread can register/unregister while we hold it).
	 */
	(void) clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += GC_STW_TIMEOUT_SEC;

	for (;;) {
		int parked = 0;
		struct timespec now;

		for (i = 0; i < nsignalled; i++) {
			if (umem_gc_threads[signalled[i]].gcti_suspended)
				parked++;
		}
		if (parked >= target)
			break;

		(void) clock_gettime(CLOCK_REALTIME, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec &&
		    now.tv_nsec >= deadline.tv_nsec)) {
			/*
			 * Not everyone parked in time.  Release the parked
			 * threads and the lock, and report failure so the
			 * caller does a best-effort (unsound-but-safe) scan
			 * rather than hanging.
			 */
			gc_stw_active = 0;
			atomic_thread_fence(memory_order_seq_cst);
			(void) mutex_unlock(&umem_gc_threads_lock);
			return (-1);
		}
		sched_yield();
	}

	/*
	 * Every signalled thread has parked (frozen in the handler spinning
	 * on gc_stw_active).  Drop the registry lock now -- the parked set
	 * can no longer change -- so mutators can register/unregister
	 * without blocking for the whole mark phase.  gc_stw_active stays
	 * true until gc_resume_the_world() so the parked threads keep
	 * spinning while we mark.
	 */
	(void) mutex_unlock(&umem_gc_threads_lock);
	return (0);
}

/*
 * Resume all suspended GC threads and end the stop-the-world window.
 * Clearing gc_stw_active releases every parked handler.  The registry
 * lock was already dropped in gc_stop_the_world() once the park barrier
 * completed, so we do NOT unlock it here.
 */
static void
gc_resume_the_world(void)
{
	gc_stw_active = 0;
	atomic_thread_fence(memory_order_seq_cst);
}

/* ----------------------------------------------------------------
 * Size class lookup
 * ---------------------------------------------------------------- */

/*
 * Find the GC cache for a given total allocation size (header + user).
 * Returns NULL if the size exceeds the largest cached size class.
 */
static umem_cache_t *
gc_cache_for_size(size_t total_size)
{
	for (int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
		if (total_size <= gc_size_classes[i])
			return (gc_caches[i]);
	}
	return (NULL);
}

/*
 * Find the size class for a given total allocation size.
 * Returns the actual allocation size from the cache, or the raw total_size
 * for oversized allocations.
 */
static size_t
gc_alloc_size(size_t total_size)
{
	for (int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
		if (total_size <= gc_size_classes[i])
			return (gc_size_classes[i]);
	}
	return (total_size);
}

/* ----------------------------------------------------------------
 * Object list management
 * ---------------------------------------------------------------- */

/*
 * Add a GC object to the global list. Caller must NOT hold gc_objects_lock.
 *
 * SIGUSR2 (the stop-the-world suspend signal) is blocked across the whole
 * critical section.  Otherwise a thread could be suspended after the object
 * is linked into gc_objects_list but before umem_sparsemap_add_object()
 * records it (or vice versa).  The collector would then find the object on
 * the global sweep list (unmarked) yet miss it in umem_gc_find_header()'s
 * sparsemap lookup while conservatively scanning the suspended thread's
 * stack/registers -- so a genuinely reachable, just-allocated object would
 * be swept.  Blocking the signal keeps add atomic with respect to STW.
 */
static void
gc_object_add(umem_gc_header_t *hdr)
{
	void *user_ptr = UMEM_GC_USER_PTR(hdr);
	sigset_t block, prev;

	(void) sigemptyset(&block);
	(void) sigaddset(&block, GC_SUSPEND_SIGNAL);
	(void) pthread_sigmask(SIG_BLOCK, &block, &prev);

	(void) pthread_mutex_lock(&gc_objects_lock);
	hdr->gc_next = gc_objects_sentinel.gc_next;
	hdr->gc_prev = &gc_objects_sentinel;
	gc_objects_sentinel.gc_next->gc_prev = hdr;
	gc_objects_sentinel.gc_next = hdr;
	if (gc_pagemap != NULL)
		(void) umem_sparsemap_add_object(gc_pagemap, hdr, user_ptr);
	(void) pthread_mutex_unlock(&gc_objects_lock);

	(void) pthread_sigmask(SIG_SETMASK, &prev, NULL);
}

/*
 * Remove a GC object from the global list. Caller must hold gc_objects_lock.
 */
static void
gc_object_remove_locked(umem_gc_header_t *hdr)
{
	void *user_ptr = UMEM_GC_USER_PTR(hdr);

	hdr->gc_prev->gc_next = hdr->gc_next;
	hdr->gc_next->gc_prev = hdr->gc_prev;
	hdr->gc_next = NULL;
	hdr->gc_prev = NULL;
	if (gc_pagemap != NULL)
		umem_sparsemap_remove_object(gc_pagemap, hdr, user_ptr);
}

/* ----------------------------------------------------------------
 * Thread registration
 * ---------------------------------------------------------------- */

/*
 * pthread_key destructor: auto-unregister thread on exit.
 */
static void
gc_thread_key_destructor(void *arg)
{
	umem_gc_thread_t *gct = arg;

	if (gct == NULL)
		return;

	(void) pthread_mutex_lock(&gc_threads_lock);

	/* Remove from linked list */
	umem_gc_thread_t **pp = &gc_thread_list;
	while (*pp != NULL) {
		if (*pp == gct) {
			*pp = gct->gct_next;
			break;
		}
		pp = &(*pp)->gct_next;
	}

	(void) pthread_mutex_unlock(&gc_threads_lock);

	umem_free(gct, sizeof (umem_gc_thread_t));
}

/*
 * Get the calling thread's stack bounds via pthread attributes.
 * Returns 0 on success, -1 on failure.
 */
static int
gc_get_stack_bounds(void **base_out, size_t *size_out)
{
	pthread_attr_t attr;
	void *base;
	size_t sz;
	int ret;

#ifdef __linux__
	ret = pthread_getattr_np(pthread_self(), &attr);
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	ret = pthread_attr_init(&attr);
	if (ret == 0)
		ret = pthread_attr_get_np(pthread_self(), &attr);
#else
	/*
	 * Fallback: cannot determine stack bounds.
	 * Root scanning will skip this thread's stack.
	 */
	*base_out = NULL;
	*size_out = 0;
	return (-1);
#endif

	if (ret != 0) {
		*base_out = NULL;
		*size_out = 0;
		return (-1);
	}

	ret = pthread_attr_getstack(&attr, &base, &sz);
	(void) pthread_attr_destroy(&attr);

	if (ret != 0) {
		*base_out = NULL;
		*size_out = 0;
		return (-1);
	}

	*base_out = base;
	*size_out = sz;
	return (0);
}

/* ----------------------------------------------------------------
 * GC cache initialization
 * ---------------------------------------------------------------- */

/*
 * Create the size-classed GC caches. Called once during umem_gc_init().
 */
static int
gc_create_caches(void)
{
	char name[32];

	for (int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
		(void) snprintf(name, sizeof (name), "gc_%zu",
		    gc_size_classes[i]);

		gc_caches[i] = umem_cache_create(name,
		    gc_size_classes[i],	/* bufsize */
		    0,			/* align (default) */
		    NULL,		/* constructor */
		    NULL,		/* destructor */
		    NULL,		/* reclaim */
		    NULL,		/* private */
		    NULL,		/* vmem source */
		    0);			/* cflags */

		if (gc_caches[i] == NULL)
			return (-1);
	}

	return (0);
}

/* ----------------------------------------------------------------
 * Mark phase: work queue for iterative marking
 * ---------------------------------------------------------------- */

#define	GC_MARK_QUEUE_INIT	256

typedef struct gc_mark_queue {
	umem_gc_header_t	**mq_items;
	size_t			mq_count;
	size_t			mq_capacity;
} gc_mark_queue_t;

static gc_mark_queue_t gc_mark_queue;

static int
gc_mark_queue_init(gc_mark_queue_t *q)
{
	q->mq_items = umem_alloc(
	    GC_MARK_QUEUE_INIT * sizeof (umem_gc_header_t *), UMEM_DEFAULT);
	if (q->mq_items == NULL)
		return (-1);
	q->mq_count = 0;
	q->mq_capacity = GC_MARK_QUEUE_INIT;
	return (0);
}

static void
gc_mark_queue_destroy(gc_mark_queue_t *q)
{
	if (q->mq_items != NULL) {
		umem_free(q->mq_items,
		    q->mq_capacity * sizeof (umem_gc_header_t *));
		q->mq_items = NULL;
	}
	q->mq_count = 0;
	q->mq_capacity = 0;
}

static int
gc_mark_queue_push(gc_mark_queue_t *q, umem_gc_header_t *hdr)
{
	if (q->mq_count >= q->mq_capacity) {
		size_t new_cap = q->mq_capacity * 2;
		umem_gc_header_t **new_items = umem_alloc(
		    new_cap * sizeof (umem_gc_header_t *), UMEM_DEFAULT);
		if (new_items == NULL)
			return (-1);
		memcpy(new_items, q->mq_items,
		    q->mq_count * sizeof (umem_gc_header_t *));
		umem_free(q->mq_items,
		    q->mq_capacity * sizeof (umem_gc_header_t *));
		q->mq_items = new_items;
		q->mq_capacity = new_cap;
	}
	q->mq_items[q->mq_count++] = hdr;
	return (0);
}

static umem_gc_header_t *
gc_mark_queue_pop(gc_mark_queue_t *q)
{
	if (q->mq_count == 0)
		return (NULL);
	return (q->mq_items[--q->mq_count]);
}

/*
 * Mark callback invoked by the root scanner for each potential pointer.
 * If the value points to a GC-managed object, mark it and enqueue
 * for recursive scanning (unless atomic).
 */
static void
gc_mark_callback(void *potential_ptr)
{
	umem_gc_header_t *hdr = umem_gc_find_header(potential_ptr);
	if (hdr == NULL)
		return;

	uint32_t mark_val = atomic_load(&gc_mark_value);
	if (atomic_load(&hdr->gc_mark) == mark_val)
		return;

	umem_gc_mark_object(hdr);

	if (!(hdr->gc_flags & UMEM_GC_ATOMIC))
		(void) gc_mark_queue_push(&gc_mark_queue, hdr);
}

/*
 * Scan a marked non-atomic object's contents for interior pointers.
 * Each word-aligned value is checked as a potential GC pointer.
 */
static void
gc_scan_object(umem_gc_header_t *hdr)
{
	void *data = UMEM_GC_USER_PTR(hdr);
	umem_gc_scan_stack(data, (char *)data + hdr->gc_size,
	    gc_mark_callback);
}

/*
 * Drain the mark queue: pop objects and scan their contents.
 * New objects discovered during scanning are pushed back onto the queue.
 */
static void
gc_drain_mark_queue(void)
{
	umem_gc_header_t *hdr;

	while ((hdr = gc_mark_queue_pop(&gc_mark_queue)) != NULL)
		gc_scan_object(hdr);
}

/* ----------------------------------------------------------------
 * Sweep helpers
 * ---------------------------------------------------------------- */

/*
 * Free the backing memory for a GC object. The header must already
 * be removed from the global object list.
 */
static void
gc_free_object(umem_gc_header_t *hdr)
{
	size_t total_size = sizeof (umem_gc_header_t) + hdr->gc_size;
	size_t alloc_sz = gc_alloc_size(total_size);
	umem_cache_t *cp = gc_cache_for_size(total_size);

	if (cp != NULL) {
		umem_cache_free(cp, hdr);
	} else {
		umem_free(hdr, alloc_sz);
	}

	atomic_fetch_sub(&gc_heap_size, hdr->gc_size);
}

/*
 * Run finalizer for a GC object, if one is registered.
 * Sets the gc_in_finalizer guard to prevent GC_MALLOC recursion.
 */
static void
gc_run_finalizer(umem_gc_header_t *hdr)
{
	if ((hdr->gc_flags & UMEM_GC_FINALIZE) && hdr->gc_finalizer != NULL) {
		void *user_ptr = UMEM_GC_USER_PTR(hdr);
		gc_in_finalizer++;
		hdr->gc_finalizer(user_ptr, hdr->gc_finalizer_data);
		gc_in_finalizer--;
		atomic_fetch_add(&gc_finalizers_run, 1);
	}
}

/*
 * Sweep phase: walk all objects, free those not marked in the current cycle.
 * Objects with finalizers are finalized first, then freed.
 *
 * Returns the number of bytes freed.
 */
static size_t
gc_sweep(void)
{
	uint32_t mark_val = atomic_load(&gc_mark_value);
	size_t bytes_freed = 0;
	uint64_t objects_freed = 0;

	/*
	 * Collect objects to finalize and objects to free into separate
	 * lists to avoid calling finalizers under the object lock.
	 */
	umem_gc_header_t *finalize_list = NULL;
	umem_gc_header_t *free_list = NULL;

	(void) pthread_mutex_lock(&gc_objects_lock);

	umem_gc_header_t *hdr = gc_objects_sentinel.gc_next;
	while (hdr != &gc_objects_sentinel) {
		umem_gc_header_t *next = hdr->gc_next;

		if (hdr->gc_flags & UMEM_GC_PINNED) {
			hdr = next;
			continue;
		}

		if (atomic_load(&hdr->gc_mark) != mark_val) {
			gc_object_remove_locked(hdr);

			if (hdr->gc_flags & UMEM_GC_FINALIZE) {
				hdr->gc_next = finalize_list;
				finalize_list = hdr;
			} else {
				hdr->gc_next = free_list;
				free_list = hdr;
			}
		}

		hdr = next;
	}

	(void) pthread_mutex_unlock(&gc_objects_lock);

	/* Finalize phase */
	atomic_store_explicit(&gc_phase, GC_PHASE_FINALIZE, memory_order_release);

	while (finalize_list != NULL) {
		hdr = finalize_list;
		finalize_list = hdr->gc_next;

		gc_run_finalizer(hdr);
		bytes_freed += hdr->gc_size;
		objects_freed++;
		gc_free_object(hdr);
	}

	/* Free unmarked non-finalizable objects */
	while (free_list != NULL) {
		hdr = free_list;
		free_list = hdr->gc_next;

		bytes_freed += hdr->gc_size;
		objects_freed++;
		gc_free_object(hdr);
	}

	atomic_fetch_add(&gc_objects_freed, objects_freed);
	return (bytes_freed);
}

/*
 * Reset all mark bits for the next cycle. This is done by incrementing
 * the mark value rather than walking all objects.
 */
static void
gc_reset_marks(void)
{
	atomic_fetch_add(&gc_mark_value, 1);

	/* Skip 0 to avoid confusion with uninitialized headers */
	if (atomic_load(&gc_mark_value) == 0)
		atomic_fetch_add(&gc_mark_value, 1);
}

/* ----------------------------------------------------------------
 * Public API: Initialization
 * ---------------------------------------------------------------- */

int
umem_gc_init(void)
{
	(void) pthread_mutex_lock(&gc_init_lock);

	if (gc_initialized) {
		(void) pthread_mutex_unlock(&gc_init_lock);
		return (0);
	}

	/* Initialize the sentinel for the doubly-linked object list */
	gc_objects_sentinel.gc_next = &gc_objects_sentinel;
	gc_objects_sentinel.gc_prev = &gc_objects_sentinel;

	/* Create pthread key for automatic thread unregistration */
	if (pthread_key_create(&gc_thread_key,
	    gc_thread_key_destructor) != 0) {
		(void) pthread_mutex_unlock(&gc_init_lock);
		return (-1);
	}

	/* Create page-level sparsemap for O(1) pointer lookup */
	gc_pagemap = umem_sparsemap_create(0);
	if (gc_pagemap == NULL) {
		(void) pthread_key_delete(gc_thread_key);
		(void) pthread_mutex_unlock(&gc_init_lock);
		return (-1);
	}

	/* Create size-classed GC caches */
	if (gc_create_caches() != 0) {
		umem_sparsemap_destroy(gc_pagemap);
		gc_pagemap = NULL;
		(void) pthread_key_delete(gc_thread_key);
		(void) pthread_mutex_unlock(&gc_init_lock);
		return (-1);
	}

	/* Set initial threshold */
	atomic_store(&gc_threshold, GC_MIN_THRESHOLD);

	/*
	 * Install STW signal handler. Currently only on Linux where
	 * SIGUSR2 + pthread_kill + spin-in-handler is well-tested.
	 * FreeBSD/Illumos fall back to best-effort root scanning.
	 */
#ifdef __linux__
	(void) gc_install_stw_signal();
#endif

	gc_initialized = 1;
	(void) pthread_mutex_unlock(&gc_init_lock);

	/* Register the calling thread */
	(void) umem_gc_register_thread();

	return (0);
}

/* ----------------------------------------------------------------
 * Public API: Thread registration
 * ---------------------------------------------------------------- */

int
umem_gc_register_thread(void)
{
	if (!gc_initialized)
		return (-1);

	/* Already registered? */
	if (pthread_getspecific(gc_thread_key) != NULL)
		return (0);

	umem_gc_thread_t *gct = umem_alloc(sizeof (umem_gc_thread_t),
	    UMEM_DEFAULT);
	if (gct == NULL)
		return (-1);

	memset(gct, 0, sizeof (umem_gc_thread_t));
	gct->gct_tid = pthread_self();
	(void) gc_get_stack_bounds(&gct->gct_stack_base, &gct->gct_stack_size);

	(void) pthread_mutex_lock(&gc_threads_lock);
	gct->gct_next = gc_thread_list;
	gc_thread_list = gct;
	(void) pthread_mutex_unlock(&gc_threads_lock);

	(void) pthread_setspecific(gc_thread_key, gct);

	/*
	 * Register in the STW thread registry too. This is the array the
	 * stop-the-world suspend signal and the Phase-3 root scan actually
	 * walk; without this the collector would suspend nobody and scan no
	 * worker stack, letting a stack-rooted live object be swept.
	 */
	(void) umem_gc_thread_register();

	return (0);
}

void
umem_gc_unregister_thread(void)
{
	umem_gc_thread_t *gct = pthread_getspecific(gc_thread_key);
	if (gct == NULL)
		return;

	(void) umem_gc_thread_unregister();
	gc_thread_key_destructor(gct);
	(void) pthread_setspecific(gc_thread_key, NULL);
}

/* ----------------------------------------------------------------
 * Public API: Allocation
 * ---------------------------------------------------------------- */

/*
 * Check if a GC collection should be triggered.
 */
static void
gc_maybe_collect(void)
{
	size_t allocated = atomic_load_explicit(&gc_bytes_allocated,
	    memory_order_acquire);
	size_t thresh = atomic_load_explicit(&gc_threshold,
	    memory_order_acquire);

	if (allocated > thresh)
		umem_gc_collect();
}

void *
umem_gc_alloc(size_t size, int flags)
{
	if (gc_in_finalizer) {
		(void) fprintf(stderr, "umem_gc_alloc: allocation during "
		    "finalization is not permitted\n");
		return (NULL);
	}

	if (!gc_initialized) {
		if (umem_gc_init() != 0)
			return (NULL);
	}

	if (size == 0)
		size = 1;

	size_t total_size = sizeof (umem_gc_header_t) + size;
	umem_gc_header_t *hdr;
	umem_cache_t *cp = gc_cache_for_size(total_size);

	if (cp != NULL) {
		hdr = umem_cache_alloc(cp, UMEM_DEFAULT);
	} else {
		size_t alloc_sz = gc_alloc_size(total_size);
		hdr = umem_alloc(alloc_sz, UMEM_DEFAULT);
	}

	if (hdr == NULL) {
		/* Try collecting and retry once */
		umem_gc_collect();

		if (cp != NULL) {
			hdr = umem_cache_alloc(cp, UMEM_DEFAULT);
		} else {
			size_t alloc_sz = gc_alloc_size(total_size);
			hdr = umem_alloc(alloc_sz, UMEM_DEFAULT);
		}

		if (hdr == NULL)
			return (NULL);
	}

	/* Initialize the GC header */
	atomic_store(&hdr->gc_mark, 0);
	hdr->gc_flags = (flags & UMEM_GC_ATOMIC) ? UMEM_GC_ATOMIC : 0;
	hdr->gc_size = size;
	hdr->gc_finalizer = NULL;
	hdr->gc_finalizer_data = NULL;

	/* Zero the user portion (GC_MALLOC semantics) */
	void *user_ptr = UMEM_GC_USER_PTR(hdr);
	memset(user_ptr, 0, size);

	/* Add to global object list */
	gc_object_add(hdr);

	/* Update statistics */
	atomic_fetch_add(&gc_bytes_allocated, size);
	atomic_fetch_add(&gc_heap_size, size);
	atomic_fetch_add(&gc_total_allocated, size);

	/* Check if we should trigger collection */
	gc_maybe_collect();

	return (user_ptr);
}

void *
umem_gc_alloc_atomic(size_t size, int flags)
{
	return (umem_gc_alloc(size, flags | UMEM_GC_ATOMIC));
}

void *
umem_gc_realloc(void *ptr, size_t new_size)
{
	if (ptr == NULL)
		return (umem_gc_alloc(new_size, 0));

	if (new_size == 0) {
		umem_gc_free(ptr);
		return (NULL);
	}

	umem_gc_header_t *old_hdr = UMEM_GC_HEADER(ptr);
	size_t old_size = old_hdr->gc_size;

	/* If shrinking and still fits in same size class, just update size */
	if (new_size <= old_size) {
		size_t old_total = sizeof (umem_gc_header_t) + old_size;
		size_t new_total = sizeof (umem_gc_header_t) + new_size;
		if (gc_alloc_size(old_total) == gc_alloc_size(new_total)) {
			old_hdr->gc_size = new_size;
			return (ptr);
		}
	}

	/*
	 * Pin old object so GC triggered by alloc won't free it.
	 * Take read lock to prevent mark phase from starting before
	 * the pin is visible — otherwise mark could read flags before
	 * pin, then sweep could free the object during our copy.
	 */
	(void) pthread_rwlock_rdlock(&gc_phase_rwlock);
	old_hdr->gc_flags |= UMEM_GC_PINNED;
	(void) pthread_rwlock_unlock(&gc_phase_rwlock);

	/* Allocate new, copy, free old */
	int flags = old_hdr->gc_flags & UMEM_GC_ATOMIC;
	void *new_ptr = umem_gc_alloc(new_size, flags);
	if (new_ptr == NULL) {
		old_hdr->gc_flags &= ~UMEM_GC_PINNED;
		return (NULL);
	}

	size_t copy_size = (old_size < new_size) ? old_size : new_size;
	memcpy(new_ptr, ptr, copy_size);

	/* Transfer finalizer if present */
	if (old_hdr->gc_flags & UMEM_GC_FINALIZE) {
		umem_gc_header_t *new_hdr = UMEM_GC_HEADER(new_ptr);
		new_hdr->gc_flags |= UMEM_GC_FINALIZE;
		new_hdr->gc_finalizer = old_hdr->gc_finalizer;
		new_hdr->gc_finalizer_data = old_hdr->gc_finalizer_data;
		old_hdr->gc_flags &= ~UMEM_GC_FINALIZE;
		old_hdr->gc_finalizer = NULL;
	}

	old_hdr->gc_flags &= ~UMEM_GC_PINNED;
	umem_gc_free(ptr);
	return (new_ptr);
}

void
umem_gc_free(void *ptr)
{
	if (ptr == NULL)
		return;

	umem_gc_header_t *hdr = UMEM_GC_HEADER(ptr);
	sigset_t block, prev_mask;

	/*
	 * Remove from global object list with SIGUSR2 blocked, so a
	 * concurrent stop-the-world can never observe this object
	 * half-removed (off the sparsemap but still on the sweep list, or
	 * vice versa).  Same reasoning as gc_object_add().
	 */
	(void) sigemptyset(&block);
	(void) sigaddset(&block, GC_SUSPEND_SIGNAL);
	(void) pthread_sigmask(SIG_BLOCK, &block, &prev_mask);

	(void) pthread_mutex_lock(&gc_objects_lock);
	gc_object_remove_locked(hdr);
	(void) pthread_mutex_unlock(&gc_objects_lock);

	(void) pthread_sigmask(SIG_SETMASK, &prev_mask, NULL);

	/* Run finalizer if registered */
	gc_run_finalizer(hdr);

	/* Update stats before freeing */
	size_t obj_size = hdr->gc_size;

	/* Free backing memory */
	gc_free_object(hdr);

	/* Reduce allocated count */
	size_t prev = atomic_load(&gc_bytes_allocated);
	if (prev >= obj_size)
		atomic_fetch_sub(&gc_bytes_allocated, obj_size);
	else
		atomic_store(&gc_bytes_allocated, 0);
}

/* ----------------------------------------------------------------
 * Public API: Collection
 * ---------------------------------------------------------------- */

void
umem_gc_collect(void)
{
	if (!gc_initialized)
		return;

	/* Only one collection at a time */
	if (pthread_mutex_trylock(&gc_collect_lock) != 0)
		return;

	size_t allocated_before = atomic_load(&gc_bytes_allocated);

	/* Phase 1: Mark
	 * Take write lock to ensure all concurrent reallocs have
	 * finished pinning their objects before we start marking. */
	(void) pthread_rwlock_wrlock(&gc_phase_rwlock);
	gc_reset_marks();
	atomic_store_explicit(&gc_phase, GC_PHASE_MARK, memory_order_release);
	(void) pthread_rwlock_unlock(&gc_phase_rwlock);

	/* Initialize mark work queue */
	if (gc_mark_queue_init(&gc_mark_queue) != 0) {
		(void) pthread_mutex_unlock(&gc_collect_lock);
		return;
	}

	/*
	 * Stop the world: suspend all other threads so root scanning
	 * sees a consistent snapshot of stacks and registers.
	 * If STW fails (timeout), fall back to best-effort scanning.
	 */
	int stw_active = 0;
	if (gc_stw_installed)
		stw_active = (gc_stop_the_world() == 0);

	/* Scan all roots (stacks, registers, data segments) */
	/*
	 * The registry lock is NOT held during marking (gc_stop_the_world
	 * drops it once the park barrier completes), so the Phase-3 root
	 * scan re-acquires it itself: pass threads_locked = 0.
	 *
	 * ponytail: a worker can park while holding an allocator lock (it
	 * was signalled mid umem_cache_alloc).  Marking here must not itself
	 * take that lock or it would block until resume.  Today the mark
	 * path does not allocate, so this holds; if mark ever needs to
	 * allocate while stopped, pre-reserve its memory before STW.
	 */
	umem_gc_scan_all_roots(gc_mark_callback, 0);

	/* Drain work queue: recursively scan non-atomic marked objects */
	gc_drain_mark_queue();

	/* Phase 2: Remark — rescan for missed references */
	atomic_store_explicit(&gc_phase, GC_PHASE_REMARK, memory_order_release);
	umem_gc_scan_all_roots(gc_mark_callback, 0);
	gc_drain_mark_queue();

	/* Resume the world after marking is complete */
	if (stw_active)
		gc_resume_the_world();

	gc_mark_queue_destroy(&gc_mark_queue);

	/* Phase 3: Sweep */
	atomic_store_explicit(&gc_phase, GC_PHASE_SWEEP, memory_order_release);
	size_t bytes_freed = gc_sweep();

	/* Update threshold based on survival rate */
	size_t survived = 0;
	if (allocated_before > bytes_freed)
		survived = allocated_before - bytes_freed;

	atomic_store(&gc_bytes_survived, survived);
	atomic_store(&gc_bytes_allocated, 0);

	size_t new_threshold = survived * GC_THRESHOLD_MULTIPLIER;
	if (new_threshold < GC_MIN_THRESHOLD)
		new_threshold = GC_MIN_THRESHOLD;
	atomic_store(&gc_threshold, new_threshold);

	atomic_fetch_add(&gc_collections, 1);

	/* Return to idle */
	atomic_store_explicit(&gc_phase, GC_PHASE_IDLE, memory_order_release);

	(void) pthread_mutex_unlock(&gc_collect_lock);
}

/* ----------------------------------------------------------------
 * Public API: Finalizers
 * ---------------------------------------------------------------- */

void
umem_gc_register_finalizer(void *ptr,
    void (*fn)(void *, void *), void *cd,
    void (**old_fn)(void *, void *), void **old_cd)
{
	if (ptr == NULL)
		return;

	umem_gc_header_t *hdr = UMEM_GC_HEADER(ptr);

	/* Return old finalizer if requested */
	if (old_fn != NULL)
		*old_fn = hdr->gc_finalizer;
	if (old_cd != NULL)
		*old_cd = hdr->gc_finalizer_data;

	/* Set new finalizer */
	hdr->gc_finalizer = fn;
	hdr->gc_finalizer_data = cd;

	if (fn != NULL)
		hdr->gc_flags |= UMEM_GC_FINALIZE;
	else
		hdr->gc_flags &= ~UMEM_GC_FINALIZE;
}

/* ----------------------------------------------------------------
 * Public API: Statistics
 * ---------------------------------------------------------------- */

/*
 * Aggregate free bytes across all GC size-classed caches.
 * Free buffers = buftotal - (slab_alloc - slab_free).
 */
static size_t
gc_aggregate_free_bytes(void)
{
	size_t free_bytes = 0;

	for (int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
		umem_cache_t *cp = gc_caches[i];
		if (cp == NULL)
			continue;
		uint64_t alloc = cp->cache_slab_alloc;
		uint64_t freed = cp->cache_slab_free;
		uint64_t total = cp->cache_buftotal;
		uint64_t in_use = (alloc >= freed) ? (alloc - freed) : 0;
		if (total > in_use)
			free_bytes += (total - in_use) * cp->cache_bufsize;
	}

	return (free_bytes);
}

void
umem_gc_get_stats(umem_gc_stats_t *stats)
{
	stats->gcs_heap_size = atomic_load(&gc_heap_size);
	stats->gcs_free_bytes = gc_aggregate_free_bytes();
	stats->gcs_total_allocated = atomic_load(&gc_total_allocated);
	stats->gcs_bytes_allocated = atomic_load(&gc_bytes_allocated);
	stats->gcs_bytes_survived = atomic_load(&gc_bytes_survived);
	stats->gcs_collections = atomic_load(&gc_collections);
	stats->gcs_objects_freed = atomic_load(&gc_objects_freed);
	stats->gcs_finalizers_run = atomic_load(&gc_finalizers_run);
}

size_t
umem_gc_get_heap_size(void)
{
	return (atomic_load(&gc_heap_size));
}

size_t
umem_gc_get_free_bytes(void)
{
	return (gc_aggregate_free_bytes());
}

size_t
umem_gc_get_total_bytes(void)
{
	return (atomic_load(&gc_total_allocated));
}

/* ----------------------------------------------------------------
 * Public API: Phase query
 * ---------------------------------------------------------------- */

umem_gc_phase_t
umem_gc_get_phase(void)
{
	return (atomic_load_explicit(&gc_phase, memory_order_acquire));
}

void
umem_gc_set_phase(umem_gc_phase_t phase)
{
	atomic_store_explicit(&gc_phase, phase, memory_order_release);
}

uint32_t
umem_gc_get_mark_value(void)
{
	return (atomic_load(&gc_mark_value));
}

/* ----------------------------------------------------------------
 * Hooks for mark/sweep (used by root scanner, Task #2)
 * ---------------------------------------------------------------- */

/*
 * Check if a pointer is a GC-managed allocation and return its header.
 *
 * Uses a page-level sparsemap for O(1) fast-path rejection: if no GC
 * objects reside on the page containing ptr, we can immediately return
 * NULL without scanning the object list. When the page is tracked, we
 * fall through to a linear scan to confirm the exact pointer.
 */
umem_gc_header_t *
umem_gc_find_header(void *ptr)
{
	if (ptr == NULL)
		return (NULL);

	/* O(1) lookup via per-page object lists in sparsemap */
	if (gc_pagemap != NULL)
		return (umem_sparsemap_find_object(gc_pagemap, ptr));

	/* Fallback: O(n) scan if sparsemap not available */
	{
		umem_gc_header_t *candidate = UMEM_GC_HEADER(ptr);

		(void) pthread_mutex_lock(&gc_objects_lock);

		umem_gc_header_t *hdr = gc_objects_sentinel.gc_next;
		while (hdr != &gc_objects_sentinel) {
			if (hdr == candidate) {
				(void) pthread_mutex_unlock(&gc_objects_lock);
				return (hdr);
			}
			hdr = hdr->gc_next;
		}

		(void) pthread_mutex_unlock(&gc_objects_lock);
	}
	return (NULL);
}

void
umem_gc_mark_object(umem_gc_header_t *hdr)
{
	if (hdr == NULL)
		return;

	uint32_t mark_val = atomic_load(&gc_mark_value);
	uint32_t old = atomic_load(&hdr->gc_mark);
	if (old == mark_val)
		return;
	atomic_compare_exchange_strong(&hdr->gc_mark, &old, mark_val);
}

void
umem_gc_walk_objects(void (*fn)(umem_gc_header_t *hdr, void *arg), void *arg)
{
	(void) pthread_mutex_lock(&gc_objects_lock);

	umem_gc_header_t *hdr = gc_objects_sentinel.gc_next;
	while (hdr != &gc_objects_sentinel) {
		umem_gc_header_t *next = hdr->gc_next;
		fn(hdr, arg);
		hdr = next;
	}

	(void) pthread_mutex_unlock(&gc_objects_lock);
}

/* ----------------------------------------------------------------
 * Thread list access (for root scanner)
 * ---------------------------------------------------------------- */

umem_gc_thread_t *
umem_gc_get_thread_list(void)
{
	return (gc_thread_list);
}

void
umem_gc_lock_threads(void)
{
	(void) pthread_mutex_lock(&gc_threads_lock);
}

void
umem_gc_unlock_threads(void)
{
	(void) pthread_mutex_unlock(&gc_threads_lock);
}

/* ----------------------------------------------------------------
 * Object list lock (for sweep coordination)
 * ---------------------------------------------------------------- */

void
umem_gc_lock_objects(void)
{
	(void) pthread_mutex_lock(&gc_objects_lock);
}

void
umem_gc_unlock_objects(void)
{
	(void) pthread_mutex_unlock(&gc_objects_lock);
}

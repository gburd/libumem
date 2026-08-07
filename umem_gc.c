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

/*
 * Global object set, sharded to cut allocator-vs-allocator contention on
 * gc_object_add() under high concurrency / CPU oversubscription (the
 * documented >=6x throughput tail).  Each shard owns an independent object
 * list AND its own page sparsemap, so a GC_MALLOC only ever contends the one
 * shard its object hashes to.  Stop-the-world walks every shard (all mutators
 * are parked, so the snapshot is still complete + quiescent); find_header
 * routes to a shard by the same hash and reads that shard's pagemap
 * locklessly (safe only under STW, as before).
 *
 * GC_NSHARDS must be a power of two.
 */
#define	GC_NSHARDS	64
typedef struct gc_shard {
	pthread_mutex_t		lock;
	umem_gc_header_t	sentinel;
	umem_sparsemap_t	*pagemap;
	char			pad[64];	/* keep shards on separate lines */
} gc_shard_t;
static gc_shard_t gc_shards[GC_NSHARDS];

/*
 * Hash a PAGE address to a shard.  Sharding is by page (not by object
 * address) so that every object residing on a given page lands in the same
 * shard's pagemap -- this is what lets umem_gc_find_header() route a
 * conservative (possibly interior) pointer to a single shard by page and
 * still find the object via that shard's per-page object list.  A
 * multiplicative hash spreads adjacent pages across shards so a burst of
 * same-size allocations does not pile onto one shard.
 *
 * GC_NSHARDS must be a power of two.
 */
static inline unsigned
gc_shard_of_page(uintptr_t page)
{
	uintptr_t k = page >> 12;		/* drop page-offset bits */
	k *= (uintptr_t)0x9E3779B97F4A7C15ULL;
	return ((unsigned)(k >> (64 - 6)) & (GC_NSHARDS - 1));
}

/* Shard for a user pointer (or interior pointer): by its containing page. */
static inline unsigned
gc_shard_of_ptr(const void *ptr)
{
	static uintptr_t pgmask;
	if (pgmask == 0) {
#ifdef _SC_PAGESIZE
		long sz = sysconf(_SC_PAGESIZE);
		pgmask = ~((uintptr_t)((sz > 0) ? sz : 4096) - 1);
#else
		pgmask = ~((uintptr_t)4096 - 1);
#endif
	}
	return (gc_shard_of_page((uintptr_t)ptr & pgmask));
}

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

/* Page-level sparsemap for O(1) GC pointer lookup: now per-shard (see
 * gc_shards[]); this global is retired. */

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
/*
 * STW park-barrier timeout.  Generous on purpose: under heavy CPU
 * oversubscription every mutator still parks (cooperatively at the alloc
 * safepoint or via the suspend signal), but a mutator may first have to
 * drain a slow allocator critical section -- e.g. finish an O(n) page
 * sparsemap resize it holds gc_objects_lock for while dozens of peers
 * queue behind it.  That drain is bounded but can take seconds at 6x
 * oversubscription.  We wait long enough for it rather than time out and
 * skip the whole collection (which would stall progress); the timeout
 * exists only to bound a genuine wedge (an allocator lock cycle a mutator
 * can never escape), after which we skip the sweep -- never sweep an
 * incomplete snapshot.
 */
#define	GC_STW_TIMEOUT_MS	30000

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

/*
 * Per-thread cache of this thread's slot in umem_gc_threads[], set at
 * registration.  -1 means "not a registered GC thread".  Lets the
 * cooperative safepoint and the signal handler find their slot in O(1)
 * instead of scanning the whole registry on every poll.
 */
static __thread int gc_self_slot = -1;

/*
 * Per-thread re-entrancy guard for parking.  Set while this thread is
 * spinning inside gc_park_self().  A cooperative park (from gc_safepoint)
 * does NOT block SIGUSR2 while it spins, so the collector's retry re-signal
 * can interrupt the spin and re-enter the handler; without this guard the
 * handler would call gc_park_self() again (nested park), double-count the
 * suspend ACK and clobber the recorded gcti_sp/gcti_regs with the handler
 * frame instead of the mutator's live frame -- which deadlocked the barrier
 * and corrupted the root snapshot.  If already parked, a fresh signal/poll
 * is a no-op: the existing spin already covers the current pause.
 */
static __thread volatile sig_atomic_t gc_parked;

/*
 * Park the calling thread for the duration of the current stop-the-world.
 * Spills callee-saved registers into gcti_regs and records the live stack
 * pointer in gcti_sp so the collector can scan this thread's roots, then
 * publishes gcti_suspended = 1 (its ACK) and spins until the collector
 * clears gc_stw_active.  Async-signal-safe: uses only sigsetjmp, plain
 * stores, and an atomic add, so it is callable both from the SIGUSR2
 * handler and from the cooperative safepoint poll.
 *
 * Re-entrant delivery is guarded by gc_parked: a thread already spinning
 * here ignores a fresh signal/poll (see the callers).  We do NOT nest.
 *
 * stack_anchor must be the address of a local in the CALLER's frame (or
 * this frame) that sits at/above the live stack top to scan; we take it as
 * an argument so the recorded SP covers the caller's frame too.
 */
static void
gc_park_self(int slot, void *stack_anchor)
{
	gc_parked = 1;

	(void) sigsetjmp(umem_gc_threads[slot].gcti_regs, 0);
	umem_gc_threads[slot].gcti_sp = stack_anchor;

	umem_gc_threads[slot].gcti_park_pending = 0;
	umem_gc_threads[slot].gcti_suspended = 1;
	atomic_fetch_add(&gc_stw_suspended_count, 1);

	/*
	 * Spin until the collector ends the pause.  sched_yield() (not a
	 * pure busy-spin) is essential under CPU oversubscription: dozens of
	 * parked threads busy-spinning on 8 cores would starve the collector
	 * (and any thread still finishing an allocation before it can park),
	 * turning the pause into a pathological slowdown that looks like a
	 * hang.  Yielding hands the cores to the collector so the pause
	 * completes promptly.  sched_yield is async-signal-safe, so this is
	 * valid when gc_park_self runs from the SIGUSR2 handler; we avoid
	 * sem_wait/condvar which are not.
	 */
	while (gc_stw_active)
		(void) sched_yield();

	umem_gc_threads[slot].gcti_suspended = 0;
	gc_parked = 0;
}

/*
 * Cooperative safepoint: a mutator calls this at a known-safe point (GC
 * allocation entry, before it touches any GC-internal lock or list).  If a
 * stop-the-world is in progress it parks the thread here -- so suspension
 * does NOT depend on async signal delivery timing, which is the fragile
 * part under CPU oversubscription.  Because the poll is outside every GC
 * critical section, a cooperatively-parked thread is never frozen holding
 * gc_objects_lock or mid list+sparsemap update, so the collector's sweep
 * can never deadlock against it and its roots are always cleanly scannable.
 */
static void
gc_safepoint(void)
{
	int slot = gc_self_slot;
	volatile int stack_anchor;

	if (!gc_stw_active || slot < 0 || gc_parked)
		return;
	if (pthread_equal(gc_stw_coordinator, pthread_self()))
		return;

	gc_park_self(slot, (void *)&stack_anchor);
}

/*
 * Enter a GC-internal critical section (holding gc_objects_lock across a
 * list+sparsemap update).  Sets the lightweight gcti_in_gc_critical flag
 * (a plain store -- no syscall, unlike the old per-allocation
 * pthread_sigmask) so a suspend signal that lands here defers parking
 * instead of freezing the thread mid-update.  Returns the thread's slot,
 * or -1 if this is not a registered GC thread (then the flag machinery is
 * a no-op and STW simply never targets it).
 */
static int
gc_critical_enter(void)
{
	int slot = gc_self_slot;

	if (slot >= 0) {
		umem_gc_threads[slot].gcti_in_gc_critical = 1;
		atomic_signal_fence(memory_order_seq_cst);
	}
	return (slot);
}

/*
 * Leave a GC-internal critical section.  Clears gcti_in_gc_critical; if a
 * suspend signal arrived while we held the section it set gcti_park_pending,
 * so park now -- at this safe point, with the lock released and the object
 * fully linked in both the sweep list and the sparsemap -- to give the
 * waiting collector our ACK.
 */
static void
gc_critical_exit(int slot)
{
	volatile int stack_anchor;

	if (slot < 0)
		return;

	umem_gc_threads[slot].gcti_in_gc_critical = 0;
	atomic_signal_fence(memory_order_seq_cst);

	if (gc_stw_active && umem_gc_threads[slot].gcti_park_pending &&
	    !pthread_equal(gc_stw_coordinator, pthread_self()))
		gc_park_self(slot, (void *)&stack_anchor);
}

static void
gc_suspend_handler(int signo)
{
	int slot = gc_self_slot;
	volatile int stack_anchor;

	(void)signo;

	/*
	 * If no stop-the-world is in progress this is a stale/late signal
	 * from a collection that has already resumed; do nothing.  Likewise
	 * if we are already parked (spinning in gc_park_self): a retry
	 * re-signal must not nest a second park.
	 */
	if (!gc_stw_active || slot < 0 || gc_parked)
		return;

	/*
	 * If the signal landed while this thread is inside a GC critical
	 * section (holding gc_objects_lock mid list+sparsemap update), we
	 * must NOT park here: parking frozen with the lock held would
	 * deadlock the collector's sweep, and scanning a half-linked object
	 * (on the sweep list but not yet in the sparsemap, or vice versa)
	 * could miss a reachable root.  Defer: record that a park is pending
	 * and return.  The thread parks itself via gc_safepoint()/the
	 * critical-section exit check the moment it leaves the section, which
	 * is imminent and lock-free.  The collector keeps waiting for its ACK.
	 */
	if (umem_gc_threads[slot].gcti_in_gc_critical) {
		umem_gc_threads[slot].gcti_park_pending = 1;
		return;
	}

	gc_park_self(slot, (void *)&stack_anchor);
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
 * Wait until every thread that parked has left gc_park_self()
 * (gcti_suspended back to 0).  Called after clearing gc_stw_active, both
 * on the normal resume path and the barrier-timeout path.  This closes an
 * ABA on gc_stw_active: without draining, the next collection could flip
 * gc_stw_active 0->1 and reset the suspended flags before a still-parked
 * thread's spin observed the 0, wedging that thread in the OLD park with
 * suspended=0 -- the new collector would then wait forever for an ACK it
 * never re-posts.  Parked threads sched_yield(), so they unpark within a
 * scheduling quantum once the collector stops hogging the CPU.
 */
static void
gc_drain_parked(void)
{
	int i;

	for (;;) {
		int busy = 0;
		for (i = 0; i < UMEM_GC_MAX_THREADS; i++) {
			if (umem_gc_threads[i].gcti_suspended) {
				busy = 1;
				break;
			}
		}
		if (!busy)
			return;
		(void) sched_yield();
	}
}

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

	/*
	 * Build the target set = every registered thread except the
	 * collector, and best-effort signal each.  The signal is only a
	 * fallback to prod a thread that is stuck in a long NON-allocating
	 * region (it will not reach the cooperative safepoint on its own);
	 * threads on the alloc path park cooperatively.  We wait for EVERY
	 * target below regardless of whether pthread_kill succeeded, so a
	 * thread whose signal could not be delivered is still awaited (it
	 * parks at its next alloc safepoint) rather than silently excluded
	 * from the snapshot -- which would let its roots go unscanned.
	 */
	for (i = 0; i < UMEM_GC_MAX_THREADS; i++) {
		if (!umem_gc_threads[i].gcti_registered)
			continue;
		if (pthread_equal(umem_gc_threads[i].gcti_thread,
		    gc_stw_coordinator))
			continue;
		(void) pthread_kill(umem_gc_threads[i].gcti_thread,
		    GC_SUSPEND_SIGNAL);
		signalled[nsignalled++] = i;
		target++;
	}

	atomic_store(&gc_stw_target_count, target);

	if (target == 0) {
		/* Nobody to park: drop the lock and mark unlocked. */
		(void) mutex_unlock(&umem_gc_threads_lock);
		return (0);
	}

	/*
	 * Wait until every target thread has parked (published
	 * gcti_suspended = 1), either cooperatively at an alloc safepoint or
	 * via the suspend signal.  gc_stw_active stays TRUE for the whole
	 * wait: a thread parks exactly once and keeps spinning, so we never
	 * tear down and re-clear its ACK (which would race a still-spinning
	 * thread and livelock).  Checking per-thread flags -- not a shared
	 * counter -- is immune to a stale signal from a prior cycle.
	 *
	 * We periodically re-signal not-yet-parked threads: under heavy
	 * oversubscription a target may have been descheduled before it could
	 * run the first signal or reach a safepoint; a nudge plus a yield
	 * lets it make progress.  If the deadline expires anyway (e.g. two
	 * mutators are wedged in a genuine allocator lock cycle and can never
	 * reach a safepoint), we tear the pause down and report failure so
	 * the caller SKIPS the sweep -- bounded, and never an unsound sweep
	 * against an incomplete snapshot.
	 */
	(void) clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += (long)GC_STW_TIMEOUT_MS * 1000000L;
	while (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec += 1;
	}

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
			 * Timed out: a mutator could not reach a safepoint or
			 * run its suspend signal in time (e.g. wedged in a
			 * genuine allocator lock cycle).  End the pause and
			 * drain any threads that DID park before returning
			 * failure, so the next collection cannot ABA-race a
			 * still-parked thread.  Caller skips the sweep -- never
			 * an unsound sweep on an incomplete snapshot.
			 */
			gc_stw_active = 0;
			atomic_thread_fence(memory_order_seq_cst);
			gc_drain_parked();
			(void) mutex_unlock(&umem_gc_threads_lock);
			return (-1);
		}

		/* Nudge any target that has not parked yet. */
		for (i = 0; i < nsignalled; i++) {
			if (!umem_gc_threads[signalled[i]].gcti_suspended)
				(void) pthread_kill(
				    umem_gc_threads[signalled[i]].gcti_thread,
				    GC_SUSPEND_SIGNAL);
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
	gc_drain_parked();
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
 * Link a GC object into the global sweep list and the page sparsemap.
 * The caller MUST already be inside a GC critical section
 * (gc_critical_enter()) so a suspend signal cannot freeze this thread
 * mid-update holding gc_objects_lock -- see gc_object_add() for the full
 * rationale.  This is the raw update, factored out so umem_gc_alloc() can
 * hold one critical section across the allocation AND the link.
 */
static void
gc_object_add_locked(umem_gc_header_t *hdr)
{
	void *user_ptr = UMEM_GC_USER_PTR(hdr);
	gc_shard_t *sh = &gc_shards[gc_shard_of_ptr(user_ptr)];

	(void) pthread_mutex_lock(&sh->lock);
	hdr->gc_next = sh->sentinel.gc_next;
	hdr->gc_prev = &sh->sentinel;
	sh->sentinel.gc_next->gc_prev = hdr;
	sh->sentinel.gc_next = hdr;
	if (sh->pagemap != NULL)
		(void) umem_sparsemap_add_object(sh->pagemap, hdr, user_ptr);
	(void) pthread_mutex_unlock(&sh->lock);
}

/*
 * Add a GC object to the global list. Caller must NOT hold gc_objects_lock
 * and must NOT already be in a GC critical section.
 *
 * The list+sparsemap update must be atomic with respect to stop-the-world:
 * a thread suspended after the object is linked into the sweep list but
 * before umem_sparsemap_add_object() records it (or vice versa) would leave
 * the object on the sweep list yet missing from umem_gc_find_header()'s
 * lookup, so a conservative scan of this thread's stack/registers would
 * find the pointer, fail to mark it, and sweep a reachable, just-allocated
 * object.
 *
 * We guarantee atomicity WITHOUT the old per-allocation pthread_sigmask()
 * (two syscalls per alloc, which made STW pathologically slow under CPU
 * oversubscription).  Instead we set the lightweight gcti_in_gc_critical
 * flag (a plain store): a suspend signal that lands inside the section
 * defers its park (see gc_suspend_handler) and the thread parks at
 * gc_critical_exit() once the update is complete and the lock released.
 */
static void
gc_object_add(umem_gc_header_t *hdr)
{
	int slot = gc_critical_enter();
	gc_object_add_locked(hdr);
	gc_critical_exit(slot);
}

/*
 * Remove a GC object from the global list. Caller must hold gc_objects_lock.
 */
/*
 * Remove a GC object from its shard list + pagemap.  Caller MUST hold the
 * object's shard lock (gc_shards[gc_shard_of_ptr(UMEM_GC_USER_PTR(hdr))].lock).
 */
static void
gc_object_remove_locked(umem_gc_header_t *hdr)
{
	void *user_ptr = UMEM_GC_USER_PTR(hdr);
	gc_shard_t *sh = &gc_shards[gc_shard_of_ptr(user_ptr)];

	hdr->gc_prev->gc_next = hdr->gc_next;
	hdr->gc_next->gc_prev = hdr->gc_prev;
	hdr->gc_next = NULL;
	hdr->gc_prev = NULL;
	if (sh->pagemap != NULL)
		umem_sparsemap_remove_object(sh->pagemap, hdr, user_ptr);
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

	/*
	 * Saturating subtract: gc_heap_size is an unsigned advisory counter
	 * (GC_get_heap_size()).  Never let it wrap to ~2^64 if a free is ever
	 * double-counted -- that surfaced as an intermittent bogus heap_before
	 * in /gc/large_heap.  Mirrors the guard umem_gc_free() applies to
	 * gc_bytes_allocated.
	 */
	{
		size_t sz = hdr->gc_size;
		size_t prev = atomic_load(&gc_heap_size);
		if (prev >= sz)
			atomic_fetch_sub(&gc_heap_size, sz);
		else
			atomic_store(&gc_heap_size, 0);
	}
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
 * Dead-set collection (STW phase).  Walk the global object list while the
 * world is still stopped and unlink every unmarked, unpinned object into
 * two caller-provided lists (finalizable and plain).  Because the world is
 * stopped, the list is quiescent: no mutator can add an object during this
 * walk, so an object allocated later (after resume) is simply not in the
 * snapshot and can never be swept this cycle.  This is what makes the
 * concurrent free phase sound WITHOUT allocate-black (which would have
 * broken child traversal in gc_mark_callback).
 *
 * Returns the number of objects collected.  The actual finalize+free is
 * done by gc_reclaim_dead() AFTER the world resumes, so finalizers and the
 * allocator free path never run under stop-the-world.
 */
static uint64_t
gc_collect_dead(umem_gc_header_t **finalize_out, umem_gc_header_t **free_out)
{
	uint32_t mark_val = atomic_load(&gc_mark_value);
	uint64_t collected = 0;
	umem_gc_header_t *finalize_list = NULL;
	umem_gc_header_t *free_list = NULL;

	(void) mark_val;
	for (unsigned s = 0; s < GC_NSHARDS; s++) {
		gc_shard_t *sh = &gc_shards[s];
		(void) pthread_mutex_lock(&sh->lock);

		umem_gc_header_t *hdr = sh->sentinel.gc_next;
		while (hdr != &sh->sentinel) {
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
				collected++;
			}

			hdr = next;
		}

		(void) pthread_mutex_unlock(&sh->lock);
	}

	*finalize_out = finalize_list;
	*free_out = free_list;
	return (collected);
}

/*
 * Reclaim the dead set collected by gc_collect_dead().  Runs AFTER the
 * world has resumed: finalizers (user code) and gc_free_object() (which
 * takes allocator locks) must not run under stop-the-world.  The objects
 * are already unlinked from the global list and sparsemap, so they are
 * unreachable to both mutators and any concurrent find_header.
 *
 * Returns the number of bytes freed.
 */
static size_t
gc_reclaim_dead(umem_gc_header_t *finalize_list, umem_gc_header_t *free_list)
{
	size_t bytes_freed = 0;
	uint64_t objects_freed = 0;
	umem_gc_header_t *hdr;

	atomic_store_explicit(&gc_phase, GC_PHASE_FINALIZE,
	    memory_order_release);

	while (finalize_list != NULL) {
		hdr = finalize_list;
		finalize_list = hdr->gc_next;

		gc_run_finalizer(hdr);
		bytes_freed += hdr->gc_size;
		objects_freed++;
		gc_free_object(hdr);
	}

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

	/* Initialize per-shard object lists + locks */
	for (unsigned s = 0; s < GC_NSHARDS; s++) {
		(void) pthread_mutex_init(&gc_shards[s].lock, NULL);
		gc_shards[s].sentinel.gc_next = &gc_shards[s].sentinel;
		gc_shards[s].sentinel.gc_prev = &gc_shards[s].sentinel;
		gc_shards[s].pagemap = NULL;
	}

	/* Create pthread key for automatic thread unregistration */
	if (pthread_key_create(&gc_thread_key,
	    gc_thread_key_destructor) != 0) {
		(void) pthread_mutex_unlock(&gc_init_lock);
		return (-1);
	}

	/* Create a per-shard page-level sparsemap for O(1) pointer lookup */
	for (unsigned s = 0; s < GC_NSHARDS; s++) {
		gc_shards[s].pagemap = umem_sparsemap_create(0);
		if (gc_shards[s].pagemap == NULL) {
			for (unsigned t = 0; t < s; t++) {
				umem_sparsemap_destroy(gc_shards[t].pagemap);
				gc_shards[t].pagemap = NULL;
			}
			(void) pthread_key_delete(gc_thread_key);
			(void) pthread_mutex_unlock(&gc_init_lock);
			return (-1);
		}
	}

	/* Create size-classed GC caches */
	if (gc_create_caches() != 0) {
		for (unsigned s = 0; s < GC_NSHARDS; s++) {
			umem_sparsemap_destroy(gc_shards[s].pagemap);
			gc_shards[s].pagemap = NULL;
		}
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

	/* Cache our slot for O(1) safepoint / suspend-handler lookup. */
	gc_self_slot = umem_gc_thread_slot();

	return (0);
}

void
umem_gc_unregister_thread(void)
{
	umem_gc_thread_t *gct = pthread_getspecific(gc_thread_key);
	if (gct == NULL)
		return;

	(void) umem_gc_thread_unregister();
	gc_self_slot = -1;
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

	/*
	 * Cooperative safepoint.  If a stop-the-world is in progress, park
	 * here -- before touching any allocator or GC-internal lock -- so
	 * this thread's roots are scannable and the collector need not rely
	 * on async signal delivery to suspend us.  This is the mechanism that
	 * makes STW sound under heavy CPU oversubscription: a mutator that
	 * cannot be scheduled to run a signal handler in time still parks the
	 * moment it next allocates, and the collector never sweeps until every
	 * thread is confirmed parked.
	 */
	gc_safepoint();

	if (size == 0)
		size = 1;

	/*
	 * Everything from here until the object is fully linked runs inside a
	 * GC critical section (gcti_in_gc_critical set).  This spans the
	 * underlying umem allocation -- which acquires cache/depot/vmem locks
	 * -- and gc_object_add.  A suspend signal that lands anywhere in this
	 * span therefore DEFERS its park (see gc_suspend_handler) instead of
	 * freezing the thread while it holds an allocator lock.  Parking mid
	 * allocator-lock is exactly what deadlocked the collector under
	 * oversubscription: the parked thread held e.g. the vmem arena lock,
	 * other workers blocked acquiring it could never reach a safepoint to
	 * park, and the collector waited forever for them.  Deferring to the
	 * critical-section exit (all locks released) breaks that cycle.
	 */
	size_t total_size = sizeof (umem_gc_header_t) + size;
	umem_gc_header_t *hdr;
	umem_cache_t *cp = gc_cache_for_size(total_size);
	int slot = gc_critical_enter();

	if (cp != NULL) {
		hdr = umem_cache_alloc(cp, UMEM_DEFAULT);
	} else {
		size_t alloc_sz = gc_alloc_size(total_size);
		hdr = umem_alloc(alloc_sz, UMEM_DEFAULT);
	}

	if (hdr == NULL) {
		/*
		 * Out of memory: leave the critical section (so the retry
		 * collection can stop the world without waiting on us), try
		 * collecting, then re-enter and retry once.
		 */
		gc_critical_exit(slot);
		umem_gc_collect();
		slot = gc_critical_enter();

		if (cp != NULL) {
			hdr = umem_cache_alloc(cp, UMEM_DEFAULT);
		} else {
			size_t alloc_sz = gc_alloc_size(total_size);
			hdr = umem_alloc(alloc_sz, UMEM_DEFAULT);
		}

		if (hdr == NULL) {
			gc_critical_exit(slot);
			return (NULL);
		}
	}

	/*
	 * Initialize the GC header.  gc_mark starts at 0 (unmarked); a live
	 * mark value is only ever written by the collector's mark phase.  We
	 * deliberately do NOT allocate-black (stamp the current mark value):
	 * doing so would make gc_mark_callback treat a freshly reachable
	 * object as "already scanned" and skip traversing its children,
	 * stranding a deep chain unmarked.  Soundness of objects born during
	 * the resume->sweep window is instead guaranteed by snapshotting the
	 * dead set under stop-the-world before resuming (see umem_gc_collect):
	 * an object added after that snapshot is simply not a sweep candidate
	 * this cycle.
	 */
	atomic_store(&hdr->gc_mark, 0);
	hdr->gc_flags = (flags & UMEM_GC_ATOMIC) ? UMEM_GC_ATOMIC : 0;
	hdr->gc_size = size;
	hdr->gc_finalizer = NULL;
	hdr->gc_finalizer_data = NULL;

	/* Zero the user portion (GC_MALLOC semantics) */
	void *user_ptr = UMEM_GC_USER_PTR(hdr);
	memset(user_ptr, 0, size);

	/* Add to global object list (list + sparsemap). */
	gc_object_add_locked(hdr);

	/*
	 * End of the critical section: all allocator/GC locks are released
	 * and the object is fully linked (on the sweep list AND in the
	 * sparsemap).  If a suspend signal deferred a park while we were in
	 * here, park now at this safe boundary.
	 */
	gc_critical_exit(slot);

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
			/*
			 * gc_heap_size was charged old_size at alloc and the
			 * eventual free subtracts hdr->gc_size, so an in-place
			 * shrink must drop the freed bytes now or the counter
			 * drifts (old_size - new_size) high per shrink.
			 */
			size_t drop = old_size - new_size;
			if (drop != 0) {
				size_t prev = atomic_load(&gc_heap_size);
				if (prev >= drop)
					atomic_fetch_sub(&gc_heap_size, drop);
				else
					atomic_store(&gc_heap_size, 0);
			}
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
	int slot;

	/*
	 * The whole free -- list+sparsemap removal, finalizer, and
	 * gc_free_object (which returns memory to the allocator and takes
	 * cache/depot/vmem locks) -- runs inside one GC critical section.
	 * A suspend signal landing anywhere here DEFERS its park to the
	 * critical-section exit (all locks released) instead of freezing the
	 * thread mid allocator-lock, which would deadlock the collector's
	 * sweep.  It also keeps the removal atomic w.r.t. stop-the-world so a
	 * collector never sees the object half-removed (off the sparsemap but
	 * still on the sweep list, or vice versa).  This replaces the old
	 * per-free pthread_sigmask() -- same reasoning, no syscall.
	 */
	slot = gc_critical_enter();

	{
		gc_shard_t *sh = &gc_shards[gc_shard_of_ptr(UMEM_GC_USER_PTR(hdr))];
		(void) pthread_mutex_lock(&sh->lock);
		gc_object_remove_locked(hdr);
		(void) pthread_mutex_unlock(&sh->lock);
	}

	/* Run finalizer if registered */
	gc_run_finalizer(hdr);

	/* Update stats before freeing */
	size_t obj_size = hdr->gc_size;

	/* Free backing memory */
	gc_free_object(hdr);

	gc_critical_exit(slot);

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
	 * Stop the world.  Parking is cooperative at the alloc safepoint, so
	 * every mutator parks either at its next allocation or when the
	 * suspend signal reaches it; the barrier waits (bounded) for ALL of
	 * them.  If it cannot stop the world within the timeout -- only
	 * possible if a mutator is wedged in a genuine allocator lock cycle
	 * and can never reach a safepoint -- gc_stop_the_world() fails and we
	 * SKIP marking and sweeping this cycle rather than proceed against an
	 * incomplete snapshot (unsound).  Bounded and always sound.  On a
	 * build without the STW signal (non-Linux) or with no other threads,
	 * gc_stop_the_world() returns success and the collector scans its own
	 * stack as the sole root source.
	 */
	int stw_ok = 0;
	if (gc_stw_installed)
		stw_ok = (gc_stop_the_world() == 0);
	else
		stw_ok = 1;	/* single-rooted best-effort */

	/* Scan all roots and sweep ONLY under a complete stop-the-world. */
	size_t bytes_freed = 0;
	if (stw_ok) {
		/*
		 * The registry lock is NOT held during marking
		 * (gc_stop_the_world drops it once the park barrier
		 * completes), so the Phase-3 root scan re-acquires it itself:
		 * pass threads_locked = 0.
		 *
		 * Marking reads the page sparsemap locklessly via
		 * umem_gc_find_header().  That is only safe because EVERY
		 * mutator is confirmed parked here: a running mutator's
		 * gc_object_add/remove can resize/free the sparsemap buckets
		 * out from under a concurrent lookup (observed as a SEGV in
		 * sm_lookup).  We therefore never mark unless the world is
		 * fully stopped -- marking a live heap is both unsound and
		 * crash-prone.
		 *
		 * A cooperatively-parked thread never parks while holding an
		 * allocator or GC-internal lock (it polls the safepoint at
		 * alloc entry, before any lock), and a signal-parked thread
		 * defers parking out of the gc_object_add/free critical
		 * section, so marking here cannot block on a lock a parked
		 * thread holds.  The mark path itself does not call
		 * umem_gc_alloc; its work-queue growth uses raw umem_alloc.
		 */
		umem_gc_scan_all_roots(gc_mark_callback, 0);
		gc_drain_mark_queue();

		/* Phase 2: Remark -- rescan for missed references */
		atomic_store_explicit(&gc_phase, GC_PHASE_REMARK,
		    memory_order_release);
		umem_gc_scan_all_roots(gc_mark_callback, 0);
		gc_drain_mark_queue();

		/*
		 * Phase 3a: collect the dead set WHILE STILL STOPPED.  Walking
		 * the object list under stop-the-world guarantees the snapshot
		 * is complete and quiescent: every unmarked object here is
		 * genuinely unreachable (all roots were just scanned), and no
		 * mutator can add an object mid-walk.  An object allocated
		 * after we resume is simply not in this snapshot, so it can
		 * never be swept this cycle -- which is what lets us reclaim
		 * concurrently below without allocate-black.
		 */
		atomic_store_explicit(&gc_phase, GC_PHASE_SWEEP,
		    memory_order_release);
		umem_gc_header_t *finalize_list = NULL;
		umem_gc_header_t *free_list = NULL;
		(void) gc_collect_dead(&finalize_list, &free_list);

		/* Resume the world; marking and the dead snapshot are done. */
		if (gc_stw_installed)
			gc_resume_the_world();

		gc_mark_queue_destroy(&gc_mark_queue);

		/*
		 * Phase 3b: finalize + free the dead set AFTER resume.  The
		 * objects are already unlinked from the list and sparsemap, so
		 * running finalizers (user code) and gc_free_object() (which
		 * takes allocator locks) here -- with the world running -- is
		 * both safe and keeps the stop-the-world pause short.
		 */
		bytes_freed = gc_reclaim_dead(finalize_list, free_list);
	} else {
		/*
		 * Could not stop the world after GC_STW_MAX_RETRIES.  We did
		 * NOT confirm every thread parked, so we must NOT mark (the
		 * lockless sparsemap read would race live mutators) and MUST
		 * NOT sweep (a reachable object rooted only on an unscanned
		 * running stack could be freed).  Skip this cycle entirely:
		 * unreachable garbage simply waits for the next collection.
		 * Bounded and always sound; the cooperative safepoint makes a
		 * sustained run of skips effectively impossible in practice.
		 */
		gc_mark_queue_destroy(&gc_mark_queue);
	}

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

	/* O(1) lookup via the per-page object lists in the ptr's shard pagemap */
	{
		gc_shard_t *sh = &gc_shards[gc_shard_of_ptr(ptr)];
		if (sh->pagemap != NULL)
			return (umem_sparsemap_find_object(sh->pagemap, ptr));
	}

	/* Fallback: O(n) scan across all shards if sparsemaps unavailable */
	{
		umem_gc_header_t *candidate = UMEM_GC_HEADER(ptr);

		for (unsigned s = 0; s < GC_NSHARDS; s++) {
			gc_shard_t *sh = &gc_shards[s];
			(void) pthread_mutex_lock(&sh->lock);
			umem_gc_header_t *hdr = sh->sentinel.gc_next;
			while (hdr != &sh->sentinel) {
				if (hdr == candidate) {
					(void) pthread_mutex_unlock(&sh->lock);
					return (hdr);
				}
				hdr = hdr->gc_next;
			}
			(void) pthread_mutex_unlock(&sh->lock);
		}
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
	for (unsigned s = 0; s < GC_NSHARDS; s++) {
		gc_shard_t *sh = &gc_shards[s];
		(void) pthread_mutex_lock(&sh->lock);
		umem_gc_header_t *hdr = sh->sentinel.gc_next;
		while (hdr != &sh->sentinel) {
			umem_gc_header_t *next = hdr->gc_next;
			fn(hdr, arg);
			hdr = next;
		}
		(void) pthread_mutex_unlock(&sh->lock);
	}
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
	for (unsigned s = 0; s < GC_NSHARDS; s++)
		(void) pthread_mutex_lock(&gc_shards[s].lock);
}

void
umem_gc_unlock_objects(void)
{
	for (unsigned s = GC_NSHARDS; s-- > 0; )
		(void) pthread_mutex_unlock(&gc_shards[s].lock);
}

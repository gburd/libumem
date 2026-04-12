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

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

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
static int gc_initialized;
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
 */
static void
gc_object_add(umem_gc_header_t *hdr)
{
	(void) pthread_mutex_lock(&gc_objects_lock);
	hdr->gc_next = gc_objects_sentinel.gc_next;
	hdr->gc_prev = &gc_objects_sentinel;
	gc_objects_sentinel.gc_next->gc_prev = hdr;
	gc_objects_sentinel.gc_next = hdr;
	(void) pthread_mutex_unlock(&gc_objects_lock);
}

/*
 * Remove a GC object from the global list. Caller must hold gc_objects_lock.
 */
static void
gc_object_remove_locked(umem_gc_header_t *hdr)
{
	hdr->gc_prev->gc_next = hdr->gc_next;
	hdr->gc_next->gc_prev = hdr->gc_prev;
	hdr->gc_next = NULL;
	hdr->gc_prev = NULL;
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
 */
static void
gc_run_finalizer(umem_gc_header_t *hdr)
{
	if ((hdr->gc_flags & UMEM_GC_FINALIZE) && hdr->gc_finalizer != NULL) {
		void *user_ptr = UMEM_GC_USER_PTR(hdr);
		hdr->gc_finalizer(user_ptr, hdr->gc_finalizer_data);
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

		if (hdr->gc_mark != mark_val) {
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
	atomic_store(&gc_phase, GC_PHASE_FINALIZE);

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

	/* Create size-classed GC caches */
	if (gc_create_caches() != 0) {
		(void) pthread_key_delete(gc_thread_key);
		(void) pthread_mutex_unlock(&gc_init_lock);
		return (-1);
	}

	/* Set initial threshold */
	atomic_store(&gc_threshold, GC_MIN_THRESHOLD);

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

	return (0);
}

void
umem_gc_unregister_thread(void)
{
	umem_gc_thread_t *gct = pthread_getspecific(gc_thread_key);
	if (gct == NULL)
		return;

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
	size_t allocated = atomic_load(&gc_bytes_allocated);
	size_t thresh = atomic_load(&gc_threshold);

	if (allocated > thresh)
		umem_gc_collect();
}

void *
umem_gc_alloc(size_t size, int flags)
{
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
	hdr->gc_mark = 0;
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

	/* Allocate new, copy, free old */
	int flags = old_hdr->gc_flags & UMEM_GC_ATOMIC;
	void *new_ptr = umem_gc_alloc(new_size, flags);
	if (new_ptr == NULL)
		return (NULL);

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

	umem_gc_free(ptr);
	return (new_ptr);
}

void
umem_gc_free(void *ptr)
{
	if (ptr == NULL)
		return;

	umem_gc_header_t *hdr = UMEM_GC_HEADER(ptr);

	/* Remove from global object list */
	(void) pthread_mutex_lock(&gc_objects_lock);
	gc_object_remove_locked(hdr);
	(void) pthread_mutex_unlock(&gc_objects_lock);

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

	/* Phase 1: Mark (stub - actual marking done by root scanner) */
	gc_reset_marks();
	atomic_store(&gc_phase, GC_PHASE_MARK);

	/*
	 * The root scanner (Task #2) will call umem_gc_mark_object() on
	 * reachable objects. For now, we proceed directly to remark/sweep.
	 * When the root scanner is integrated, it will be called here:
	 *   umem_gc_scan_roots();
	 */

	/* Phase 2: Remark (STW pause placeholder) */
	atomic_store(&gc_phase, GC_PHASE_REMARK);

	/* Phase 3: Sweep */
	atomic_store(&gc_phase, GC_PHASE_SWEEP);
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
	atomic_store(&gc_phase, GC_PHASE_IDLE);

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

void
umem_gc_get_stats(umem_gc_stats_t *stats)
{
	stats->gcs_heap_size = atomic_load(&gc_heap_size);
	stats->gcs_free_bytes = 0; /* TODO: aggregate from caches */
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
	return (0); /* TODO: aggregate from GC caches */
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
	return (atomic_load(&gc_phase));
}

void
umem_gc_set_phase(umem_gc_phase_t phase)
{
	atomic_store(&gc_phase, phase);
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
 * This performs a linear scan of the global object list. For production
 * use, this should be replaced with a hash table or interval tree for
 * O(1) lookup. The current implementation is correct but O(n).
 */
umem_gc_header_t *
umem_gc_find_header(void *ptr)
{
	if (ptr == NULL)
		return (NULL);

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
	return (NULL);
}

void
umem_gc_mark_object(umem_gc_header_t *hdr)
{
	if (hdr == NULL)
		return;

	uint32_t mark_val = atomic_load(&gc_mark_value);
	hdr->gc_mark = mark_val;
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

/*
 * Application allocator hooks implementation
 *
 * LOCKING / LIFETIME.  See the CONCURRENCY CONTRACT block in umem_hooks.h;
 * this file implements it.  Summary of the mechanism:
 *
 *   hook_list_lock protects the list linkage, hook_active, hook_refcnt,
 *   hook_walk_gen, and every statistics counter.  It is NEVER held while
 *   user code runs (L2).
 *
 *   A hook is "usable" only while hook_active != 0.  Every operation that
 *   will run user code or touch statistics takes a reference first
 *   (hook_hold), runs the user code with the lock dropped, then releases
 *   (hook_rele).  hook_rele broadcasts so a waiting unregister can finish.
 *
 *   umem_hook_unregister() clears hook_active (no new references possible),
 *   unlinks, then waits for hook_refcnt to drain to zero.  When it returns,
 *   no thread is inside the hook and the caller may free it (L1).
 *
 * WHY A REFERENCE COUNT AND NOT JUST THE LOCK.  The user callback can
 * allocate, block, or re-enter this API, so holding hook_list_lock across it
 * would deadlock (that was the previous behaviour of umem_hook_walk).  The
 * reference count decouples "this object is alive" from "the registry is
 * quiescent".
 *
 * ponytail: one global registry lock and one condvar.  Registration is a
 * process-startup event and tracking is a few counter adds; if hook tracking
 * ever becomes hot, give each hook its own lock (ordered below the registry
 * lock) or make the counters atomic.
 */

#include "umem_hooks.h"
#include "umem_impl.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* Global hook list */
static umem_hook_t hook_list_head = {
	.hook_name = "<head>",
	.hook_next = &hook_list_head,
	.hook_prev = &hook_list_head,
	.hook_active = 0
};

static pthread_mutex_t hook_list_lock = PTHREAD_MUTEX_INITIALIZER;
/* Signalled whenever a hook reference is dropped, or a walk completes. */
static pthread_cond_t hook_quiesce_cv = PTHREAD_COND_INITIALIZER;
/* Serializes walks so hook_walk_gen is unambiguous (L4). */
static int hook_walk_busy;
static uint32_t hook_walk_gen_next = 1;

/*
 * Take a reference so the hook cannot be freed under us.  Caller must hold
 * hook_list_lock.  Returns 0 if the hook is not usable (never registered, or
 * unregister already started).
 */
static int
hook_hold(umem_hook_t *hook)
{
	if (hook == NULL || !hook->hook_active)
		return (0);
	/*
	 * hook_refcnt is 16-bit so umem_hook_t's size stays ABI-stable (see the
	 * note in umem_hooks.h).  It counts threads currently inside a callback
	 * for this hook, so it is bounded by the thread count in practice --
	 * but refuse rather than wrap, because a wrap to 0 would let
	 * umem_hook_unregister() free the hook while callers are still in it.
	 * Declining a hold degrades to "this allocation is not tracked", which
	 * is the same outcome as an inactive hook.
	 */
	if (hook->hook_refcnt == UINT16_MAX)
		return (0);
	hook->hook_refcnt++;
	return (1);
}

/* Drop a reference.  Caller must hold hook_list_lock. */
static void
hook_rele(umem_hook_t *hook)
{
	if (--hook->hook_refcnt == 0)
		(void) pthread_cond_broadcast(&hook_quiesce_cv);
}

/* umem_hook_find() body, with the lock already held. */
static umem_hook_t *
hook_find_locked(const char *name)
{
	umem_hook_t *hook = hook_list_head.hook_next;

	while (hook != &hook_list_head) {
		if (strcmp(hook->hook_name, name) == 0)
			return (hook);
		hook = hook->hook_next;
	}

	return (NULL);
}

/*
 * Register an application allocator hook
 *
 * Returns 0 on success, -1 on error.
 */
int
umem_hook_register(umem_hook_t *hook)
{
	if (hook == NULL || hook->hook_name == NULL) {
		return (-1);
	}

	if (hook->hook_alloc == NULL && hook->hook_free == NULL) {
		return (-1);
	}

	(void) pthread_mutex_lock(&hook_list_lock);

	/* Check if already registered */
	if (hook_find_locked(hook->hook_name) != NULL) {
		(void) pthread_mutex_unlock(&hook_list_lock);
		return (-1);
	}

	/* Initialize statistics */
	hook->alloc_count = 0;
	hook->free_count = 0;
	hook->realloc_count = 0;
	hook->bytes_allocated = 0;
	hook->bytes_freed = 0;
	hook->bytes_current = 0;
	hook->peak_bytes = 0;
	hook->hook_refcnt = 0;
	hook->hook_walk_gen = 0;
	hook->hook_active = 1;

	/* Add to list */
	hook->hook_next = hook_list_head.hook_next;
	hook->hook_prev = &hook_list_head;
	hook_list_head.hook_next->hook_prev = hook;
	hook_list_head.hook_next = hook;

	(void) pthread_mutex_unlock(&hook_list_lock);

	return (0);
}

/*
 * Unregister an application allocator hook.
 *
 * Contract L1: does not return while any thread can still be inside the
 * hook, so the caller may free the structure immediately afterwards.
 */
void
umem_hook_unregister(umem_hook_t *hook)
{
	if (hook == NULL) {
		return;
	}

	(void) pthread_mutex_lock(&hook_list_lock);

	if (!hook->hook_active) {
		/* Never registered, or a concurrent unregister already won.
		 * Still wait out any in-flight callers before returning, so
		 * the "safe to free on return" guarantee holds for both
		 * callers of a racing double unregister. */
		while (hook->hook_refcnt > 0)
			(void) pthread_cond_wait(&hook_quiesce_cv,
			    &hook_list_lock);
		(void) pthread_mutex_unlock(&hook_list_lock);
		return;
	}

	/*
	 * Close the door first: hook_hold() fails from here on, so the
	 * reference count can only fall.
	 */
	hook->hook_active = 0;

	/* Remove from list */
	hook->hook_prev->hook_next = hook->hook_next;
	hook->hook_next->hook_prev = hook->hook_prev;
	hook->hook_next = hook->hook_prev = NULL;

	/* Wait for in-flight callbacks and statistics updates to finish. */
	while (hook->hook_refcnt > 0)
		(void) pthread_cond_wait(&hook_quiesce_cv, &hook_list_lock);

	(void) pthread_mutex_unlock(&hook_list_lock);
}

/*
 * Track an allocation through a hook
 *
 * Applications call this after their allocator returns a pointer.
 */
void *
umem_hook_track_alloc(umem_hook_t *hook, size_t size)
{
	void *ptr = NULL;

	if (hook == NULL) {
		return (NULL);
	}

	(void) pthread_mutex_lock(&hook_list_lock);
	if (!hook_hold(hook)) {
		(void) pthread_mutex_unlock(&hook_list_lock);
		return (NULL);
	}
	(void) pthread_mutex_unlock(&hook_list_lock);

	if (hook->hook_alloc != NULL) {
		ptr = hook->hook_alloc(size, hook->hook_arg);
	}

	(void) pthread_mutex_lock(&hook_list_lock);
	if (ptr != NULL) {
		hook->alloc_count++;
		hook->bytes_allocated += size;
		hook->bytes_current += size;

		if (hook->bytes_current > hook->peak_bytes) {
			hook->peak_bytes = hook->bytes_current;
		}
	}
	hook_rele(hook);
	(void) pthread_mutex_unlock(&hook_list_lock);

	return (ptr);
}

/*
 * Track a free through a hook
 */
void
umem_hook_track_free(umem_hook_t *hook, void *ptr, size_t size)
{
	if (hook == NULL || ptr == NULL) {
		return;
	}

	(void) pthread_mutex_lock(&hook_list_lock);
	if (!hook_hold(hook)) {
		(void) pthread_mutex_unlock(&hook_list_lock);
		return;
	}
	(void) pthread_mutex_unlock(&hook_list_lock);

	if (hook->hook_free != NULL) {
		hook->hook_free(ptr, hook->hook_arg);
	}

	(void) pthread_mutex_lock(&hook_list_lock);
	hook->free_count++;
	hook->bytes_freed += size;
	if (hook->bytes_current >= size) {
		hook->bytes_current -= size;
	}
	hook_rele(hook);
	(void) pthread_mutex_unlock(&hook_list_lock);
}

/*
 * Track a realloc through a hook
 */
void *
umem_hook_track_realloc(umem_hook_t *hook, void *ptr,
    size_t old_size, size_t new_size)
{
	void *new_ptr = NULL;

	if (hook == NULL) {
		return (NULL);
	}

	(void) pthread_mutex_lock(&hook_list_lock);
	if (!hook_hold(hook)) {
		(void) pthread_mutex_unlock(&hook_list_lock);
		return (NULL);
	}
	(void) pthread_mutex_unlock(&hook_list_lock);

	if (hook->hook_realloc != NULL) {
		new_ptr = hook->hook_realloc(ptr, new_size, hook->hook_arg);
	} else {
		/* Emulate realloc with alloc + free */
		if (hook->hook_alloc != NULL && hook->hook_free != NULL) {
			new_ptr = hook->hook_alloc(new_size, hook->hook_arg);
			if (new_ptr != NULL && ptr != NULL) {
				size_t copy_size = old_size < new_size ? old_size : new_size;
				(void) memcpy(new_ptr, ptr, copy_size);
				hook->hook_free(ptr, hook->hook_arg);
			}
		}
	}

	(void) pthread_mutex_lock(&hook_list_lock);
	if (new_ptr != NULL) {
		hook->realloc_count++;

		/* Update byte counters */
		if (hook->bytes_current >= old_size) {
			hook->bytes_current -= old_size;
		}
		hook->bytes_freed += old_size;

		hook->bytes_current += new_size;
		hook->bytes_allocated += new_size;

		if (hook->bytes_current > hook->peak_bytes) {
			hook->peak_bytes = hook->bytes_current;
		}
	}
	hook_rele(hook);
	(void) pthread_mutex_unlock(&hook_list_lock);

	return (new_ptr);
}

/*
 * Dump statistics for one hook
 */
void
umem_hook_dump_one(FILE *fp, umem_hook_t *hook)
{
	if (fp == NULL || hook == NULL) {
		return;
	}

	(void) fprintf(fp, "Hook: %s\n", hook->hook_name);
	(void) fprintf(fp, "  Allocations:     %llu\n",
	    (unsigned long long)hook->alloc_count);
	(void) fprintf(fp, "  Frees:           %llu\n",
	    (unsigned long long)hook->free_count);
	(void) fprintf(fp, "  Reallocs:        %llu\n",
	    (unsigned long long)hook->realloc_count);
	(void) fprintf(fp, "  Bytes allocated: %llu\n",
	    (unsigned long long)hook->bytes_allocated);
	(void) fprintf(fp, "  Bytes freed:     %llu\n",
	    (unsigned long long)hook->bytes_freed);
	(void) fprintf(fp, "  Current bytes:   %llu\n",
	    (unsigned long long)hook->bytes_current);
	(void) fprintf(fp, "  Peak bytes:      %llu\n",
	    (unsigned long long)hook->peak_bytes);
}

struct hook_dump_arg {
	FILE *fp;
	int count;
};

static int
hook_dump_walker(umem_hook_t *hook, void *arg)
{
	struct hook_dump_arg *da = arg;
	umem_hook_dump_one(da->fp, hook);
	(void) fprintf(da->fp, "\n");
	da->count++;
	return (0);
}

/*
 * Dump statistics for all hooks
 *
 * Uses umem_hook_walk() so stdio runs without the registry lock held (L2):
 * fprintf can allocate, and under malloc interposition that allocation can
 * re-enter this API.
 */
void
umem_hook_dump(FILE *fp)
{
	struct hook_dump_arg da;

	if (fp == NULL) {
		fp = stderr;
	}
	da.fp = fp;
	da.count = 0;

	(void) fprintf(fp, "Application Allocator Hooks\n");
	(void) fprintf(fp, "===========================\n\n");

	(void) umem_hook_walk(hook_dump_walker, &da);

	if (da.count == 0) {
		(void) fprintf(fp, "No hooks registered\n");
	} else {
		(void) fprintf(fp, "Total: %d hook(s)\n", da.count);
	}
}

/*
 * Find a hook by name.
 *
 * Contract L5: returns a bare pointer with no reference taken.
 */
umem_hook_t *
umem_hook_find(const char *name)
{
	umem_hook_t *hook;

	if (name == NULL) {
		return (NULL);
	}

	(void) pthread_mutex_lock(&hook_list_lock);
	hook = hook_find_locked(name);
	(void) pthread_mutex_unlock(&hook_list_lock);

	return (hook);
}

/*
 * Walk all hooks, calling func(hook, arg) with NO lock held (L2/L4).
 *
 * Restart-safe iteration: each pass takes the lock, finds the first hook not
 * yet visited in this walk generation, references it, drops the lock, calls
 * the callback, then releases.  Because the reference keeps the hook linked-in
 * data valid and hook_walk_gen records progress, a concurrent
 * register/unregister cannot make the walk revisit or skip a hook that was
 * present throughout.
 *
 * Returns 0 if every callback returned 0, -1 if func is NULL or a callback
 * returned nonzero (i.e. the walk stopped early).
 */
int
umem_hook_walk(umem_hook_walk_f func, void *arg)
{
	uint32_t gen;
	int stopped = 0;

	if (func == NULL)
		return (-1);

	(void) pthread_mutex_lock(&hook_list_lock);
	/* One walk at a time: hook_walk_gen is a single field per hook. */
	while (hook_walk_busy)
		(void) pthread_cond_wait(&hook_quiesce_cv, &hook_list_lock);
	hook_walk_busy = 1;
	gen = hook_walk_gen_next++;

	for (;;) {
		umem_hook_t *hook = hook_list_head.hook_next;

		while (hook != &hook_list_head && hook->hook_walk_gen == gen)
			hook = hook->hook_next;
		if (hook == &hook_list_head)
			break;

		hook->hook_walk_gen = gen;
		if (!hook_hold(hook))
			continue;	/* being unregistered; skip it */
		(void) pthread_mutex_unlock(&hook_list_lock);

		int ret = func(hook, arg);

		(void) pthread_mutex_lock(&hook_list_lock);
		hook_rele(hook);
		if (ret != 0) {
			stopped = 1;
			break;
		}
	}

	hook_walk_busy = 0;
	(void) pthread_cond_broadcast(&hook_quiesce_cv);
	(void) pthread_mutex_unlock(&hook_list_lock);

	return (stopped ? -1 : 0);
}

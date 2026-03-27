/*
 * Application allocator hooks implementation
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
	umem_hook_t *existing = umem_hook_find(hook->hook_name);
	if (existing != NULL) {
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
 * Unregister an application allocator hook
 */
void
umem_hook_unregister(umem_hook_t *hook)
{
	if (hook == NULL || !hook->hook_active) {
		return;
	}

	(void) pthread_mutex_lock(&hook_list_lock);

	/* Remove from list */
	hook->hook_prev->hook_next = hook->hook_next;
	hook->hook_next->hook_prev = hook->hook_prev;

	hook->hook_active = 0;

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
	if (hook == NULL || !hook->hook_active) {
		return (NULL);
	}

	void *ptr = NULL;

	if (hook->hook_alloc != NULL) {
		ptr = hook->hook_alloc(size, hook->hook_arg);
	}

	if (ptr != NULL) {
		(void) pthread_mutex_lock(&hook_list_lock);

		hook->alloc_count++;
		hook->bytes_allocated += size;
		hook->bytes_current += size;

		if (hook->bytes_current > hook->peak_bytes) {
			hook->peak_bytes = hook->bytes_current;
		}

		(void) pthread_mutex_unlock(&hook_list_lock);
	}

	return (ptr);
}

/*
 * Track a free through a hook
 */
void
umem_hook_track_free(umem_hook_t *hook, void *ptr, size_t size)
{
	if (hook == NULL || !hook->hook_active || ptr == NULL) {
		return;
	}

	if (hook->hook_free != NULL) {
		hook->hook_free(ptr, hook->hook_arg);
	}

	(void) pthread_mutex_lock(&hook_list_lock);

	hook->free_count++;
	hook->bytes_freed += size;
	if (hook->bytes_current >= size) {
		hook->bytes_current -= size;
	}

	(void) pthread_mutex_unlock(&hook_list_lock);
}

/*
 * Track a realloc through a hook
 */
void *
umem_hook_track_realloc(umem_hook_t *hook, void *ptr,
    size_t old_size, size_t new_size)
{
	if (hook == NULL || !hook->hook_active) {
		return (NULL);
	}

	void *new_ptr = NULL;

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

	if (new_ptr != NULL) {
		(void) pthread_mutex_lock(&hook_list_lock);

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

		(void) pthread_mutex_unlock(&hook_list_lock);
	}

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

/*
 * Dump statistics for all hooks
 */
void
umem_hook_dump(FILE *fp)
{
	if (fp == NULL) {
		fp = stderr;
	}

	(void) fprintf(fp, "Application Allocator Hooks\n");
	(void) fprintf(fp, "===========================\n\n");

	(void) pthread_mutex_lock(&hook_list_lock);

	umem_hook_t *hook = hook_list_head.hook_next;
	int count = 0;

	while (hook != &hook_list_head) {
		umem_hook_dump_one(fp, hook);
		(void) fprintf(fp, "\n");
		hook = hook->hook_next;
		count++;
	}

	(void) pthread_mutex_unlock(&hook_list_lock);

	if (count == 0) {
		(void) fprintf(fp, "No hooks registered\n");
	} else {
		(void) fprintf(fp, "Total: %d hook(s)\n", count);
	}
}

/*
 * Find a hook by name
 *
 * Note: Caller should hold hook_list_lock
 */
umem_hook_t *
umem_hook_find(const char *name)
{
	if (name == NULL) {
		return (NULL);
	}

	umem_hook_t *hook = hook_list_head.hook_next;

	while (hook != &hook_list_head) {
		if (strcmp(hook->hook_name, name) == 0) {
			return (hook);
		}
		hook = hook->hook_next;
	}

	return (NULL);
}

/*
 * Walk all hooks
 *
 * Calls func(hook, arg) for each registered hook.
 * Returns 0 if all callbacks returned 0, else -1.
 */
int
umem_hook_walk(umem_hook_walk_f func, void *arg)
{
	int result = 0;

	if (func == NULL) {
		return (-1);
	}

	(void) pthread_mutex_lock(&hook_list_lock);

	umem_hook_t *hook = hook_list_head.hook_next;

	while (hook != &hook_list_head) {
		int ret = func(hook, arg);
		if (ret != 0) {
			result = -1;
		}
		hook = hook->hook_next;
	}

	(void) pthread_mutex_unlock(&hook_list_lock);

	return (result);
}

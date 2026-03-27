/*
 * Example: PostgreSQL palloc integration with umem hooks
 *
 * This demonstrates how to integrate PostgreSQL's memory context system
 * with umem for allocation tracking and debugging.
 *
 * Compile: gcc -o palloc_example palloc_integration.c -lumem
 */

#include <umem_hooks.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Simplified PostgreSQL memory context (for demonstration)
 */
typedef struct MemoryContext {
	const char *name;
	size_t total_allocated;
	size_t current_allocated;
	int active;
} MemoryContext;

static MemoryContext TopMemoryContext = {
	.name = "TopMemoryContext",
	.total_allocated = 0,
	.current_allocated = 0,
	.active = 1
};

/*
 * Simplified palloc implementation
 */
static void *
palloc_impl(size_t size, void *arg)
{
	MemoryContext *context = (MemoryContext *)arg;

	if (!context->active) {
		return NULL;
	}

	void *ptr = malloc(size);
	if (ptr) {
		context->total_allocated += size;
		context->current_allocated += size;
	}

	return ptr;
}

/*
 * Simplified pfree implementation
 */
static void
pfree_impl(void *ptr, void *arg)
{
	MemoryContext *context = (MemoryContext *)arg;

	if (ptr) {
		/* In real PostgreSQL, we'd look up the allocation size */
		/* For this example, we'll just free it */
		free(ptr);

		/* Note: Real implementation would track size */
	}

	(void)context;
}

/*
 * Simplified repalloc implementation
 */
static void *
repalloc_impl(void *ptr, size_t size, void *arg)
{
	MemoryContext *context = (MemoryContext *)arg;

	if (!context->active) {
		return NULL;
	}

	void *new_ptr = realloc(ptr, size);

	/* In real implementation, we'd track old and new sizes */
	return new_ptr;
}

/*
 * umem hook for palloc
 */
static umem_hook_t palloc_hook = {
	.hook_name = "palloc",
	.hook_alloc = palloc_impl,
	.hook_free = pfree_impl,
	.hook_realloc = repalloc_impl,
	.hook_arg = &TopMemoryContext
};

/*
 * Public API wrappers that use umem tracking
 */
void *
palloc(size_t size)
{
	return umem_hook_track_alloc(&palloc_hook, size);
}

void
pfree(void *ptr)
{
	/* In real implementation, we'd track the size */
	umem_hook_track_free(&palloc_hook, ptr, 0);
}

void *
repalloc(void *ptr, size_t size)
{
	/* In real implementation, we'd know old_size */
	return umem_hook_track_realloc(&palloc_hook, ptr, 0, size);
}

/*
 * Example usage
 */
int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	printf("PostgreSQL palloc integration with umem hooks\n");
	printf("==============================================\n\n");

	/* Register the hook */
	if (umem_hook_register(&palloc_hook) != 0) {
		fprintf(stderr, "Failed to register palloc hook\n");
		return 1;
	}

	printf("palloc hook registered\n\n");

	/* Simulate some PostgreSQL allocations */
	printf("Allocating memory with palloc...\n");

	void *ptr1 = palloc(64);
	printf("  palloc(64) = %p\n", ptr1);

	void *ptr2 = palloc(128);
	printf("  palloc(128) = %p\n", ptr2);

	void *ptr3 = palloc(256);
	printf("  palloc(256) = %p\n", ptr3);

	/* Show statistics */
	printf("\nStatistics after allocations:\n");
	umem_hook_dump_one(stdout, &palloc_hook);

	/* Free some memory */
	printf("\nFreeing memory...\n");
	pfree(ptr1);
	pfree(ptr2);

	/* Realloc */
	printf("\nReallocating ptr3 from 256 to 512 bytes...\n");
	ptr3 = repalloc(ptr3, 512);
	printf("  repalloc = %p\n", ptr3);

	/* Show final statistics */
	printf("\nFinal statistics:\n");
	umem_hook_dump_one(stdout, &palloc_hook);

	/* Cleanup */
	pfree(ptr3);

	/* Dump all hooks */
	printf("\n");
	umem_hook_dump(stdout);

	/* Unregister */
	umem_hook_unregister(&palloc_hook);
	printf("\npalloc hook unregistered\n");

	return 0;
}

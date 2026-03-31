/*
 * Test program for LLDB debugger extension
 *
 * This program creates various allocations to test the LLDB commands:
 * - umem-cache-list
 * - umem-whatis
 * - umem-bufinfo
 * - umem-leak-detect
 * - umem-stats
 *
 * Build with:
 *   clang -g -O0 -o test_lldb_extension test_lldb_extension.c -L../.. -lumem -I../..
 *
 * Run with LLDB:
 *   LD_LIBRARY_PATH=../.. lldb ./test_lldb_extension
 *
 * In LLDB:
 *   (lldb) command script import ../../tools/lldb/umem_lldb.py
 *   (lldb) b main
 *   (lldb) run
 *   (lldb) b test_allocations  # Set breakpoint after allocations
 *   (lldb) continue
 *   (lldb) umem-cache-list
 *   (lldb) umem-whatis p1
 *   (lldb) umem-bufinfo p1
 *   (lldb) umem-stats
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../umem.h"

/* Global pointers so they're visible in debugger */
umem_cache_t *test_cache_small = NULL;
umem_cache_t *test_cache_medium = NULL;
umem_cache_t *test_cache_large = NULL;

void *p1 = NULL;
void *p2 = NULL;
void *p3 = NULL;
void *p4 = NULL;
void *p5 = NULL;
void *p6 = NULL;

void test_allocations(void)
{
	printf("Creating test caches...\n");

	/* Create caches of various sizes */
	test_cache_small = umem_cache_create(
	    "test_small", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);

	test_cache_medium = umem_cache_create(
	    "test_medium", 256, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);

	test_cache_large = umem_cache_create(
	    "test_large", 1024, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);

	printf("Caches created:\n");
	printf("  test_small  (64 bytes):   %p\n", (void *)test_cache_small);
	printf("  test_medium (256 bytes):  %p\n", (void *)test_cache_medium);
	printf("  test_large  (1024 bytes): %p\n", (void *)test_cache_large);
	printf("\n");

	/* Allocate from small cache */
	printf("Allocating from small cache...\n");
	p1 = umem_cache_alloc(test_cache_small, UMEM_DEFAULT);
	p2 = umem_cache_alloc(test_cache_small, UMEM_DEFAULT);
	printf("  p1 = %p\n", p1);
	printf("  p2 = %p\n", p2);

	/* Allocate from medium cache */
	printf("Allocating from medium cache...\n");
	p3 = umem_cache_alloc(test_cache_medium, UMEM_DEFAULT);
	p4 = umem_cache_alloc(test_cache_medium, UMEM_DEFAULT);
	printf("  p3 = %p\n", p3);
	printf("  p4 = %p\n", p4);

	/* Allocate from large cache */
	printf("Allocating from large cache...\n");
	p5 = umem_cache_alloc(test_cache_large, UMEM_DEFAULT);
	p6 = umem_cache_alloc(test_cache_large, UMEM_DEFAULT);
	printf("  p5 = %p\n", p5);
	printf("  p6 = %p\n", p6);

	/* Write some data to verify allocations */
	memset(p1, 0xAA, 64);
	memset(p2, 0xBB, 64);
	memset(p3, 0xCC, 256);
	memset(p4, 0xDD, 256);
	memset(p5, 0xEE, 1024);
	memset(p6, 0xFF, 1024);

	printf("\n");
	printf("Allocations complete.\n");
	printf("Set breakpoint here and use LLDB commands:\n");
	printf("  (lldb) umem-cache-list\n");
	printf("  (lldb) umem-whatis p1\n");
	printf("  (lldb) umem-bufinfo p1\n");
	printf("  (lldb) umem-stats\n");
	printf("\n");

	/* Breakpoint location for testing */
	printf("Press Enter to continue and free memory...\n");
	getchar();
}

void test_cleanup(void)
{
	printf("Freeing allocations...\n");

	/* Free some but not all allocations (to test leak detection) */
	umem_cache_free(test_cache_small, p1);
	umem_cache_free(test_cache_medium, p3);
	umem_cache_free(test_cache_large, p5);

	printf("Freed p1, p3, p5\n");
	printf("Leaked p2, p4, p6 (intentional for testing)\n");
	printf("\n");

	printf("Set breakpoint here and use:\n");
	printf("  (lldb) umem-leak-detect\n");
	printf("\n");

	/* Keep caches alive for inspection */
	printf("Press Enter to destroy caches...\n");
	getchar();

	/* Note: Not freeing p2, p4, p6 intentionally to test leak detection */
	/* In production code, this would be a memory leak */
}

int main(int argc, char **argv)
{
	printf("libumem LLDB Extension Test\n");
	printf("============================\n\n");

	if (getenv("UMEM_DEBUG")) {
		printf("UMEM_DEBUG=%s\n", getenv("UMEM_DEBUG"));
	} else {
		printf("UMEM_DEBUG not set. Set UMEM_DEBUG=audit for full features.\n");
	}
	printf("\n");

	test_allocations();
	test_cleanup();

	printf("Destroying caches...\n");
	umem_cache_destroy(test_cache_small);
	umem_cache_destroy(test_cache_medium);
	umem_cache_destroy(test_cache_large);

	printf("Test complete.\n");
	return 0;
}

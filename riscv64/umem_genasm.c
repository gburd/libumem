/*
 * RISC-V (rv64gc) genasm implementation for Per-Thread Cache (PTC)
 *
 * This file generates assembly code for fast-path allocation and deallocation
 * using RISC-V's register conventions and thread-local storage.
 */

#include "../config.h"
#include <umem_impl.h>
#include <stdio.h>

/*
 * RISC-V rv64gc register conventions (calling convention)
 *
 * Arguments: a0-a7 (x10-x17)
 *   a0 (x10) = size parameter for malloc
 *   a1 (x11) = ptr parameter for free
 *
 * Return value: a0 (x10)
 *
 * Temporaries: t0-t6 (x5-x7, x28-x31)
 *
 * Thread pointer: tp (x4)
 *   Used for thread-local storage access
 *
 * Callee-saved: s0-s11 (x8-x9, x18-x27)
 */

#define REG_SIZE      "a0"    /* Size argument / return value */
#define REG_PTR       "a1"    /* Pointer argument for free */
#define REG_TMEM      "t0"    /* tmem pointer */
#define REG_CACHE     "t1"    /* Cache pointer */
#define REG_ROUNDS    "t2"    /* Rounds/index */
#define REG_TEMP      "t3"    /* Temporary */
#define REG_TP        "tp"    /* Thread pointer (x4) */

/*
 * PTC is supported on RISC-V
 */
int umem_genasm_supported = 1;

/*
 * Generate assembly for malloc fast path
 *
 * Fast path:
 * 1. Get tmem pointer from thread-local storage
 * 2. Index into tmem->tm_roots array for this size class
 * 3. Check if cache has available buffers (rounds > 0)
 * 4. If yes, decrement rounds and return buffer
 * 5. If no, fall back to umem_cache_alloc
 */
static void
genasm_malloc(umem_cache_t *cp, int max_size, int cache_flags)
{
	const char *cache_name = cp->cache_name;
	int cache_idx = cp->cache_bufsize / UMEM_ALIGN;

	printf("\n\t.globl\tumem_genasm_malloc_%d\n", cache_idx);
	printf("\t.type\tumem_genasm_malloc_%d, @function\n", cache_idx);
	printf("umem_genasm_malloc_%d:\n", cache_idx);

	/*
	 * Get tmem base from thread pointer
	 * tmem is at a fixed offset from thread pointer
	 */
	printf("\t# Get tmem pointer\n");
	printf("\tld\t%s, %%tprel_add(_tmem)(%s)\n", REG_TMEM, REG_TP);

	/*
	 * Calculate offset into tmem->tm_roots array
	 * offset = cache_idx * sizeof(tmem_t.tm_roots[0])
	 */
	printf("\t# Get cache for size class %d\n", cache_idx);
	printf("\tli\t%s, %d\n", REG_TEMP, cache_idx);
	printf("\tslli\t%s, %s, 3\n", REG_TEMP, REG_TEMP);  /* Multiply by 8 (sizeof ptr) */
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_TMEM, REG_TEMP);

	/*
	 * Check if cache has available buffers
	 * if (tmem->tm_roots[cache_idx].tr_rounds > 0)
	 */
	printf("\t# Check rounds\n");
	printf("\tlw\t%s, 0(%s)\n", REG_ROUNDS, REG_CACHE);  /* Load rounds */
	printf("\tble\t%s, zero, .Lslow_path_%d\n", REG_ROUNDS, cache_idx);

	/*
	 * Fast path: pop buffer from cache
	 * rounds--
	 * return tmem->tm_roots[cache_idx].tr_objects[rounds]
	 */
	printf("\t# Fast path: pop buffer\n");
	printf("\taddi\t%s, %s, -1\n", REG_ROUNDS, REG_ROUNDS);
	printf("\tsw\t%s, 0(%s)\n", REG_ROUNDS, REG_CACHE);  /* Store new rounds */

	/* Get buffer address from tr_objects array */
	printf("\tslli\t%s, %s, 3\n", REG_TEMP, REG_ROUNDS);  /* rounds * 8 */
	printf("\taddi\t%s, %s, 8\n", REG_CACHE, REG_CACHE);  /* Skip rounds field */
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_CACHE, REG_TEMP);
	printf("\tld\t%s, 0(%s)\n", REG_SIZE, REG_CACHE);  /* Load buffer pointer */

	printf("\tret\n");

	/*
	 * Slow path: call umem_cache_alloc
	 */
	printf(".Lslow_path_%d:\n", cache_idx);
	printf("\t# Slow path: call umem_cache_alloc\n");
	printf("\lla\t%s, umem_alloc_sizes + %d\n", REG_SIZE, cache_idx * 8);
	printf("\tld\t%s, 0(%s)\n", REG_SIZE, REG_SIZE);  /* Load cache pointer */
	printf("\tli\ta1, %d\n", cache_flags);  /* flags argument */
	printf("\ttail\tumem_cache_alloc\n");

	printf("\t.size\tumem_genasm_malloc_%d, .-umem_genasm_malloc_%d\n\n",
	    cache_idx, cache_idx);

	(void)cache_name;
	(void)max_size;
}

/*
 * Generate assembly for free fast path
 *
 * Fast path:
 * 1. Get tmem pointer
 * 2. Check if cache has space (rounds < MAX_ROUNDS)
 * 3. If yes, push buffer and increment rounds
 * 4. If no, fall back to umem_cache_free
 */
static void
genasm_free(umem_cache_t *cp, int max_size)
{
	int cache_idx = cp->cache_bufsize / UMEM_ALIGN;

	printf("\n\t.globl\tumem_genasm_free_%d\n", cache_idx);
	printf("\t.type\tumem_genasm_free_%d, @function\n", cache_idx);
	printf("umem_genasm_free_%d:\n", cache_idx);

	/* Get tmem pointer */
	printf("\t# Get tmem pointer\n");
	printf("\tld\t%s, %%tprel_add(_tmem)(%s)\n", REG_TMEM, REG_TP);

	/* Get cache for this size class */
	printf("\t# Get cache for size class %d\n", cache_idx);
	printf("\tli\t%s, %d\n", REG_TEMP, cache_idx);
	printf("\tslli\t%s, %s, 3\n", REG_TEMP, REG_TEMP);
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_TMEM, REG_TEMP);

	/* Check if cache has space */
	printf("\t# Check if cache has space\n");
	printf("\tlw\t%s, 0(%s)\n", REG_ROUNDS, REG_CACHE);
	printf("\tli\t%s, %d\n", REG_TEMP, UMEM_GENASM_MAGAZINE_SIZE);
	printf("\tbge\t%s, %s, .Lslow_free_%d\n", REG_ROUNDS, REG_TEMP, cache_idx);

	/* Fast path: push buffer to cache */
	printf("\t# Fast path: push buffer\n");
	printf("\tslli\t%s, %s, 3\n", REG_TEMP, REG_ROUNDS);
	printf("\taddi\t%s, %s, 8\n", REG_CACHE, REG_CACHE);
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_CACHE, REG_TEMP);
	printf("\tsd\t%s, 0(%s)\n", REG_PTR, REG_CACHE);

	/* Increment rounds */
	printf("\taddi\t%s, %s, 1\n", REG_ROUNDS, REG_ROUNDS);
	printf("\tsubi\t%s, %s, 8\n", REG_CACHE, REG_CACHE);
	printf("\tsw\t%s, 0(%s)\n", REG_ROUNDS, REG_CACHE);

	printf("\tret\n");

	/* Slow path */
	printf(".Lslow_free_%d:\n", cache_idx);
	printf("\t# Slow path: call umem_cache_free\n");
	printf("\lla\t%s, umem_alloc_sizes + %d\n", REG_SIZE, cache_idx * 8);
	printf("\tld\t%s, 0(%s)\n", REG_SIZE, REG_SIZE);
	printf("\tmv\ta1, %s\n", REG_PTR);
	printf("\tmv\ta0, %s\n", REG_SIZE);
	printf("\ttail\tumem_cache_free\n");

	printf("\t.size\tumem_genasm_free_%d, .-umem_genasm_free_%d\n\n",
	    cache_idx, cache_idx);

	(void)max_size;
}

/*
 * Main entry point: generate malloc functions for all size classes
 */
void
umem_genasm_malloc(umem_cache_t **caches, int ncaches, int cache_flags)
{
	printf("\t.text\n");
	printf("\t.align\t2\n");

	for (int i = 0; i < ncaches; i++) {
		genasm_malloc(caches[i], umem_genasm_minfragsize, cache_flags);
	}
}

/*
 * Generate free functions for all size classes
 */
void
umem_genasm_free(umem_cache_t **caches, int ncaches)
{
	printf("\t.text\n");
	printf("\t.align\t2\n");

	for (int i = 0; i < ncaches; i++) {
		genasm_free(caches[i], umem_genasm_minfragsize);
	}
}

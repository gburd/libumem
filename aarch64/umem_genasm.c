/*
 * aarch64 (ARM64) genasm implementation for Per-Thread Cache (PTC)
 *
 * This file generates assembly code for fast-path allocation and deallocation
 * using ARM64's register conventions and thread-local storage.
 */

#include "../config.h"
#include <umem_impl.h>
#include <stdio.h>

/*
 * ARM64 (aarch64) register conventions (AAPCS64)
 *
 * Arguments: x0-x7
 *   x0 = size parameter for malloc / return value
 *   x1 = ptr parameter for free
 *
 * Return value: x0
 *
 * Temporaries: x9-x15
 *
 * Thread pointer: TPIDR_EL0 (varies by OS)
 *   Linux: __thread variables accessed via compiler
 *   macOS: Different mechanism
 *
 * Callee-saved: x19-x28, x29 (FP), x30 (LR)
 */

#define REG_SIZE      "x0"    /* Size argument / return value */
#define REG_PTR       "x1"    /* Pointer argument for free */
#define REG_TMEM      "x9"    /* tmem pointer */
#define REG_CACHE     "x10"   /* Cache pointer */
#define REG_ROUNDS    "w11"   /* Rounds (32-bit) */
#define REG_TEMP      "x12"   /* Temporary */
#define REG_TEMP2     "x13"   /* Temporary 2 */

/*
 * PTC is supported on aarch64
 */
int umem_genasm_supported = 1;

/*
 * Generate assembly for malloc fast path
 */
static void
genasm_malloc(umem_cache_t *cp, int max_size, int cache_flags)
{
	const char *cache_name = cp->cache_name;
	int cache_idx = cp->cache_bufsize / UMEM_ALIGN;

	printf("\n\t.globl\tumem_genasm_malloc_%d\n", cache_idx);
	printf("\t.type\tumem_genasm_malloc_%d, %%function\n", cache_idx);
	printf("\t.align\t2\n");
	printf("umem_genasm_malloc_%d:\n", cache_idx);

	/*
	 * Get tmem pointer from thread-local storage
	 * On Linux, this is typically done via:
	 *   adrp + ldr with :gottprel: relocation
	 */
	printf("\t// Get tmem pointer\n");
	printf("\tadrp\t%s, :gottprel:_tmem\n", REG_TMEM);
	printf("\tldr\t%s, [%s, #:gottprel_lo12:_tmem]\n", REG_TMEM, REG_TMEM);
	printf("\tmrs\t%s, tpidr_el0\n", REG_TEMP);
	printf("\tadd\t%s, %s, %s\n", REG_TMEM, REG_TEMP, REG_TMEM);

	/*
	 * Calculate offset into tmem->tm_roots array
	 */
	printf("\t// Get cache for size class %d\n", cache_idx);
	printf("\tmov\t%s, #%d\n", REG_TEMP, cache_idx);
	printf("\tlsl\t%s, %s, #3\n", REG_TEMP, REG_TEMP);  /* Multiply by 8 */
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_TMEM, REG_TEMP);

	/*
	 * Check if cache has available buffers
	 */
	printf("\t// Check rounds\n");
	printf("\tldr\t%s, [%s]\n", REG_ROUNDS, REG_CACHE);  /* Load rounds */
	printf("\tcbz\t%s, .Lslow_path_%d\n", REG_ROUNDS, cache_idx);

	/*
	 * Fast path: pop buffer from cache
	 */
	printf("\t// Fast path: pop buffer\n");
	printf("\tsub\t%s, %s, #1\n", REG_ROUNDS, REG_ROUNDS);
	printf("\tstr\t%s, [%s]\n", REG_ROUNDS, REG_CACHE);

	/* Get buffer address */
	printf("\tuxtw\t%s, %s\n", REG_TEMP, REG_ROUNDS);  /* Zero-extend to 64-bit */
	printf("\tlsl\t%s, %s, #3\n", REG_TEMP, REG_TEMP);
	printf("\tadd\t%s, %s, #8\n", REG_CACHE, REG_CACHE);  /* Skip rounds field */
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_CACHE, REG_TEMP);
	printf("\tldr\t%s, [%s]\n", REG_SIZE, REG_CACHE);

	printf("\tret\n");

	/*
	 * Slow path: call umem_cache_alloc
	 */
	printf(".Lslow_path_%d:\n", cache_idx);
	printf("\t// Slow path: call umem_cache_alloc\n");
	printf("\tadrp\t%s, umem_alloc_sizes\n", REG_SIZE);
	printf("\tadd\t%s, %s, :lo12:umem_alloc_sizes\n", REG_SIZE, REG_SIZE);
	printf("\tldr\t%s, [%s, #%d]\n", REG_SIZE, REG_SIZE, cache_idx * 8);
	printf("\tmov\tx1, #%d\n", cache_flags);
	printf("\tb\tumem_cache_alloc\n");

	printf("\t.size\tumem_genasm_malloc_%d, .-umem_genasm_malloc_%d\n\n",
	    cache_idx, cache_idx);

	(void)cache_name;
	(void)max_size;
}

/*
 * Generate assembly for free fast path
 */
static void
genasm_free(umem_cache_t *cp, int max_size)
{
	int cache_idx = cp->cache_bufsize / UMEM_ALIGN;

	printf("\n\t.globl\tumem_genasm_free_%d\n", cache_idx);
	printf("\t.type\tumem_genasm_free_%d, %%function\n", cache_idx);
	printf("\t.align\t2\n");
	printf("umem_genasm_free_%d:\n", cache_idx);

	/* Get tmem pointer */
	printf("\t// Get tmem pointer\n");
	printf("\tadrp\t%s, :gottprel:_tmem\n", REG_TMEM);
	printf("\tldr\t%s, [%s, #:gottprel_lo12:_tmem]\n", REG_TMEM, REG_TMEM);
	printf("\tmrs\t%s, tpidr_el0\n", REG_TEMP);
	printf("\tadd\t%s, %s, %s\n", REG_TMEM, REG_TEMP, REG_TMEM);

	/* Get cache */
	printf("\t// Get cache for size class %d\n", cache_idx);
	printf("\tmov\t%s, #%d\n", REG_TEMP, cache_idx);
	printf("\tlsl\t%s, %s, #3\n", REG_TEMP, REG_TEMP);
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_TMEM, REG_TEMP);

	/* Check if cache has space */
	printf("\t// Check if cache has space\n");
	printf("\tldr\t%s, [%s]\n", REG_ROUNDS, REG_CACHE);
	printf("\tmov\t%s, #%d\n", REG_TEMP2, UMEM_GENASM_MAGAZINE_SIZE);
	printf("\tcmp\t%s, %s\n", REG_ROUNDS, REG_TEMP2);
	printf("\tb.ge\t.Lslow_free_%d\n", cache_idx);

	/* Fast path: push buffer */
	printf("\t// Fast path: push buffer\n");
	printf("\tuxtw\t%s, %s\n", REG_TEMP, REG_ROUNDS);
	printf("\tlsl\t%s, %s, #3\n", REG_TEMP, REG_TEMP);
	printf("\tadd\t%s, %s, #8\n", REG_CACHE, REG_CACHE);
	printf("\tadd\t%s, %s, %s\n", REG_CACHE, REG_CACHE, REG_TEMP);
	printf("\tstr\t%s, [%s]\n", REG_PTR, REG_CACHE);

	/* Increment rounds */
	printf("\tadd\t%s, %s, #1\n", REG_ROUNDS, REG_ROUNDS);
	printf("\tsub\t%s, %s, #8\n", REG_CACHE, REG_CACHE);
	printf("\tstr\t%s, [%s]\n", REG_ROUNDS, REG_CACHE);

	printf("\tret\n");

	/* Slow path */
	printf(".Lslow_free_%d:\n", cache_idx);
	printf("\t// Slow path: call umem_cache_free\n");
	printf("\tadrp\t%s, umem_alloc_sizes\n", REG_SIZE);
	printf("\tadd\t%s, %s, :lo12:umem_alloc_sizes\n", REG_SIZE, REG_SIZE);
	printf("\tldr\t%s, [%s, #%d]\n", REG_SIZE, REG_SIZE, cache_idx * 8);
	printf("\tmov\tx1, %s\n", REG_PTR);
	printf("\tmov\tx0, %s\n", REG_SIZE);
	printf("\tb\tumem_cache_free\n");

	printf("\t.size\tumem_genasm_free_%d, .-umem_genasm_free_%d\n\n",
	    cache_idx, cache_idx);

	(void)max_size;
}

/*
 * Generate malloc functions for all size classes
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

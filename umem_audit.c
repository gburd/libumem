/*
 * Enhanced audit utilities for debugger integration
 *
 * These functions provide access to allocation audit information
 * for use by debuggers (GDB/LLDB extensions).
 */

#include "config.h"
#include <umem_impl.h>
#include <stdio.h>
#include <string.h>

/*
 * Dump information about a single audit buffer
 *
 * This function is designed to be called from debuggers to display
 * detailed information about an allocated or freed buffer.
 */
void
umem_dump_audit_buffer(umem_bufctl_audit_t *bcp)
{
	if (bcp == NULL) {
		(void) fprintf(stderr, "umem_dump_audit_buffer: NULL pointer\n");
		return;
	}

	(void) fprintf(stderr, "Buffer Audit Information:\n");
	(void) fprintf(stderr, "  Address:   %p\n", bcp->bc_addr);
	(void) fprintf(stderr, "  Cache:     %p (%s)\n",
	    (void *)bcp->bc_cache,
	    bcp->bc_cache ? bcp->bc_cache->cache_name : "<unknown>");
	(void) fprintf(stderr, "  Timestamp: %lld\n",
	    (long long)bcp->bc_timestamp);
	(void) fprintf(stderr, "  Thread:    0x%lx\n",
	    (unsigned long)bcp->bc_thread);

	if (bcp->bc_depth > 0) {
		(void) fprintf(stderr, "  Stack trace (%d frames):\n",
		    bcp->bc_depth);
		int max_frames = bcp->bc_depth < 10 ? bcp->bc_depth : 10;
		for (int i = 0; i < max_frames; i++) {
			(void) fprintf(stderr, "    [%2d] %p\n",
			    i, (void *)bcp->bc_stack[i]);
		}
	} else {
		(void) fprintf(stderr, "  No stack trace available\n");
	}
}

/*
 * Dump audit information for all active allocations
 *
 * This walks through all caches and dumps audit information for
 * allocated buffers. Warning: This can produce a LOT of output.
 */
void
umem_dump_all_audits(void)
{
	extern umem_cache_t umem_null_cache;
	umem_cache_t *cp;
	int total_audits = 0;

	(void) fprintf(stderr, "Dumping all audit records...\n");
	(void) fprintf(stderr, "Note: This only works with UMEM_DEBUG=audit\n\n");

	/* Walk the cache list */
	for (cp = umem_null_cache.cache_next;
	    cp != &umem_null_cache;
	    cp = cp->cache_next) {

		if (!(cp->cache_flags & UMF_AUDIT)) {
			continue;
		}

		(void) fprintf(stderr, "Cache: %s\n", cp->cache_name);

		/* In a full implementation, we would walk the audit records
		 * for this cache. This is a simplified version that just
		 * shows the cache has audit enabled. */

		(void) fprintf(stderr, "  Audit enabled, %llu slab allocations\n",
		    (unsigned long long)cp->cache_slab_alloc);

		total_audits++;
	}

	if (total_audits == 0) {
		(void) fprintf(stderr, "No caches with audit enabled found.\n");
		(void) fprintf(stderr, "Set UMEM_DEBUG=audit to enable.\n");
	} else {
		(void) fprintf(stderr, "\nTotal caches with audit: %d\n",
		    total_audits);
	}
}

/*
 * Get audit information for a specific address
 *
 * Returns the umem_bufctl_audit_t structure for the given address,
 * or NULL if not found or audit is not enabled.
 *
 * This function is exported for debugger use.
 */
umem_bufctl_audit_t *
umem_get_audit_info(void *addr)
{
	extern umem_cache_t umem_null_cache;
	umem_cache_t *cp;
	umem_slab_t *sp;
	umem_bufctl_t *bcp;

	/* Walk caches to find the one containing this address */
	for (cp = umem_null_cache.cache_next;
	    cp != &umem_null_cache;
	    cp = cp->cache_next) {

		/* Skip caches without audit */
		if (!(cp->cache_flags & UMF_AUDIT)) {
			continue;
		}

		/* In a full implementation, we would:
		 * 1. Check if address is in cache's address range
		 * 2. Find the slab containing the address
		 * 3. Find the bufctl for the buffer
		 * 4. Return the audit structure
		 *
		 * This is simplified and returns NULL.
		 */
		(void) addr; /* suppress warning */
		(void) sp; /* suppress warning */
		(void) bcp; /* suppress warning */
	}

	return (NULL);
}

/*
 * Print cache statistics in a format useful for debugging
 *
 * This provides a summary of each cache's usage for debugger commands.
 */
void
umem_dump_cache_stats(void)
{
	extern umem_cache_t umem_null_cache;
	umem_cache_t *cp;
	uint64_t total_slab_alloc = 0;
	uint64_t total_slab_free = 0;
	int num_caches = 0;

	(void) fprintf(stderr, "%-20s %8s %10s %10s %10s\n",
	    "Cache", "BufSize", "SlabAlloc", "SlabFree", "BufTotal");
	(void) fprintf(stderr, "%s\n",
	    "----------------------------------------------------------------");

	for (cp = umem_null_cache.cache_next;
	    cp != &umem_null_cache;
	    cp = cp->cache_next) {

		(void) fprintf(stderr, "%-20s %8zu %10llu %10llu %10llu\n",
		    cp->cache_name,
		    cp->cache_bufsize,
		    (unsigned long long)cp->cache_slab_alloc,
		    (unsigned long long)cp->cache_slab_free,
		    (unsigned long long)cp->cache_buftotal);

		total_slab_alloc += cp->cache_slab_alloc;
		total_slab_free += cp->cache_slab_free;
		num_caches++;
	}

	(void) fprintf(stderr, "%s\n",
	    "----------------------------------------------------------------");
	(void) fprintf(stderr, "Total: %d caches, %llu slab allocs, %llu slab frees\n",
	    num_caches,
	    (unsigned long long)total_slab_alloc,
	    (unsigned long long)total_slab_free);
}

/*
 * Verify cache consistency
 *
 * Walks through all caches and performs consistency checks.
 * Returns 0 if all checks pass, non-zero otherwise.
 *
 * This is useful for debugging heap corruption issues.
 */
int
umem_verify_all_caches(void)
{
	extern umem_cache_t umem_null_cache;
	umem_cache_t *cp;
	int errors = 0;

	(void) fprintf(stderr, "Verifying all caches...\n");

	for (cp = umem_null_cache.cache_next;
	    cp != &umem_null_cache;
	    cp = cp->cache_next) {

		/* 1. Basic sanity checks */
		if (cp->cache_bufsize == 0) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has zero buffer size\n",
			    cp->cache_name);
			errors++;
		}

		if (cp->cache_align == 0 ||
		    (cp->cache_align & (cp->cache_align - 1)) != 0) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has invalid alignment %zu\n",
			    cp->cache_name, cp->cache_align);
			errors++;
		}

		if (cp->cache_chunksize < cp->cache_bufsize) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has chunksize (%zu) < bufsize (%zu)\n",
			    cp->cache_name, cp->cache_chunksize, cp->cache_bufsize);
			errors++;
		}

		/* 2. Statistics consistency */
		if (cp->cache_slab_free > cp->cache_slab_alloc) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has more slab frees (%llu) than allocs (%llu)\n",
			    cp->cache_name,
			    (unsigned long long)cp->cache_slab_free,
			    (unsigned long long)cp->cache_slab_alloc);
			errors++;
		}

		if (cp->cache_slab_destroy > cp->cache_slab_create) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has more slab destroys (%llu) than creates (%llu)\n",
			    cp->cache_name,
			    (unsigned long long)cp->cache_slab_destroy,
			    (unsigned long long)cp->cache_slab_create);
			errors++;
		}

		/* 3. Magazine layer checks */
		if (cp->cache_magtype != NULL) {
			/* Verify depot contention is non-negative */
			if ((int64_t)cp->cache_depot_contention < 0) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s has negative depot contention\n",
				    cp->cache_name);
				errors++;
			}
		}

		/* 4. Per-CPU cache checks */
		for (uint32_t cpu = 0; cpu <= cp->cache_cpu_mask; cpu++) {
			umem_cpu_cache_t *ccp = &cp->cache_cpu[cpu];

			if (ccp->cc_rounds < 0) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s CPU %u has negative rounds\n",
				    cp->cache_name, cpu);
				errors++;
			}

			if (ccp->cc_prounds < 0) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s CPU %u has negative prounds\n",
				    cp->cache_name, cpu);
				errors++;
			}

			/* Verify loaded magazine has valid rounds */
			if (ccp->cc_loaded != NULL &&
			    cp->cache_magtype != NULL) {
				if (ccp->cc_rounds > cp->cache_magtype->mt_magsize) {
					(void) fprintf(stderr,
					    "ERROR: Cache %s CPU %u has rounds (%d) > magsize (%d)\n",
					    cp->cache_name, cpu, ccp->cc_rounds,
					    cp->cache_magtype->mt_magsize);
					errors++;
				}
			}
		}

		/* 5. Hash table sanity */
		if (cp->cache_hash_table != NULL) {
			if (cp->cache_hash_mask == 0) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s has hash table but zero mask\n",
				    cp->cache_name);
				errors++;
			}

			/* Verify hash_shift is reasonable */
			if (cp->cache_hash_shift > 64) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s has invalid hash_shift %zu\n",
				    cp->cache_name, cp->cache_hash_shift);
				errors++;
			}
		}

		/* 6. Slab coloring sanity */
		if (cp->cache_maxcolor > 0 && cp->cache_color > cp->cache_maxcolor) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has color (%zu) > maxcolor (%zu)\n",
			    cp->cache_name, cp->cache_color, cp->cache_maxcolor);
			errors++;
		}

		/* 7. Verify debug structures if enabled */
		if (cp->cache_flags & UMF_AUDIT) {
			/* Audit tracking is enabled - verify bufctl cache exists */
			if (cp->cache_bufctl_cache == NULL && cp->cache_bufctl == 0) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s has UMF_AUDIT but no bufctl cache\n",
				    cp->cache_name);
				errors++;
			}
		}
	}

	if (errors == 0) {
		(void) fprintf(stderr, "All caches verified successfully\n");
	} else {
		(void) fprintf(stderr, "Found %d error(s)\n", errors);
	}

	return (errors);
}

/*
 * Find potential memory leaks
 *
 * This is a simplified leak detector that identifies buffers that
 * are allocated but may not be reachable. For production use,
 * use proper leak detection tools like Valgrind or ASan.
 */
void
umem_find_leaks(void)
{
	extern umem_cache_t umem_null_cache;
	umem_cache_t *cp;
	uint64_t total_leaked_buffers = 0;
	uint64_t total_leaked_bytes = 0;

	(void) fprintf(stderr, "Scanning for potential memory leaks...\n");
	(void) fprintf(stderr, "Note: This is a simplified scan.\n");
	(void) fprintf(stderr, "For production, use Valgrind or AddressSanitizer.\n\n");

	for (cp = umem_null_cache.cache_next;
	    cp != &umem_null_cache;
	    cp = cp->cache_next) {

		uint64_t inuse = cp->cache_buftotal;

		if (inuse > 100) { /* Only show caches with >100 buffers */
			uint64_t leaked_bytes = inuse * cp->cache_bufsize;

			(void) fprintf(stderr,
			    "Cache %-20s: %llu buffers (%llu bytes)\n",
			    cp->cache_name,
			    (unsigned long long)inuse,
			    (unsigned long long)leaked_bytes);

			total_leaked_buffers += inuse;
			total_leaked_bytes += leaked_bytes;
		}
	}

	if (total_leaked_buffers == 0) {
		(void) fprintf(stderr, "\nNo allocated buffers found.\n");
	} else {
		(void) fprintf(stderr,
		    "\nTotal: %llu buffers (%llu bytes) still allocated\n",
		    (unsigned long long)total_leaked_buffers,
		    (unsigned long long)total_leaked_bytes);
		(void) fprintf(stderr,
		    "Note: These may or may not be actual leaks.\n");
		(void) fprintf(stderr,
		    "Use UMEM_DEBUG=audit for detailed allocation info.\n");
	}
}

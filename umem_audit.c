/*
 * Enhanced audit utilities for debugger integration.
 *
 * This file used to contain stubs that claimed to implement leak
 * detection and audit-record walking but did not actually do either.
 * The real implementations now live in umem_inspect.c / umem_inspect.h;
 * the entry points here are preserved for ABI compatibility and forward
 * to the inspection API.
 *
 * New code should call the umem_inspect_* / umem_findleaks() /
 * umem_walk_* / umem_whatis() entry points directly.
 */

#include "config.h"
#include <umem_impl.h>
#include <stdio.h>
#include <string.h>
#include "umem_stacktrace.h"
#include "umem_inspect.h"

void
umem_dump_audit_buffer(umem_bufctl_audit_t *bcp)
{
	if (bcp == NULL) {
		(void) fprintf(stderr, "umem_dump_audit_buffer: NULL\n");
		return;
	}
	(void) umem_bufctl_audit_dump(stderr, bcp->bc_addr);
}

void
umem_dump_all_audits(void)
{
	(void) umem_log_dump(stderr, UMEM_FMT_TEXT, 0);
}

umem_bufctl_audit_t *
umem_get_audit_info(void *addr)
{
	umem_buffer_info_t info;
	if (umem_whatis(addr, &info) != 0)
		return (NULL);
	return ((umem_bufctl_audit_t *)info.bufctl);
}

void
umem_dump_cache_stats(void)
{
	umem_status_dump(stderr, UMEM_FMT_TEXT);
}

/*
 * Verify cache consistency.  Walks every cache and performs structural
 * invariant checks; useful for debugging heap corruption.  Returns the
 * number of invariants violated (0 = clean).
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
			    cp->cache_name, cp->cache_chunksize,
			    cp->cache_bufsize);
			errors++;
		}

		if (cp->cache_slab_free > cp->cache_slab_alloc) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has more slab frees (%llu) "
			    "than allocs (%llu)\n",
			    cp->cache_name,
			    (unsigned long long)cp->cache_slab_free,
			    (unsigned long long)cp->cache_slab_alloc);
			errors++;
		}

		if (cp->cache_slab_destroy > cp->cache_slab_create) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has more slab destroys (%llu) "
			    "than creates (%llu)\n",
			    cp->cache_name,
			    (unsigned long long)cp->cache_slab_destroy,
			    (unsigned long long)cp->cache_slab_create);
			errors++;
		}

		if (cp->cache_magtype != NULL &&
		    (int64_t)cp->cache_depot_contention < 0) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has negative depot contention\n",
			    cp->cache_name);
			errors++;
		}

		for (uint32_t cpu = 0; cpu <= cp->cache_cpu_mask; cpu++) {
			umem_cpu_cache_t *ccp = &cp->cache_cpu[cpu];

			if (ccp->cc_rounds < -1) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s CPU %u has "
				    "invalid rounds (%d)\n",
				    cp->cache_name, cpu, ccp->cc_rounds);
				errors++;
			}
			if (ccp->cc_prounds < -1) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s CPU %u has "
				    "invalid prounds (%d)\n",
				    cp->cache_name, cpu, ccp->cc_prounds);
				errors++;
			}
			if (ccp->cc_loaded != NULL &&
			    cp->cache_magtype != NULL &&
			    ccp->cc_rounds > cp->cache_magtype->mt_magsize) {
				(void) fprintf(stderr,
				    "ERROR: Cache %s CPU %u has rounds (%d) "
				    "> magsize (%d)\n",
				    cp->cache_name, cpu, ccp->cc_rounds,
				    cp->cache_magtype->mt_magsize);
				errors++;
			}
		}

		if (cp->cache_hash_table != NULL && cp->cache_hash_mask == 0) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has hash table but zero mask\n",
			    cp->cache_name);
			errors++;
		}

		if (cp->cache_maxcolor > 0 &&
		    cp->cache_color > cp->cache_maxcolor) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has color (%zu) > maxcolor (%zu)\n",
			    cp->cache_name, cp->cache_color, cp->cache_maxcolor);
			errors++;
		}

		if ((cp->cache_flags & UMF_AUDIT) &&
		    cp->cache_bufctl_cache == NULL && cp->cache_bufctl == 0) {
			(void) fprintf(stderr,
			    "ERROR: Cache %s has UMF_AUDIT but no bufctl cache\n",
			    cp->cache_name);
			errors++;
		}
	}

	if (errors == 0)
		(void) fprintf(stderr, "All caches verified successfully\n");
	else
		(void) fprintf(stderr, "Found %d error(s)\n", errors);

	return (errors);
}

/*
 * Legacy entry point.  Forwards to umem_findleaks() so callers that
 * linked against the stubbed version get real output now.
 */
void
umem_find_leaks(void)
{
	(void) umem_findleaks(stderr, UMEM_FMT_TEXT, 50);
}

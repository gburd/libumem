/*
 * oracle_null_shim.c -- a DELIBERATELY BROKEN allocator, built as a shared
 * library, for validating that the concurrency oracle discriminates (P2.3).
 *
 * WHY A SHIM AND NOT ONLY THE ENV KNOB
 *   stress_concurrency_oracle's ORACLE_INJECT knob breaks the allocation
 *   inside the oracle's own wrapper.  That is enough to exercise the
 *   accounting, but it is a self-test: the oracle is judging a defect it
 *   injected itself, one function call deep.  This shim instead replaces the
 *   REAL allocator underneath, at the umem_alloc symbol the oracle actually
 *   calls, so the oracle is judging an allocator that genuinely misbehaves and
 *   has no idea anything was injected.  If the oracle passes against this, its
 *   PASS means nothing.
 *
 * TWO MODES, chosen by ORACLE_SHIM:
 *   null[:N]     umem_alloc returns NULL for every request after the first N.
 *                Models exhaustion / a broken allocator.  Pre-2026-09-22 the
 *                oracle PASSED against this and reported hundreds of millions
 *                of ops/s.
 *   alias[:N]    after N allocations, umem_alloc hands out a buffer that is
 *                ALREADY LIVE and owned by another caller -- a real
 *                double-allocation.  This is the defect class the oracle
 *                exists to detect (two owners stamping one buffer), produced
 *                by the allocator rather than by a memory poke.
 *
 * Build/run: see test/stress/oracle_control.sh, which builds this with the
 * same compiler as the tree and LD_PRELOADs it.  Not part of the library and
 * never installed.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*umem_alloc_fn)(size_t, int);
typedef void (*umem_free_fn)(void *, size_t);

static umem_alloc_fn real_alloc;
static umem_free_fn real_free;

enum mode { M_OFF, M_NULL, M_ALIAS };
static enum mode mode = M_OFF;
static unsigned long long after = 1000;
static atomic_ullong seen;

/* For alias mode: the one buffer we hand out repeatedly. */
static void *alias_buf;
static size_t alias_sz;
static atomic_int alias_armed;

__attribute__((constructor))
static void init(void)
{
	real_alloc = (umem_alloc_fn)dlsym(RTLD_NEXT, "umem_alloc");
	real_free = (umem_free_fn)dlsym(RTLD_NEXT, "umem_free");

	const char *s = getenv("ORACLE_SHIM");
	if (s == NULL || *s == '\0')
		return;
	const char *colon = strchr(s, ':');
	size_t klen = colon ? (size_t)(colon - s) : strlen(s);
	if (klen == 4 && strncmp(s, "null", 4) == 0)
		mode = M_NULL;
	else if (klen == 5 && strncmp(s, "alias", 5) == 0)
		mode = M_ALIAS;
	else {
		fprintf(stderr, "oracle_null_shim: bad ORACLE_SHIM '%s'\n", s);
		_exit(2);
	}
	if (colon != NULL)
		after = strtoull(colon + 1, NULL, 10);
	fprintf(stderr, "oracle_null_shim: BROKEN ALLOCATOR ACTIVE mode=%.*s "
	    "after=%llu\n", (int)klen, s, after);
}

void *
umem_alloc(size_t sz, int flags)
{
	if (real_alloc == NULL)          /* nothing to delegate to */
		return (NULL);

	unsigned long long n = atomic_fetch_add(&seen, 1) + 1;

	if (mode == M_NULL && n > after)
		return (NULL);

	if (mode == M_ALIAS && n > after) {
		/*
		 * Hand out a buffer that is already live and owned by someone
		 * else: a genuine double-allocation.  Arm once, then keep
		 * returning the same live buffer to every caller, which is
		 * exactly the aliasing the oracle's owner tokens detect.
		 */
		if (atomic_exchange(&alias_armed, 1) == 0) {
			alias_sz = sz;
			alias_buf = real_alloc(sz, flags);
			return (alias_buf);
		}
		if (alias_buf != NULL && sz <= alias_sz)
			return (alias_buf);
		/* Size does not fit the aliased buffer: serve normally rather
		 * than hand out something too small (that would be a buffer
		 * overflow in the test, not an aliasing signal). */
	}

	return (real_alloc(sz, flags));
}

void
umem_free(void *p, size_t sz)
{
	if (real_free == NULL)
		return;
	/* Never actually free the aliased buffer: several owners believe they
	 * hold it, and a real free would turn the aliasing signal into a
	 * use-after-free crash instead. */
	if (mode == M_ALIAS && p != NULL && p == alias_buf)
		return;
	real_free(p, sz);
}

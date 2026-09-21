/*
 * External-consumer header check.
 *
 * Compiled against an INSTALLED prefix only (-I<prefix>/include), never
 * against the source tree.  Its whole job is to fail if any installed
 * header needs something that is not installed: "config.h", umem_impl.h,
 * umem_base.h, or any other build-private header.
 *
 * Run it with:
 *
 *   make install DESTDIR=/tmp/p
 *   cc -I/tmp/p/usr/local/include -c test/install/external_consumer.c
 *
 * or via `make install-check prefix=...` (see Makefile.am).
 *
 * This exists because --enable-rseq and --enable-numa used to install
 * umem_rseq.h and umem_numa.h, both of which start with #include
 * "config.h" -- a file the install never copies.  An external program
 * including them failed to compile with a missing-header error that looked
 * like the user's mistake.
 *
 * Deliberately includes nothing but the installed headers, and calls a few
 * entry points so the link step proves the library exports them too.
 */

#define UMEM_ENABLE_EXPERIMENTAL 1

#include <umem.h>
#include <sys/vmem.h>
#include <umem_hooks.h>
#include <umem_arena.h>
#include <umem_own.h>
#include <umem_profile.h>
#include <umem_stacktrace.h>
#include <umem_inspect.h>

#include <stdio.h>
#include <string.h>

int
main(void)
{
	/* Core API: the thing every consumer actually calls. */
	void *p = umem_alloc(128, UMEM_DEFAULT);

	if (p == NULL) {
		(void) fprintf(stderr, "umem_alloc(128) returned NULL\n");
		return (1);
	}
	memset(p, 0xa5, 128);
	umem_free(p, 128);

	/* Object caches. */
	umem_cache_t *cp = umem_cache_create("external_consumer", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	if (cp == NULL) {
		(void) fprintf(stderr, "umem_cache_create failed\n");
		return (1);
	}
	void *o = umem_cache_alloc(cp, UMEM_DEFAULT);
	if (o == NULL) {
		(void) fprintf(stderr, "umem_cache_alloc failed\n");
		return (1);
	}
	umem_cache_free(cp, o);
	umem_cache_destroy(cp);

	(void) printf("external_consumer: installed headers and library OK\n");
	return (0);
}

/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * See usr/src/OPENSOLARIS.LICENSE or http://www.opensolaris.org/os/licensing.
 *
 * CDDL HEADER END
 */

/*
 * pal_aal_smoke.c -- the PAL/AAL port smoke test.
 *
 * References every required PAL and AAL symbol so that a port which forgot
 * one fails to LINK on the porting box (the C analogue of snmalloc's Concept
 * compile error for a missing method; docs/design/PAL.md).  It also prints the
 * selected PAL/AAL identity and the capability bitmask, so a porter can see at
 * a glance which platform/arch was selected and what the compile-time
 * detection decided.
 *
 * It does NOT exercise the allocator; behaviour validation is make check.
 * This is the link-contract check (validation point E / the smoke test).
 *
 * Exit 0 if every symbol resolved and the identity is sane; nonzero otherwise.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pal/pal.h"
#include "aal/aal.h"
#include "misc.h"

int
main(void)
{
	int rc = 0;

	/* --- AAL identity + required primitives --- */
	printf("AAL: %s (ptr=%d word=%d)\n", UMEM_AAL_NAME,
	    UMEM_AAL_PTR_BITS, UMEM_AAL_WORD_BITS);
	if (UMEM_AAL_PTR_BITS != 32 && UMEM_AAL_PTR_BITS != 64) {
		fprintf(stderr, "AAL: bad pointer width\n");
		rc = 1;
	}

	/* umem_aal_pause(): must be callable (inline). */
	umem_aal_pause();

	/*
	 * getpcstack(): reference it so a port missing the arch frame walk
	 * fails to link.  Called in a way the compiler cannot elide (the
	 * result feeds rc).  getfp() is part of the AAL contract only where it
	 * is a real symbol (illumos sparc/x86 subr); on Linux the walk uses
	 * __builtin_frame_address directly and there is no getfp to link.
	 */
	{
		uintptr_t stack[8];
		int n = getpcstack(stack, 8, 0);
		if (n < 0) {
			fprintf(stderr, "AAL: getpcstack returned %d\n", n);
			rc = 1;
		}
#if defined(__sun) || defined(__illumos__)
		{
			void *fp = getfp();
			(void) fp;
		}
#endif
		printf("AAL: getpcstack depth=%d\n", n);
	}

	/* --- PAL identity + required ops --- */
	printf("PAL: %s features=0x%04x\n", UMEM_PAL_NAME,
	    umem_pal_features());

	{
		size_t ps = umem_pal_page_size();
		long nc = umem_pal_ncpus();
		int hint = umem_pal_cpu_hint();
		uint8_t ent[16];
		int got;

		/*
		 * page_size is set by umem_startup(); referencing it here
		 * before the library has initialised can return 0, so this is
		 * a link + sanity reference, not a hard assert on the value.
		 */
		printf("PAL: page_size=%zu ncpus=%ld cpu_hint=%d\n",
		    ps, nc, hint);
		if (nc < 1) {
			fprintf(stderr, "PAL: ncpus < 1 (%ld)\n", nc);
			rc = 1;
		}

		umem_pal_yield();

		memset(ent, 0, sizeof (ent));
		got = umem_pal_getentropy(ent, sizeof (ent));
		printf("PAL: getentropy rc=%d\n", got);
		/* got == -1 is allowed (caller has its own fallback). */
	}

	/*
	 * umem_pal_abort is referenced but NOT called (it aborts).  Taking its
	 * address is enough to require the symbol at link time.
	 */
	{
		void (*ap)(void) = umem_pal_abort;
		if (ap == NULL)
			rc = 1;
	}

	if (rc == 0)
		printf("PAL/AAL smoke: OK\n");
	else
		printf("PAL/AAL smoke: FAIL (rc=%d)\n", rc);
	return (rc);
}

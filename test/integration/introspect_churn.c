/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * introspect_churn -- a long-running libumem workload for exercising the
 * umemctl introspection channel (G1-G4 tests). It continuously allocates and
 * frees a mix of sizes (to churn slabs so logtail streams events), and it
 * deliberately LEAKS a fixed size from one call site so `umemctl leaks` and
 * `break leaked` have something to find.
 *
 * Run with UMEM_OPTIONS=introspect=1 [UMEM_DEBUG=audit for leaks].
 * argv[1] = seconds to run (default: run until killed).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "umem.h"

/* A distinct call site whose allocations are never freed -- the "leak". */
static void *
leak_site(size_t sz)
{
	void *p = umem_alloc(sz, UMEM_DEFAULT);
	if (p != NULL)
		memset(p, 0xAB, sz);
	return (p);
}

int
main(int argc, char **argv)
{
	long secs = (argc > 1) ? strtol(argv[1], NULL, 10) : 0;
	time_t start = time(NULL);
	unsigned long iter = 0;
	void *ring[64];
	int rn = 0;

	memset(ring, 0, sizeof (ring));
	fprintf(stderr, "churn: pid=%ld sock=/tmp/umem.%ld.sock\n",
	    (long)getpid(), (long)getpid());

	for (;;) {
		size_t sz = 16 + ((iter * 24) % 480);	/* varied sizes */
		void *p = umem_alloc(sz, UMEM_DEFAULT);
		if (ring[rn] != NULL)
			umem_free(ring[rn], 32);  /* free earlier alloc */
		ring[rn] = umem_alloc(32, UMEM_DEFAULT);
		rn = (rn + 1) % 64;
		if (p != NULL)
			umem_free(p, sz);

		/* Leak one 200-byte buffer every 1000 iterations. */
		if ((iter % 1000) == 0)
			(void) leak_site(200);

		iter++;
		if ((iter % 20000) == 0)
			usleep(1000);	/* let slabs settle / reap fire */

		if (secs > 0 && (time(NULL) - start) >= secs)
			break;
	}
	fprintf(stderr, "churn: done, %lu iterations\n", iter);
	return (0);
}

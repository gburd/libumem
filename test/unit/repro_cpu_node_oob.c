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
 * CDDL HEADER END
 */

/*
 * Standalone reproduction for the umem_cpu_node[] out-of-bounds read in
 * umem_depot_alloc on high-core machines.  See
 * docs/results/2026-07-23-cpu_node-oob-finding.md.
 *
 * EXPECTED-FAIL (reproduction): on a machine where umem's detected
 * umem_max_ncpus > UMEM_MAX_DEPOT_CPUS (256) -- e.g. a 192-vCPU metal
 * instance -- a single umem_alloc()/umem_free() reads past the fixed
 * 256-entry umem_cpu_node[] array.  Built with --enable-asan this ABORTS
 * with a global-buffer-overflow in umem_depot_alloc (umem.c:2152).
 *
 * On machines with <= 256 depot CPUs (the common case, and the CI/low-core
 * roles) this exits 0.  It is therefore SAFE to keep in the suite as a
 * living reproduction: it turns red only on the affected hardware, which is
 * exactly the signal we want until the core fix lands.
 *
 * This is a CORE allocator bug, out of scope for Workstream H to fix;
 * committed here as the reproduction the plan requires.
 */

#include <umem.h>
#include <stdio.h>

int
main(void)
{
	/* A single allocation is enough to enter the depot layer. */
	void *p = umem_alloc(16, UMEM_DEFAULT);
	if (p == NULL) {
		fprintf(stderr, "umem_alloc returned NULL\n");
		return (2);
	}
	umem_free(p, 16);

	/* Churn a bit so the depot NUMA-stealing loop is exercised. */
	for (int i = 0; i < 1024; i++) {
		void *q = umem_alloc(32 + (size_t)(i & 63), UMEM_DEFAULT);
		if (q != NULL)
			umem_free(q, 32 + (size_t)(i & 63));
	}

	printf("repro_cpu_node_oob: survived (umem_max_ncpus <= 256 here)\n");
	return (0);
}

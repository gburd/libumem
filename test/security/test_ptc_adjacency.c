/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * P5.12: no user allocation may be adjacent to a per-thread cache struct.
 *
 * THE DEFECT.  umem_ptc_t (the per-thread cache: 36 bins of slot pointers,
 * two magazines per class, a pool of cached object addresses) was allocated
 * with umem_alloc(sizeof (umem_ptc_t)) -- i.e. from the umem_alloc_24576
 * size class, in the same slabs as every user request of 20481..24576 bytes.
 * Its first field is a pointer (bins[0].slots); its pool[] is the list of
 * addresses the next umem_alloc() hands out without checking.  A user
 * buffer in that class overrun into its slab neighbour could rewrite what
 * the allocator returns next.  Attacker position D.  glibc's tcache has the
 * same adjacency and safe-links its entries; libumem's slots are raw.
 *
 * WHAT THIS TESTS.  Spawn NTHREADS workers so NTHREADS umem_ptc_t are live
 * and record their addresses (thread_ptc is the thread's own; each worker
 * reports it).  Then allocate many user buffers of every size that maps to
 * the class sizeof (umem_ptc_t) would fall in, and for each check whether
 * [buf, buf + class_size) is within one class_size of any recorded PTC --
 * i.e. shares a slab neighbourhood with it.
 *
 *   PASS: no user buffer within one object-size (inclusive) of any live PTC.
 *   FAIL: at least one is.  Pre-fix, with 8 PTCs live and hundreds of
 *         same-class user buffers, adjacency is near-certain.
 *
 * Post-fix umem_ptc_t comes from umem_ptc_cache (UMC_INTERNAL, its own slabs
 * in umem_internal_arena), so the nearest user object is in a different
 * span.  Reads umem_ptc.h for sizeof and thread_ptc; nothing else internal.
 */

#include "umem_base.h"
#include "umem_ptc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#define	NTHREADS	8
#define	NUSER		2000

static uintptr_t ptc_addr[NTHREADS];
static atomic_int reported, go;

static void *
worker(void *arg)
{
	int i = (int)(intptr_t)arg;
	void *p = umem_alloc(64, UMEM_DEFAULT);	/* creates this thread's PTC */

	umem_free(p, 64);
	ptc_addr[i] = (uintptr_t)thread_ptc;
	atomic_fetch_add(&reported, 1);
	while (!atomic_load(&go))
		usleep(1000);
	return (NULL);
}

int
main(void)
{
	pthread_t th[NTHREADS];
	void **user;
	size_t sz = sizeof (umem_ptc_t);
	size_t class_sz, i;
	int t, adjacent = 0, checked = 0;
	umem_cache_t *cp;

	if (!umem_ptc_enabled) {
		printf("SKIP: PTC disabled\n");
		return (77);
	}

	for (t = 0; t < NTHREADS; t++)
		(void) pthread_create(&th[t], NULL, worker, (void *)(intptr_t)t);
	while (atomic_load(&reported) < NTHREADS)
		usleep(1000);

	for (t = 0; t < NTHREADS; t++) {
		if (ptc_addr[t] == 0) {
			printf("SKIP: worker %d has no PTC\n", t);
			atomic_store(&go, 1);
			return (77);
		}
	}

	/* The user size class sizeof (umem_ptc_t) would have landed in. */
	cp = umem_alloc_table[(sz - 1) >> UMEM_ALIGN_SHIFT];
	class_sz = cp->cache_bufsize;
	printf("sizeof(umem_ptc_t)=%zu -> user class %s (%zu B)\n", sz,
	    cp->cache_name, class_sz);

	user = calloc(NUSER, sizeof (void *));
	for (i = 0; i < NUSER; i++) {
		/* Spread requests across the class so every slab position is hit. */
		size_t req = class_sz - (i % 64) * 8;
		user[i] = umem_alloc(req, UMEM_DEFAULT);
		if (user[i] == NULL)
			continue;
		memset(user[i], 0x5a, req);
		checked++;
		for (t = 0; t < NTHREADS; t++) {
			uintptr_t u = (uintptr_t)user[i];
			uintptr_t d = u > ptc_addr[t] ? u - ptc_addr[t] :
			    ptc_addr[t] - u;
			/*
			 * <= not <: in this class a slab holds ONE object (the
			 * 16-object floor stops at 64 KiB slabs), so the next
			 * object is the next span, exactly class_sz away, and
			 * spans from the va arena's 4 MiB qcache slab are
			 * contiguous.  An overrun of one byte past the end of
			 * a user object lands in the PTC at +class_sz.
			 */
			if (d <= class_sz) {
				if (adjacent < 5)
					printf("  user %p is %zu B from PTC %p\n",
					    user[i], (size_t)d,
					    (void *)ptc_addr[t]);
				adjacent++;
			}
		}
	}
	for (i = 0; i < NUSER; i++)
		if (user[i] != NULL)
			umem_free(user[i], class_sz - (i % 64) * 8);
	free(user);
	atomic_store(&go, 1);
	for (t = 0; t < NTHREADS; t++)
		(void) pthread_join(th[t], NULL);

	printf("checked %d user buffers against %d live PTCs: %d adjacent\n",
	    checked, NTHREADS, adjacent);
	if (adjacent) {
		printf("RESULT: FAIL (user buffers share slabs with per-thread "
		    "cache structs; an overrun reaches the allocator's slot "
		    "pointers)\n");
		return (1);
	}
	printf("RESULT: PASS (no user buffer within one object of any PTC)\n");
	return (0);
}

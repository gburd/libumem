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
 * O6 regression: a thread that exits holding an EXACTLY FULL per-thread
 * magazine must hand that magazine to the depot's full list whole, not
 * break it up object by object.
 *
 * THE DEFECT.  umem_ptc_mag_flush_all() (thread exit) passed
 * &cp->cache_empty for both the loaded and the previous magazine.
 * umem_ptc_mag_return() puts a magazine on the full list only when the
 * caller asked for cache_full AND rounds == capacity, so every full
 * magazine a thread exited with was drained by umem_mag_drain(), one
 * _umem_cache_free() per round, and the empty shell went on the empty
 * list.  Nothing was lost; the cost was capacity frees where one list push
 * would do.  Measured by the UMEM_BARN_PROBE build at t=192: 886,721
 * objects over 12 runs (docs/results/2026-09-27-sheaves-barn-review.md).
 *
 * THE ORACLE, exact.  Thread T fills its PTC bin, then frees objects one at
 * a time until its loaded magazine holds exactly its capacity, records the
 * shell's address, and exits.  After the join, main walks the cache's depot
 * lists (global and per-CPU, under each ml_lock) for that address:
 *   on a full list   -> PASS (handed over whole);
 *   anywhere else    -> FAIL.  Pre-fix the shell is drained and pushed on
 *                       the empty list, where the bin drain that follows in
 *                       umem_ptc_destroy() usually pops it straight back
 *                       out for the CPU layer, so "neither" is the common
 *                       pre-fix result.  It is retried TRIES times only
 *                       because a reap could in principle destroy a full
 *                       magazine first (ws_reap needs a nonzero reaplimit
 *                       from a previous interval, so not in practice).
 * T frees nothing else before it exits: its remaining objects are freed by
 * main after the inspection, so no CPU-layer traffic can move the shell.
 *
 * rseq is switched off for the run (umem_rseq_enabled = 0 after init).
 * With it on, _umem_free sends a PTC overflow to the rseq per-CPU layer
 * before the per-thread magazine, so whether T ever loads one depends on
 * the depot state; the path under test is the same either way.
 */

#include "umem_base.h"
#include "umem_ptc.h"
#include "umem_impl.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define	SZ	64
#define	MAXOBJ	4096
#define	TRIES	5

static umem_cache_t *g_cp;
static void *g_shell;
static void *bufs[MAXOBJ];
static int g_nbufs, g_first_rest;
static int g_rounds, g_cap;

static void *
filler(void *arg)
{
	umem_ptc_mag_t *mag;
	int bin = umem_ptc_size_to_bin(SZ), i, n;

	(void) arg;
	g_nbufs = g_first_rest = 0;
	for (n = 0; n < MAXOBJ; n++)
		if ((bufs[n] = umem_alloc(SZ, UMEM_DEFAULT)) == NULL)
			break;
	g_nbufs = g_first_rest = n;
	if (thread_ptc == NULL || bin < 0)
		return (NULL);
	mag = &thread_ptc->mags[bin];
	for (i = 0; i < n; i++) {
		umem_free(bufs[i], SZ);
		if (mag->loaded != NULL && mag->rounds == mag->magsize) {
			g_cp = mag->cache;
			g_shell = mag->loaded;
			g_rounds = mag->rounds;
			g_cap = mag->magsize;
			break;
		}
	}
	g_first_rest = i + 1;		/* main frees the rest afterwards */
	return (NULL);			/* exit: umem_ptc_mag_flush_all runs */
}

static int
on_list(umem_maglist_t *mlp, void *mp)
{
	umem_magazine_t *m;
	int hit = 0;

	(void) mutex_lock(&mlp->ml_lock);
	for (m = mlp->ml_list; m != NULL; m = m->mag_next)
		if (m == mp)
			hit = 1;
	(void) mutex_unlock(&mlp->ml_lock);
	return (hit);
}

/* 1 = on a full list, 0 = on an empty list, -1 = on neither. */
static int
where(umem_cache_t *cp, void *mp)
{
	int i;

	if (on_list(&cp->cache_full, mp))
		return (1);
	if (on_list(&cp->cache_empty, mp))
		return (0);
	for (i = 0; i < cp->cache_depot_ncpus; i++) {
		if (on_list(&cp->cache_depot_full[i], mp))
			return (1);
		if (on_list(&cp->cache_depot_empty[i], mp))
			return (0);
	}
	return (-1);
}

int
main(void)
{
	pthread_t t;
	int try, w;
	void *warm = umem_alloc(SZ, UMEM_DEFAULT);

	if (warm == NULL || thread_ptc == NULL || umem_ptc_size_to_bin(SZ) < 0) {
		printf("SKIP: PTC disabled or %d B not PTC-eligible\n", SZ);
		return (77);
	}
	umem_free(warm, SZ);
#ifdef UMEM_RSEQ_AVAILABLE
	umem_rseq_enabled = 0;
#endif
	for (try = 0; try < TRIES; try++) {
		g_shell = NULL;
		if (pthread_create(&t, NULL, filler, NULL) != 0) {
			printf("FAIL: pthread_create\n");
			return (1);
		}
		(void) pthread_join(t, NULL);
		if (g_shell == NULL) {
			printf("FAIL: thread never held a full PTC magazine "
			    "in %d frees (the setup did not reach the path)\n",
			    MAXOBJ);
			return (1);
		}
		w = where(g_cp, g_shell);
		for (; g_first_rest < g_nbufs; g_first_rest++)
			umem_free(bufs[g_first_rest], SZ);
		printf("try %d: shell %p rounds=%d cap=%d -> %s\n", try,
		    g_shell, g_rounds, g_cap, w == 1 ? "FULL list" :
		    w == 0 ? "EMPTY list" : "neither (drained, shell reused)");
		if (w == 1) {
			printf("PASS: a full magazine held at thread exit "
			    "went to the depot full list whole\n");
			return (0);
		}
		if (w == 0)
			break;
	}
	printf("FAIL: a full magazine held at thread exit did not reach the "
	    "depot full list: it was drained round by round (%d "
	    "_umem_cache_free calls) instead of pushed once\n", g_rounds);
	return (1);
}

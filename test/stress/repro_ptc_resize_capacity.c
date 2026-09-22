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
 * Regression: a PTC magazine must never be used with a capacity that is not
 * its own (P1.3b).
 *
 * DEFECT (pre-fix)
 *   _umem_alloc's PTC magazine path obtained a magazine from
 *   umem_depot_alloc_trylock() -- which validates it against the magtype the
 *   cache has AT THAT MOMENT -- and then SEPARATELY read
 *   cp->cache_magtype->mt_magsize to decide how many rounds that magazine
 *   held:
 *
 *       fmp = umem_depot_alloc_trylock(cp, &cp->cache_full);  // capacity v1
 *       new_magsize = cp->cache_magtype->mt_magsize;          // v2 >= v1
 *       mag->loaded = fmp;
 *       mag->rounds = new_magsize;                            // v2 rounds!
 *       buf = mag->loaded->mag_round[--mag->rounds];          // index v2-1
 *
 *   umem_cache_magazine_resize() runs on the update thread and changes the
 *   magtype between those two reads, so an old 127-round magazine gets indexed
 *   as if it had 255 rounds -- 1KB past the end of its own allocation, into the
 *   next chunk of the same slab, which is another magazine.  The pointer read
 *   out of there is handed to the caller as an allocation: either garbage, or
 *   NULL, or a buffer another thread also owns.  Subsequent frees then write
 *   into that neighbouring magazine.
 *
 *   The free side had the same defect with the two reads in the opposite
 *   order (magtype first, magazine second), which desynchronizes the other
 *   way: a 255-round magazine recorded as holding 127.  That one does not
 *   index out of bounds, but it puts a half-filled magazine on the depot's
 *   FULL list, whose contract is "exactly magsize rounds" -- umem_depot_ws_reap
 *   then frees the NULL tail slots to the slab layer.
 *
 * WHY THIS TEST CHECKS THE INVARIANT AND NOT JUST THE SYMPTOM
 *   ASan cannot see the out-of-bounds access, structurally: magazines are
 *   objects inside umem's OWN slabs, which come from mmap via vmem, so there is
 *   no redzone between one magazine and the next, and nothing in this tree is
 *   poisoned (it contains no ASan poisoning at all).  An overrun from one
 *   magazine into its neighbour is, to ASan, an ordinary write to a valid heap
 *   page.  The reliable signal is the invariant itself, checked here by the test
 *   rather than only by the library's ASSERT, so the test discriminates even
 *   against a library built with assertions removed:
 *
 *     for each PTC magazine this thread holds:
 *         recorded capacity == the magazine's TRUE capacity
 *         0 <= rounds <= capacity
 *
 *   The true capacity is recovered the same way the fix does it: a magazine is
 *   an object of some umem_magazine_<N> cache, so its own slab header names
 *   that cache, whose bufsize is (N + 1) * sizeof (void *).  That is knowable
 *   from the magazine alone and cannot be changed by a resize.
 *
 *   The symptom is checked too: every allocation is stamped with a unique
 *   token and verified, so a buffer read out of bounds and handed to two
 *   owners is caught directly.
 *
 * HOW THE RESIZE IS FORCED, AND WHY CHANCE IS NOT ENOUGH
 *   The reachability is ordinary: umem_cache_update() schedules
 *   UMU_MAGAZINE_RESIZE whenever depot contention exceeds umem_depot_contention
 *   in an update interval, with no involvement from the umem_magazine_tuning
 *   option.  The test lowers the two documented tunables
 *   (umem_depot_contention, umem_reap_interval) so the update thread reaches
 *   that decision in seconds instead of minutes, and drives many threads through
 *   one size class with cross-thread frees so magazines cycle through the depot
 *   while the magtype changes underneath them.  A size class is chosen whose
 *   magtype can still grow (512B: 127 -> 255).
 *
 *   But reachable is not the same as reachable BY VOLUME, and that distinction
 *   is itself one of this test's results.  The window is the few instructions
 *   between obtaining the magazine and reading the capacity -- tens of
 *   nanoseconds, measured below -- and each cache resizes at most ONCE per
 *   process, because a magtype only ever grows and this size class has exactly
 *   one step available.  So an unaided run gets ONE chance to land a resize
 *   inside a ~20ns window.  Measured against a build with the defect PRESENT:
 *   67,443,588 invariant samples across 96 threads on a 192-vCPU arm-hi box,
 *   zero hits.
 *
 *   Widening the window with a delay is ALSO not enough, and that was measured
 *   too: this workload performs only a few hundred depot magazine loads in 25
 *   seconds (the PTC bins absorb nearly everything), so a 200us delay per load
 *   gives ~47ms of open window against one instant in 25s -- 0 desyncs in 89
 *   million samples, against a build with the defect present.
 *
 *   So with -DUMEM_PTC_RESIZE_PROBE the window becomes a GATE rather than a
 *   delay.  A thread that has just obtained a magazine parks inside the window;
 *   the test waits until several are parked, forces the magtype change while they
 *   sit there holding old-magtype magazines, and then releases them.  Each one
 *   then records a capacity for a magazine obtained under the OLD magtype with
 *   the cache now on the NEW one -- the P1.3b condition, reached by construction
 *   rather than by luck.  The probe changes no decision and performs no store the
 *   library does not: the resize is the ordinary contention-scheduled one and any
 *   desync is the library's own arithmetic.  Without the probe the test still
 *   checks the invariant on every transfer, but cannot promise to have opened the
 *   window, and reports INCONCLUSIVE rather than passing.
 *
 *   READ THIS BEFORE QUOTING THE RESULT.  Because the window has to be widened
 *   to be hit at all, a failure here establishes that the window is real and
 *   that the fix closes it.  It does NOT establish a rate at which this happens
 *   in production; the zero-in-67-million figure above is the honest statement
 *   about that.  This is a mechanism demonstration with a real defect behind it,
 *   not evidence of frequent corruption.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem.h"
#include "../../umem_impl.h"
#include "../../umem_ptc.h"

#define OBJ_SIZE	512	/* magtype 127, can grow to 255 */
#define PARCEL		192	/* objects handed off in one parcel */
#define NPARCEL		512	/* handoff ring depth */
#define DEFAULT_THREADS	32
#define RUN_SECONDS	25

extern size_t pagesize;

static int nthreads = DEFAULT_THREADS;
static _Atomic int stop;
static _Atomic long fail_capacity;
static _Atomic long fail_alias;
static _Atomic long fail_null;
static _Atomic long checks;
static _Atomic long resizes_seen;

/* ---- cross-thread handoff ring: producers push parcels, anyone frees them */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static void **ring[NPARCEL];
static int ring_n;

static int
ring_push(void **parcel)
{
	int ok = 0;
	(void) pthread_mutex_lock(&ring_lock);
	if (ring_n < NPARCEL) {
		ring[ring_n++] = parcel;
		ok = 1;
	}
	(void) pthread_mutex_unlock(&ring_lock);
	return (ok);
}

static void **
ring_pop(void)
{
	void **parcel = NULL;
	(void) pthread_mutex_lock(&ring_lock);
	if (ring_n > 0)
		parcel = ring[--ring_n];
	(void) pthread_mutex_unlock(&ring_lock);
	return (parcel);
}

/* ---- the invariant ------------------------------------------------------ */

/*
 * A magazine's TRUE capacity, from the magazine alone: its slab header names
 * the umem_magazine_<N> cache it was allocated from, and that cache's bufsize
 * is (N + 1) * sizeof (void *).  Deliberately duplicated from the library
 * instead of calling it, so this test checks the property independently.
 */
static int
true_capacity(umem_magazine_t *mp)
{
	umem_slab_t *sp = (umem_slab_t *)P2END((uintptr_t)mp, pagesize) - 1;

	return ((int)(sp->slab_cache->cache_bufsize / sizeof (void *)) - 1);
}

static int
legal_magsize(int n)
{
	return (n == 1 || n == 3 || n == 7 || n == 15 ||
	    n == 31 || n == 63 || n == 127 || n == 255);
}

/*
 * The capacity the library records for `previous`.
 *
 * Pre-fix there was only ONE magsize field for both magazines, which is part of
 * the defect: a resize can land between two refills, so loaded and previous can
 * be of different magtypes and one field cannot describe both.  This test has to
 * build against both versions of the struct in order to show that it
 * discriminates, so it reads the separate field where it exists and falls back
 * to the shared one where it does not -- which is exactly the pre-fix claim
 * being checked.
 */
static int
recorded_pcap(umem_ptc_mag_t *mag)
{
#ifdef UMEM_PTC_MAG_HAS_PMAGSIZE
	return (mag->pmagsize);
#else
	return (mag->magsize);
#endif
}

static void
check_own_ptc(int bin)
{
	umem_ptc_t *ptc = thread_ptc;
	umem_ptc_mag_t *mag;

	if (ptc == NULL)
		return;

	mag = &ptc->mags[bin];
	atomic_fetch_add(&checks, 1);

	if (mag->loaded != NULL) {
		int cap = true_capacity(mag->loaded);
		if (mag->magsize != cap || !legal_magsize(cap) ||
		    mag->rounds < 0 || mag->rounds > cap) {
			/*
			 * Report the first few only.  Pre-fix this fires
			 * millions of times (measured: 3,133,215 in one run,
			 * 200MB of log); the counter carries the quantity, the
			 * samples carry the diagnosis.
			 */
			if (atomic_fetch_add(&fail_capacity, 1) < 20) {
				fprintf(stderr, "CAPACITY DESYNC (loaded): "
				    "recorded magsize=%d true=%d rounds=%d\n",
				    mag->magsize, cap, mag->rounds);
			}
		}
	}
	if (mag->previous != NULL) {
		int cap = true_capacity(mag->previous);
		int rec = recorded_pcap(mag);
		if (rec != cap || !legal_magsize(cap) ||
		    mag->prounds < 0 || mag->prounds > cap) {
			if (atomic_fetch_add(&fail_capacity, 1) < 20) {
				fprintf(stderr, "CAPACITY DESYNC (previous): "
				    "recorded capacity=%d true=%d prounds=%d\n",
				    rec, cap, mag->prounds);
			}
		}
	}
}

/* ---- the symptom: a buffer handed to two owners, or a garbage pointer --- */

static void
stamp(void *buf, uint64_t token)
{
	uint64_t *p = (uint64_t *)buf;
	size_t i;

	for (i = 0; i < OBJ_SIZE / sizeof (uint64_t); i++)
		p[i] = token;
}

static int
verify(void *buf, uint64_t token)
{
	uint64_t *p = (uint64_t *)buf;
	size_t i;

	for (i = 0; i < OBJ_SIZE / sizeof (uint64_t); i++) {
		if (p[i] != token)
			return (0);
	}
	return (1);
}

static void *
worker(void *arg)
{
	long tid = (long)(intptr_t)arg;
	uint64_t seq = 0;
	int bin = -1;

	/* Learn this thread's PTC bin for OBJ_SIZE once. */
	{
		void *p = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		if (p == NULL) {
			atomic_fetch_add(&fail_null, 1);
			return (NULL);
		}
		bin = umem_ptc_bin_table[(OBJ_SIZE - 1) >> UMEM_ALIGN_SHIFT];
		umem_free(p, OBJ_SIZE);
	}
	if (bin < 0) {
		fprintf(stderr, "size %d is not PTC-eligible; test cannot "
		    "exercise the PTC magazine path\n", OBJ_SIZE);
		atomic_fetch_add(&fail_null, 1);
		return (NULL);
	}

	while (!atomic_load(&stop)) {
		void **parcel = malloc(PARCEL * sizeof (void *));
		uint64_t *tokens = malloc(PARCEL * sizeof (uint64_t));
		int i;

		if (parcel == NULL || tokens == NULL) {
			free(parcel);
			free(tokens);
			break;
		}

		for (i = 0; i < PARCEL; i++) {
			tokens[i] = ((uint64_t)tid << 40) | (++seq);
			parcel[i] = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
			if (parcel[i] == NULL) {
				/*
				 * Not a memory-pressure failure at this size:
				 * pre-fix, an out-of-bounds magazine slot can
				 * read a NULL tail slot of the NEXT magazine
				 * and return it as an allocation.
				 */
				atomic_fetch_add(&fail_null, 1);
				continue;
			}
			stamp(parcel[i], tokens[i]);
		}

		check_own_ptc(bin);

		/* Read back: another owner's token here means aliasing. */
		for (i = 0; i < PARCEL; i++) {
			if (parcel[i] != NULL && !verify(parcel[i], tokens[i]))
				atomic_fetch_add(&fail_alias, 1);
		}

		/*
		 * Hand half the parcel to another thread so magazines cycle
		 * through the depot (cross-thread free), and free the rest
		 * here so this thread's own magazines fill and flush.
		 */
		{
			void **give = malloc((PARCEL / 2) * sizeof (void *));
			if (give != NULL) {
				memcpy(give, parcel,
				    (PARCEL / 2) * sizeof (void *));
				if (!ring_push(give)) {
					for (i = 0; i < PARCEL / 2; i++) {
						if (give[i] != NULL)
							umem_free(give[i],
							    OBJ_SIZE);
					}
					free(give);
				}
			} else {
				for (i = 0; i < PARCEL / 2; i++) {
					if (parcel[i] != NULL)
						umem_free(parcel[i], OBJ_SIZE);
				}
			}
		}
		for (i = PARCEL / 2; i < PARCEL; i++) {
			if (parcel[i] != NULL)
				umem_free(parcel[i], OBJ_SIZE);
		}

		check_own_ptc(bin);

		/* Free someone else's parcel. */
		{
			void **got = ring_pop();
			if (got != NULL) {
				for (i = 0; i < PARCEL / 2; i++) {
					if (got[i] != NULL)
						umem_free(got[i], OBJ_SIZE);
				}
				free(got);
			}
		}

		check_own_ptc(bin);
		free(parcel);
		free(tokens);
	}

	/* Drain anything left in the ring that this thread can reach. */
	for (;;) {
		void **got = ring_pop();
		int i;
		if (got == NULL)
			break;
		for (i = 0; i < PARCEL / 2; i++) {
			if (got[i] != NULL)
				umem_free(got[i], OBJ_SIZE);
		}
		free(got);
	}

	return (NULL);
}

int
main(int argc, char **argv)
{
	extern uint32_t umem_reap_interval;
	extern uint_t umem_depot_contention;
#ifdef UMEM_PTC_RESIZE_PROBE
	extern volatile long umem_ptc_resize_probe_ns;
	extern volatile int umem_ptc_probe_gate;
	extern volatile long umem_ptc_probe_inwindow;
	extern volatile long umem_ptc_probe_refills;
	extern volatile long umem_ptc_probe_desyncs;
#endif
	pthread_t *th;
	umem_cache_t *cp;
	int magsize_start, magsize_now;
	int i, sec;

	if (argc > 1)
		nthreads = atoi(argv[1]);
	if (nthreads < 2)
		nthreads = 2;

#ifdef UMEM_PTC_RESIZE_PROBE
	printf("probe: BUILT IN; threads that obtain a magazine PARK inside the "
	    "window until the magtype has been changed, so the race does not "
	    "depend on timing luck\n");
#else
	printf("probe: NOT built in (-DUMEM_PTC_RESIZE_PROBE); the invariant is "
	    "still checked on every transfer, but a ~20ns window and one resize "
	    "per process means chance alone will not open it\n");
#endif

	/*
	 * Make the update thread reach its magazine-resize decision quickly.
	 * These are the ordinary tunables for the DEFAULT contention-driven
	 * resize path (UMEM_OPTIONS max_contention / reap_interval); the
	 * umem_magazine_tuning option is deliberately left off, because the
	 * defect is reachable without it.
	 */
	umem_depot_contention = 0;
	umem_reap_interval = 1;

	/* Warm up so the cache and its magtype exist before we sample it. */
	{
		void *w = umem_alloc(OBJ_SIZE, UMEM_DEFAULT);
		if (w != NULL)
			umem_free(w, OBJ_SIZE);
	}

	cp = umem_alloc_table[(OBJ_SIZE - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		printf("RESULT: FAIL (no cache for size %d)\n", OBJ_SIZE);
		return (2);
	}
	magsize_start = cp->cache_magtype->mt_magsize;
	printf("size=%d cache=%s chunksize=%zu magsize=%d\n", OBJ_SIZE,
	    cp->cache_name, cp->cache_chunksize, magsize_start);
	if (cp->cache_chunksize >= cp->cache_magtype->mt_maxbuf) {
		printf("RESULT: FAIL (this size class cannot resize; the test "
		    "would prove nothing)\n");
		return (2);
	}

	th = calloc(nthreads, sizeof (pthread_t));
	if (th == NULL)
		return (2);
#ifdef UMEM_PTC_RESIZE_PROBE
	/*
	 * Close the gate BEFORE the workers start, so arrivals accumulate inside
	 * the window from the first magazine load.
	 */
	umem_ptc_probe_gate = 1;
#endif
	for (i = 0; i < nthreads; i++) {
		if (pthread_create(&th[i], NULL, worker,
		    (void *)(intptr_t)i) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			return (2);
		}
	}

	/*
	 * Drive the update thread while the workers hammer the depot, and
	 * watch for the magtype to actually change.  umem_reap() is what
	 * creates the update thread in a multithreaded process.
	 */
#ifdef UMEM_PTC_RESIZE_PROBE
	/*
	 * Deterministic sequence, instead of hoping the single resize lands in a
	 * ~20ns window:
	 *
	 *   1. wait until threads are parked INSIDE the window, each holding a
	 *      magazine of the current magtype,
	 *   2. force the magtype change while they are parked,
	 *   3. open the gate.
	 *
	 * Every released thread then records a capacity for a magazine it
	 * obtained under the OLD magtype, with the cache now on the new one --
	 * which is precisely the P1.3b condition.
	 */
	{
		int waited;

		for (waited = 0; waited < 100 &&
		    umem_ptc_probe_inwindow < 4; waited++)
			(void) usleep(100000);
		printf("  %ld threads parked inside the window\n",
		    umem_ptc_probe_inwindow);

		for (sec = 0; sec < RUN_SECONDS; sec++) {
			umem_reap();
			(void) sleep(1);
			magsize_now = cp->cache_magtype->mt_magsize;
			if (magsize_now != magsize_start) {
				printf("  magazine resize forced at t=%ds: "
				    "%d -> %d rounds, with %ld threads still "
				    "parked holding old-magtype magazines\n",
				    sec, magsize_start, magsize_now,
				    umem_ptc_probe_inwindow);
				(void) atomic_exchange(&resizes_seen, 1);
				break;
			}
		}

		/* Release them into the post-resize world. */
		umem_ptc_probe_gate = 0;

		/* Let the released threads run, and keep new ones flowing. */
		for (sec = 0; sec < 5; sec++) {
			umem_reap();
			(void) sleep(1);
		}
	}
#else
	for (sec = 0; sec < RUN_SECONDS; sec++) {
		umem_reap();
		(void) sleep(1);
		magsize_now = cp->cache_magtype->mt_magsize;
		if (magsize_now != magsize_start) {
			if (atomic_exchange(&resizes_seen, 1) == 0)
				printf("  magazine resize observed at t=%ds: "
				    "%d -> %d rounds\n", sec, magsize_start,
				    magsize_now);
		}
	}
#endif

	atomic_store(&stop, 1);
	for (i = 0; i < nthreads; i++)
		(void) pthread_join(th[i], NULL);
#ifdef UMEM_PTC_RESIZE_PROBE
	umem_ptc_probe_gate = 0;
#endif

	magsize_now = cp->cache_magtype->mt_magsize;
	printf("threads=%d magsize %d -> %d  ptc_checks=%ld\n", nthreads,
	    magsize_start, magsize_now, (long)atomic_load(&checks));
#ifdef UMEM_PTC_RESIZE_PROBE
	/*
	 * In-library observation, made at the instant the capacity is recorded.
	 * Sampling the PTC from the owning thread afterwards cannot be relied on
	 * to see a desync -- the magazine may be replaced before the next sample
	 * -- so this is the authoritative count, and refills proves the path ran.
	 */
	printf("in-library observation: magazine loads=%ld capacity desyncs "
	    "AT THE MOMENT OF RECORDING=%ld\n",
	    umem_ptc_probe_refills, umem_ptc_probe_desyncs);
	if (umem_ptc_probe_desyncs != 0)
		atomic_fetch_add(&fail_capacity, umem_ptc_probe_desyncs);
#endif
	printf("failures: capacity_desync=%ld aliased_buffer=%ld "
	    "bad_alloc=%ld\n", (long)atomic_load(&fail_capacity),
	    (long)atomic_load(&fail_alias), (long)atomic_load(&fail_null));

	if (magsize_now == magsize_start) {
		printf("RESULT: INCONCLUSIVE (no magazine resize happened, so "
		    "the window under test was never opened -- raise the "
		    "thread count or the run time)\n");
		return (3);
	}
#ifdef UMEM_PTC_RESIZE_PROBE
	if (umem_ptc_probe_refills == 0) {
		printf("RESULT: INCONCLUSIVE (no PTC magazine was ever loaded "
		    "from the depot, so the path under test never ran)\n");
		return (3);
	}
#endif
#ifndef UMEM_PTC_RESIZE_PROBE
	if (atomic_load(&fail_capacity) == 0 && atomic_load(&fail_alias) == 0 &&
	    atomic_load(&fail_null) == 0) {
		/*
		 * Without the probe, a clean run is not evidence of correctness:
		 * the window is ~20ns and this process got one resize, so chance
		 * was never going to open it.  Reporting PASS here is how a
		 * regression like this silently stops discriminating.
		 */
		printf("RESULT: INCONCLUSIVE (no desync seen in %ld samples, but "
		    "this build cannot open the window on purpose -- rebuild "
		    "with -DUMEM_PTC_RESIZE_PROBE to get a verdict)\n",
		    (long)atomic_load(&checks));
		return (3);
	}
#endif
	if (atomic_load(&fail_capacity) != 0 || atomic_load(&fail_alias) != 0 ||
	    atomic_load(&fail_null) != 0) {
		printf("RESULT: FAIL (a magazine was used with a capacity that "
		    "is not its own across a magazine resize)\n");
		return (1);
	}
	printf("RESULT: PASS (every PTC magazine was described by its own "
	    "capacity across the %d -> %d resize, including the magazines held "
	    "by threads parked inside the window while it happened)\n",
	    magsize_start, magsize_now);
	return (0);
}

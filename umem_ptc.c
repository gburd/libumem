/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>

#include "umem_ptc.h"
#include "umem_base.h"
#include "umem_impl.h"

_Static_assert(_UMEM_PTC_CACHE_LINE == UMEM_CACHE_LINE_SIZE,
    "PTC cache line size must match UMEM_CACHE_LINE_SIZE");

/*
 * External reference to umem_alloc_table
 */
extern umem_cache_t *umem_alloc_table[];

/*
 * Global configuration (can be tuned via UMEM_OPTIONS)
 */
size_t umem_ptc_maxsize = 8192;      /* max cached size */
int umem_ptc_enabled = 1;            /* UMEM_OPTIONS=ptc=0 disables */

/*
 * Thread-local storage for ptc
 * Using __thread with initial-exec TLS model for fast access.
 * Non-static so umem.c can inline the PTC fast path.
 */
__thread umem_ptc_t *thread_ptc
    __attribute__((tls_model("initial-exec"))) = NULL;

/*
 * pthread key for cleanup
 */
static pthread_key_t ptc_key;
static int ptc_key_initialized = 0;

/*
 * The cache umem_ptc_t structs come from (P5.12).
 *
 * They used to come from umem_alloc(sizeof (umem_ptc_t)), i.e. from the
 * umem_alloc_24576 size class, in the same slabs as every user allocation of
 * 20481..24576 bytes.  A umem_ptc_t is an array of slot POINTERS: its first
 * field is bins[0].slots (a pointer into the pool), and pool[] holds the
 * cached objects' addresses that the next umem_alloc() hands out unchecked.
 * A user buffer in that class overrun into the next slab object could
 * therefore rewrite what the allocator returns next -- attacker position D
 * (controls allocation patterns and buffer contents).  glibc's tcache has
 * the same adjacency and mitigates it with safe-linking on the entries;
 * libumem's slots were raw.  P5.4 mangles the freelist links inside FREED
 * buffers for the same reason and did not reach this structure.
 *
 * Fix: a dedicated UMC_INTERNAL cache in umem_internal_arena, which is
 * what the magazines (umem_magazine_N) already get.  Its slabs hold only
 * umem_ptc_t objects, so no user buffer is ever adjacent to one.  The
 * arena is separate address space from the user heap (its spans are
 * imported from the heap arena, not shared with umem_default_arena).  This
 * does not mangle the slots; P5.13 does (UMEM_SLOT_MANGLE, umem_impl.h), so
 * the two are independent: adjacency removed here, chosen-address writes
 * defeated there.
 */
struct umem_cache *umem_ptc_cache;	/* set by umem_cache_init() */

#ifdef UMEM_PTC_RESIZE_PROBE
volatile long umem_ptc_probe_exit_stranded = 0;
/*
 * P6.3 ledger: lock-taking hand-offs made to drain bins at thread exit, and
 * the number of non-empty bins drained.  Pre-fix every object was its own
 * hand-off (one cc_lock each), so handoffs == objects; post-fix a bin is one
 * hand-off, so handoffs == bins.  Exact in both builds.
 */
volatile long umem_ptc_probe_exit_handoffs = 0;
volatile long umem_ptc_probe_exit_bins = 0;
/*
 * P1.3d ledger: PTCs the fork child drained, and PTCs it had to leak because
 * the snapshot caught them mid-swap (fork_busy).  drained + leaked == the
 * number of non-forking threads that had a PTC; leaked is expected to be 0
 * in a quiescent fork and small under load.
 */
volatile long umem_ptc_probe_fork_drained = 0;
volatile long umem_ptc_probe_fork_busy_leaked = 0;
volatile long umem_ptc_probe_fork_top_dropped = 0;	/* bins' top slots leaked */
#define	UMEM_PTC_PROBE_COUNT(var, n)					\
	(void) __atomic_add_fetch(&(var), (long)(n), __ATOMIC_RELAXED)
#define	UMEM_PTC_PROBE_STRANDED(n)					\
	do {								\
		if ((n) > 0)						\
			(void) __atomic_add_fetch(			\
			    &umem_ptc_probe_exit_stranded, (long)(n),	\
			    __ATOMIC_RELAXED);				\
	} while (0)
#else
#define	UMEM_PTC_PROBE_STRANDED(n)	((void)0)
#define	UMEM_PTC_PROBE_COUNT(var, n)	((void)0)
#endif

/*
 * Registry of live PTCs (P1.3d part 2).  See umem_ptc_t.reg_next in
 * umem_ptc.h for what it is for.  umem_ptc_list_lock is in the fork lock
 * order (umem_fork.c, step 1a: taken right after the interposer locks and
 * before umem_init_lock, because umem_ptc_get() takes it while holding no
 * allocator lock and nothing takes an allocator lock while holding it).
 */
static umem_ptc_t *umem_ptc_list;
static pthread_mutex_t umem_ptc_list_lock = PTHREAD_MUTEX_INITIALIZER;

static void
umem_ptc_register(umem_ptc_t *ptc)
{
	(void) pthread_mutex_lock(&umem_ptc_list_lock);
	ptc->reg_prev = NULL;
	ptc->reg_next = umem_ptc_list;
	if (umem_ptc_list != NULL)
		umem_ptc_list->reg_prev = ptc;
	umem_ptc_list = ptc;
	(void) pthread_mutex_unlock(&umem_ptc_list_lock);
}

static void
umem_ptc_unregister(umem_ptc_t *ptc)
{
	(void) pthread_mutex_lock(&umem_ptc_list_lock);
	if (ptc->reg_prev != NULL)
		ptc->reg_prev->reg_next = ptc->reg_next;
	else if (umem_ptc_list == ptc)
		umem_ptc_list = ptc->reg_next;
	if (ptc->reg_next != NULL)
		ptc->reg_next->reg_prev = ptc->reg_prev;
	ptc->reg_next = ptc->reg_prev = NULL;
	(void) pthread_mutex_unlock(&umem_ptc_list_lock);
}

void
umem_ptc_fork_lockup(void)
{
	(void) pthread_mutex_lock(&umem_ptc_list_lock);
}

void
umem_ptc_fork_release(void)
{
	(void) pthread_mutex_unlock(&umem_ptc_list_lock);
}

/*
 * Fork child: drain every PTC that belonged to a thread that did not survive
 * the fork, i.e. every registered PTC except the forking thread's own.
 *
 * Runs from the child's atfork handler AFTER every allocator lock has been
 * released (umem_fork.c), because the drain allocates and frees.  The
 * registry lock is re-initialised rather than unlocked: its owner at the
 * snapshot may have been a thread that no longer exists.
 *
 * A PTC snapshotted with fork_busy set is mid-swap in the parent (P1.3d
 * rule 2) and its loaded/previous may alias; draining it would free one
 * magazine twice.  Those are unlinked and LEAKED -- the pre-P1.3d behaviour
 * for every PTC -- and counted under the probe build.  Everything else is
 * consistent by rule 1 and goes through umem_ptc_destroy(), the same drain
 * a thread exit does.
 */
void
umem_ptc_fork_release_child(void)
{
	umem_ptc_t *p, *next;
	pthread_t self = pthread_self();

	(void) pthread_mutex_init(&umem_ptc_list_lock, NULL);

	p = umem_ptc_list;
	umem_ptc_list = NULL;
	for (; p != NULL; p = next) {
		next = p->reg_next;
		p->reg_next = p->reg_prev = NULL;
		if (pthread_equal(p->owner, self)) {
			/* The forking thread's own PTC: still live, relink. */
			p->reg_next = umem_ptc_list;
			if (umem_ptc_list != NULL)
				umem_ptc_list->reg_prev = p;
			umem_ptc_list = p;
			continue;
		}
		if (p->fork_busy) {
			UMEM_PTC_PROBE_COUNT(umem_ptc_probe_fork_busy_leaked, 1);
			continue;
		}
		/*
		 * Pushes are not fork-ordered (umem_ptc.h, rule 1), so the top
		 * entry of every non-empty bin and of each loaded/previous
		 * magazine may be a stale pointer over a count bumped one too
		 * far.  Drop them: leak at most 36 + 2 objects per PTC rather
		 * than risk freeing something the application owns.  A dropped
		 * magazine round leaves a non-NULL stale entry at mag_round
		 * [rounds]; umem_mag_drain() reads only [0, rounds) so it is
		 * never touched.
		 */
		{
			int b, dropped = 0;

			for (b = 0; b < PTC_NBINS; b++) {
				if (p->bins[b].count > 0) {
					p->bins[b].count--;
					dropped++;
				}
				if (p->mags[b].loaded != NULL &&
				    p->mags[b].rounds > 0) {
					p->mags[b].rounds--;
					dropped++;
				}
				if (p->mags[b].previous != NULL &&
				    p->mags[b].prounds > 0) {
					p->mags[b].prounds--;
					dropped++;
				}
			}
			UMEM_PTC_PROBE_COUNT(umem_ptc_probe_fork_top_dropped,
			    dropped);
		}
		UMEM_PTC_PROBE_COUNT(umem_ptc_probe_fork_drained, 1);
		umem_ptc_destroy(p);
	}
}

/*
 * Size class table for quick bin lookup
 * Maps allocation sizes to bin indices, indexed by size / 8, so it must
 * reach the largest PTC class / 8 (8192 / 8 = 1024).
 * Zero-initialized by C static storage rules; umem_ptc_init() fills it
 * with valid bin indices (and -1 for unmapped entries).  The
 * ptc_table_ready flag guards against use before initialization,
 * since a zero entry would silently alias every size to bin 0.
 */
#define	PTC_SIZE_TO_BIN_ENTRIES	(8192 / 8 + 1)
static int8_t size_to_bin_table[PTC_SIZE_TO_BIN_ENTRIES];
static int ptc_table_ready;

/*
 * Pre-computed bin table indexed by umem_alloc_table index.
 * For each index in [0, UMEM_MAXBUF >> UMEM_ALIGN_SHIFT), stores the
 * PTC bin index or -1 if not PTC-eligible. Populated by umem_ptc_init().
 */
int8_t umem_ptc_bin_table[UMEM_MAXBUF >> UMEM_ALIGN_SHIFT] = { [0 ... (UMEM_MAXBUF >> UMEM_ALIGN_SHIFT) - 1] = -1 };

/*
 * Size classes we cache: the umem_alloc_sizes entries up to 8192.  Bin
 * index also selects a capacity tier (ptc_bin_capacity: PTC_BIN_MEDIUM /
 * LARGE / XLARGE thresholds).  On LP64 the three 0 entries after 2048 are
 * padding so 2560 lands at PTC_BIN_XLARGE, so this is NOT "the first
 * PTC_NBINS entries of umem_alloc_sizes"; the table builder skips zeros.
 */
static const size_t ptc_size_classes[PTC_NBINS] = {
#ifdef _LP64
	8, 16, 32, 48,          /* 1*8, 1*16, 2*16, 3*16 */
	64, 80, 96, 112,        /* 4*16, 5*16, 6*16, 7*16 */
	128, 160, 192, 224,     /* 4*32, 5*32, 6*32, 7*32 */
	256, 320, 384, 448,     /* 4*64, 5*64, 6*64, 7*64 */
	512, 640, 768, 896,     /* 4*128, 5*128, 6*128, 7*128 */
	1024, 1280, 1536, 1792, /* 4*256, 5*256, 6*256, 7*256 */
	2048, 0, 0, 0,          /* 8*256, padding to PTC_BIN_XLARGE */
	2560, 3072, 3584, 4096, /* 5*512, 6*512, 7*512, 8*512 */
	5120, 6144, 7168, 8192  /* 5*1024, 6*1024, 7*1024, 8*1024 */
#else
	8, 16, 24, 32,          /* 1*8, 2*8, 3*8, 4*8 */
	40, 48, 56, 64,         /* 5*8, 6*8, 7*8, 4*16 */
	80, 96, 112, 128,       /* 5*16, 6*16, 7*16, 4*32 */
	160, 192, 224, 256,     /* 5*32, 6*32, 7*32, 4*64 */
	320, 384, 448, 512,     /* 5*64, 6*64, 7*64, 4*128 */
	640, 768, 896, 1024,    /* 5*128, 6*128, 7*128, 4*256 */
	1280, 1536, 1792, 2048, /* 5*256, 6*256, 7*256, 8*256 */
	2560, 3072, 3584, 4096, /* 5*512, 6*512, 7*512, 8*512 */
	5120, 6144, 7168, 8192  /* 5*1024, 6*1024, 7*1024, 8*1024 */
#endif
};

/*
 * Cleanup callback for pthread_key
 */
static void
umem_ptc_cleanup(void *arg)
{
	umem_ptc_t *ptc = (umem_ptc_t *)arg;

	if (ptc != NULL) {
		umem_ptc_unregister(ptc);
		umem_ptc_destroy(ptc);
		thread_ptc = NULL;
	}
}

/*
 * Initialize ptc subsystem
 */
void
umem_ptc_init(void)
{
	int i, bin;
	size_t size;

	if (!umem_ptc_enabled) {
		return;
	}

	/* Initialize pthread key for cleanup */
	if (!ptc_key_initialized) {
		if (pthread_key_create(&ptc_key, umem_ptc_cleanup) != 0) {
			umem_ptc_enabled = 0;
			return;
		}
		ptc_key_initialized = 1;
	}

	/*
	 * Own cache, own slabs, no user neighbours (P5.12).  Created by
	 * umem_cache_init() in umem.c alongside the magazine caches, because
	 * umem_internal_arena is private to that file; if it did not get made,
	 * there is no PTC.
	 */
	if (umem_ptc_cache == NULL) {
		umem_ptc_enabled = 0;
		return;
	}

	/* Build size-to-bin lookup table */
	for (i = 0; i < (int)(sizeof(size_to_bin_table)); i++) {
		size_to_bin_table[i] = -1;
	}

	for (bin = 0; bin < PTC_NBINS; bin++) {
		size = ptc_size_classes[bin];
		if (size / 8 < sizeof(size_to_bin_table)) {
			size_to_bin_table[size / 8] = bin;
		}
	}

	/*
	 * Disable PTC for size classes whose backing cache has debug
	 * flags enabled. PTC bypasses the magazine layer and would
	 * skip UMF_AUDIT/UMF_DEADBEEF/UMF_REDZONE checking.
	 */
	for (bin = 0; bin < PTC_NBINS; bin++) {
		umem_cache_t *cp;
		size = ptc_size_classes[bin];
		if (size == 0)
			continue;
		cp = umem_alloc_table[(size - 1) >> UMEM_ALIGN_SHIFT];
		if (cp != NULL &&
		    (cp->cache_flags &
		    (UMF_AUDIT | UMF_DEADBEEF | UMF_REDZONE))) {
			if (size / 8 < sizeof(size_to_bin_table)) {
				size_to_bin_table[size / 8] = -1;
			}
		}
	}

	/*
	 * Build umem_ptc_bin_table: for each alloc_table index,
	 * compute the PTC bin. This allows umem.c to inline the
	 * PTC lookup without calling umem_ptc_size_to_bin().
	 *
	 * A request of (idx+1)<<UMEM_ALIGN_SHIFT bytes is served by the
	 * cache umem_alloc_table[idx], whose object size is one of the umem
	 * size classes (which == the PTC size classes for the cached range).
	 * We must map the index to the PTC bin of the *backing cache's*
	 * object size, NOT of the raw request rounded to 8: rounding to 8
	 * lands between size classes (e.g. a 176-byte request rounds to
	 * bin_idx 22, which is not a PTC class, so it was wrongly marked -1
	 * even though its backing umem_alloc_192 cache has PTC bin 10). That
	 * gap forced every 161-176B (and similar) allocation onto the locked
	 * cc_lock path -- the same-size-class scaling bottleneck.
	 */
	{
		size_t idx;
		size_t table_size = UMEM_MAXBUF >> UMEM_ALIGN_SHIFT;

		for (idx = 0; idx < table_size; idx++) {
			size_t alloc_size = (idx + 1) << UMEM_ALIGN_SHIFT;
			umem_cache_t *cp;
			size_t obj_size;
			size_t bin_idx;

			if (alloc_size > umem_ptc_maxsize) {
				umem_ptc_bin_table[idx] = -1;
				continue;
			}

			/*
			 * Map through the backing cache's object size so the
			 * PTC bin matches the size class actually allocated.
			 */
			cp = umem_alloc_table[idx];
			obj_size = (cp != NULL) ? cp->cache_bufsize :
			    ((alloc_size + 7) & ~(size_t)7);

			if (obj_size > umem_ptc_maxsize) {
				umem_ptc_bin_table[idx] = -1;
				continue;
			}

			bin_idx = obj_size / 8;
			if (bin_idx >= sizeof(size_to_bin_table)) {
				umem_ptc_bin_table[idx] = -1;
			} else {
				umem_ptc_bin_table[idx] =
				    size_to_bin_table[bin_idx];
			}
		}
	}

	/*
	 * ptc_table_ready is a plain int with no release store, and the
	 * inlined fast paths in umem.c (_umem_alloc/_umem_free) read
	 * umem_ptc_bin_table without consulting it.  What orders the
	 * tables before their readers is umem_init(): this runs inside it,
	 * and umem_init() publishes umem_ready = UMEM_READY under
	 * umem_init_lock with cond_broadcast, so any thread that waited on
	 * init sees the filled tables.  A thread that reads bin_table
	 * without ever taking umem_init_lock sees either the static -1 or
	 * the final value: entries are int8_t, written once, and -1 means
	 * "not PTC" which sends the call to the ordinary path.  The flag
	 * only guards umem_ptc_size_to_bin() and umem_sbo_enabled(), the
	 * out-of-line callers.
	 */
	ptc_table_ready = 1;
}

/*
 * Bin slot push/pop.  Slots are stored mangled (P5.13, UMEM_SLOT_MANGLE in
 * umem_impl.h); these are the only readers and writers in this file.  The
 * inlined fast paths in umem.c do the same transform in place.
 */
static inline void
ptc_slot_push(umem_ptc_bin_t *bin, void *ptr)
{
	void **sp = &bin->slots[bin->count++];

	*sp = UMEM_SLOT_MANGLE(sp, ptr);
}

static inline void *
ptc_slot_pop(umem_ptc_bin_t *bin)
{
	void **sp = &bin->slots[--bin->count];

	return (UMEM_SLOT_DEMANGLE(sp, *sp));
}

/*
 * Map allocation size to bin index
 * Returns -1 if size is not eligible for per-thread caching
 */
int
umem_ptc_size_to_bin(size_t size)
{
	size_t index;

	if (!umem_ptc_enabled || !ptc_table_ready || size > umem_ptc_maxsize) {
		return (-1);
	}

	/* Round up to 8-byte alignment */
	size = (size + 7) & ~7;

	index = size / 8;
	if (index >= sizeof(size_to_bin_table)) {
		return (-1);
	}

	return (size_to_bin_table[index]);
}

/*
 * Get the actual size for a bin
 */
static size_t
umem_ptc_bin_size(int bin)
{
	if (bin < 0 || bin >= PTC_NBINS) {
		return (0);
	}
	return (ptc_size_classes[bin]);
}

/*
 * Get or create the current thread's cache
 */
umem_ptc_t *
umem_ptc_get(void)
{
	umem_ptc_t *ptc;

	if (!umem_ptc_enabled) {
		return (NULL);
	}

	ptc = thread_ptc;
	if (ptc != NULL) {
		return (ptc);
	}

	/* Allocate new ptc using umem_alloc to avoid recursion */
	ptc = (umem_ptc_t *)umem_cache_alloc(umem_ptc_cache, UMEM_DEFAULT);
	if (ptc == NULL) {
		return (NULL);
	}

	(void) memset(ptc, 0, offsetof(umem_ptc_t, pool));

	/*
	 * Carve each bin's slot array out of the pool at its real capacity.
	 * Set once here; the fast paths in umem.c read bins[i].slots and never
	 * write it.  The pool itself is not zeroed: only slots[0 .. count) are
	 * ever read, and count starts at 0.
	 */
	{
		void **p = ptc->pool;
		int b;

		for (b = 0; b < PTC_NBINS; b++) {
			ptc->bins[b].slots = p;
			p += ptc_bin_capacity(b);
		}
		ASSERT(p == ptc->pool + PTC_TOTAL_SLOTS);
	}

	/* Store in TLS and pthread-specific data */
	thread_ptc = ptc;
	if (ptc_key_initialized) {
		/*
		 * If this fails there is no destructor for this thread, so its
		 * cached objects would be lost at exit rather than drained.
		 * Rather than cache silently-unreclaimable objects, decline to
		 * use a PTC on this thread: the caller falls back to the
		 * magazine layer, which is slower but loses nothing.
		 */
		if (pthread_setspecific(ptc_key, ptc) != 0) {
			thread_ptc = NULL;
			umem_cache_free(umem_ptc_cache, ptc);
			return (NULL);
		}
	}

	/* Fully built and owned; now visible to the fork child (P1.3d). */
	ptc->owner = pthread_self();
	umem_ptc_register(ptc);

	return (ptc);
}

/*
 * Allocate from thread cache
 */
void *
umem_ptc_alloc(size_t size)
{
	umem_ptc_t *ptc;
	umem_ptc_bin_t *bin;
	void *ptr;
	int bin_idx;

	if (!umem_ptc_enabled) {
		return (NULL);
	}

	bin_idx = umem_ptc_size_to_bin(size);
	if (bin_idx < 0) {
		return (NULL);
	}

	ptc = umem_ptc_get();
	if (ptc == NULL) {
		return (NULL);
	}

	bin = &ptc->bins[bin_idx];

	/* Fast path: take from cache */
	if (bin->count > 0) {
		ptr = ptc_slot_pop(bin);
		return (ptr);
	}

	/* Cache miss - try to refill from magazine layer */
	if (umem_ptc_bin_refill(bin, umem_ptc_bin_size(bin_idx)) == 0) {
		if (bin->count > 0) {
			ptr = ptc_slot_pop(bin);
			return (ptr);
		}
	}

	return (NULL);
}

/*
 * Free to thread cache
 */
int
umem_ptc_free(void *ptr, size_t size)
{
	umem_ptc_t *ptc;
	umem_ptc_bin_t *bin;
	int bin_idx;

	if (!umem_ptc_enabled || ptr == NULL) {
		return (-1);
	}

	bin_idx = umem_ptc_size_to_bin(size);
	if (bin_idx < 0) {
		return (-1);
	}

	ptc = umem_ptc_get();
	if (ptc == NULL) {
		return (-1);
	}

	bin = &ptc->bins[bin_idx];

	/* Fast path: cache it */
	if (bin->count < ptc_bin_capacity(bin_idx)) {
		ptc_slot_push(bin, ptr);
		return (0);
	}

	/* Cache full - flush half to magazine layer */
	umem_ptc_bin_flush(bin, umem_ptc_bin_size(bin_idx));

	/* Try again after flush */
	if (bin->count < ptc_bin_capacity(bin_idx)) {
		ptc_slot_push(bin, ptr);
		return (0);
	}

	return (-1);
}

/*
 * Flush cached objects from a bin to the magazine layer.
 *
 * `all` selects the policy:
 *   0 -- flush about half, the steady-state behaviour when a bin overflows:
 *        keep some objects for reuse so the thread does not immediately have
 *        to refill.
 *   1 -- flush EVERY object.  Required at thread exit: the bin is about to be
 *        freed, so anything left behind loses its only reference while the
 *        slab layer still counts it as allocated.
 *
 * Objects flushed here enter the per-CPU magazine -- one at a time through
 * _umem_cache_free() for the steady-state half flush, as a batch under one
 * cc_lock through umem_cache_free_batch() at exit -- which is intentional:
 * the magazine layer is the correct next level in the caching hierarchy
 * (PTC -> magazine -> depot -> slab).
 */
/*
 * Exact drain accounting for the P1.3a regression.
 *
 * COMPILED OUT unless -DUMEM_PTC_RESIZE_PROBE is passed (see Makefile.am's
 * libumem_ptcprobe); the default build has neither the counter nor a branch.
 *
 * Counts objects that were still in a PTC bin when the PTC was freed at thread
 * exit -- i.e. objects whose only reference was about to be dropped while the
 * slab layer still counted them as allocated.  Pre-fix this is nonzero (the
 * half-flush left a remainder behind); post-fix it is exactly zero.
 *
 * This exists because the original regression compared OUTSTANDING BUFFER
 * COUNTS between a PTC-on and PTC-off arm.  That works, but the signal sits on
 * top of ordinary magazine/depot/slab retention, which varies run to run: when
 * an unrelated change reduced the control arm's retention, the comparison
 * tightened and the test grew flaky without the behaviour under test having
 * changed at all.  An exact in-library count has no such coupling -- the same
 * lesson as the P1.3c probe.
 */

static void
umem_ptc_bin_flush_impl(umem_ptc_bin_t *bin, size_t size, int all)
{
	int i;
	int flush_count;
	void *ptr;
	umem_cache_t *cp;

	if (bin->count == 0) {
		return;
	}

	if (all) {
		flush_count = bin->count;
	} else {
		flush_count = bin->count / 2;
		if (flush_count == 0) {
			flush_count = bin->count;
		}
	}

	/* Look up the appropriate cache */
	cp = umem_alloc_table[(size - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		return;
	}

	/* Free to magazine layer */
	if (all) {
		/*
		 * Thread exit: hand the whole bin back in one lock acquisition
		 * (P6.3).  Per-object _umem_cache_free took cc_lock once per
		 * object -- up to 600 acquisitions per exiting thread -- and
		 * at 16,000 simultaneous exits that queue stalled unrelated
		 * threads for 37 ms (P6.3 table in
		 * docs/plans/2026-09-21-production-readiness.md; fixed b8c39e6,
		 * 06559e5: 1 ms).  umem_cache_free_batch fills the per-CPU
		 * magazines under one cc_lock; anything the magazine layer
		 * cannot take goes to the slab layer inside it and is counted
		 * in its return, so it never leaves an object behind.
		 */
		int n;

		/*
		 * Demangle in place: the bin is being torn down (all == 1
		 * is umem_ptc_destroy only), so slots[0 .. count) are never
		 * read as slots again after this.
		 */
		for (i = 0; i < flush_count; i++)
			bin->slots[i] = UMEM_SLOT_DEMANGLE(&bin->slots[i],
			    bin->slots[i]);
#ifdef	UMEM_PTC_DRAIN_PER_OBJECT
		/*
		 * P6.3b control arm ONLY (not a supported build): return the
		 * exit bin one object per cc_lock instead of one batch, to
		 * A/B the batch hold-time against the per-object path at a
		 * fixed base.  Slots are already demangled above.
		 */
		for (i = 0; i < flush_count; i++)
			_umem_cache_free(cp, bin->slots[i]);
		n = flush_count;
#else
		n = umem_cache_free_batch(cp, bin->slots, flush_count);
#endif

		ASSERT(n == flush_count);
		UMEM_PTC_PROBE_COUNT(umem_ptc_probe_exit_bins, 1);
		UMEM_PTC_PROBE_COUNT(umem_ptc_probe_exit_handoffs, 1);
		bin->count -= (uint16_t)n;
		return;
	}
	for (i = 0; i < flush_count; i++) {
		ptr = ptc_slot_pop(bin);
		_umem_cache_free(cp, ptr);
	}
}

void
umem_ptc_bin_flush(umem_ptc_bin_t *bin, size_t size)
{
	umem_ptc_bin_flush_impl(bin, size, 0);
}

/*
 * Flush a bin completely.  Used only by umem_ptc_destroy().
 */
void
umem_ptc_bin_flush_all(umem_ptc_bin_t *bin, size_t size)
{
	umem_ptc_bin_flush_impl(bin, size, 1);
}

/*
 * Refill a bin from the magazine layer
 */
int
umem_ptc_bin_refill(umem_ptc_bin_t *bin, size_t size)
{
	int i;
	int refill_count;
	int bin_idx;
	void *ptr;
	umem_cache_t *cp;

	/* Determine bin index from size to get correct capacity */
	bin_idx = umem_ptc_size_to_bin(size);
	if (bin_idx < 0)
		return (-1);

	/* Refill half the bin */
	refill_count = ptc_bin_capacity(bin_idx) / 2;

	/* Look up the appropriate cache */
	cp = umem_alloc_table[(size - 1) >> UMEM_ALIGN_SHIFT];
	if (cp == NULL) {
		return (-1);
	}

	/* Allocate from magazine layer */
	for (i = 0; i < refill_count; i++) {
		ptr = _umem_cache_alloc(cp, UMEM_DEFAULT);
		if (ptr == NULL) {
			break;
		}
		ptc_slot_push(bin, ptr);
	}

	return (i > 0 ? 0 : -1);
}

/*
 * Destroy thread cache
 */
void
umem_ptc_destroy(umem_ptc_t *ptc)
{
	int bin_idx;
	umem_ptc_bin_t *bin;

	if (ptc == NULL) {
		return;
	}

	/* Flush all per-thread magazines back to depot first */
	umem_ptc_mag_flush_all(ptc);

	/*
	 * Drain every bin COMPLETELY.
	 *
	 * This used to call umem_ptc_bin_flush(), which deliberately flushes
	 * only half a bin, exactly once per bin -- and then freed the PTC
	 * below.  Everything still in a bin at that point lost its only
	 * reference while the slab layer went on counting it as allocated, so
	 * a thread exiting with 128 cached objects permanently leaked 64 of
	 * them.  Thread churn made that cumulative.  (The old comment here
	 * claimed "Flush all bins", which the code did not do.)
	 */
	for (bin_idx = 0; bin_idx < PTC_NBINS; bin_idx++) {
		bin = &ptc->bins[bin_idx];
		if (bin->count == 0)
			continue;

		umem_ptc_bin_flush_all(bin, umem_ptc_bin_size(bin_idx));
		/*
		 * Deliberately NOT an ASSERT here.  Under the probe build the
		 * stranded count below is this regression's oracle, and
		 * asserting would abort on the first exiting thread before the
		 * test could read it -- turning a legible "N objects stranded"
		 * result into a bare SIGABRT (observed: rc=134 with no output
		 * at all).  The invariant is still enforced, by the test, via
		 * that counter.
		 *
		 * There used to be a sched_yield() every 64 objects here to
		 * bound how long one exiting thread kept cc_lock busy.  The
		 * bin is now one batched hand-off (P6.3), so the lock is
		 * taken once per bin and released before the next; nothing
		 * left to yield around.
		 */
	}

	/* Free the ptc structure itself.  Anything still in a bin here loses
	 * its only reference, so count it before it is unreachable. */
	{
		int bi;
		int stranded = 0;

		for (bi = 0; bi < PTC_NBINS; bi++)
			stranded += ptc->bins[bi].count;
		UMEM_PTC_PROBE_STRANDED(stranded);
	}
	umem_cache_free(umem_ptc_cache, ptc);
}

/* ================================================================
 * Small-Buffer Optimization (SBO)
 *
 * Thread-local bump allocator for tiny allocations (<= 128 bytes).
 * ================================================================ */

static __thread char sbo_buf[UMEM_SBO_BUFSZ]
    __attribute__((tls_model("initial-exec"), aligned(UMEM_SBO_ALIGN)));
static __thread size_t sbo_offset
    __attribute__((tls_model("initial-exec"))) = 0;

/*
 * SBO is disabled when umem_flags carries any of UMF_AUDIT, UMF_DEADBEEF,
 * UMF_REDZONE (the process-wide debug flags, not any one cache's), or when
 * PTC itself is disabled.
 */
int
umem_sbo_enabled(void)
{
	if (!umem_ptc_enabled || !ptc_table_ready) {
		return (0);
	}
	if (umem_flags & (UMF_AUDIT | UMF_DEADBEEF | UMF_REDZONE)) {
		return (0);
	}
	return (1);
}

void *
umem_sbo_alloc(size_t size, int umflags)
{
	size_t aligned_size;
	size_t new_offset;
	void *ptr;

	(void)umflags;

	if (!umem_sbo_enabled()) {
		return (NULL);
	}

	if (size == 0 || size > UMEM_SBO_MAXALLOC) {
		return (NULL);
	}

	/* Round up to alignment boundary */
	aligned_size = (size + UMEM_SBO_ALIGN - 1) & ~(UMEM_SBO_ALIGN - 1);
	new_offset = sbo_offset + aligned_size;

	if (new_offset > UMEM_SBO_BUFSZ) {
		/* Buffer full — reset and retry once */
		sbo_offset = 0;
		new_offset = aligned_size;
		if (new_offset > UMEM_SBO_BUFSZ) {
			return (NULL);
		}
	}

	ptr = &sbo_buf[sbo_offset];
	sbo_offset = new_offset;
	return (ptr);
}

int
umem_sbo_free(void *ptr, size_t size)
{
	(void)size;

	/* Check if ptr falls within our thread-local SBO buffer */
	if ((char *)ptr >= sbo_buf &&
	    (char *)ptr < sbo_buf + UMEM_SBO_BUFSZ) {
		/* No-op: SBO memory is freed on reset */
		return (1);
	}
	return (0);
}

void
umem_sbo_reset(void)
{
	sbo_offset = 0;
}

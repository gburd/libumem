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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 *
 * Portions Copyright 2012 Joyent, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Portions Copyright 2006-2008 Message Systems, Inc. All rights reserved.
 */

/*
 * Copyright (c) 2014 Joyent, Inc.  All rights reserved.
 * Copyright (c) 2015 by Delphix. All rights reserved.
 */

/*!
 * \mainpage Main Page
 *
 * \section README
 *
 * \include README
 *
 * \section Nuances
 *
 * Returning memory to the OS.  On Solaris the kernel's low-memory callback
 * drives kmem_reap(); a user process has no such signal, so for a long time
 * this port only reaped when the application called umem_reap() or a backend
 * allocation failed, and freed objects parked in depot magazines were never
 * returned.  That is no longer the case: when reclaim is enabled (the
 * default) the periodic update pass reaps the depot's working-set excess
 * and the slab reclaimer then advises away pages of slabs that have been
 * empty for reclaim_delay seconds and destroys them after twice that.
 * umem_reap() remains available to force an immediate reap.
 *
 * \section References
 *
 * http://docs.sun.com/app/docs/doc/816-5173/6mbb8advq?a=view
 *
 * http://access1.sun.com/techarticles/libumem.html
 *
 * \section Overview
 *
 * \code
 * based on usr/src/uts/common/os/kmem.c r1.64 from 2001/12/18
 *
 * The slab allocator, as described in the following two papers:
 *
 *	Jeff Bonwick,
 *	The Slab Allocator: An Object-Caching Kernel Memory Allocator.
 *	Proceedings of the Summer 1994 Usenix Conference.
 *	Available as /shared/sac/PSARC/1994/028/materials/kmem.pdf.
 *
 *	Jeff Bonwick and Jonathan Adams,
 *	Magazines and vmem: Extending the Slab Allocator to Many CPUs and
 *	Arbitrary Resources.
 *	Proceedings of the 2001 Usenix Conference.
 *	Available as /shared/sac/PSARC/2000/550/materials/vmem.pdf.
 *
 * 1. Overview
 * -----------
 * umem is very close to kmem in implementation.  There are seven major
 * areas of divergence:
 *
 *	* Initialization
 *
 *	* CPU handling
 *
 *	* umem_update()
 *
 *	* KM_SLEEP v.s. UMEM_NOFAIL
 *
 *  * lock ordering
 *
 *	* changing UMEM_MAXBUF
 *
 *	* Per-thread caching for malloc/free
 *
 * 2. Initialization
 * -----------------
 * kmem is initialized early on in boot, and knows that no one will call
 * into it before it is ready.  umem does not have these luxuries. Instead,
 * initialization is divided into two phases:
 *
 *	* library initialization, and
 *
 *	* first use
 *
 * umem's full initialization happens at the time of the first allocation
 * request (via malloc() and friends, umem_alloc(), or umem_zalloc()),
 * or the first call to umem_cache_create().
 *
 * umem_free(), and umem_cache_alloc() do not require special handling,
 * since the only way to get valid arguments for them is to successfully
 * call a function from the first group.
 *
 * 2.1. Library Initialization: umem_startup()
 * -------------------------------------------
 * umem_startup() is libumem.so's .init section.  It calls pthread_atfork()
 * to install the handlers necessary for umem's Fork1-Safety.  Because of
 * race condition issues, all other pre-umem_init() initialization is done
 * statically (i.e. by the dynamic linker).
 *
 * For standalone use, umem_startup() returns everything to its initial
 * state.
 *
 * 2.2. First use: umem_init()
 * ------------------------------
 * The first time any memory allocation function is used, we have to
 * create the backing caches and vmem arenas which are needed for it.
 * umem_init() is the central point for that task.  When it completes,
 * umem_ready is either UMEM_READY (all set) or UMEM_READY_INIT_FAILED (unable
 * to initialize, probably due to lack of memory).
 *
 * There are four different paths from which umem_init() is called:
 *
 *	* from umem_alloc() or umem_zalloc(), with 0 < size < UMEM_MAXBUF,
 *
 *	* from umem_alloc() or umem_zalloc(), with size > UMEM_MAXBUF,
 *
 *	* from umem_cache_create(), and
 *
 *	* from memalign(), with align > UMEM_ALIGN.
 *
 * The last three just check if umem is initialized, and call umem_init()
 * if it is not.  For performance reasons, the first case is more complicated.
 *
 * 2.2.1. umem_alloc()/umem_zalloc(), with 0 < size < UMEM_MAXBUF
 * -----------------------------------------------------------------
 * In this case, umem_cache_alloc(&umem_null_cache, ...) is called.
 * There is special case code in which causes any allocation on
 * &umem_null_cache to fail by returning (NULL), regardless of the
 * flags argument.
 *
 * So umem_cache_alloc() returns NULL, and umem_alloc()/umem_zalloc() call
 * umem_alloc_retry().  umem_alloc_retry() sees that the allocation
 * was agains &umem_null_cache, and calls umem_init().
 *
 * If initialization is successful, umem_alloc_retry() returns 1, which
 * causes umem_alloc()/umem_zalloc() to start over, which causes it to load
 * the (now valid) cache pointer from umem_alloc_table.
 *
 * 2.2.2. Dealing with race conditions
 * -----------------------------------
 * There are a couple race conditions resulting from the initialization
 * code that we have to guard against:
 *
 *	* In umem_cache_create(), there is a special UMC_INTERNAL cflag
 *	that is passed for caches created during initialization.  It
 *	is illegal for a user to try to create a UMC_INTERNAL cache.
 *	This allows initialization to proceed, but any other
 *	umem_cache_create()s will block by calling umem_init().
 *
 *	* Since umem_null_cache has a 1-element cache_cpu, it's cache_cpu_mask
 *	is always zero.  umem_cache_alloc uses cp->cache_cpu_mask to
 *	mask the cpu number.  This prevents a race between grabbing a
 *	cache pointer out of umem_alloc_table and growing the cpu array.
 *
 *
 * 3. CPU handling
 * ---------------
 * kmem uses the CPU's sequence number to determine which "cpu cache" to
 * use for an allocation.  Currently, there is no way to get the sequence
 * number in userspace.
 *
 * umem keeps track of cpu information in umem_cpus, an array of umem_max_ncpus
 * umem_cpu_t structures.  CURCPU() is a a "hint" function, which we then mask
 * with either umem_cpu_mask or cp->cache_cpu_mask to find the actual "cpu" id.
 * The mechanics of this is all in the CPU(mask) macro.
 *
 * Currently, umem uses _lwp_self() as its hint.
 *
 *
 * 4. The update thread
 * --------------------
 * kmem uses a task queue, kmem_taskq, to do periodic maintenance on
 * every kmem cache.  vmem has a periodic timeout for hash table resizing.
 * The kmem_taskq also provides a separate context for kmem_cache_reap()'s
 * to be done in, avoiding issues of the context of kmem_reap() callers.
 *
 * Instead, umem has the concept of "updates", which are asynchronous requests
 * for work attached to single caches.  All caches with pending work are
 * on a doubly linked list rooted at the umem_null_cache.  All update state
 * is protected by the umem_update_lock mutex, and the umem_update_cv is used
 * for notification between threads.
 *
 * 4.1. Cache states with regards to updates
 * -----------------------------------------
 * A given cache is in one of three states:
 *
 * Inactive		cache_uflags is zero, cache_u{next,prev} are NULL
 *
 * Work Requested	cache_uflags is non-zero (but UMU_ACTIVE is not set),
 *			cache_u{next,prev} link the cache onto the global
 *			update list
 *
 * Active		cache_uflags has UMU_ACTIVE set, cache_u{next,prev}
 *			are NULL, and either umem_update_thr or
 *			umem_st_update_thr are actively doing work on the
 *			cache.
 *
 * An update can be added to any cache in any state -- if the cache is
 * Inactive, it transitions to being Work Requested.  If the cache is
 * Active, the worker will notice the new update and act on it before
 * transitioning the cache to the Inactive state.
 *
 * If a cache is in the Active state, UMU_NOTIFY can be set, which asks
 * the worker to broadcast the umem_update_cv when it has finished.
 *
 * 4.2. Update interface
 * ---------------------
 * umem_add_update() adds an update to a particular cache.
 * umem_updateall() adds an update to all caches.
 * umem_remove_updates() returns a cache to the Inactive state.
 *
 * umem_process_updates() process all caches in the Work Requested state.
 *
 * 4.3. Reaping
 * ------------
 * When umem_reap() is called (at the time of heap growth), it schedule
 * UMU_REAP updates on every cache.  It then checks to see if the update
 * thread exists (umem_update_thr != 0).  If it is, it broadcasts
 * the umem_update_cv to wake the update thread up, and returns.
 *
 * If the update thread does not exist (umem_update_thr == 0), and the
 * program currently has multiple threads, umem_reap() attempts to create
 * a new update thread.
 *
 * If the process is not multithreaded, or the creation fails, umem_reap()
 * calls umem_st_update() to do an inline update.
 *
 * 4.4. The update thread
 * ----------------------
 * The update thread spends most of its time in cond_timedwait() on the
 * umem_update_cv.  It wakes up under two conditions:
 *
 *	* The timedwait times out, in which case it needs to run a global
 *	update, or
 *
 *	* someone cond_broadcast(3THR)s the umem_update_cv, in which case
 *	it needs to check if there are any caches in the Work Requested
 *	state.
 *
 * When it is time for another global update, umem calls umem_cache_update()
 * on every cache, then calls vmem_update(), which tunes the vmem structures.
 * umem_cache_update() can request further work using umem_add_update().
 *
 * After any work from the global update completes, the update timer is
 * reset to umem_reap_interval seconds in the future.  This makes the
 * updates self-throttling.
 *
 * Reaps are similarly self-throttling.  After a UMU_REAP update has
 * been scheduled on all caches, umem_reap() sets a flag and wakes up the
 * update thread.  The update thread notices the flag, and resets the
 * reap state.
 *
 * 4.5. Inline updates
 * -------------------
 * If the update thread is not running, umem_st_update() is used instead.  It
 * immediately does a global update (as above), then calls
 * umem_process_updates() to process both the reaps that umem_reap() added and
 * any work generated by the global update.  Afterwards, it resets the reap
 * state.
 *
 * While the umem_st_update() is running, umem_st_update_thr holds the thread
 * id of the thread performing the update.
 *
 * 4.6. Updates and fork1()
 * ------------------------
 * umem has fork1() pre- and post-handlers which lock up (and release) every
 * mutex in every cache.  They also lock up the umem_update_lock.  Since
 * fork1() only copies over a single lwp, other threads (including the update
 * thread) could have been actively using a cache in the parent.  This
 * can lead to inconsistencies in the child process.
 *
 * Because we locked all of the mutexes, the only possible inconsistancies are:
 *
 *	* a umem_cache_alloc() could leak its buffer.
 *
 *	* a caller of umem_depot_alloc() could leak a magazine, and all the
 *	buffers contained in it.
 *
 *	* a cache could be in the Active update state.  In the child, there
 *	would be no thread actually working on it.
 *
 *	* a umem_hash_rescale() could leak the new hash table.
 *
 *	* a umem_magazine_resize() could be in progress.
 *
 *	* a umem_reap() could be in progress.
 *
 * The memory leaks we can't do anything about.  umem_release_child() resets
 * the update state, moves any caches in the Active state to the Work Requested
 * state.  This might cause some updates to be re-run, but UMU_REAP and
 * UMU_HASH_RESCALE are effectively idempotent, and the worst that can
 * happen from umem_magazine_resize() is resizing the magazine twice in close
 * succession.
 *
 * Much of the cleanup in umem_release_child() is skipped if
 * umem_st_update_thr == thr_self().  This is so that applications which call
 * fork1() from a cache callback does not break.  Needless to say, any such
 * application is tremendously broken.
 *
 *
 * 5. KM_SLEEP v.s. UMEM_NOFAIL
 * ----------------------------
 * Allocations against kmem and vmem have two basic modes:  SLEEP and
 * NOSLEEP.  A sleeping allocation is will go to sleep (waiting for
 * more memory) instead of failing (returning NULL).
 *
 * SLEEP allocations presume an extremely multithreaded model, with
 * a lot of allocation and deallocation activity.  umem cannot presume
 * that its clients have any particular type of behavior.  Instead,
 * it provides two types of allocations:
 *
 *	* UMEM_DEFAULT, equivalent to KM_NOSLEEP (i.e. return NULL on
 *	failure)
 *
 *	* UMEM_NOFAIL, which, on failure, calls an optional callback
 *	(registered with umem_nofail_callback()).
 *
 * The callback is invoked with no locks held, and can do an arbitrary
 * amount of work.  It then has a choice between:
 *
 *	* Returning UMEM_CALLBACK_RETRY, which will cause the allocation
 *	to be restarted.
 *
 *	* Returning UMEM_CALLBACK_EXIT(status), which will cause exit(2)
 *	to be invoked with status.  If multiple threads attempt to do
 *	this simultaneously, only one will call exit(2).
 *
 *	* Doing some kind of non-local exit (thr_exit(3thr), longjmp(3C),
 *	etc.)
 *
 * The default callback returns UMEM_CALLBACK_EXIT(255).
 *
 * To have these callbacks without risk of state corruption (in the case of
 * a non-local exit), we have to ensure that the callbacks get invoked
 * close to the original allocation, with no inconsistent state or held
 * locks.  The following steps are taken:
 *
 *	* All invocations of vmem are VM_NOSLEEP.
 *
 *	* All constructor callbacks (which can themselves to allocations)
 *	are passed UMEM_DEFAULT as their required allocation argument.  This
 *	way, the constructor will fail, allowing the highest-level allocation
 *	invoke the nofail callback.
 *
 *	If a constructor callback _does_ do a UMEM_NOFAIL allocation, and
 *	the nofail callback does a non-local exit, we will leak the
 *	partially-constructed buffer.
 *
 *
 * 6. Lock Ordering
 * ----------------
 * umem has a few more locks than kmem does, mostly in the update path.  The
 * overall lock ordering (earlier locks must be acquired first) is:
 *
 *	umem_init_lock
 *
 *	vmem_list_lock
 *	vmem_nosleep_lock.vmpl_mutex
 *	vmem_t's:
 *		vm_lock
 *	sbrk_lock
 *
 *	umem_cache_lock
 *	umem_update_lock
 *	umem_flags_lock
 *	umem_cache_t's:
 *		cache_cpu[*].cc_lock
 *		cache_full.ml_lock / cache_empty.ml_lock
 *		cache_lock
 *	umem_log_header_t's:
 *		lh_cpu[*].clh_lock
 *		lh_lock
 *
 * \endcode
 *
 * 7. Changing UMEM_MAXBUF
 * -----------------------
 *
 * When changing UMEM_MAXBUF extra care has to be taken. It is not sufficient to
 * simply increase this number. First, one must update the umem_alloc_table to
 * have the appropriate number of entires based upon the new size. If this is
 * not done, this will lead to libumem blowing an assertion.
 *
 * The second place to update, which is not required, is the umem_alloc_sizes.
 * These determine the default cache sizes that we're going to support.
 */

#include "config.h"
#include <stdatomic.h>
/* #include "mtlib.h" */
#include <umem_impl.h>
#include <sys/vmem_impl_user.h>
#include "umem_base.h"
#include "vmem_base.h"
#include "umem_ptc.h"
#include "umem_simd.h"
#include "umem_stacktrace.h"
#include "umem_profile.h"
#include "umem_introspect.h"

#ifdef UMEM_NUMA_AVAILABLE
#include "umem_numa.h"
#endif

/*
 * Include rseq support when available.
 * umem_rseq.h defines UMEM_RSEQ_AVAILABLE internally, which guards
 * all rseq-dependent code in this file.
 */
#if defined(__linux__) && defined(HAVE_LINUX_RSEQ_H)
#include "umem_rseq.h"
#endif

#if HAVE_SYS_PROCESSOR_H
#include <sys/processor.h>
#endif
#if HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#if HAVE_ALLOCA_H
#include <alloca.h>
#elif defined(__GNUC__)
# ifndef alloca
#  define alloca __builtin_alloca
# endif
#endif
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if HAVE_STRINGS_H
#include <strings.h>
#endif
#include <signal.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#if HAVE_ATOMIC_H
#include <atomic.h>
#else
#include "atomic.h"
#endif

#include "misc.h"

#define UMEM_VMFLAGS(umflag)    (VM_NOSLEEP)

/*
 * Compile-time checks for cache line alignment of critical structures.
 * False sharing between per-CPU or per-thread structures kills performance.
 */
_Static_assert(sizeof(umem_cpu_cache_t) % UMEM_CACHE_LINE_SIZE == 0,
    "umem_cpu_cache_t size must be a multiple of cache line size");
_Static_assert(_Alignof(umem_cpu_cache_t) >= UMEM_CACHE_LINE_SIZE,
    "umem_cpu_cache_t must be cache-line aligned");
#ifdef UMEM_RSEQ_AVAILABLE
/* Carved after the two depot arrays in cache_percpu_map; must tile. */
_Static_assert(sizeof(umem_rseq_cache_t) % UMEM_CACHE_LINE_SIZE == 0,
    "umem_rseq_cache_t must be a whole number of cache lines");
#endif
_Static_assert(sizeof(umem_maglist_t) % UMEM_CACHE_LINE_SIZE == 0,
    "umem_maglist_t size must be a multiple of cache line size");
_Static_assert(_Alignof(umem_maglist_t) >= UMEM_CACHE_LINE_SIZE,
    "umem_maglist_t must be cache-line aligned");
/* Disabled: umem_ptc_bin_t size varies by platform (pointer size, alignment) */
/* _Static_assert(sizeof(umem_ptc_bin_t) % UMEM_CACHE_LINE_SIZE == 0,
    "umem_ptc_bin_t size must be a multiple of cache line size"); */

size_t pagesize;

/*
 * The default set of caches to back umem_alloc().
 * These sizes should be reevaluated periodically.
 *
 * We want allocations that are multiples of the coherency granularity
 * (64 bytes) to be satisfied from a cache which is a multiple of 64
 * bytes, so that it will be 64-byte aligned.  For all multiples of 64,
 * the next kmem_cache_size greater than or equal to it must be a
 * multiple of 64.
 *
 * This table must be in sorted order, from smallest to highest.  The
 * highest slot must be UMEM_MAXBUF, and every slot afterwards must be
 * zero.
 */
/*
 * Size classes with ~1.25x spacing to reduce internal fragmentation.
 * Old doubling scheme wasted ~50% at boundaries (e.g., 33B in 64B class).
 * New spacing keeps worst-case waste under ~25%.
 */
static int umem_alloc_sizes[] = {
#ifdef _LP64
	8, 16, 32, 48, 64, 80, 96, 112,
#else
	8, 16, 24, 32, 40, 48, 56, 64,
	80, 96, 112,
#endif
	128, 160, 192, 224, 256,
	320, 384, 448, 512,
	640, 768, 896, 1024,
	1280, 1536, 1792, 2048,
	2560, 3072, 3584, 4096,
	5120, 6144, 7168, 8192,
	10240, 12288, 14336, 16384,
	20480, 24576, 32768, 40960,
	49152, 57344, 65536, 73728,
	81920, 98304, 114688,
	UMEM_MAXBUF,
	/* 16 slots for user expansion */
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
};
#define NUM_ALLOC_SIZES (sizeof (umem_alloc_sizes) / sizeof (*umem_alloc_sizes))

/*
 * Magazine type table: {magsize, align, minbuf, maxbuf}
 *
 * Larger magazines for small objects amortize depot access cost.
 * A 64-byte object now gets 127-slot magazines instead of 15-slot.
 *
 * The 2048..8192 band was 31 rounds for 2-4 KB chunks and 15 for 4-8 KB,
 * Solaris tuning that assumed a 64 KB heap quantum and hence a slab of 16
 * such objects; on Linux 3f2e67c now packs these 16 per slab too, and a
 * magazine that holds fewer objects than a slab makes every 31 (or 15)
 * operations per CPU a depot round trip.  63 for the whole band (P8.2b
 * part b), the same as the 1-2 KB class.  Magazine shells are objects of
 * umem_magazine_<magsize> caches created at init from this table, so two
 * rows with one magsize would create a duplicate cache; the rows are merged
 * instead.
 */
static umem_magtype_t umem_magtype[] = {
	{ 1,    8,      65536,  UMEM_MAXBUF },
	{ 3,    16,     16384,  65536   },
	{ 7,    32,     8192,   16384   },
	{ 63,   64,     1024,   8192    },
	{ 127,  64,     256,    1024    },
	{ 255,  64,     0,      256     },
};

/*
 * umem tunables
 */
uint32_t umem_max_ncpus;        /* # of CPU caches. */

uint32_t umem_stack_depth = 15; /* # stack frames in a bufctl_audit */
uint32_t umem_reap_interval = 10; /* max reaping rate (seconds) */
uint_t umem_depot_contention = 2; /* max failed trylocks per real interval */
/*
 * Max nearby per-CPU depot stripes scanned in the NON-BLOCKING (trylock)
 * refill path before giving up to the blocking depot/slab path. The depot
 * has one stripe per possible CPU (up to umem_max_ncpus, e.g. 512); scanning
 * them all on every trylock miss is O(ncpus) churn that cripples
 * single-thread hold-heavy workloads (frag) whose depot is empty. Bounding
 * only the trylock scan keeps the blocking umem_depot_alloc cross-CPU steal
 * intact for cross-thread handoff (prodcons).
 * ponytail: fixed bound, revisit if a trylock-only workload proves it needs
 * wider stealing.
 */
int umem_depot_steal_max = 8;
#define UMEM_DEPOT_STEAL_MAX umem_depot_steal_max
uint_t umem_magazine_tuning = 0; /* magazine size auto-tuning (0=off, 1=on) */
uint_t umem_abort = 1;          /* whether to abort on error */
uint_t umem_output = 0;         /* whether to write to standard error */
uint_t umem_logging = 0;        /* umem_log_enter() override */
uint32_t umem_mtbf = 0;         /* mean time between failures [default: off] */
size_t umem_transaction_log_size; /* size of transaction log */
size_t umem_content_log_size;   /* size of content log */
size_t umem_failure_log_size;   /* failure log [4 pages per CPU] */
size_t umem_slab_log_size;      /* slab create log [4 pages per CPU] */
size_t umem_content_maxsave = 256; /* UMF_CONTENTS max bytes to log */
size_t umem_lite_minsize = 0;   /* minimum buffer size for UMF_LITE */
size_t umem_lite_maxalign = 1024; /* maximum buffer alignment for UMF_LITE */
size_t umem_maxverify;          /* maximum bytes to inspect in debug routines */
size_t umem_minfirewall;        /* hardware-enforced redzone threshold */
size_t umem_ptc_size = 1048576;	/* size of per-thread cache (in bytes) */

uint32_t umem_reclaim_enabled = 1;  /* background page reclamation via madvise */
uint32_t umem_reclaim_delay = 30;   /* seconds before reclaiming dirty slabs */

uint_t umem_flags = 0;

mutex_t                 umem_init_lock = DEFAULTMUTEX;          /* locks initialization */
cond_t                  umem_init_cv = DEFAULTCV;               /* initialization CV */
thread_t                umem_init_thr;          /* thread initializing */
int                     umem_init_env_ready;    /* environ pre-initted */
int                     umem_ready = UMEM_READY_STARTUP;

/* Legacy ABI symbol for test binaries */
const int		umem_genasm_supported = 0;

static umem_nofail_callback_t *nofail_callback;
static mutex_t          umem_nofail_exit_lock = DEFAULTMUTEX;
static thread_t         umem_nofail_exit_thr;

static umem_cache_t     *umem_slab_cache;
static umem_cache_t     *umem_bufctl_cache;
static umem_cache_t     *umem_bufctl_audit_cache;

mutex_t                 umem_flags_lock = DEFAULTMUTEX;

static vmem_t           *heap_arena;
static vmem_alloc_t     *heap_alloc;
static vmem_free_t      *heap_free;

static vmem_t           *umem_internal_arena;
static vmem_t           *umem_cache_arena;
static vmem_t           *umem_hash_arena;
static vmem_t           *umem_log_arena;
static vmem_t           *umem_oversize_arena;
static vmem_t           *umem_va_arena;
static vmem_t           *umem_default_arena;
static vmem_t           *umem_firewall_va_arena;
static vmem_t           *umem_firewall_arena;

vmem_t                  *umem_memalign_arena;

umem_log_header_t *umem_transaction_log;
umem_log_header_t *umem_content_log;
umem_log_header_t *umem_failure_log;
umem_log_header_t *umem_slab_log;

extern thread_t _thr_self(void);
#ifndef CPUHINT
# define CPUHINT()      ((int)(uintptr_t)(_thr_self()))
#endif
#define CPUHINT_MAX()           INT_MAX

/*
 * Both go through get_cached_cpu_hint() (umem_impl.h): the raw CPUHINT()
 * is pthread_self() on this port, whose low bits are always zero, so
 * `CPUHINT() & mask` selected slot 0 for every thread.  See the comment on
 * get_cached_cpu_hint for the measurement.
 */
#define CPU(mask)               (umem_cpus + (get_cached_cpu_hint() & (mask)))
#define CPU_CACHED(mask)        (umem_cpus + (get_cached_cpu_hint() & (mask)))
static umem_cpu_t umem_startup_cpu = {  /* initial, single, cpu */
	UMEM_CACHE_SIZE(0),
	0
};

static uint32_t umem_cpu_mask = 0;                      /* global cpu mask */
umem_cpu_t *umem_cpus = &umem_startup_cpu;              /* cpu list */

/*
 * Per-thread cached CPU hint to reduce CPUHINT() syscall overhead.
 * Initialized to -1 to force refresh on first access.
 */
__thread int cached_cpu_hint = -1;

/*
 * NUMA cpu-to-node mapping table.
 *
 * Maps each depot CPU slot to its NUMA node. On non-NUMA systems
 * or when libnuma is unavailable, all entries are 0 (single node).
 * Populated during umem_init() after umem_max_ncpus is finalized.
 *
 * Dynamically sized to umem_max_ncpus (which is rounded up to a power of
 * two and can exceed UMEM_MAX_DEPOT_CPUS on high-core machines).  The
 * depot stealing loop indexes this table over [0, cache_depot_ncpus),
 * and cache_depot_ncpus == umem_max_ncpus, so a fixed-size table would be
 * read out of bounds on machines with more than UMEM_MAX_DEPOT_CPUS CPUs.
 * umem_cpu_node_static is the fallback used before init / if allocation
 * fails (single-node assumption, all zeros).
 */
#define UMEM_MAX_DEPOT_CPUS 256
static int umem_cpu_node_static[UMEM_MAX_DEPOT_CPUS];
static int *umem_cpu_node = umem_cpu_node_static;
static uint32_t umem_cpu_node_ncpus = UMEM_MAX_DEPOT_CPUS;
static int umem_num_nodes = 1;

volatile uint32_t umem_reaping;

thread_t                umem_update_thr;
struct timeval          umem_update_next;       /* timeofday of next update */
volatile thread_t       umem_st_update_thr;     /* only used when single-thd */

#define IN_UPDATE()     (thr_self() == umem_update_thr || \
			    thr_self() == umem_st_update_thr)
#define IN_REAP()       IN_UPDATE()

mutex_t                 umem_update_lock = DEFAULTMUTEX;        /* cache_u{next,prev,flags} */
cond_t                  umem_update_cv = DEFAULTCV;

volatile hrtime_t umem_reap_next;       /* min hrtime of next reap */

mutex_t                 umem_cache_lock = DEFAULTMUTEX; /* inter-cache linkage only */

#ifdef UMEM_STANDALONE
umem_cache_t            umem_null_cache;
static const umem_cache_t umem_null_cache_template = {
#else
umem_cache_t            umem_null_cache = {
#endif
	0, 0, 0, 0, 0,
	0, 0,
	0, 0,
	0, 0,
	0, 0,
	0, 0,
	"invalid_cache",
	0, 0,
	NULL, NULL, NULL, NULL,
	NULL,
	0, 0, 0, 0,
	&umem_null_cache, &umem_null_cache,
	&umem_null_cache, &umem_null_cache,
	0,
	DEFAULTMUTEX,                           /* start of slab layer */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	&umem_null_cache.cache_nullslab,
	{
		&umem_null_cache,
		NULL,
		&umem_null_cache.cache_nullslab,
		&umem_null_cache.cache_nullslab,
		NULL,
		-1,
		0
	},
	NULL,
	NULL,
	NULL, {                                 /* start of depot layer */
		DEFAULTMUTEX, NULL, 0, 0, 0, 0 /* cache_full */
	}, {
		DEFAULTMUTEX, NULL, 0, 0, 0, 0 /* cache_empty */
	},
	0,					/* cache_depot_ncpus */
	NULL,					/* cache_depot_full */
	NULL,					/* cache_depot_empty */
	0,					/* cache_depot_local */
	0,					/* cache_depot_remote */
	0,					/* cache_depot_cross_node */
	0,					/* cache_mag_total */
#ifdef UMEM_RSEQ_AVAILABLE
	NULL,					/* cache_rseq */
#endif
#ifdef UMEM_NUMA_AVAILABLE
	NULL,					/* cache_numa_info */
#endif
	{
		{
			/* umem_cpu_cache_t: cc_lock, cc_alloc, cc_free, cc_loaded, cc_ploaded, cc_rounds, cc_prounds, cc_magsize, cc_flags */
			DEFAULTMUTEX,           /* cc_lock */
			0,                      /* cc_alloc */
			0,                      /* cc_free */
			NULL,                   /* cc_loaded */
			NULL,                   /* cc_ploaded */
			-1,                     /* cc_rounds - atomic, initialized directly */
			-1,                     /* cc_prounds */
			0,                      /* cc_magsize */
			0                       /* cc_flags */
		}
	}
};

#define ALLOC_TABLE_4 \
	&umem_null_cache, &umem_null_cache, &umem_null_cache, &umem_null_cache

#define ALLOC_TABLE_64 \
	ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4, \
	ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4, \
	ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4, \
	ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4, ALLOC_TABLE_4

#define ALLOC_TABLE_1024 \
	ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64, \
	ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64, \
	ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64, \
	ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64, ALLOC_TABLE_64

umem_cache_t *umem_alloc_table[UMEM_MAXBUF >> UMEM_ALIGN_SHIFT] = {
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024,
	ALLOC_TABLE_1024
};


/* Used to constrain audit-log stack traces */
caddr_t                 umem_min_stack;
caddr_t                 umem_max_stack;

#ifndef UMEM_STANDALONE
static pthread_once_t umem_forkhandler_once = PTHREAD_ONCE_INIT;
#endif

/*
 * we use the _ versions, since we don't want to be cancelled.
 * Actually, this is automatically taken care of by including "mtlib.h".
 * On Solaris/Illumos, _cond_wait is provided by the system.
 */
#ifndef HAVE_THREAD_H
extern int _cond_wait(cond_t *cv, mutex_t *mutex);
#endif

#define UMERR_MODIFIED  0       /* buffer modified while on freelist */
#define UMERR_REDZONE   1       /* redzone violation (write past end of buf) */
#define UMERR_DUPFREE   2       /* freed a buffer twice */
#define UMERR_BADADDR   3       /* freed a bad (unallocated) address */
#define UMERR_BADBUFTAG 4       /* buftag corrupted */
#define UMERR_BADBUFCTL 5       /* bufctl corrupted */
#define UMERR_BADCACHE  6       /* freed a buffer to the wrong cache */
#define UMERR_BADSIZE   7       /* alloc size != free size */
#define UMERR_BADBASE   8       /* buffer base address wrong */

struct {
	hrtime_t        ump_timestamp;  /* timestamp of error */
	int             ump_error;      /* type of umem error (UMERR_*) */
	void            *ump_buffer;    /* buffer that induced abort */
	void            *ump_realbuf;   /* real start address for buffer */
	umem_cache_t    *ump_cache;     /* buffer's cache according to client */
	umem_cache_t    *ump_realcache; /* actual cache containing buffer */
	umem_slab_t     *ump_slab;      /* slab accoring to umem_findslab() */
	umem_bufctl_t   *ump_bufctl;    /* bufctl */
} umem_abort_info;

static void
copy_pattern(uint64_t pattern, void *buf_arg, size_t size)
{
	uint64_t *bufend = (uint64_t *)((char *)buf_arg + size);
	uint64_t *buf = buf_arg;

	while (buf < bufend)
		*buf++ = pattern;
}

static void *
verify_pattern(uint64_t pattern, void *buf_arg, size_t size)
{
	uint64_t *bufend = (uint64_t *)((char *)buf_arg + size);
	uint64_t *buf;

	for (buf = buf_arg; buf < bufend; buf++)
		if (*buf != pattern)
			return (buf);
	return (NULL);
}

static void *
verify_and_copy_pattern(uint64_t old, uint64_t new, void *buf_arg, size_t size)
{
	uint64_t *bufend = (uint64_t *)((char *)buf_arg + size);
	uint64_t *buf;

	for (buf = buf_arg; buf < bufend; buf++) {
		if (*buf != old) {
			copy_pattern(old, buf_arg,
			    (char *)buf - (char *)buf_arg);
			return (buf);
		}
		*buf = new;
	}

	return (NULL);
}

void
umem_cache_applyall(void (*func)(umem_cache_t *))
{
	umem_cache_t *cp;

	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next)
		func(cp);
	(void) mutex_unlock(&umem_cache_lock);
}

/*
 * umem_dump_contention - dump already-tracked contention counters (D1).
 *
 * Walks the cache list and prints, per active cache: depot reload counts
 * (cache_full.ml_alloc = full-magazine reloads into a CPU, cache_empty.ml_alloc
 * = empty-magazine reloads), depot local/remote/cross-node hits, depot
 * trylock-fail contention count, and (when rseq is active) aggregated per-CPU
 * rseq alloc/free/restart counts plus the summed per-CPU slab cc_alloc.
 * These are all counters the allocator already maintains on its normal paths
 * -- this is a read-only observability hook for benchmarks, not a new
 * hot-path cost.  Safe to call from a benchmark after a run.
 */
void
umem_dump_contention(FILE *fp)
{
	umem_cache_t *cp;

	if (fp == NULL)
		fp = stderr;

	fprintf(fp, "# umem contention dump\n");
#ifdef UMEM_RSEQ_AVAILABLE
	fprintf(fp, "# rseq_enabled=%d asm_safe=%d ncpus=%d\n",
	    umem_rseq_enabled, umem_rseq_asm_safe,
	    umem_rseq_enabled ? umem_rseq_get_ncpus() : 0);
#endif
	fprintf(fp,
	    "%-24s %14s %12s %12s %12s %12s %12s %14s %14s %12s\n",
	    "cache", "cc_alloc", "full_reload", "empty_reld", "dep_local",
	    "dep_remote", "dep_conten", "rseq_alloc", "rseq_free",
	    "rseq_rstrt");

	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		uint64_t rseq_alloc = 0, rseq_free = 0, rseq_restart = 0;
		uint64_t cc_alloc = 0;
		uint32_t ci;

		/* Sum per-CPU slab-magazine allocation counters. */
		for (ci = 0; ci <= cp->cache_cpu_mask; ci++) {
			umem_cpu_cache_t *tc =
			    (umem_cpu_cache_t *)((char *)cp +
			    umem_cpus[ci].cpu_cache_offset);
			cc_alloc += tc->cc_alloc;
		}

#ifdef UMEM_RSEQ_AVAILABLE
		if (cp->cache_rseq != NULL && umem_rseq_enabled) {
			int n = umem_rseq_get_ncpus();
			for (int i = 0; i < n; i++) {
				umem_rseq_cache_t *rc = &cp->cache_rseq[i];
				rseq_alloc += rc->alloc_count;
				rseq_free += rc->free_count;
				rseq_restart += rc->restart_count;
			}
		}
#endif
		/* Skip caches with no activity at all. */
		if (cc_alloc == 0 && rseq_alloc == 0 &&
		    cp->cache_full.ml_alloc == 0 && cp->cache_slab_alloc == 0)
			continue;

		fprintf(fp,
		    "%-24s %14llu %12llu %12llu %12llu %12llu %12llu "
		    "%14llu %14llu %12llu\n",
		    cp->cache_name,
		    (unsigned long long)cc_alloc,
		    (unsigned long long)cp->cache_full.ml_alloc,
		    (unsigned long long)cp->cache_empty.ml_alloc,
		    (unsigned long long)cp->cache_depot_local,
		    (unsigned long long)cp->cache_depot_remote,
		    (unsigned long long)cp->cache_depot_contention,
		    (unsigned long long)rseq_alloc,
		    (unsigned long long)rseq_free,
		    (unsigned long long)rseq_restart);
	}
	(void) mutex_unlock(&umem_cache_lock);
	fflush(fp);
}

static void
umem_add_update_unlocked(umem_cache_t *cp, int flags)
{
	umem_cache_t *cnext, *cprev;

	flags &= ~UMU_ACTIVE;

	if (!flags)
		return;

	if (cp->cache_uflags & UMU_ACTIVE) {
		cp->cache_uflags |= flags;
	} else {
		if (cp->cache_unext != NULL) {
			ASSERT(cp->cache_uflags != 0);
			cp->cache_uflags |= flags;
		} else {
			ASSERT(cp->cache_uflags == 0);
			cp->cache_uflags = flags;
			cp->cache_unext = cnext = &umem_null_cache;
			cp->cache_uprev = cprev = umem_null_cache.cache_uprev;
			cnext->cache_uprev = cp;
			cprev->cache_unext = cp;
		}
	}
}

static void
umem_add_update(umem_cache_t *cp, int flags)
{
	(void) mutex_lock(&umem_update_lock);

	umem_add_update_unlocked(cp, flags);

	if (!IN_UPDATE())
		(void) cond_broadcast(&umem_update_cv);

	(void) mutex_unlock(&umem_update_lock);
}

/*
 * Remove a cache from the update list, waiting for any in-progress work to
 * complete first.
 */
static void
umem_remove_updates(umem_cache_t *cp)
{
	(void) mutex_lock(&umem_update_lock);

	/*
	 * Get it out of the active state
	 */
	while (cp->cache_uflags & UMU_ACTIVE) {
		int cancel_state;

		ASSERT(cp->cache_unext == NULL);

		cp->cache_uflags |= UMU_NOTIFY;

		/*
		 * Make sure the update state is sane, before we wait
		 */
		ASSERT(umem_update_thr != 0 || umem_st_update_thr != 0);
		ASSERT(umem_update_thr != thr_self() &&
		    umem_st_update_thr != thr_self());

		(void) pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
		    &cancel_state);
		(void) cond_wait(&umem_update_cv, &umem_update_lock);
		(void) pthread_setcancelstate(cancel_state, NULL);
	}
	/*
	 * Get it out of the Work Requested state
	 */
	if (cp->cache_unext != NULL) {
		cp->cache_uprev->cache_unext = cp->cache_unext;
		cp->cache_unext->cache_uprev = cp->cache_uprev;
		cp->cache_uprev = cp->cache_unext = NULL;
		cp->cache_uflags = 0;
	}
	/*
	 * Make sure it is in the Inactive state
	 */
	ASSERT(cp->cache_unext == NULL && cp->cache_uflags == 0);
	(void) mutex_unlock(&umem_update_lock);
}

static void
umem_updateall(int flags)
{
	umem_cache_t *cp;

	/*
	 * NOTE:  To prevent deadlock, umem_cache_lock is always acquired first.
	 *
	 * (umem_add_update is called from things run via umem_cache_applyall)
	 */
	(void) mutex_lock(&umem_cache_lock);
	(void) mutex_lock(&umem_update_lock);

	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next)
		umem_add_update_unlocked(cp, flags);

	if (!IN_UPDATE())
		(void) cond_broadcast(&umem_update_cv);

	(void) mutex_unlock(&umem_update_lock);
	(void) mutex_unlock(&umem_cache_lock);
}

/*
 * Debugging support.  Given a buffer address, find its slab.
 */
static umem_slab_t *
umem_findslab(umem_cache_t *cp, void *buf)
{
	umem_slab_t *sp;

	(void) mutex_lock(&cp->cache_lock);
	for (sp = cp->cache_nullslab.slab_next;
	    sp != &cp->cache_nullslab; sp = sp->slab_next) {
		if (UMEM_SLAB_MEMBER(sp, buf)) {
			(void) mutex_unlock(&cp->cache_lock);
			return (sp);
		}
	}
	(void) mutex_unlock(&cp->cache_lock);

	return (NULL);
}

/*
 * Debugger breakpoint hook.  See umem_inspect.h.  Deliberately weak and
 * separate from umem_event_error so the debugger hook stays callable
 * even when UMEM_INSPECT_EVENTS is off.
 */
extern void umem_event_error(int code, void *buf, void *cache);

static void
umem_error(int error, umem_cache_t *cparg, void *bufarg)
{
	umem_buftag_t *btp = NULL;
	umem_bufctl_t *bcp = NULL;
	umem_cache_t *cp = cparg;
	umem_slab_t *sp;
	uint64_t *off;
	void *buf = bufarg;

	umem_event_error(error, buf, cparg);

	int old_logging = umem_logging;

	umem_logging = 0;       /* stop logging when a bad thing happens */

	umem_abort_info.ump_timestamp = gethrtime();

	sp = umem_findslab(cp, buf);
	if (unlikely(sp == NULL)) {
		for (cp = umem_null_cache.cache_prev; cp != &umem_null_cache;
		    cp = cp->cache_prev) {
			if ((sp = umem_findslab(cp, buf)) != NULL)
				break;
		}
	}

	if (unlikely(sp == NULL)) {
		cp = NULL;
		error = UMERR_BADADDR;
	} else {
		if (cp != cparg)
			error = UMERR_BADCACHE;
		else
			buf = (char *)bufarg - ((uintptr_t)bufarg -
			    (uintptr_t)sp->slab_base) % cp->cache_chunksize;
		if (buf != bufarg)
			error = UMERR_BADBASE;
		if (unlikely(cp->cache_flags & UMF_BUFTAG))
			btp = UMEM_BUFTAG(cp, buf);
		if (unlikely(cp->cache_flags & UMF_HASH)) {
			(void) mutex_lock(&cp->cache_lock);
			for (bcp = *UMEM_HASH(cp, buf); bcp; bcp = bcp->bc_next)
				if (bcp->bc_addr == buf)
					break;
			(void) mutex_unlock(&cp->cache_lock);
			if (bcp == NULL && btp != NULL)
				bcp = btp->bt_bufctl;
			if (umem_findslab(cp->cache_bufctl_cache, bcp) ==
			    NULL || P2PHASE((uintptr_t)bcp, UMEM_ALIGN) ||
			    bcp->bc_addr != buf) {
				error = UMERR_BADBUFCTL;
				bcp = NULL;
			}
		}
	}

	umem_abort_info.ump_error = error;
	umem_abort_info.ump_buffer = bufarg;
	umem_abort_info.ump_realbuf = buf;
	umem_abort_info.ump_cache = cparg;
	umem_abort_info.ump_realcache = cp;
	umem_abort_info.ump_slab = sp;
	umem_abort_info.ump_bufctl = bcp;

	umem_printf("umem allocator: ");

	switch (error) {

	case UMERR_MODIFIED:
		umem_printf("buffer modified after being freed\n");
		off = verify_pattern(UMEM_FREE_PATTERN, buf, cp->cache_verify);
		if (off == NULL)        /* shouldn't happen */
			off = buf;
		umem_printf("modification occurred at offset 0x%lx "
		    "(0x%llx replaced by 0x%llx)\n",
		    (uintptr_t)off - (uintptr_t)buf,
		    (longlong_t)UMEM_FREE_PATTERN, (longlong_t)*off);
		break;

	case UMERR_REDZONE:
		umem_printf("redzone violation: write past end of buffer\n");
		break;

	case UMERR_BADADDR:
		umem_printf("invalid free: buffer not in cache\n");
		break;

	case UMERR_DUPFREE:
		umem_printf("duplicate free: buffer freed twice\n");
		break;

	case UMERR_BADBUFTAG:
		umem_printf("boundary tag corrupted\n");
		umem_printf("bcp ^ bxstat = %lx, should be %lx\n",
		    (intptr_t)btp->bt_bufctl ^ btp->bt_bxstat,
		    UMEM_BUFTAG_FREE);
		break;

	case UMERR_BADBUFCTL:
		umem_printf("bufctl corrupted\n");
		break;

	case UMERR_BADCACHE:
		umem_printf("buffer freed to wrong cache\n");
		umem_printf("buffer was allocated from %s,\n", cp->cache_name);
		umem_printf("caller attempting free to %s.\n",
		    cparg->cache_name);
		break;

	case UMERR_BADSIZE:
		umem_printf("bad free: free size (%u) != alloc size (%u)\n",
		    UMEM_SIZE_DECODE(((uint32_t *)btp)[0]),
		    UMEM_SIZE_DECODE(((uint32_t *)btp)[1]));
		break;

	case UMERR_BADBASE:
		umem_printf("bad free: free address (%p) != alloc address "
		    "(%p)\n", bufarg, buf);
		break;
	}

	umem_printf("buffer=%p  bufctl=%p  cache: %s\n",
	    bufarg, (void *)bcp, cparg->cache_name);

	if (bcp != NULL && unlikely(cp->cache_flags & UMF_AUDIT) &&
	    error != UMERR_BADBUFCTL) {
		timespec_t ts;
		hrtime_t diff;
		umem_bufctl_audit_t *bcap = (umem_bufctl_audit_t *)bcp;

		diff = umem_abort_info.ump_timestamp - bcap->bc_timestamp;
		ts.tv_sec = diff / NANOSEC;
		ts.tv_nsec = diff % NANOSEC;

		umem_printf("previous transaction on buffer %p:\n", buf);
		umem_printf("thread=%p  time=T-%ld.%09ld  slab=%p  cache: %s\n",
		    (void *)(intptr_t)bcap->bc_thread, ts.tv_sec, ts.tv_nsec,
		    (void *)sp, cp->cache_name);
		umem_stacktrace_print(bcap->bc_stack,
		    (int)MIN(bcap->bc_depth, umem_stack_depth), NULL);
	}

	umem_err_recoverable("umem: heap corruption detected");

	umem_logging = old_logging;     /* resume logging */
}

void
umem_nofail_callback(umem_nofail_callback_t *cb)
{
	nofail_callback = cb;
}

static int
umem_alloc_retry(umem_cache_t *cp, int umflag)
{
	if (cp == &umem_null_cache) {
		if (umem_init())
			return (1);                             /* retry */
		/*
		 * Initialization failed.  Do normal failure processing.
		 */
	}
	if (unlikely(umem_flags & UMF_CHECKNULL)) {
		umem_err_recoverable("umem: out of heap space");
	}
	if (umflag & UMEM_NOFAIL) {
		int def_result = UMEM_CALLBACK_EXIT(255);
		int result = def_result;
		umem_nofail_callback_t *callback = nofail_callback;

		if (callback != NULL)
			result = callback();

		if (result == UMEM_CALLBACK_RETRY)
			return (1);

		if ((result & ~0xFF) != UMEM_CALLBACK_EXIT(0)) {
			log_message("nofail callback returned %x\n", result);
			result = def_result;
		}

		/*
		 * only one thread will call exit
		 */
		if (umem_nofail_exit_thr == thr_self())
			umem_panic("recursive UMEM_CALLBACK_EXIT()\n");

		(void) mutex_lock(&umem_nofail_exit_lock);
		umem_nofail_exit_thr = thr_self();
		exit(result & 0xFF);
		/*NOTREACHED*/
	}
	return (0);
}

static umem_log_header_t *
umem_log_init(size_t logsize)
{
	umem_log_header_t *lhp;
	int nchunks = 4 * umem_max_ncpus;
	size_t lhsize = offsetof(umem_log_header_t, lh_cpu[umem_max_ncpus]);
	int i;

	if (logsize == 0)
		return (NULL);

	/*
	 * Make sure that lhp->lh_cpu[] is nicely aligned
	 * to prevent false sharing of cache lines.
	 */
	lhsize = P2ROUNDUP(lhsize, UMEM_ALIGN);
	lhp = vmem_xalloc(umem_log_arena, lhsize, 64, P2NPHASE(lhsize, 64), 0,
	    NULL, NULL, VM_NOSLEEP);
	if (unlikely(lhp == NULL))
		goto fail;

	bzero(lhp, lhsize);

	(void) mutex_init(&lhp->lh_lock, USYNC_THREAD, NULL);
	lhp->lh_nchunks = nchunks;
	lhp->lh_chunksize = P2ROUNDUP(logsize / nchunks, PAGESIZE);
	if (lhp->lh_chunksize == 0)
		lhp->lh_chunksize = PAGESIZE;

	lhp->lh_base = vmem_alloc(umem_log_arena,
	    lhp->lh_chunksize * nchunks, VM_NOSLEEP);
	if (unlikely(lhp->lh_base == NULL))
		goto fail;

	lhp->lh_free = vmem_alloc(umem_log_arena,
	    nchunks * sizeof (int), VM_NOSLEEP);
	if (unlikely(lhp->lh_free == NULL))
		goto fail;

	bzero(lhp->lh_base, lhp->lh_chunksize * nchunks);

	for (i = 0; i < umem_max_ncpus; i++) {
		umem_cpu_log_header_t *clhp = &lhp->lh_cpu[i];
		(void) mutex_init(&clhp->clh_lock, USYNC_THREAD, NULL);
		clhp->clh_chunk = i;
	}

	for (i = umem_max_ncpus; i < nchunks; i++)
		lhp->lh_free[i] = i;

	lhp->lh_head = umem_max_ncpus;
	lhp->lh_tail = 0;

	return (lhp);

fail:
	if (lhp != NULL) {
		if (lhp->lh_base != NULL)
			vmem_free(umem_log_arena, lhp->lh_base,
			    lhp->lh_chunksize * nchunks);

		vmem_xfree(umem_log_arena, lhp, lhsize);
	}
	return (NULL);
}

static void *
umem_log_enter(umem_log_header_t *lhp, void *data, size_t size)
{
	void *logspace;
	umem_cpu_log_header_t *clhp =
	    &lhp->lh_cpu[CPU(umem_cpu_mask)->cpu_number];

	if (unlikely(lhp == NULL || umem_logging == 0))
		return (NULL);

	(void) mutex_lock(&clhp->clh_lock);
	clhp->clh_hits++;
	if (size > clhp->clh_avail) {
		(void) mutex_lock(&lhp->lh_lock);
		lhp->lh_hits++;
		lhp->lh_free[lhp->lh_tail] = clhp->clh_chunk;
		lhp->lh_tail = (lhp->lh_tail + 1) % lhp->lh_nchunks;
		clhp->clh_chunk = lhp->lh_free[lhp->lh_head];
		lhp->lh_head = (lhp->lh_head + 1) % lhp->lh_nchunks;
		clhp->clh_current = lhp->lh_base +
		    clhp->clh_chunk * lhp->lh_chunksize;
		clhp->clh_avail = lhp->lh_chunksize;
		if (size > lhp->lh_chunksize)
			size = lhp->lh_chunksize;
		(void) mutex_unlock(&lhp->lh_lock);
	}
	logspace = clhp->clh_current;
	clhp->clh_current += size;
	clhp->clh_avail -= size;
	bcopy(data, logspace, size);
	(void) mutex_unlock(&clhp->clh_lock);
	return (logspace);
}

#define UMEM_AUDIT(lp, cp, bcp)                                         \
{                                                                       \
	umem_bufctl_audit_t *_bcp = (umem_bufctl_audit_t *)(bcp);       \
	_bcp->bc_timestamp = gethrtime();                               \
	_bcp->bc_thread = thr_self();                                   \
	_bcp->bc_depth = getpcstack(_bcp->bc_stack, umem_stack_depth,   \
	    (cp != NULL) && unlikely(cp->cache_flags & UMF_CHECKSIGNAL));       \
	_bcp->bc_lastlog = umem_log_enter((lp), _bcp,                   \
	    UMEM_BUFCTL_AUDIT_SIZE);                                    \
}

static void
umem_log_event(umem_log_header_t *lp, umem_cache_t *cp,
	umem_slab_t *sp, void *addr)
{
	umem_bufctl_audit_t *bcp;
	UMEM_LOCAL_BUFCTL_AUDIT(&bcp);

	bzero(bcp, UMEM_BUFCTL_AUDIT_SIZE);
	bcp->bc_addr = addr;
	bcp->bc_slab = sp;
	bcp->bc_cache = cp;
	UMEM_AUDIT(lp, cp, bcp);
}

/*
 * Per-process secret XORed into every slab freelist link (P5.4).  See
 * UMEM_LINK_MANGLE() in umem_impl.h for what it protects and why.
 *
 * Zero means "not yet derived".  umem_link_cookie_init() is idempotent by
 * construction -- every source it mixes is fixed for the life of the process
 * -- so concurrent callers compute and store the SAME value and the race is
 * benign, which is why this needs no lock, no CAS and no pthread_once (and
 * therefore no allocation, which umem_init() cannot tolerate).
 *
 * Ordering: the only producer of mangled links is umem_slab_create(), which
 * calls this before building any, and it publishes the finished slab by
 * linking it into cache_freelist under cache_lock.  Every other thread
 * reaches a link only through a slab it found under that same lock, so the
 * lock already orders the cookie store ahead of any demangle.
 */
uintptr_t umem_link_cookie = 0;

#if defined(__has_include)
#if __has_include(<sys/auxv.h>)
#include <sys/auxv.h>
#define	UMEM_HAVE_AUXV	1
#endif
#endif

static void
umem_link_cookie_init(void)
{
	uintptr_t c = 0;

	if (likely(umem_link_cookie != 0))
		return;

#if defined(UMEM_HAVE_AUXV) && defined(AT_RANDOM)
	{
		/*
		 * AT_RANDOM is 16 kernel-supplied random bytes placed in auxv
		 * at exec time.  Reading it is a pure function of the process
		 * image: no syscall, no allocation, no file descriptor, and
		 * the same answer in every thread -- which is what makes this
		 * initializer idempotent.
		 */
		const unsigned long at = getauxval(AT_RANDOM);
		if (at != 0) {
			uintptr_t v;
			bcopy((const void *)(uintptr_t)at, &v, sizeof (v));
			c = v;
		}
	}
#endif

	/*
	 * Fallback, and additional mixing: the load address of a static in
	 * this object (ASLR) and the process id.  Weaker than AT_RANDOM -- it
	 * is one mapping's worth of entropy, not the kernel's -- but it is
	 * never worse than the unmangled behaviour this replaces, and it keeps
	 * platforms without auxv from silently getting cookie 0.
	 */
	c ^= (uintptr_t)&umem_link_cookie;
	c ^= (uintptr_t)getpid() * 0x9e3779b97f4a7c15ULL;

	/*
	 * Never publish 0: that is the "not yet derived" sentinel, so a
	 * cookie of 0 would make every later call recompute.  Correct either
	 * way, but pointless work on the allocation path.
	 */
	umem_link_cookie = c | 1;
}

/*
 * Validate a demangled slab freelist link before it is dereferenced or
 * handed out (P5.4).
 *
 * The mangling in UMEM_LINK_MANGLE() means an attacker who overwrites a link
 * with a chosen address gets an unpredictable demangled value, not that
 * address.  Two cheap structural checks turn "unpredictable" into "reported":
 *
 *   - alignment: bufctls are UMEM_ALIGN-aligned, as glibc's safe-linking
 *     also checks;
 *   - containment: a slab freelist link points at a bufctl belonging to THIS
 *     slab, so for a non-hash cache the buffer it names must be inside the
 *     slab (UMEM_SLAB_MEMBER), and for a hash cache the bufctl itself lives
 *     outside the buffer region and is only reachable through metadata the
 *     overflow class cannot touch, so the offset check does not apply.
 *
 * NULL is the legal end of the freelist and passes.  Returns nonzero if the
 * link is usable; the caller reports UMERR_BADADDR and stops otherwise --
 * deliberately without dereferencing bcp.
 *
 * ponytail: structural checks only, not a MAC.  They reject the overwhelming
 * majority of corrupted links (any non-multiple-of-8 value, and any value
 * outside one slab-sized window) but cannot distinguish a forged link that
 * happens to satisfy both from a real one.  Upgrade path if that ever
 * matters: keep a per-slab bitmap of free chunks and check membership, which
 * costs a load and a bit test on the same hot path.
 */
static int
umem_slab_link_valid(umem_cache_t *cp, umem_slab_t *sp, umem_bufctl_t *bcp)
{
	if (bcp == NULL)
		return (1);
	if (P2PHASE((uintptr_t)bcp, UMEM_ALIGN) != 0)
		return (0);
	if (!(cp->cache_flags & UMF_HASH) &&
	    !UMEM_SLAB_MEMBER(sp, UMEM_BUF(cp, bcp)))
		return (0);
	return (1);
}

/*
 * Create a new slab for cache cp.
 */
static umem_slab_t *
umem_slab_create(umem_cache_t *cp, int umflag)
{
	size_t slabsize = cp->cache_slabsize;
	size_t chunksize = cp->cache_chunksize;
	int cache_flags = cp->cache_flags;
	size_t color, chunks;
	char *buf, *slab;
	umem_slab_t *sp;
	umem_bufctl_t *bcp;
	vmem_t *vmp = cp->cache_arena;

	/*
	 * Before any mangled link is built.  See umem_link_cookie_init().
	 */
	umem_link_cookie_init();

	/*
	 * Slab coloring: rotate through different offsets to reduce
	 * cache conflicts. Protected by cache_lock to prevent data races.
	 */
	(void) mutex_lock(&cp->cache_lock);
	color = cp->cache_color + cp->cache_align;
	if (color > cp->cache_maxcolor)
		color = cp->cache_mincolor;
	cp->cache_color = color;
	(void) mutex_unlock(&cp->cache_lock);

	slab = vmem_alloc(vmp, slabsize, UMEM_VMFLAGS(umflag));

	if (unlikely(slab == NULL))
		goto vmem_alloc_failure;

	ASSERT(P2PHASE((uintptr_t)slab, vmp->vm_quantum) == 0);

	if (!(cp->cache_cflags & UMC_NOTOUCH) &&
	    unlikely(cp->cache_flags & UMF_DEADBEEF))
		copy_pattern(UMEM_UNINITIALIZED_PATTERN, slab, slabsize);

	if (unlikely(cache_flags & UMF_HASH)) {
		if (unlikely((sp = _umem_cache_alloc(umem_slab_cache, umflag)) == NULL))
			goto slab_alloc_failure;
		chunks = (slabsize - color) / chunksize;
	} else {
		sp = UMEM_SLAB(cp, slab);
		chunks = (slabsize - sizeof (umem_slab_t) - color) / chunksize;
	}

	sp->slab_cache  = cp;
	sp->slab_head   = NULL;
	sp->slab_refcnt = 0;
	sp->slab_state  = SLAB_ACTIVE;
	sp->slab_idle_time = 0;
	sp->slab_base   = buf = slab + color;
	sp->slab_chunks = chunks;

	ASSERT(chunks > 0);
	while (chunks-- != 0) {
		if (unlikely(cache_flags & UMF_HASH)) {
			bcp = _umem_cache_alloc(cp->cache_bufctl_cache, umflag);
			if (unlikely(bcp == NULL))
				goto bufctl_alloc_failure;
			if (unlikely(cache_flags & UMF_AUDIT)) {
				umem_bufctl_audit_t *bcap =
				    (umem_bufctl_audit_t *)bcp;
				bzero(bcap, UMEM_BUFCTL_AUDIT_SIZE);
				bcap->bc_cache = cp;
			}
			bcp->bc_addr = buf;
			bcp->bc_slab = sp;
		} else {
			bcp = UMEM_BUFCTL(cp, buf);
		}
		if (unlikely(cache_flags & UMF_BUFTAG)) {
			umem_buftag_t *btp = UMEM_BUFTAG(cp, buf);
			btp->bt_redzone = UMEM_REDZONE_PATTERN;
			btp->bt_bufctl = bcp;
			btp->bt_bxstat = (intptr_t)bcp ^ UMEM_BUFTAG_FREE;
			if (unlikely(cache_flags & UMF_DEADBEEF)) {
				copy_pattern(UMEM_FREE_PATTERN, buf,
				    cp->cache_verify);
			}
		}
		bcp->bc_next = UMEM_LINK_MANGLE(&bcp->bc_next, sp->slab_head);
		sp->slab_head = bcp;
		buf += chunksize;
	}

	umem_log_event(umem_slab_log, cp, sp, slab);

	return (sp);

bufctl_alloc_failure:

	while ((bcp = sp->slab_head) != NULL) {
		sp->slab_head = UMEM_LINK_DEMANGLE(&bcp->bc_next, bcp->bc_next);
		_umem_cache_free(cp->cache_bufctl_cache, bcp);
	}
	_umem_cache_free(umem_slab_cache, sp);

slab_alloc_failure:

	vmem_free(vmp, slab, slabsize);

vmem_alloc_failure:

	umem_log_event(umem_failure_log, cp, NULL, NULL);
	atomic_add_64(&cp->cache_alloc_fail, 1);

	return (NULL);
}

/*
 * Destroy a slab.
 */
static void
umem_slab_destroy(umem_cache_t *cp, umem_slab_t *sp)
{
	vmem_t *vmp = cp->cache_arena;
	void *slab = (void *)P2ALIGN((uintptr_t)sp->slab_base, vmp->vm_quantum);

	if (unlikely(cp->cache_flags & UMF_HASH)) {
		umem_bufctl_t *bcp;
		while ((bcp = sp->slab_head) != NULL) {
			sp->slab_head =
			    UMEM_LINK_DEMANGLE(&bcp->bc_next, bcp->bc_next);
			_umem_cache_free(cp->cache_bufctl_cache, bcp);
		}
		_umem_cache_free(umem_slab_cache, sp);
	}
	vmem_free(vmp, slab, cp->cache_slabsize);
}

/*
 * Allocate a raw (unconstructed) buffer from cp's slab layer.
 */
static void *
umem_slab_alloc(umem_cache_t *cp, int umflag)
{
	umem_bufctl_t *bcp, *next, **hash_bucket;
	umem_slab_t *sp;
	void *buf;

	(void) mutex_lock(&cp->cache_lock);
	cp->cache_slab_alloc++;
	sp = cp->cache_freelist;

	/*
	 * Prefetch slab metadata for the allocation path.
	 * Medium locality (2) since we'll be accessing multiple
	 * fields of this slab during the allocation.
	 */
	__builtin_prefetch(sp, 0, 2);

	ASSERT(sp->slab_cache == cp);
	if (sp->slab_head == NULL) {
		/*
		 * The freelist is empty.  Create a new slab.
		 */
		(void) mutex_unlock(&cp->cache_lock);
		if (cp == &umem_null_cache)
			return (NULL);
		if ((sp = umem_slab_create(cp, umflag)) == NULL)
			return (NULL);
		(void) mutex_lock(&cp->cache_lock);
		cp->cache_slab_create++;
		if ((cp->cache_buftotal += sp->slab_chunks) > cp->cache_bufmax)
			cp->cache_bufmax = cp->cache_buftotal;
		sp->slab_next = cp->cache_freelist;
		sp->slab_prev = cp->cache_freelist->slab_prev;
		sp->slab_next->slab_prev = sp;
		sp->slab_prev->slab_next = sp;
		cp->cache_freelist = sp;
	}

	/*
	 * Skip slabs being reclaimed (madvise in progress).
	 * Walk forward to find a usable slab, or create a new one.
	 */
	if (sp->slab_state == SLAB_RECLAIMING) {
		umem_slab_t *nullsp = &cp->cache_nullslab;
		sp = sp->slab_next;
		while (sp != nullsp && (sp->slab_state == SLAB_RECLAIMING ||
		    sp->slab_head == NULL))
			sp = sp->slab_next;
		if (sp == nullsp || sp->slab_head == NULL) {
			(void) mutex_unlock(&cp->cache_lock);
			if (cp == &umem_null_cache)
				return (NULL);
			if ((sp = umem_slab_create(cp, umflag)) == NULL)
				return (NULL);
			(void) mutex_lock(&cp->cache_lock);
			cp->cache_slab_create++;
			if ((cp->cache_buftotal += sp->slab_chunks) >
			    cp->cache_bufmax)
				cp->cache_bufmax = cp->cache_buftotal;
			sp->slab_next = cp->cache_freelist;
			sp->slab_prev = cp->cache_freelist->slab_prev;
			sp->slab_next->slab_prev = sp;
			sp->slab_prev->slab_next = sp;
			cp->cache_freelist = sp;
		}
	}

	/*
	 * Reactivate a slab that was idle (DIRTY or CLEAN).
	 *
	 * Nothing has to be rebuilt here, and that is an invariant the
	 * reclaim side maintains, not an accident: a CLEAN slab only ever
	 * had pages discarded if the cache keeps no allocator metadata
	 * inside its buffer region (no embedded bufctls, no buftags/free
	 * pattern).  See umem_slab_keeps_metadata().  A zero-filled page is
	 * therefore indistinguishable from an untouched one to every reader
	 * below this point.  It is NOT sufficient to rely on the
	 * constructor: umem_cache_alloc_debug() validates bt_bxstat and the
	 * free pattern before the constructor runs.
	 *
	 * slab_state is read and written only under cache_lock, which this
	 * path holds.
	 */
	if (sp->slab_state != SLAB_ACTIVE) {
		sp->slab_state = SLAB_ACTIVE;
		sp->slab_idle_time = 0;
	}

	sp->slab_refcnt++;
	ASSERT(sp->slab_refcnt <= sp->slab_chunks);

	/*
	 * If we're taking the last buffer in the slab,
	 * remove the slab from the cache's freelist.
	 *
	 * The link is mangled (P5.4): demangle, then validate before it is
	 * published as the new slab_head or followed by anyone.  A corrupted
	 * link -- which for a non-hash cache means an overflow reached a freed
	 * buffer's tail -- is reported and the allocation fails, rather than
	 * the allocator handing out an attacker-named address.
	 */
	bcp = sp->slab_head;
	next = UMEM_LINK_DEMANGLE(&bcp->bc_next, bcp->bc_next);
	if (unlikely(!umem_slab_link_valid(cp, sp, next))) {
		void *badbuf = (cp->cache_flags & UMF_HASH) ?
		    bcp->bc_addr : UMEM_BUF(cp, bcp);
		/*
		 * Truncate the freelist at the corrupted link rather than
		 * leaving it in place: it is not followable, and leaving it
		 * would re-report on every subsequent allocation.  The free
		 * chunks beyond it become unreachable, which is the correct
		 * trade against following a corrupted chain; refcnt is put
		 * back because no buffer is handed out, so the slab is still
		 * destroyed normally once its live buffers are returned.
		 */
		sp->slab_head = NULL;
		cp->cache_freelist = sp->slab_next;
		sp->slab_refcnt--;
		(void) mutex_unlock(&cp->cache_lock);
		umem_error(UMERR_BADADDR, cp, badbuf);
		return (NULL);
	}
	if ((sp->slab_head = next) == NULL) {
		cp->cache_freelist = sp->slab_next;
		ASSERT(sp->slab_refcnt == sp->slab_chunks);
	}

	if (unlikely(cp->cache_flags & UMF_HASH)) {
		/*
		 * Add buffer to allocated-address hash table.
		 */
		buf = bcp->bc_addr;
		hash_bucket = UMEM_HASH(cp, buf);
		bcp->bc_next = *hash_bucket;
		*hash_bucket = bcp;
		if ((cp->cache_flags & (UMF_AUDIT | UMF_BUFTAG)) == UMF_AUDIT) {
			UMEM_AUDIT(umem_transaction_log, cp, bcp);
		}
	} else {
		buf = UMEM_BUF(cp, bcp);
	}

	ASSERT(UMEM_SLAB_MEMBER(sp, buf));

	(void) mutex_unlock(&cp->cache_lock);

	return (buf);
}

/*
 * Free a raw (unconstructed) buffer to cp's slab layer.
 */
static void
umem_slab_free(umem_cache_t *cp, void *buf)
{
	umem_slab_t *sp;
	umem_bufctl_t *bcp, **prev_bcpp;

	ASSERT(buf != NULL);

	(void) mutex_lock(&cp->cache_lock);
	cp->cache_slab_free++;

	if (unlikely(cp->cache_flags & UMF_HASH)) {
		/*
		 * Look up buffer in allocated-address hash table.
		 */
		prev_bcpp = UMEM_HASH(cp, buf);
		while ((bcp = *prev_bcpp) != NULL) {
			if (bcp->bc_addr == buf) {
				*prev_bcpp = bcp->bc_next;
				sp = bcp->bc_slab;
				break;
			}
			cp->cache_lookup_depth++;
			prev_bcpp = &bcp->bc_next;
		}
	} else {
		bcp = UMEM_BUFCTL(cp, buf);
		sp = UMEM_SLAB(cp, buf);
	}

	if (bcp == NULL || sp->slab_cache != cp || !UMEM_SLAB_MEMBER(sp, buf)) {
		(void) mutex_unlock(&cp->cache_lock);
		umem_error(UMERR_BADADDR, cp, buf);
		return;
	}

	if ((cp->cache_flags & (UMF_AUDIT | UMF_BUFTAG)) == UMF_AUDIT) {
		if (unlikely(cp->cache_flags & UMF_CONTENTS))
			((umem_bufctl_audit_t *)bcp)->bc_contents =
			    umem_log_enter(umem_content_log, buf,
			    cp->cache_contents);
		UMEM_AUDIT(umem_transaction_log, cp, bcp);
	}

	/*
	 * If this slab isn't currently on the freelist, put it there.
	 */
	if (sp->slab_head == NULL) {
		ASSERT(sp->slab_refcnt == sp->slab_chunks);
		ASSERT(cp->cache_freelist != sp);
		sp->slab_next->slab_prev = sp->slab_prev;
		sp->slab_prev->slab_next = sp->slab_next;
		sp->slab_next = cp->cache_freelist;
		sp->slab_prev = cp->cache_freelist->slab_prev;
		sp->slab_next->slab_prev = sp;
		sp->slab_prev->slab_next = sp;
		cp->cache_freelist = sp;
	}

	/* Mangled on store; see UMEM_LINK_MANGLE (P5.4). */
	bcp->bc_next = UMEM_LINK_MANGLE(&bcp->bc_next, sp->slab_head);
	sp->slab_head = bcp;

	ASSERT(sp->slab_refcnt >= 1);
	if (--sp->slab_refcnt == 0) {
		if (umem_reclaim_enabled) {
			/*
			 * Mark the slab as dirty and leave it on the
			 * freelist.  The update thread will madvise the
			 * pages away after umem_reclaim_delay seconds.
			 */
			sp->slab_state = SLAB_DIRTY;
			sp->slab_idle_time = 0;
		} else {
			/*
			 * Reclaim disabled: destroy the slab immediately.
			 */
			sp->slab_next->slab_prev = sp->slab_prev;
			sp->slab_prev->slab_next = sp->slab_next;
			if (sp == cp->cache_freelist)
				cp->cache_freelist = sp->slab_next;
			cp->cache_slab_destroy++;
			cp->cache_buftotal -= sp->slab_chunks;
			(void) mutex_unlock(&cp->cache_lock);
			umem_slab_destroy(cp, sp);
			return;
		}
	}
	(void) mutex_unlock(&cp->cache_lock);
}

static int
umem_cache_alloc_debug(umem_cache_t *cp, void *buf, int umflag)
{
	umem_buftag_t *btp = UMEM_BUFTAG(cp, buf);
	umem_bufctl_audit_t *bcp = (umem_bufctl_audit_t *)btp->bt_bufctl;
	uint32_t mtbf;
	int flags_nfatal;

	if (btp->bt_bxstat != ((intptr_t)bcp ^ UMEM_BUFTAG_FREE)) {
		umem_error(UMERR_BADBUFTAG, cp, buf);
		return (-1);
	}

	btp->bt_bxstat = (intptr_t)bcp ^ UMEM_BUFTAG_ALLOC;

	if (unlikely(cp->cache_flags & UMF_HASH) && bcp->bc_addr != buf) {
		umem_error(UMERR_BADBUFCTL, cp, buf);
		return (-1);
	}

	btp->bt_redzone = UMEM_REDZONE_PATTERN;

	if (unlikely(cp->cache_flags & UMF_DEADBEEF)) {
		if (verify_and_copy_pattern(UMEM_FREE_PATTERN,
		    UMEM_UNINITIALIZED_PATTERN, buf, cp->cache_verify)) {
			umem_error(UMERR_MODIFIED, cp, buf);
			return (-1);
		}
	}

	if ((mtbf = umem_mtbf | cp->cache_mtbf) != 0 &&
	    gethrtime() % mtbf == 0 &&
	    (umflag & (UMEM_FATAL_FLAGS)) == 0) {
		umem_log_event(umem_failure_log, cp, NULL, NULL);
	} else {
		mtbf = 0;
	}

	/*
	 * We do not pass fatal flags on to the constructor.  This prevents
	 * leaking buffers in the event of a subordinate constructor failing.
	 */
	flags_nfatal = UMEM_DEFAULT;
	if (mtbf || (cp->cache_constructor != NULL &&
	    cp->cache_constructor(buf, cp->cache_private, flags_nfatal) != 0)) {
		atomic_add_64(&cp->cache_alloc_fail, 1);
		btp->bt_bxstat = (intptr_t)bcp ^ UMEM_BUFTAG_FREE;
		copy_pattern(UMEM_FREE_PATTERN, buf, cp->cache_verify);
		umem_slab_free(cp, buf);
		return (-1);
	}

	if (unlikely(cp->cache_flags & UMF_AUDIT)) {
		UMEM_AUDIT(umem_transaction_log, cp, bcp);
	}

	return (0);
}

static int
umem_cache_free_debug(umem_cache_t *cp, void *buf)
{
	umem_buftag_t *btp = UMEM_BUFTAG(cp, buf);
	umem_bufctl_audit_t *bcp = (umem_bufctl_audit_t *)btp->bt_bufctl;
	umem_slab_t *sp;

	if (btp->bt_bxstat != ((intptr_t)bcp ^ UMEM_BUFTAG_ALLOC)) {
		if (btp->bt_bxstat == ((intptr_t)bcp ^ UMEM_BUFTAG_FREE)) {
			umem_error(UMERR_DUPFREE, cp, buf);
			return (-1);
		}
		sp = umem_findslab(cp, buf);
		if (sp == NULL || sp->slab_cache != cp)
			umem_error(UMERR_BADADDR, cp, buf);
		else
			umem_error(UMERR_REDZONE, cp, buf);
		return (-1);
	}

	btp->bt_bxstat = (intptr_t)bcp ^ UMEM_BUFTAG_FREE;

	if (unlikely(cp->cache_flags & UMF_HASH) && bcp->bc_addr != buf) {
		umem_error(UMERR_BADBUFCTL, cp, buf);
		return (-1);
	}

	if (btp->bt_redzone != UMEM_REDZONE_PATTERN) {
		umem_error(UMERR_REDZONE, cp, buf);
		return (-1);
	}

	if (unlikely(cp->cache_flags & UMF_AUDIT)) {
		if (unlikely(cp->cache_flags & UMF_CONTENTS))
			bcp->bc_contents = umem_log_enter(umem_content_log,
			    buf, cp->cache_contents);
		UMEM_AUDIT(umem_transaction_log, cp, bcp);
	}

	if (cp->cache_destructor != NULL)
		cp->cache_destructor(buf, cp->cache_private);

	if (unlikely(cp->cache_flags & UMF_DEADBEEF))
		copy_pattern(UMEM_FREE_PATTERN, buf, cp->cache_verify);

	return (0);
}

/*
 * Free each object in magazine mp to cp's slab layer, and free mp itself.
 */
static void
umem_magazine_destroy(umem_cache_t *cp, umem_magazine_t *mp, int nrounds)
{
	int round;

	ASSERT(cp->cache_next == NULL || IN_UPDATE());

	/*
	 * Fast path: Use SIMD to check if magazine has any allocations.
	 * If all slots are NULL, skip the loop entirely.
	 * This optimization helps during cleanup of empty magazines.
	 */
	if (nrounds > 0 && !umem_mag_scan_notnull(mp->mag_round, nrounds)) {
		goto done;
	}

	for (round = 0; round < nrounds; round++) {
		void *buf = mp->mag_round[round];

		/*
		 * Prefetch next 4 magazine slots ahead during batch operations.
		 * Low locality (1) since we only access each slot once.
		 * This helps pipeline the loop by bringing future slots into
		 * cache while processing current ones.
		 */
		if (round + 4 < nrounds) {
			__builtin_prefetch(&mp->mag_round[round + 4], 0, 1);
		}

		if (unlikely(cp->cache_flags & UMF_DEADBEEF) &&
		    verify_pattern(UMEM_FREE_PATTERN, buf,
		    cp->cache_verify) != NULL) {
			umem_error(UMERR_MODIFIED, cp, buf);
			continue;
		}

		if (!unlikely(cp->cache_flags & UMF_BUFTAG) &&
		    cp->cache_destructor != NULL)
			cp->cache_destructor(buf, cp->cache_private);

		umem_slab_free(cp, buf);
	}

done:
	ASSERT(UMEM_MAGAZINE_VALID(cp, mp));
	atomic_add_64(&cp->cache_mag_total, -1ULL);
	_umem_cache_free(cp->cache_magtype->mt_cache, mp);
}

/*
 * Atomic load of tagged pointer.
 * Uses C11 acquire ordering for safe publication.
 */
static inline umem_tagged_ptr_t
atomic_load_tagged_ptr(volatile umem_tagged_ptr_t *ptr)
{
	umem_tagged_ptr_t val;
	val.raw = atomic_load_explicit(
	    (_Atomic(uint64_t) *)&ptr->raw, memory_order_acquire);
	return val;
}

/*
 * Atomic compare-and-swap of tagged pointer.
 * Returns 1 on success, 0 on failure.
 * On failure, 'expected' is updated with the current value.
 * Uses acq_rel for success (publish new head), acquire for failure.
 */
static inline int
atomic_cas_tagged_ptr(volatile umem_tagged_ptr_t *ptr,
                     umem_tagged_ptr_t *expected,
                     umem_tagged_ptr_t desired)
{
	return atomic_compare_exchange_strong_explicit(
	    (_Atomic(uint64_t) *)&ptr->raw,
	    &expected->raw, desired.raw,
	    memory_order_acq_rel, memory_order_acquire);
}

/*
 * Select depot stripe based on thread ID and NUMA node.
 * When NUMA is enabled, stripes are partitioned by node so threads on the
 * the depot is a cold path — complexity budget goes to the magazine
 * fast path, not here.
 */

/*
 * P8.5 ledger (COMPILED OUT unless -DUMEM_PTC_RESIZE_PROBE, like the P1.3
 * probes below): depot stripe pops attempted, and how many of them found the
 * list EMPTY.  A pop that takes a stripe's lock only to read a NULL head paid
 * a lock/unlock pair and held that stripe against the CPU that owns it, for
 * nothing.  With the unlocked empty check, the "empty" count still rises but
 * no lock was taken for it; the regression counts the LOCKED empties by
 * subtraction (see test_depot_empty_scan).
 */
#ifdef UMEM_PTC_RESIZE_PROBE
volatile long umem_ptc_probe_depot_pops = 0;
volatile long umem_ptc_probe_depot_pops_empty = 0;
volatile long umem_ptc_probe_depot_pops_locked_empty = 0;
#define	UMEM_PTC_PROBE_DEPOT_POP(empty)	\
	do {	\
		(void) atomic_add_64((uint64_t *)&umem_ptc_probe_depot_pops, 1); \
		if (empty)	\
			(void) atomic_add_64((uint64_t *)	\
			    &umem_ptc_probe_depot_pops_empty, 1);	\
	} while (0)
#define	UMEM_PTC_PROBE_DEPOT_POP_LOCKED_EMPTY()	\
	(void) atomic_add_64((uint64_t *)&umem_ptc_probe_depot_pops_locked_empty, 1)
#else
#define	UMEM_PTC_PROBE_DEPOT_POP(empty)	((void)0)
#define	UMEM_PTC_PROBE_DEPOT_POP_LOCKED_EMPTY()	((void)0)
#endif

/*
 * Pop a magazine from a single maglist, returning NULL if empty.
 * Caller does NOT hold mlp->ml_lock; this function acquires it.
 * Tracks contention on the cache if trylock fails.
 *
 * EMPTY-CHECK BEFORE THE LOCK (P8.5).  Both pop primitives read ml_list
 * unlocked first and return NULL if it is NULL.  The read is racy and that
 * is fine: a magazine pushed a moment later is found on the next call, which
 * is exactly the outcome of a failed trylock and of the blocking pop's own
 * miss.  What it buys: a steal scan over ncpus stripes, almost all empty,
 * no longer takes and releases every stripe's lock to read a NULL head, and
 * so no longer holds each stripe's lock against the CPU that owns it.  frag
 * 16:64 at t=8 on c7i.2xlarge: 434k blocking-pop contention events per 80k
 * successful reloads (5.4 per success) before; see the P8.5 STATUS entry.
 */
static umem_magazine_t *
umem_depot_pop(umem_cache_t *cp, umem_maglist_t *mlp)
{
	umem_magazine_t *mp;

	/*
	 * Prefetch the list head before acquiring the lock.
	 * On a lock miss, this lets the cache line warm up
	 * during the mutex spin/sleep, saving ~50-100ns on
	 * the critical path after the lock is acquired.
	 */
	UMEM_PREFETCH_READ(&mlp->ml_list);

	if (*(umem_magazine_t *volatile *)&mlp->ml_list == NULL) {
		UMEM_PTC_PROBE_DEPOT_POP(1);
		return (NULL);
	}

	if (mutex_trylock(&mlp->ml_lock) != 0) {
		atomic_add_64(&cp->cache_depot_contention, 1);
		(void) mutex_lock(&mlp->ml_lock);
	}

	mp = mlp->ml_list;
	UMEM_PTC_PROBE_DEPOT_POP(mp == NULL);
	if (mp == NULL) {
		UMEM_PTC_PROBE_DEPOT_POP_LOCKED_EMPTY();
		(void) mutex_unlock(&mlp->ml_lock);
		return (NULL);
	}

	mlp->ml_list = mp->mag_next;
	mlp->ml_total--;
	if (mlp->ml_total < mlp->ml_min)
		mlp->ml_min = mlp->ml_total;
	mlp->ml_alloc++;

	(void) mutex_unlock(&mlp->ml_lock);

	/*
	 * Prefetch the first few magazine rounds so they are
	 * warm in cache when the caller starts allocating.
	 */
	UMEM_PREFETCH_READ(&mp->mag_round[0]);

	return (mp);
}

/*
 * Push a magazine onto a single maglist.
 */
static void
umem_depot_push(umem_maglist_t *mlp, umem_magazine_t *mp)
{
	(void) mutex_lock(&mlp->ml_lock);

	mp->mag_next = mlp->ml_list;
	mlp->ml_list = mp;
	mlp->ml_total++;

	(void) mutex_unlock(&mlp->ml_lock);
}

/* Forward declaration for use in trylock depot functions */
static void umem_depot_destroy_stale(umem_cache_t *, int, umem_magazine_t *);

/*
 * The cache a magazine shell was actually allocated from.
 *
 * A magazine is an object of some umem_magazine_<N> cache, so its own slab
 * header names its source magtype cache.  This is knowable from the magazine
 * ALONE, which is the point: cp->cache_magtype can change under us at any
 * time (umem_cache_magazine_resize, from the update thread), so it is not a
 * safe way to describe a magazine we are already holding.
 */
static inline umem_cache_t *
umem_mag_source_cache(umem_magazine_t *mp)
{
	return (((umem_slab_t *)P2END((uintptr_t)(mp), PAGESIZE) - 1)->
	    slab_cache);
}

/*
 * A magazine's OWN capacity, in rounds.
 *
 * umem_cache_init() creates each magtype cache with bufsize
 * (mt_magsize + 1) * sizeof (void *) -- one slot for mag_next plus magsize
 * rounds -- so the inverse recovers the true capacity of this magazine,
 * whatever magtype the cache has moved on to.
 *
 * Use this, never cp->cache_magtype->mt_magsize, to bound indexing into a
 * magazine already in hand (P1.3b): a resize between obtaining the magazine
 * and reading the cache's current magtype would otherwise index an old
 * 127-round magazine as if it had 255, past the end of its allocation.
 */
static inline int
umem_mag_capacity(umem_magazine_t *mp)
{
	return ((int)(umem_mag_source_cache(mp)->cache_bufsize /
	    sizeof (void *)) - 1);
}

/*
 * Test-only probe for the P1.3b window.
 *
 * COMPILED OUT unless -DUMEM_PTC_RESIZE_PROBE is passed; the default build has
 * no trace of it, not even a load of a global.  It exists because the window it
 * widens is a few nanoseconds long and each cache resizes at most ONCE per
 * process (a magtype only grows, and every size class has at most one step to
 * take), so the race cannot be reached by volume: 67 million invariant samples
 * across 96 threads on a 192-vCPU arm-hi box hit it zero times on a build with
 * the defect present.
 *
 * The probe adds a delay at the one point that matters -- between obtaining a
 * magazine and deciding what capacity to use it with -- so that when the single
 * genuine resize lands, threads are actually sitting inside the window.  It
 * changes NO decision: the resize is still the ordinary
 * contention-scheduled one, and any desync observed is the library's own.
 */
#ifdef UMEM_PTC_RESIZE_PROBE
#include <time.h>
/*
 * Test-only gate for the P1.3b window, driven by
 * test/stress/repro_ptc_resize_capacity.c.
 *
 * A timed delay here is not enough, and the measurement proved it: this workload
 * performs only a few hundred depot magazine loads in 25 seconds (the PTC bins
 * absorb nearly everything), and a cache resizes exactly once, so even a 200us
 * delay per load leaves ~47ms of open window against a single instant somewhere
 * in 25 seconds -- 0 hits in 89 million samples.
 *
 * So the probe BLOCKS instead of sleeping: a thread that has just obtained a
 * magazine parks here and publishes that it is inside the window.  The test
 * waits until enough threads are parked, forces the magtype change, and then
 * releases them -- at which point they proceed to record a capacity, with the
 * magtype provably having changed while they held the magazine.  No timing luck.
 *
 * Blocking here is safe: umem_depot_alloc_trylock() releases ml_lock before
 * returning, so a thread parked at this point holds no allocator lock, and the
 * update thread's resize (which takes cc_lock and ml_lock) cannot be blocked by
 * it.  The wait is bounded so a mis-driven test cannot hang the process.
 */
volatile long umem_ptc_resize_probe_ns = 0;
volatile int umem_ptc_probe_gate = 0;		/* 1 = park arrivals here */
volatile long umem_ptc_probe_inwindow = 0;	/* threads parked right now */
volatile long umem_ptc_probe_refills = 0;	/* magazines loaded */
volatile long umem_ptc_probe_desyncs = 0;	/* capacity != magazine's own */
/*
 * P1.3c ledger.  Counted at the ONE instant loss can occur: a magazine shell
 * being freed back to its magtype cache.  Anything still sitting in its rounds
 * at that moment has just lost its only reference, while the slab layer goes on
 * counting it as allocated.
 *
 * This is an observation of behaviour, not of structure, so it is meaningful in
 * both builds and exactly comparable between them: pre-fix the count is the
 * number of objects destroyed, post-fix it is zero because the magazine was
 * drained first.  Exact and deterministic -- no statistics, unlike counting
 * outstanding buffers, where ordinary retention swamps the signal.
 */
volatile long umem_ptc_probe_shell_frees = 0;
volatile long umem_ptc_probe_objects_lost = 0;
/*
 * P8.6 ledger: umem_depot_alloc_trylock() calls made from the PTC alloc/free
 * paths.  Each is 1 + min(ncpus, UMEM_DEPOT_STEAL_MAX) + 1 trylock/unlock
 * pairs when the lists are empty.  With a working L2 this is O(N / magsize)
 * for N objects crossing the bin boundary; unprimed it was O(N).
 */
volatile long umem_ptc_probe_depot_trylocks = 0;
#define	UMEM_PTC_PROBE_DEPOT_TRYLOCK()	\
	(void) atomic_add_64((uint64_t *)&umem_ptc_probe_depot_trylocks, 1)

static void
umem_ptc_resize_probe(void)
{
	long ns = umem_ptc_resize_probe_ns;
	struct timespec ts;

	if (umem_ptc_probe_gate) {
		int spins = 0;

		(void) atomic_add_64((uint64_t *)&umem_ptc_probe_inwindow, 1);
		/* Bounded: 30s at 1ms, so a mis-driven test cannot hang. */
		while (umem_ptc_probe_gate && spins++ < 30000)
			(void) usleep(1000);
		(void) atomic_add_64((uint64_t *)&umem_ptc_probe_inwindow,
		    -1ULL);
		return;
	}

	if (ns <= 0)
		return;
	ts.tv_sec = ns / 1000000000L;
	ts.tv_nsec = ns % 1000000000L;
	(void) nanosleep(&ts, NULL);
}

/*
 * Record whether the capacity just written for `mp` is in fact that magazine's
 * own capacity.  Observation only: changes no decision.
 *
 * This has to be done HERE rather than by the test sampling its own PTC later:
 * a desynchronized magazine is typically replaced before the next sample, so an
 * after-the-fact check looks at state that has already moved on (measured: 0
 * observed desyncs in 86M such samples on a build with the defect present).
 */
static void
umem_ptc_probe_observe(umem_magazine_t *mp, int recorded)
{
	(void) atomic_add_64((uint64_t *)&umem_ptc_probe_refills, 1);
	if (recorded != umem_mag_capacity(mp))
		(void) atomic_add_64((uint64_t *)&umem_ptc_probe_desyncs, 1);
}

/*
 * Count the objects still in `mp` as its shell is freed.  See the ledger
 * comment above.  Observation only: frees nothing, changes no decision.
 *
 * Two things about WHERE and WHAT this counts, both learned the hard way:
 *
 * WHERE: it is called AFTER umem_mag_drain() has run, and is handed the
 * count the drain was given.  What it measures is "objects the drain did
 * not take out", i.e. owned slots still non-NULL when the shell goes.  Put
 * before the drain it would count every object in every stale magazine,
 * fixed or not.
 *
 * WHAT: only slots below `rounds`.  The alloc paths pop mag_round[--rounds]
 * without clearing the slot, so a magazine that was once full carries a
 * tail of stale pointers to objects that are live elsewhere.  This ledger
 * used to scan all `cap` slots and count that tail as lost -- 127 or 254
 * at a time, exactly the size of the real defect it was written to catch.
 * It went unseen because the pre-fix magazines it was validated against
 * were full (rounds == cap, no tail), and because until the update thread
 * was started at init, magazine resizes -- the only thing that makes a PTC
 * shell stale -- ran only under umem_reap().  Once resizes ran routinely,
 * the false positive fired in ~1 run in 8 and was about to be filed as a
 * P1.3c regression.
 */
static void
umem_ptc_probe_shell_free(umem_magazine_t *mp, int rounds)
{
	int cap = umem_mag_capacity(mp);
	long live = 0;
	int r;

	if (rounds > cap)
		rounds = cap;
	for (r = 0; r < rounds; r++) {
		if (mp->mag_round[r] != NULL)
			live++;
	}
	(void) atomic_add_64((uint64_t *)&umem_ptc_probe_shell_frees, 1);
	if (live != 0)
		(void) atomic_add_64((uint64_t *)&umem_ptc_probe_objects_lost,
		    (uint64_t)live);
}
#define	UMEM_PTC_RESIZE_PROBE_POINT()	umem_ptc_resize_probe()
#define	UMEM_PTC_PROBE_OBSERVE(mp, cap)	umem_ptc_probe_observe((mp), (cap))
#define	UMEM_PTC_PROBE_SHELL_FREE(mp, n)	umem_ptc_probe_shell_free((mp), (n))
#else
#define	UMEM_PTC_RESIZE_PROBE_POINT()	((void)0)
#define	UMEM_PTC_PROBE_OBSERVE(mp, cap)	((void)0)
#define	UMEM_PTC_PROBE_SHELL_FREE(mp, n)	((void)0)
#define	UMEM_PTC_PROBE_DEPOT_TRYLOCK()	((void)0)
#endif

/*
 * Debug check for the invariant P1.3b broke: every magazine a PTC thread
 * holds is described by the capacity of THAT magazine, and its round count
 * is within that capacity.
 *
 * Placed at the refill/flush points where the state changes -- once per depot
 * transfer, not once per object -- so it costs nothing measurable, and it
 * fires exactly when a resize has desynchronized a magazine from the size it
 * is about to be indexed with.
 */
static inline void
umem_ptc_mag_check(umem_ptc_mag_t *mag)
{
	ASSERT(mag->loaded == NULL ||
	    mag->magsize == umem_mag_capacity(mag->loaded));
	ASSERT(mag->previous == NULL ||
	    mag->pmagsize == umem_mag_capacity(mag->previous));
	ASSERT(mag->rounds >= 0 && mag->rounds <= mag->magsize);
	ASSERT(mag->prounds >= 0 && mag->prounds <= mag->pmagsize);
}

/*
 * Non-blocking depot pop for PTC refill path.
 * Returns NULL immediately if the lock is contended or list is empty.
 * Never blocks on a mutex, eliminating p99 latency spikes.
 */
static umem_magazine_t *
umem_depot_pop_trylock(umem_maglist_t *mlp)
{
	umem_magazine_t *mp;

	UMEM_PREFETCH_READ(&mlp->ml_list);

	/* See umem_depot_pop: an unlocked empty check costs no one anything. */
	if (*(umem_magazine_t *volatile *)&mlp->ml_list == NULL) {
		UMEM_PTC_PROBE_DEPOT_POP(1);
		return (NULL);
	}

	if (mutex_trylock(&mlp->ml_lock) != 0)
		return (NULL);

	mp = mlp->ml_list;
	UMEM_PTC_PROBE_DEPOT_POP(mp == NULL);
	if (mp == NULL) {
		UMEM_PTC_PROBE_DEPOT_POP_LOCKED_EMPTY();
		(void) mutex_unlock(&mlp->ml_lock);
		return (NULL);
	}

	mlp->ml_list = mp->mag_next;
	mlp->ml_total--;
	if (mlp->ml_total < mlp->ml_min)
		mlp->ml_min = mlp->ml_total;
	mlp->ml_alloc++;

	(void) mutex_unlock(&mlp->ml_lock);

	UMEM_PREFETCH_READ(&mp->mag_round[0]);

	return (mp);
}

/*
 * Non-blocking depot push for PTC flush path.
 * Returns 0 on success, -1 if lock contended.
 */
static int
umem_depot_push_trylock(umem_maglist_t *mlp, umem_magazine_t *mp)
{
	if (mutex_trylock(&mlp->ml_lock) != 0)
		return (-1);

	mp->mag_next = mlp->ml_list;
	mlp->ml_list = mp;
	mlp->ml_total++;

	(void) mutex_unlock(&mlp->ml_lock);
	return (0);
}

/*
 * Non-blocking depot alloc for PTC refill path.
 * Tries per-CPU depots then global depot, all with trylock.
 * Returns NULL rather than blocking on any contended lock.
 */
static umem_magazine_t *
umem_depot_alloc_trylock(umem_cache_t *cp, umem_maglist_t *mlp)
{
	umem_magazine_t *mp;
	umem_maglist_t *pcpu_arr;
	int ncpus = cp->cache_depot_ncpus;
	int is_full = (mlp == &cp->cache_full);

	UMEM_PTC_PROBE_DEPOT_TRYLOCK();

	if (ncpus > 0) {
		int cpu = get_cached_cpu_hint() & (ncpus - 1);

		pcpu_arr = is_full ?
		    cp->cache_depot_full : cp->cache_depot_empty;

		/* Try local CPU depot (trylock) */
		mp = umem_depot_pop_trylock(&pcpu_arr[cpu]);
		if (mp != NULL) {
			if (unlikely(!UMEM_MAGAZINE_VALID(cp, mp))) {
				umem_depot_destroy_stale(
				    cp, is_full, mp);
			} else {
				cp->cache_depot_local++;
				return (mp);
			}
		}

		/*
		 * Scan a bounded set of nearby CPU stripes with trylock only.
		 * Scanning all ncpus (up to umem_max_ncpus, e.g. 512) empty
		 * stripes on every miss is O(ncpus) trylock/unlock churn that
		 * dominates single-thread hold-heavy workloads whose per-CPU
		 * depot is legitimately empty (perf showed 82% of frag CPU in
		 * pthread_mutex_trylock scanning empties). This is only the
		 * non-blocking PTC-refill fast path: on a miss the caller falls
		 * through to _umem_cache_alloc -> umem_depot_alloc (blocking),
		 * which still does the full NUMA-aware cross-CPU steal, so the
		 * cross-thread-handoff (prodcons) case is unaffected. A freed
		 * magazine lands on the freeing CPU's stripe, so a nearby scan
		 * captures the common locality-steal here.
		 * ponytail: fixed bound; widen if a trylock-only workload needs it.
		 */
		int scan = ncpus < UMEM_DEPOT_STEAL_MAX ?
		    ncpus : UMEM_DEPOT_STEAL_MAX;
		for (int i = 1; i < scan; i++) {
			int other = (cpu + i) & (ncpus - 1);
			mp = umem_depot_pop_trylock(&pcpu_arr[other]);
			if (mp != NULL) {
				if (unlikely(
				    !UMEM_MAGAZINE_VALID(cp, mp))) {
					umem_depot_destroy_stale(
					    cp, is_full, mp);
					continue;
				}
				cp->cache_depot_remote++;
				return (mp);
			}
		}
	}

	/* Try global depot (trylock) */
	mp = umem_depot_pop_trylock(mlp);
	if (mp != NULL) {
		if (unlikely(!UMEM_MAGAZINE_VALID(cp, mp))) {
			umem_depot_destroy_stale(cp, is_full, mp);
			return (NULL);
		}
	}
	return (mp);
}

/*
 * Non-blocking depot free for PTC flush path.
 * Falls back to blocking push only if trylock fails on all targets.
 */
static void
umem_depot_free_trylock(umem_cache_t *cp, umem_maglist_t *mlp,
    umem_magazine_t *mp)
{
	int ncpus = cp->cache_depot_ncpus;

	if (ncpus > 0) {
		int cpu = get_cached_cpu_hint() & (ncpus - 1);
		umem_maglist_t *pcpu_arr;

		pcpu_arr = (mlp == &cp->cache_full) ?
		    cp->cache_depot_full : cp->cache_depot_empty;

		if (umem_depot_push_trylock(&pcpu_arr[cpu], mp) == 0)
			return;

		/* Local contended, try other CPUs */
		for (int i = 1; i < ncpus; i++) {
			int other = (cpu + i) & (ncpus - 1);
			if (umem_depot_push_trylock(
			    &pcpu_arr[other], mp) == 0)
				return;
		}
	}

	/* All trylock failed — fall back to blocking push */
	umem_depot_push(mlp, mp);
}

/*
 * Allocate a magazine from the depot.
 * Tries per-CPU depot first (our CPU, then steal from same NUMA node,
 * then steal from any CPU), then falls back to the global depot list.
 *
 * NUMA-aware stealing order:
 *   1. Local CPU depot
 *   2. Other CPUs on the same NUMA node
 *   3. CPUs on remote NUMA nodes
 *   4. Global depot
 *
 * This ordering keeps memory locality high on NUMA systems: objects freed
 * on a CPU are physically close to that CPU's node, so stealing from a
 * same-node CPU avoids cross-node memory traffic.
 *
 * Lock ordering: caller must NOT hold cp->cache_lock.
 * The depot locks (ml_lock) are below cache_lock in the hierarchy.
 * Holding cache_lock while acquiring ml_lock would invert the order
 * used by umem_lockup_cache() in the fork handler.
 */

/*
 * Destroy a magazine that belongs to a stale magtype.
 * After a magazine resize, PTC or rseq threads may return magazines
 * allocated from the old magtype. When popped, these fail
 * UMEM_MAGAZINE_VALID. Drain any objects and free the magazine
 * shell to its actual slab cache.
 *
 * The is_full flag indicates whether the magazine was from a full
 * depot (contains objects to drain) or empty depot (no objects).
 */
static void
umem_depot_destroy_stale(umem_cache_t *cp, int is_full,
    umem_magazine_t *mp)
{
	umem_cache_t *mag_cache;

	mag_cache = ((umem_slab_t *)P2END(
	    (uintptr_t)(mp), PAGESIZE) - 1)->slab_cache;

	if (is_full) {
		int magsize = (mag_cache->cache_bufsize /
		    sizeof (void *)) - 1;
		for (int r = 0; r < magsize; r++) {
			void *buf = mp->mag_round[r];
			mp->mag_round[r] = NULL;
			if (buf != NULL)
				umem_slab_free(cp, buf);
		}
	}

	atomic_add_64(&cp->cache_mag_total, -1ULL);
	_umem_cache_free(mag_cache, mp);
}

static umem_magazine_t *
umem_depot_alloc(umem_cache_t *cp, umem_maglist_t *mlp)
{
	umem_magazine_t *mp;

/*
 * Bounds-safe CPU->node lookup.  umem_cpu_node is sized to umem_max_ncpus
 * (== cache_depot_ncpus) at init; this guard protects the rare fallback
 * where dynamic allocation failed and the table stayed at the static size.
 */
#define	UMEM_CPU_NODE(idx) \
	(((uint32_t)(idx) < umem_cpu_node_ncpus) ? umem_cpu_node[(idx)] : 0)
	umem_maglist_t *pcpu_arr;
	int ncpus = cp->cache_depot_ncpus;
	int is_full = (mlp == &cp->cache_full);

	if (ncpus > 0) {
		int cpu = get_cached_cpu_hint() & (ncpus - 1);
		int local_node = UMEM_CPU_NODE(cpu);

		pcpu_arr = is_full ?
		    cp->cache_depot_full : cp->cache_depot_empty;

		/* 1. Try local CPU depot */
		mp = umem_depot_pop(cp, &pcpu_arr[cpu]);
		if (mp != NULL) {
			if (unlikely(!UMEM_MAGAZINE_VALID(cp, mp))) {
				umem_depot_destroy_stale(
				    cp, is_full, mp);
			} else {
				cp->cache_depot_local++;
				return (mp);
			}
		}

		/*
		 * 2/3. Steal from other CPU stripes (same NUMA node first,
		 * then remote nodes). This scan must cover the FULL stripe
		 * range (not a shallow subset): umem_depot_free() pushes a
		 * freed magazine only onto the *freeing* thread's own local
		 * stripe, so under cross-thread free (prodcons: producers
		 * alloc, consumers free, disjoint CPU sets) a producer's own
		 * stripe is essentially always empty and the magazine it needs
		 * can legitimately be on any of the other ~255 stripes a
		 * consumer thread landed on. An earlier attempt at this fix
		 * capped the scan to UMEM_DEPOT_STEAL_MAX (8) stripes to bound
		 * worst-case lock-hold time; measured on intel-hi it instead
		 * made things worse (p999 157us -> 348us, RSS 180MB -> 4.2GB)
		 * because producers stopped finding consumer-filled magazines
		 * within 8 hops and fell through to allocating brand-new
		 * magazines from the slab layer instead -- breadth is load-
		 * bearing for this workload, not a tunable to shrink.
		 *
		 * The actual problem isn't scan breadth, it's that each visited
		 * stripe used umem_depot_pop() -- trylock, and on failure a
		 * BLOCKING mutex_lock() -- while this whole function runs
		 * under the caller's per-CPU cc_lock (see _umem_cache_alloc/
		 * _umem_cache_free and the *_batch variants). Sustained-load
		 * evidence (docs/results/2026-09-08-allocator-shootout.md sec
		 * 7, docs/results/2026-09-09-sustained-depot-contention-
		 * diagnosis.md): dep_conten (blocking-lock-fallback count) ran
		 * into the hundreds of thousands per cache over a 180s 192-
		 * thread prodcons run -- every one of those is a thread
		 * blocking on a futex while holding cc_lock, and under 192-way
		 * concurrency enough of those overlap to form a lock convoy
		 * that only builds up over sustained duration (a short burst
		 * doesn't have enough concurrent misses in flight to show it).
		 *
		 * Fix: scan with umem_depot_pop_trylock() (same non-blocking
		 * primitive the PTC-refill fast path already uses) instead of
		 * the blocking umem_depot_pop(). A trylock miss on a momentarily-
		 * busy stripe just moves on to the next candidate in the same
		 * pass -- full breadth is preserved, no thread ever blocks on
		 * another CPU's depot lock while holding its own cc_lock. A
		 * magazine transiently missed this way is still valid and gets
		 * picked up on a later call; this is the same accepted trade-off
		 * the trylock-only PTC path already relies on.
		 */

		/* 2. Steal from same NUMA node first (non-blocking). */
		for (int i = 1; i < ncpus; i++) {
			int other = (cpu + i) & (ncpus - 1);
			if (UMEM_CPU_NODE(other) != local_node)
				continue;
			mp = umem_depot_pop_trylock(&pcpu_arr[other]);
			if (mp != NULL) {
				if (unlikely(
				    !UMEM_MAGAZINE_VALID(cp, mp))) {
					umem_depot_destroy_stale(
					    cp, is_full, mp);
					continue;
				}
				cp->cache_depot_remote++;
				return (mp);
			}
		}

		/* 3. Steal from remote NUMA nodes (non-blocking). */
		for (int i = 1; i < ncpus; i++) {
			int other = (cpu + i) & (ncpus - 1);
			if (UMEM_CPU_NODE(other) == local_node)
				continue;
			mp = umem_depot_pop_trylock(&pcpu_arr[other]);
			if (mp != NULL) {
				if (unlikely(
				    !UMEM_MAGAZINE_VALID(cp, mp))) {
					umem_depot_destroy_stale(
					    cp, is_full, mp);
					continue;
				}
				cp->cache_depot_cross_node++;
				return (mp);
			}
		}
	}

	/* 4. Fall back to global depot */
	mp = umem_depot_pop(cp, mlp);
	if (mp != NULL) {
		if (unlikely(!UMEM_MAGAZINE_VALID(cp, mp))) {
			umem_depot_destroy_stale(cp, is_full, mp);
			return (NULL);
		}
	}
	return (mp);
}
#undef UMEM_CPU_NODE

/*
 * Free a magazine to the depot.
 * Pushes to per-CPU depot if available, otherwise to global list.
 *
 * Lock ordering: caller must NOT hold cp->cache_lock.
 * See umem_depot_alloc() for rationale.
 */
static void
umem_depot_free(umem_cache_t *cp, umem_maglist_t *mlp,
    umem_magazine_t *mp)
{
	int ncpus = cp->cache_depot_ncpus;

	if (ncpus > 0) {
		int cpu = get_cached_cpu_hint() & (ncpus - 1);
		umem_maglist_t *pcpu_arr;

		pcpu_arr = (mlp == &cp->cache_full) ?
		    cp->cache_depot_full : cp->cache_depot_empty;

		umem_depot_push(&pcpu_arr[cpu], mp);
		return;
	}

	umem_depot_push(mlp, mp);
}

/*
 * Drain `rounds` objects out of a magazine back to the cache.
 * The magazine is left with every drained slot NULL.
 *
 * `cap` is the magazine's own capacity (see umem_mag_capacity): rounds is
 * clamped to it so a stale count can never walk off the end of the shell.
 */
static void
umem_mag_drain(umem_cache_t *cp, umem_magazine_t *mp, int rounds, int cap)
{
	int r;

	if (rounds > cap)
		rounds = cap;

	for (r = 0; r < rounds; r++) {
		void *buf = mp->mag_round[r];
		mp->mag_round[r] = NULL;
		if (buf != NULL)
			_umem_cache_free(cp, buf);
	}
}

/*
 * Return a PTC magazine holding `rounds` objects to the depot.
 *
 * Two invariants are enforced here, at the one shared function every PTC
 * magazine hand-off goes through, rather than at each call site:
 *
 *   1. NOTHING IS DISCARDED WITH OBJECTS STILL IN IT (P1.3c).  If the
 *      magazine's magtype no longer matches the cache -- a
 *      umem_cache_magazine_resize() happened while this thread held it --
 *      the shell cannot go back on a depot list, but the objects inside are
 *      live and the slab layer still counts them as allocated.  This used to
 *      free only the shell, so every object in a full magazine (127 or 255
 *      of them) lost its only reference.  Drain first, then free the shell.
 *
 *   2. THE FULL/EMPTY CLASSIFICATION IS THE CALLER'S COUNT, NOT A GUESS.
 *      The depot's two lists have exact contracts: a magazine on ml_full
 *      holds capacity rounds (umem_depot_ws_reap destroys those with
 *      full_rounds == magsize), one on ml_empty holds none (destroyed with
 *      0).  A partially filled magazine satisfies neither, and pushing one
 *      onto the empty list -- which the resize paths did with a populated
 *      `previous` magazine -- loses its contents just as surely as freeing
 *      the shell.  So a magazine that is not exactly full is drained and
 *      goes on the empty list.
 */
static void
umem_ptc_mag_return(umem_cache_t *cp, umem_maglist_t *mlp,
    umem_magazine_t *mp, int rounds)
{
	int cap = umem_mag_capacity(mp);

	if (!UMEM_MAGAZINE_VALID(cp, mp)) {
		umem_mag_drain(cp, mp, rounds, cap);
		atomic_add_64(&cp->cache_mag_total, -1ULL);
		UMEM_PTC_PROBE_SHELL_FREE(mp, rounds);
		_umem_cache_free(umem_mag_source_cache(mp), mp);
		return;
	}

	if (mlp != &cp->cache_full || rounds != cap) {
		umem_mag_drain(cp, mp, rounds, cap);
		mlp = &cp->cache_empty;
	}

	umem_depot_free(cp, mlp, mp);
}

/*
 * Non-blocking variant of umem_ptc_mag_return for PTC fast paths.
 * Uses trylock depot access. Falls back to blocking only for stale
 * magazine destruction (rare, only during magazine resize).
 * Same two invariants; see umem_ptc_mag_return.
 */
static void
umem_ptc_mag_return_trylock(umem_cache_t *cp, umem_maglist_t *mlp,
    umem_magazine_t *mp, int rounds)
{
	int cap = umem_mag_capacity(mp);

	if (!UMEM_MAGAZINE_VALID(cp, mp)) {
		umem_mag_drain(cp, mp, rounds, cap);
		atomic_add_64(&cp->cache_mag_total, -1ULL);
		UMEM_PTC_PROBE_SHELL_FREE(mp, rounds);
		_umem_cache_free(umem_mag_source_cache(mp), mp);
		return;
	}

	if (mlp != &cp->cache_full || rounds != cap) {
		umem_mag_drain(cp, mp, rounds, cap);
		mlp = &cp->cache_empty;
	}

	umem_depot_free_trylock(cp, mlp, mp);
}

/*
 * Allocate one empty magazine for a PTC L2 that has none (P8.6).
 *
 * The depot's empty list is fed only when a CPU-layer magazine drains, which
 * a steady alloc/free loop never does, so on a cache one thread cycles it
 * stayed empty forever: the per-thread magazines were never primed, and
 * every object past the bin paid the failed-trylock stripe scan and then
 * cc_lock.  This is what _umem_cache_free does for the CPU layer ("no empty
 * magazines in the depot, so try to allocate a new one"), for the PTC.
 * Called once per thread per size class: after it the loaded/previous pair
 * cycles and the depot is touched once per magsize objects.
 *
 * No magtype-changed retry, unlike the CPU layer: the caller reads capacity
 * from the magazine itself (P1.3b), and a shell whose magtype the cache has
 * since moved past is drained and freed by umem_ptc_mag_return* when it is
 * handed back (P1.3c).  No lock is held here.
 *
 * Out of line: this runs once per thread per class and has no business in
 * _umem_free's text.  (An earlier version of this comment attributed a 9 %
 * t=1 loss to inlining it.  That number was the A/B rig's: it built the
 * bench binary per arm and a printf change in bench_pairs.c had moved the
 * bench's own loop; with one bench binary the library is 3-8 % FASTER at
 * that point.  scripts/ec2/hotpath_ab.sh records the correction.)
 */
static __attribute__((noinline)) umem_magazine_t *
umem_ptc_mag_prime(umem_cache_t *cp)
{
	umem_magazine_t *emp;

	emp = _umem_cache_alloc(cp->cache_magtype->mt_cache, UMEM_DEFAULT);
	if (emp != NULL) {
		atomic_add_64(&cp->cache_mag_total, 1);
		umem_mag_init_fast(emp->mag_round, umem_mag_capacity(emp));
	}
	return (emp);
}

/*
 * Flush all per-thread magazines back to depot.
 * Called from umem_ptc_destroy() at thread exit.
 * umem_ptc_mag_return() drains whatever the magazine still holds before it
 * goes anywhere, so both magazines are simply handed over with their counts.
 */
void
umem_ptc_mag_flush_all(umem_ptc_t *ptc)
{
	int bin_idx;

	for (bin_idx = 0; bin_idx < PTC_NBINS; bin_idx++) {
		umem_ptc_mag_t *mag = &ptc->mags[bin_idx];
		umem_cache_t *cp = (umem_cache_t *)mag->cache;

		if (cp == NULL)
			continue;

		if (mag->loaded != NULL) {
			umem_ptc_mag_return(cp, &cp->cache_empty,
			    mag->loaded, mag->rounds);
			mag->loaded = NULL;
			mag->rounds = 0;
		}

		if (mag->previous != NULL) {
			umem_ptc_mag_return(cp, &cp->cache_empty,
			    mag->previous, mag->prounds);
			mag->previous = NULL;
			mag->prounds = 0;
		}
	}
}

#ifdef UMEM_RSEQ_AVAILABLE
/*
 * UNUSED: see docs/results/2026-09-09-rseq-reload-analysis-v2.md (and the
 * original docs/results/2026-08-06-rseq-reload-analysis.md) for the full,
 * hardware-verified analysis of why arming this needs new asm, not a C
 * wrapper. A plain-C reload here races the lock-free rseq asm fastpath
 * across a CPU migration; a lock does not help because the fastpath never
 * takes one, and rechecking cpu_id in C does not close the window either
 * (measured ~42-47% double-issue rate under contention -- see
 * test/stress/repro_naive_reload_race.c). The precise assembly design for
 * arming this correctly is specified in
 * docs/results/2026-09-09-rseq-reload-asm-design.md. Kept for reference /
 * as a starting point; not called anywhere.
 */
static void *
umem_rseq_alloc_slowpath(umem_cache_t *cp, int cpu_id)
{
	umem_rseq_cache_t *rc;
	umem_magazine_t *fmp;
	umem_magazine_t *old_mag;
	void *buf;

	rc = &cp->cache_rseq[cpu_id];

	fmp = umem_depot_alloc(cp, &cp->cache_full);
	if (fmp == NULL)
		return (NULL);

	old_mag = (umem_magazine_t *)rc->loaded_mag;
	if (old_mag != NULL)
		umem_depot_free(cp, &cp->cache_empty, old_mag);

	rc->loaded_mag = fmp;
	rc->rounds = rc->magsize;
	rc->rounds--;
	buf = fmp->mag_round[rc->rounds];
	rc->alloc_count++;

	return (buf);
}

/*
 * UNUSED: see docs/results/2026-09-09-rseq-reload-analysis-v2.md for the
 * full analysis. Same migration-race hazard as
 * umem_rseq_alloc_slowpath() above; not called anywhere.
 */
static int
umem_rseq_free_slowpath(umem_cache_t *cp, int cpu_id, void *buf)
{
	umem_rseq_cache_t *rc;
	umem_magazine_t *emp;
	umem_magazine_t *old_mag;

	rc = &cp->cache_rseq[cpu_id];

	emp = umem_depot_alloc(cp, &cp->cache_empty);
	if (emp == NULL)
		return (-1);

	old_mag = (umem_magazine_t *)rc->loaded_mag;
	if (old_mag != NULL)
		umem_depot_free(cp, &cp->cache_full, old_mag);

	rc->loaded_mag = emp;
	rc->rounds = 0;
	emp->mag_round[0] = buf;
	rc->rounds = 1;
	rc->free_count++;
	return (0);
}
#endif /* UMEM_RSEQ_AVAILABLE */

/*
 * Update working set statistics for a single maglist.
 */
static void
umem_maglist_ws_update(umem_maglist_t *mlp)
{
	(void) mutex_lock(&mlp->ml_lock);
	mlp->ml_reaplimit = mlp->ml_min;
	mlp->ml_min = mlp->ml_total;
	(void) mutex_unlock(&mlp->ml_lock);
}

/*
 * Does any of cp's depot lists hold magazines beyond its working set?
 * (i.e. would a ws_reap right now reap anything.)  Read without ml_lock:
 * a stale answer costs one interval either way and the reap itself
 * re-derives the count under the lock.
 */
static int
umem_depot_ws_excess(umem_cache_t *cp)
{
	int i;

	if (MIN(cp->cache_full.ml_reaplimit, cp->cache_full.ml_min) > 0 ||
	    MIN(cp->cache_empty.ml_reaplimit, cp->cache_empty.ml_min) > 0)
		return (1);
	for (i = 0; i < cp->cache_depot_ncpus; i++) {
		if (MIN(cp->cache_depot_full[i].ml_reaplimit,
		    cp->cache_depot_full[i].ml_min) > 0 ||
		    MIN(cp->cache_depot_empty[i].ml_reaplimit,
		    cp->cache_depot_empty[i].ml_min) > 0)
			return (1);
	}
	return (0);
}

/*
 * Update the working set statistics for cp's depot.
 */
static void
umem_depot_ws_update(umem_cache_t *cp)
{
	int i;

	umem_maglist_ws_update(&cp->cache_full);
	umem_maglist_ws_update(&cp->cache_empty);

	for (i = 0; i < cp->cache_depot_ncpus; i++) {
		umem_maglist_ws_update(&cp->cache_depot_full[i]);
		umem_maglist_ws_update(&cp->cache_depot_empty[i]);
	}
}

/*
 * Pop a magazine from a list for reaping.
 * Caller must hold mlp->ml_lock.
 */
static umem_magazine_t *
umem_depot_reap_pop(umem_maglist_t *mlp)
{
	umem_magazine_t *mp;

	mp = mlp->ml_list;
	if (mp == NULL)
		return (NULL);
	mlp->ml_list = mp->mag_next;
	mlp->ml_total--;
	mlp->ml_alloc++;
	return (mp);
}

/*
 * Reap magazines from a single maglist that have fallen out of
 * the working set.  full_rounds is the number of rounds to pass
 * to umem_magazine_destroy (magsize for full mags, 0 for empty).
 */
static void
umem_maglist_ws_reap(umem_cache_t *cp, umem_maglist_t *mlp,
    int full_rounds)
{
	long reap;
	umem_magazine_t *mp;
	umem_magazine_t *destroy_list = NULL;

	/*
	 * Pop the working-set-excess magazines under ml_lock into a private
	 * chain, then release the lock BEFORE destroying them.
	 *
	 * umem_magazine_destroy() frees the magazine's rounds and then frees
	 * the magazine struct itself back to cp via _umem_cache_free(), which
	 * re-enters the depot (umem_depot_alloc -> umem_depot_pop) and can try
	 * to lock a depot maglist -- including THIS mlp, since the striped
	 * depot_alloc scan may land on the same stripe.  Holding ml_lock
	 * across the destroy therefore self-deadlocks on the non-recursive
	 * mutex (observed as all threads wedged in umem_depot_pop under high
	 * concurrency).  Collect under the lock, destroy unlocked -- the same
	 * pattern umem_cache_reclaim_pages() uses for its slab surgery.
	 */
	(void) mutex_lock(&mlp->ml_lock);
	reap = MIN(mlp->ml_reaplimit, mlp->ml_min);
	while (reap-- > 0) {
		mp = umem_depot_reap_pop(mlp);
		if (mp == NULL)
			break;
		mp->mag_next = destroy_list;
		destroy_list = mp;
	}
	(void) mutex_unlock(&mlp->ml_lock);

	while (destroy_list != NULL) {
		mp = destroy_list;
		destroy_list = mp->mag_next;
		if (unlikely(!UMEM_MAGAZINE_VALID(cp, mp))) {
			umem_depot_destroy_stale(cp, (full_rounds > 0), mp);
			continue;
		}
		umem_magazine_destroy(cp, mp, full_rounds);
	}
}

/*
 * Maximum magazines per per-CPU depot slot.  After normal working-set
 * reaping, any per-CPU list still above this threshold gets its
 * ml_min lowered so the NEXT reap cycle will drain the excess
 * through the normal two-update-cycle working-set path.
 */
#define	UMEM_DEPOT_PERCPU_MAX	8

/*
 * After reaping, mark per-CPU depot lists that still hold too many
 * magazines so that the next working-set update cycle will make them
 * reapable.  This avoids destroying magazines that are still in the
 * active working set while bounding long-term accumulation.
 */
static void
umem_maglist_mark_excess(umem_maglist_t *mlp)
{
	(void) mutex_lock(&mlp->ml_lock);
	if (mlp->ml_total > UMEM_DEPOT_PERCPU_MAX) {
		long excess = mlp->ml_total - UMEM_DEPOT_PERCPU_MAX;
		if (mlp->ml_reaplimit < excess)
			mlp->ml_reaplimit = excess;
		/*
		 * Do NOT lower ml_min here.  The next reap takes
		 * MIN(ml_reaplimit, ml_min); with ml_min clamped to
		 * UMEM_DEPOT_PERCPU_MAX (8) that reaped at most 8 magazines
		 * per list per pass -- 8 lists x 8 x 31 rounds x 4 KiB = 8 MB
		 * per 10 s interval against a 2 GB surplus (measured: 5 MB in
		 * 100 s; ~40 minutes to drain).  The clamp inverted the intent
		 * of the comment above: it was written to make the excess
		 * reapable and instead bounded how much of it could ever be
		 * reaped.  ml_min is the working-set trough the depot_pop
		 * paths maintain; leave it, and let the excess drain at the
		 * rate ws_update already computed.
		 */
	}
	(void) mutex_unlock(&mlp->ml_lock);
}

/*
 * Reap all magazines that have fallen out of the depot's working set.
 */
static void
umem_depot_ws_reap(umem_cache_t *cp)
{
	int magsize = cp->cache_magtype->mt_magsize;
	int i;

	ASSERT(cp->cache_next == NULL || IN_REAP());

	umem_maglist_ws_reap(cp, &cp->cache_full, magsize);
	umem_maglist_ws_reap(cp, &cp->cache_empty, 0);

	for (i = 0; i < cp->cache_depot_ncpus; i++) {
		umem_maglist_ws_reap(cp, &cp->cache_depot_full[i], magsize);
		umem_maglist_ws_reap(cp, &cp->cache_depot_empty[i], 0);

		/*
		 * Mark excess per-CPU depot magazines for reaping
		 * on the next update cycle.
		 */
		umem_maglist_mark_excess(&cp->cache_depot_full[i]);
		umem_maglist_mark_excess(&cp->cache_depot_empty[i]);
	}
}


/*
 * Assembly fastpath prototypes for rseq critical sections.
 * These are implemented in umem_rseq_x86_64.S (or equivalent).
 */
#ifdef UMEM_RSEQ_AVAILABLE
#if defined(__x86_64__) || defined(__aarch64__)
extern void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache,
    int cpu_id);
extern int umem_rseq_free_fastpath(umem_rseq_cache_t *cache,
    void *buf, int cpu_id);
#endif
#endif

static inline void __attribute__((always_inline))
umem_cpu_reload(umem_cpu_cache_t *ccp, umem_magazine_t *mp, int rounds)
{
	int current_rounds;

	/*
	 * This function is always called under cc_lock.
	 */
	current_rounds = ccp->cc_rounds;

	ASSERT((ccp->cc_loaded == NULL && current_rounds == -1) ||
	    (ccp->cc_loaded && current_rounds + rounds == ccp->cc_magsize));
	ASSERT(ccp->cc_magsize > 0);

	/*
	 * Prefetch cc_ploaded before swap. This magazine will be accessed
	 * soon for magazine exchange operations. High locality (3) since
	 * we frequently swap between loaded and ploaded magazines.
	 */
	if (ccp->cc_ploaded != NULL) {
		__builtin_prefetch(ccp->cc_ploaded, 0, 3);
	}

	ccp->cc_ploaded = ccp->cc_loaded;
	ccp->cc_prounds = current_rounds;
	ccp->cc_loaded = mp;
	ccp->cc_rounds = rounds;

	/*
	 * Reset CPU hint cache on magazine reload to detect thread migration.
	 * This ensures the cached hint stays fresh across potential CPU changes.
	 */
	reset_cpu_hint_cache();
}

/*
 * Allocate a constructed object from cache cp.
 */
#ifndef NO_WEAK_SYMBOLS
#pragma weak umem_cache_alloc = _umem_cache_alloc
#endif
void *
_umem_cache_alloc(umem_cache_t *cp, int umflag)
{
	umem_cpu_cache_t *ccp;
	umem_magazine_t *fmp;
	void *buf;
	int flags_nfatal;
	int rounds;

retry:
	ccp = UMEM_CPU_CACHE(cp, CPU_CACHED(cp->cache_cpu_mask));

	/*
	 * RSEQ fast path: true lock-free per-CPU magazine access.
	 * Uses assembly critical sections when we own the rseq
	 * registration (not glibc). Falls through to locked path
	 * when the magazine is empty or rseq is not available.
	 */
#ifdef UMEM_RSEQ_AVAILABLE
	if (likely(umem_rseq_enabled) && cp->cache_rseq != NULL) {
		if (unlikely(!umem_rseq_registered))
			umem_rseq_register_thread();
		if (umem_rseq_cpu_idp != NULL) {
			int cpu = (int)*umem_rseq_cpu_idp;
			if (cpu >= 0 && cpu < umem_rseq_get_ncpus()) {
				umem_rseq_cache_t *rc = &cp->cache_rseq[cpu];
#if defined(__x86_64__) || defined(__aarch64__)
				if (umem_rseq_asm_safe) {
					buf = umem_rseq_alloc_fastpath(rc, cpu);
					if (buf != NULL) {
						if (unlikely(ccp->cc_flags & UMF_BUFTAG) &&
						    umem_cache_alloc_debug(cp, buf,
						    umflag) == -1) {
							if (umem_alloc_retry(cp, umflag))
								goto retry;
							return (NULL);
						}
						return (buf);
					}
				}
#endif
			}
		}
	}
#endif

	(void) mutex_lock(&ccp->cc_lock);
	for (;;) {
		/*
		 * Re-check rounds under lock. Another thread might have
		 * reloaded the magazine.
		 */
		rounds = ccp->cc_rounds;
		if (rounds > 0) {
			/*
			 * Prefetch the loaded magazine before accessing.
			 * High locality (3) since we access this frequently
			 * in the hot allocation path.
			 */
			__builtin_prefetch(ccp->cc_loaded, 0, 3);

			/*
			 * Decrement rounds. We hold the lock so this is safe.
			 */
			ccp->cc_rounds = rounds - 1;
			buf = ccp->cc_loaded->mag_round[rounds - 1];
			ccp->cc_alloc++;
			(void) mutex_unlock(&ccp->cc_lock);
			if (unlikely(ccp->cc_flags & UMF_BUFTAG) &&
			    umem_cache_alloc_debug(cp, buf, umflag) == -1) {
				if (umem_alloc_retry(cp, umflag)) {
					goto retry;
				}

				return (NULL);
			}
			return (buf);
		}

		/*
		 * The loaded magazine is empty.  If the previously loaded
		 * magazine was full, exchange them and try again.
		 */
		if (ccp->cc_prounds > 0) {
			umem_cpu_reload(ccp, ccp->cc_ploaded, ccp->cc_prounds);
			continue;
		}

		/*
		 * If the magazine layer is disabled, break out now.
		 */
		if (ccp->cc_magsize == 0)
			break;

		/*
		 * Try to get a full magazine from the depot.
		 */
		fmp = umem_depot_alloc(cp, &cp->cache_full);
		if (fmp != NULL) {
			if (ccp->cc_ploaded != NULL)
				umem_depot_free(cp, &cp->cache_empty,
				    ccp->cc_ploaded);
			umem_cpu_reload(ccp, fmp, ccp->cc_magsize);
			continue;
		}

		/*
		 * There are no full magazines in the depot,
		 * so fall through to the slab layer.
		 */
		break;
	}
	(void) mutex_unlock(&ccp->cc_lock);

	/*
	 * We couldn't allocate a constructed object from the magazine layer,
	 * so get a raw buffer from the slab layer and apply its constructor.
	 */
	buf = umem_slab_alloc(cp, umflag);

	if (unlikely(buf == NULL)) {
		if (cp == &umem_null_cache)
			return (NULL);
		if (umem_alloc_retry(cp, umflag)) {
			goto retry;
		}

		return (NULL);
	}

	if (unlikely(cp->cache_flags & UMF_BUFTAG)) {
		/*
		 * Let umem_cache_alloc_debug() apply the constructor for us.
		 */
		if (umem_cache_alloc_debug(cp, buf, umflag) == -1) {
			if (umem_alloc_retry(cp, umflag)) {
				goto retry;
			}
			return (NULL);
		}
		return (buf);
	}

	/*
	 * We do not pass fatal flags on to the constructor.  This prevents
	 * leaking buffers in the event of a subordinate constructor failing.
	 */
	flags_nfatal = UMEM_DEFAULT;
	if (cp->cache_constructor != NULL &&
	    cp->cache_constructor(buf, cp->cache_private, flags_nfatal) != 0) {
		atomic_add_64(&cp->cache_alloc_fail, 1);
		umem_slab_free(cp, buf);

		if (umem_alloc_retry(cp, umflag)) {
			goto retry;
		}
		return (NULL);
	}

	return (buf);
}

/*
 * Allocate multiple constructed objects from cache cp in a single lock
 * acquisition.  Returns the number of objects actually allocated (may be
 * less than count).  Objects are stored in bufs[0..returned-1].
 *
 * Magazine objects are pre-constructed: if the cache has a constructor,
 * it was already called when the object was first allocated from the slab.
 * Callers must not re-invoke the constructor on returned objects.
 */
int
umem_cache_alloc_batch(umem_cache_t *cp, void **bufs, int count, int umflag)
{
	umem_cpu_cache_t *ccp;
	umem_magazine_t *fmp;
	int rounds;
	int got = 0;

	if (count <= 0)
		return (0);

	ccp = UMEM_CPU_CACHE(cp, CPU_CACHED(cp->cache_cpu_mask));

	(void) mutex_lock(&ccp->cc_lock);
	while (got < count) {
		rounds = ccp->cc_rounds;
		if (rounds > 0) {
			/* Drain as many as we can from the loaded magazine */
			int avail = rounds;
			if (avail > count - got)
				avail = count - got;
			for (int i = 0; i < avail; i++) {
				bufs[got++] =
				    ccp->cc_loaded->mag_round[--ccp->cc_rounds];
			}
			ccp->cc_alloc += avail;
			continue;
		}

		/* Loaded magazine empty - try the previously loaded one */
		if (ccp->cc_prounds > 0) {
			umem_cpu_reload(ccp, ccp->cc_ploaded,
			    ccp->cc_prounds);
			continue;
		}

		if (ccp->cc_magsize == 0)
			break;

		/* Try to get a full magazine from the depot */
		fmp = umem_depot_alloc(cp, &cp->cache_full);
		if (fmp != NULL) {
			if (ccp->cc_ploaded != NULL)
				umem_depot_free(cp, &cp->cache_empty,
				    ccp->cc_ploaded);
			umem_cpu_reload(ccp, fmp, ccp->cc_magsize);
			continue;
		}

		/* No more magazines available */
		break;
	}
	(void) mutex_unlock(&ccp->cc_lock);

	/* Fall back to slab layer for remaining objects */
	while (got < count) {
		void *buf = umem_slab_alloc(cp, umflag);
		if (buf == NULL)
			break;
		if (cp->cache_constructor != NULL &&
		    cp->cache_constructor(buf, cp->cache_private,
		    UMEM_DEFAULT) != 0) {
			atomic_add_64(&cp->cache_alloc_fail, 1);
			umem_slab_free(cp, buf);
			break;
		}
		bufs[got++] = buf;
	}

	return (got);
}

/*
 * Free a constructed object to cache cp.
 */
#ifndef NO_WEAK_SYMBOLS
#pragma weak umem_cache_free = _umem_cache_free
#endif
void
_umem_cache_free(umem_cache_t *cp, void *buf)
{
	umem_cpu_cache_t *ccp = UMEM_CPU_CACHE(cp, CPU_CACHED(cp->cache_cpu_mask));
	umem_magazine_t *emp;
	umem_magtype_t *mtp;
	int rounds, magsize;

	/*
	 * NULL is not an object (P1.8): stored into a magazine it would come
	 * back out of umem_cache_alloc() as a failure with errno 0.  The man
	 * page said the argument must not be NULL; a silent no-op is what
	 * every caller of free(NULL) expects and costs one predictable branch.
	 */
	if (unlikely(buf == NULL))
		return;

	if (unlikely(ccp->cc_flags & UMF_BUFTAG))
		if (umem_cache_free_debug(cp, buf) == -1)
			return;

#ifdef UMEM_RSEQ_AVAILABLE
	if (likely(umem_rseq_enabled) && cp->cache_rseq != NULL) {
		if (unlikely(!umem_rseq_registered))
			umem_rseq_register_thread();
		if (umem_rseq_cpu_idp != NULL) {
			int cpu = (int)*umem_rseq_cpu_idp;
			if (cpu >= 0 && cpu < umem_rseq_get_ncpus()) {
				umem_rseq_cache_t *rc = &cp->cache_rseq[cpu];
#if defined(__x86_64__) || defined(__aarch64__)
				if (umem_rseq_asm_safe) {
					if (umem_rseq_free_fastpath(rc, buf,
					    cpu) == 0)
						return;
				}
#endif
			}
		}
	}
#endif

	(void) mutex_lock(&ccp->cc_lock);
	for (;;) {
		/*
		 * Re-check rounds under lock. Another thread might have
		 * reloaded the magazine.
		 */
		rounds = ccp->cc_rounds;
		magsize = ccp->cc_magsize;

		if ((uint_t)rounds < magsize) {
			/*
			 * Prefetch the loaded magazine before accessing.
			 * High locality (3) since we access this frequently
			 * in the hot free path.
			 */
			__builtin_prefetch(ccp->cc_loaded, 0, 3);

			/*
			 * Increment rounds. We hold the lock so this is safe.
			 */
			ccp->cc_rounds = rounds + 1;
			ccp->cc_loaded->mag_round[rounds] = buf;
			ccp->cc_free++;
			(void) mutex_unlock(&ccp->cc_lock);
			return;
		}

		/*
		 * The loaded magazine is full.  If the previously loaded
		 * magazine was empty, exchange them and try again.
		 */
		if (ccp->cc_prounds == 0) {
			umem_cpu_reload(ccp, ccp->cc_ploaded, ccp->cc_prounds);
			continue;
		}

		/*
		 * If the magazine layer is disabled, break out now.
		 */
		if (ccp->cc_magsize == 0)
			break;

		/*
		 * Try to get an empty magazine from the depot.
		 */
		emp = umem_depot_alloc(cp, &cp->cache_empty);
		if (emp != NULL) {
			if (ccp->cc_ploaded != NULL)
				umem_depot_free(cp, &cp->cache_full,
				    ccp->cc_ploaded);
			umem_cpu_reload(ccp, emp, 0);
			continue;
		}

		/*
		 * There are no empty magazines in the depot,
		 * so try to allocate a new one.  We must drop all locks
		 * across umem_cache_alloc() because lower layers may
		 * attempt to allocate from this cache.
		 */
		mtp = cp->cache_magtype;
		(void) mutex_unlock(&ccp->cc_lock);
		emp = _umem_cache_alloc(mtp->mt_cache, UMEM_DEFAULT);
		(void) mutex_lock(&ccp->cc_lock);

		if (emp != NULL) {
			atomic_add_64(&cp->cache_mag_total, 1);

			/*
			 * Initialize the new magazine with SIMD.
			 * This zeroes all pointers efficiently using
			 * vectorized stores when available (AVX2/SSE2/NEON).
			 */
			umem_mag_init_fast(emp->mag_round, mtp->mt_magsize);

			/*
			 * We successfully allocated an empty magazine.
			 * However, we had to drop ccp->cc_lock to do it,
			 * so the cache's magazine size may have changed.
			 * If so, free the magazine and try again.
			 */
			if (ccp->cc_magsize != mtp->mt_magsize) {
				(void) mutex_unlock(&ccp->cc_lock);
				_umem_cache_free(mtp->mt_cache, emp);
				atomic_add_64(&cp->cache_mag_total, -1ULL);
				(void) mutex_lock(&ccp->cc_lock);
				continue;
			}

			/*
			 * We got a magazine of the right size.  Add it to
			 * the depot and try the whole dance again.
			 */
			umem_depot_free(cp, &cp->cache_empty, emp);
			continue;
		}

		/*
		 * We couldn't allocate an empty magazine,
		 * so fall through to the slab layer.
		 */
		break;
	}
	(void) mutex_unlock(&ccp->cc_lock);

	/*
	 * We couldn't free our constructed object to the magazine layer,
	 * so apply its destructor and free it to the slab layer.
	 * Note that if UMF_BUFTAG is in effect, umem_cache_free_debug()
	 * will have already applied the destructor.
	 */
	if (!unlikely(cp->cache_flags & UMF_BUFTAG) && cp->cache_destructor != NULL)
		cp->cache_destructor(buf, cp->cache_private);

	umem_slab_free(cp, buf);
}

/*
 * Free multiple constructed objects to cache cp in a single lock
 * acquisition.  Returns the number of objects actually freed.
 */
int
umem_cache_free_batch(umem_cache_t *cp, void **bufs, int count)
{
	umem_cpu_cache_t *ccp;
	umem_magazine_t *emp;
	umem_magtype_t *mtp;
	int rounds, magsize;
	int freed = 0;

	if (count <= 0)
		return (0);

	ccp = UMEM_CPU_CACHE(cp, CPU_CACHED(cp->cache_cpu_mask));

	(void) mutex_lock(&ccp->cc_lock);
	while (freed < count) {
		rounds = ccp->cc_rounds;
		magsize = ccp->cc_magsize;

		if ((uint_t)rounds < magsize) {
			/* Fill as many as we can into the loaded magazine */
			int space = magsize - rounds;
			if (space > count - freed)
				space = count - freed;
			for (int i = 0; i < space; i++) {
				ccp->cc_loaded->mag_round[ccp->cc_rounds++] =
				    bufs[freed++];
			}
			ccp->cc_free += space;
			continue;
		}

		/* Loaded magazine full - try the previously loaded one */
		if (ccp->cc_prounds == 0) {
			umem_cpu_reload(ccp, ccp->cc_ploaded,
			    ccp->cc_prounds);
			continue;
		}

		if (ccp->cc_magsize == 0)
			break;

		/* Try to get an empty magazine from the depot */
		emp = umem_depot_alloc(cp, &cp->cache_empty);
		if (emp != NULL) {
			if (ccp->cc_ploaded != NULL)
				umem_depot_free(cp, &cp->cache_full,
				    ccp->cc_ploaded);
			umem_cpu_reload(ccp, emp, 0);
			continue;
		}

		/* Try to allocate a new empty magazine */
		mtp = cp->cache_magtype;
		(void) mutex_unlock(&ccp->cc_lock);
		emp = _umem_cache_alloc(mtp->mt_cache, UMEM_DEFAULT);
		(void) mutex_lock(&ccp->cc_lock);

		if (emp != NULL) {
			atomic_add_64(&cp->cache_mag_total, 1);
			umem_mag_init_fast(emp->mag_round, mtp->mt_magsize);
			if (ccp->cc_magsize != mtp->mt_magsize) {
				(void) mutex_unlock(&ccp->cc_lock);
				_umem_cache_free(mtp->mt_cache, emp);
				atomic_add_64(&cp->cache_mag_total, -1ULL);
				(void) mutex_lock(&ccp->cc_lock);
				continue;
			}
			umem_depot_free(cp, &cp->cache_empty, emp);
			continue;
		}

		/* No space in magazine layer */
		break;
	}
	(void) mutex_unlock(&ccp->cc_lock);

	/* Fall back to slab layer for remaining objects */
	while (freed < count) {
		if (cp->cache_destructor != NULL)
			cp->cache_destructor(bufs[freed], cp->cache_private);
		umem_slab_free(cp, bufs[freed]);
		freed++;
	}

	return (freed);
}

#ifndef NO_WEAK_SYMBOLS
#pragma weak umem_zalloc = _umem_zalloc
#endif
void *
_umem_zalloc(size_t size, int umflag)
{
	size_t index = (size - 1) >> UMEM_ALIGN_SHIFT;
	void *buf;

retry:
	if (index < UMEM_MAXBUF >> UMEM_ALIGN_SHIFT) {
		umem_cache_t *cp = umem_alloc_table[index];
		buf = _umem_cache_alloc(cp, umflag);
		if (buf != NULL) {
			if (unlikely(cp->cache_flags & UMF_BUFTAG)) {
				umem_buftag_t *btp = UMEM_BUFTAG(cp, buf);
				((uint8_t *)buf)[size] = UMEM_REDZONE_BYTE;
				((uint32_t *)btp)[1] = UMEM_SIZE_ENCODE(size);
			}
			bzero(buf, size);
		} else if (umem_alloc_retry(cp, umflag))
			goto retry;
	} else {
		buf = _umem_alloc(size, umflag);        /* handles failure */
		if (buf != NULL)
			bzero(buf, size);
	}
	return (buf);
}

#ifndef NO_WEAK_SYMBOLS
#pragma weak umem_alloc = _umem_alloc
#endif
void *
_umem_alloc(size_t size, int umflag)
{
	size_t index = (size - 1) >> UMEM_ALIGN_SHIFT;
	void *buf;
	static __thread int ptc_initializing = 0;
umem_alloc_retry:
	if (index < UMEM_MAXBUF >> UMEM_ALIGN_SHIFT) {
		umem_cache_t *cp = umem_alloc_table[index];
		/*
		 * Inlined PTC fast path for small allocations.
		 * The bin_table encodes -1 for disabled PTC, debug
		 * caches, and ineligible sizes — one check covers all.
		 *
		 * The cp lookup is done before the PTC block so the
		 * CPU can begin the dependent load while we check the
		 * thread-local cache, avoiding a pipeline stall on
		 * PTC miss under contention.
		 */
		{
			int8_t bin = umem_ptc_bin_table[index];
			if (likely(bin >= 0) &&
			    likely(!umem_introspect_break_armed)) {
				umem_ptc_t *ptc = thread_ptc;
				if (unlikely(ptc == NULL) &&
				    !ptc_initializing) {
					ptc_initializing = 1;
					ptc = umem_ptc_get();
					ptc_initializing = 0;
				}
				if (likely(ptc != NULL)) {
					umem_ptc_bin_t *b =
					    &ptc->bins[(int)bin];
					if (likely(b->count > 0)) {
						return
						    (b->slots[--b->count]);
					}
					/*
					 * PTC bin empty — try per-thread
					 * magazine before taking cc_lock.
					 */
					{
					umem_ptc_mag_t *mag =
					    &ptc->mags[(int)bin];
					if (mag->rounds > 0) {
						mag->rounds--;
						buf = mag->loaded->
						    mag_round[mag->rounds];
						return (buf);
					}
					/*
					 * Loaded mag empty — swap with
					 * previous magazine.
					 */
					if (mag->prounds > 0) {
						umem_magazine_t *tmp;
						int tmp_r;
						tmp = mag->loaded;
						tmp_r = mag->rounds;
						mag->loaded = mag->previous;
						mag->rounds = mag->prounds;
						mag->previous = tmp;
						mag->prounds = tmp_r;
						tmp_r = mag->magsize;
						mag->magsize = mag->pmagsize;
						mag->pmagsize = tmp_r;
						mag->rounds--;
						buf = mag->loaded->
						    mag_round[mag->rounds];
						return (buf);
					}
					/*
					 * Both mags empty — refill from
					 * depot (trylock only, never blocks).
					 */
					if (mag->cache == NULL)
						mag->cache = cp;
					{
					umem_magazine_t *fmp;
					fmp = umem_depot_alloc_trylock(cp,
					    &cp->cache_full);
					if (fmp != NULL) {
					    UMEM_PTC_RESIZE_PROBE_POINT();
					    /*
					     * Retire the empty magazines we
					     * hold.  Both are empty here
					     * (rounds == prounds == 0), and
					     * umem_ptc_mag_return_trylock
					     * handles a magazine whose
					     * magtype the cache has since
					     * moved past.
					     */
					    if (mag->loaded != NULL) {
						if (mag->previous != NULL)
						    umem_ptc_mag_return_trylock(
							cp,
							&cp->cache_empty,
							mag->previous,
							mag->prounds);
						mag->previous = mag->loaded;
						mag->prounds = mag->rounds;
						mag->pmagsize = mag->magsize;
					    }
					    /*
					     * Capacity comes from the magazine
					     * we just obtained, NOT from
					     * cp->cache_magtype: a resize
					     * between the two reads would
					     * index an old 127-round magazine
					     * as if it had 255 (P1.3b).  The
					     * magazine and the capacity it is
					     * used with are now one decision.
					     */
					    mag->loaded = fmp;
					    mag->magsize =
						umem_mag_capacity(fmp);
					    mag->rounds = mag->magsize;
					    UMEM_PTC_PROBE_OBSERVE(fmp,
						mag->magsize);
					    umem_ptc_mag_check(mag);
					    mag->rounds--;
					    buf = mag->loaded->
						mag_round[mag->rounds];
					    return (buf);
					}
					}
					}
				}
			}
		}

		buf = _umem_cache_alloc(cp, umflag);
		if (unlikely(cp->cache_flags & UMF_BUFTAG) && buf != NULL) {
			umem_buftag_t *btp = UMEM_BUFTAG(cp, buf);
			((uint8_t *)buf)[size] = UMEM_REDZONE_BYTE;
			((uint32_t *)btp)[1] = UMEM_SIZE_ENCODE(size);
		}
		if (buf == NULL && umem_alloc_retry(cp, umflag))
			goto umem_alloc_retry;
#ifdef UMEM_INTROSPECT
		if (unlikely(umem_introspect_break_armed) && buf != NULL)
			umem_introspect_break_check(buf, size, cp);
#endif
		return (buf);
	}
	if (size == 0)
		return (NULL);
	if (unlikely(umem_oversize_arena == NULL)) {
		if (umem_init())
			ASSERT(umem_oversize_arena != NULL);
		else
			return (NULL);
	}
	buf = vmem_alloc(umem_oversize_arena, size, UMEM_VMFLAGS(umflag));
	if (unlikely(buf == NULL)) {
		umem_log_event(umem_failure_log, NULL, NULL, (void *)size);
		if (umem_alloc_retry(NULL, umflag))
			goto umem_alloc_retry;
	}
	return (buf);
}

#ifndef NO_WEAK_SYMBOLS
#pragma weak umem_alloc_align = _umem_alloc_align
#endif
void *
_umem_alloc_align(size_t size, size_t align, int umflag)
{
	void *buf;

	if (size == 0)
		return (NULL);
	if ((align & (align - 1)) != 0)
		return (NULL);
	if (align < UMEM_ALIGN)
		align = UMEM_ALIGN;

umem_alloc_align_retry:
	if (unlikely(umem_memalign_arena == NULL)) {
		if (umem_init())
			ASSERT(umem_oversize_arena != NULL);
		else
			return (NULL);
	}
	buf = vmem_xalloc(umem_memalign_arena, size, align, 0, 0, NULL, NULL,
	    UMEM_VMFLAGS(umflag));
	if (unlikely(buf == NULL)) {
		umem_log_event(umem_failure_log, NULL, NULL, (void *)size);
		if (umem_alloc_retry(NULL, umflag))
			goto umem_alloc_align_retry;
	}
	return (buf);
}

#ifndef NO_WEAK_SYMBOLS
#pragma weak umem_free = _umem_free
#endif
void
_umem_free(void *buf, size_t size)
{
	size_t index = (size - 1) >> UMEM_ALIGN_SHIFT;
	static __thread int ptc_initializing_free = 0;

	/*
	 * Freeing NULL is a no-op for every size (P1.8).  Without this the
	 * PTC and magazine stores below took NULL as an object and the next
	 * umem_alloc(size) on this thread returned it, with errno 0.  This
	 * is the ONE place for the check: the size-class paths, the oversize
	 * branch and (via _umem_cache_free) the CPU layer all sit below it.
	 */
	if (unlikely(buf == NULL))
		return;

	if (index < UMEM_MAXBUF >> UMEM_ALIGN_SHIFT) {
		umem_cache_t *cp = umem_alloc_table[index];

		/*
		 * Inlined PTC free fast path for small allocations.
		 * The bin_table encodes -1 for debug caches, so PTC
		 * never bypasses buftag validation.
		 *
		 * The cp lookup is done before the PTC block so the
		 * CPU can begin the dependent load while we check the
		 * thread-local cache.
		 */
		{
			int8_t bin = umem_ptc_bin_table[index];
			if (likely(bin >= 0)) {
				umem_ptc_t *ptc = thread_ptc;
				if (unlikely(ptc == NULL) &&
				    !ptc_initializing_free) {
					ptc_initializing_free = 1;
					ptc = umem_ptc_get();
					ptc_initializing_free = 0;
				}
				if (likely(ptc != NULL)) {
					umem_ptc_bin_t *b =
					    &ptc->bins[(int)bin];
					if (likely(b->count <
					    ptc_bin_capacity((int)bin))) {
						b->slots[b->count++] = buf;
						return;
					}
					/*
					 * PTC bin full — try per-thread
					 * magazine before taking cc_lock.
					 */
					{
					umem_ptc_mag_t *mag =
					    &ptc->mags[(int)bin];
					if (mag->cache == NULL)
						mag->cache = cp;
					if (mag->loaded != NULL &&
					    mag->rounds < mag->magsize) {
						mag->loaded->
						    mag_round[mag->rounds] =
						    buf;
						mag->rounds++;
						return;
					}
					/*
					 * Loaded mag full — swap with
					 * previous magazine.
					 */
					if (mag->previous != NULL &&
					    mag->prounds < mag->pmagsize) {
						umem_magazine_t *tmp;
						int tmp_r;
						tmp = mag->loaded;
						tmp_r = mag->rounds;
						mag->loaded = mag->previous;
						mag->rounds = mag->prounds;
						mag->previous = tmp;
						mag->prounds = tmp_r;
						tmp_r = mag->magsize;
						mag->magsize = mag->pmagsize;
						mag->pmagsize = tmp_r;
						mag->loaded->
						    mag_round[mag->rounds] =
						    buf;
						mag->rounds++;
						return;
					}
					/*
					 * Both mags full — flush loaded
					 * to depot, get empty magazine.
					 * Uses trylock to avoid blocking.
					 */
					{
					umem_magazine_t *emp;
					if (mag->loaded != NULL) {
						/*
						 * Hand over the count with
						 * the magazine: the callee
						 * needs it both to honour the
						 * depot's full-list contract
						 * and to drain the objects if
						 * the magazine can no longer
						 * go on a list at all (P1.3c).
						 */
						umem_ptc_mag_return_trylock(cp,
						    &cp->cache_full,
						    mag->loaded, mag->rounds);
						mag->loaded = NULL;
						mag->rounds = 0;
					}
					emp = umem_depot_alloc_trylock(cp,
					    &cp->cache_empty);
					if (emp == NULL)
						emp = umem_ptc_mag_prime(cp);
					if (emp != NULL) {
						UMEM_PTC_RESIZE_PROBE_POINT();
						/*
						 * Capacity from the magazine
						 * itself, not from the
						 * cache's current magtype
						 * (P1.3b) -- see the matching
						 * comment in _umem_alloc.
						 */
						mag->loaded = emp;
						mag->magsize =
						    umem_mag_capacity(emp);
						mag->rounds = 0;
						UMEM_PTC_PROBE_OBSERVE(emp,
						    mag->magsize);
						umem_ptc_mag_check(mag);
						mag->loaded->
						    mag_round[mag->rounds] =
						    buf;
						mag->rounds++;
						return;
					}
					/*
					 * No empty magazine from the depot
					 * and none could be allocated -- loaded
					 * was already donated, so fall through
					 * with none.
					 */
					}
					}
				}
			}
		}

		if (unlikely(cp->cache_flags & UMF_BUFTAG)) {
			umem_buftag_t *btp = UMEM_BUFTAG(cp, buf);
			uint32_t *ip = (uint32_t *)btp;
			if (ip[1] != UMEM_SIZE_ENCODE(size)) {
				if (*(uint64_t *)buf == UMEM_FREE_PATTERN) {
					umem_error(UMERR_DUPFREE, cp, buf);
					return;
				}
				if (UMEM_SIZE_VALID(ip[1])) {
					ip[0] = UMEM_SIZE_ENCODE(size);
					umem_error(UMERR_BADSIZE, cp, buf);
				} else {
					umem_error(UMERR_REDZONE, cp, buf);
				}
				return;
			}
			if (((uint8_t *)buf)[size] != UMEM_REDZONE_BYTE) {
				umem_error(UMERR_REDZONE, cp, buf);
				return;
			}
			btp->bt_redzone = UMEM_REDZONE_PATTERN;
		}

		_umem_cache_free(cp, buf);
	} else {
		vmem_free(umem_oversize_arena, buf, size);
	}
}

#ifndef NO_WEAK_SYMBOLS
#pragma weak umem_free_align = _umem_free_align
#endif
void
_umem_free_align(void *buf, size_t size)
{
	/* Same contract as umem_free (P1.8).  Before: umem_panic "bad free". */
	if (buf == NULL)
		return;
	vmem_xfree(umem_memalign_arena, buf, size);
}

static void *
umem_firewall_va_alloc(vmem_t *vmp, size_t size, int vmflag)
{
	size_t realsize = size + vmp->vm_quantum;

	/*
	 * Annoying edge case: if 'size' is just shy of ULONG_MAX, adding
	 * vm_quantum will cause integer wraparound.  Check for this, and
	 * blow off the firewall page in this case.  Note that such a
	 * giant allocation (the entire address space) can never be
	 * satisfied, so it will either fail immediately (VM_NOSLEEP)
	 * or sleep forever (VM_SLEEP).  Thus, there is no need for a
	 * corresponding check in umem_firewall_va_free().
	 */
	if (realsize < size)
		realsize = size;

	return (vmem_alloc(vmp, realsize, vmflag | VM_NEXTFIT));
}

static void
umem_firewall_va_free(vmem_t *vmp, void *addr, size_t size)
{
	vmem_free(vmp, addr, size + vmp->vm_quantum);
}

static void umem_cache_reclaim_pages(umem_cache_t *cp);

/*
 * Reclaim all unused memory from a cache.
 */
static void
umem_cache_reap(umem_cache_t *cp)
{
	/*
	 * Ask the cache's owner to free some memory if possible.
	 * The idea is to handle things like the inode cache, which
	 * typically sits on a bunch of memory that it doesn't truly
	 * *need*.  Reclaim policy is entirely up to the owner; this
	 * callback is just an advisory plea for help.
	 */
	if (cp->cache_reclaim != NULL)
		cp->cache_reclaim(cp->cache_private);

	umem_depot_ws_reap(cp);

	/*
	 * Slab page reclamation (madvise) is handled by the update
	 * thread via umem_cache_update() → umem_cache_reclaim_pages().
	 * We don't call it here because reclaim_pages drops and
	 * reacquires cache_lock internally, creating a race window
	 * under heavy allocation pressure.
	 */

	/*
	 * Reap the magazine shell cache too, so magazine shells
	 * return to vmem when the user cache shrinks.
	 */
	if (cp->cache_magtype && cp->cache_magtype->mt_cache) {
		umem_depot_ws_reap(cp->cache_magtype->mt_cache);
	}
}

/*
 * Purge all magazines from a cache and set its magazine limit to zero.
 * All calls are serialized by being done by the update thread, except for
 * the final call from umem_cache_destroy().
 */
static void
umem_cache_magazine_purge(umem_cache_t *cp)
{
	umem_cpu_cache_t *ccp;
	umem_magazine_t *mp, *pmp;
	int rounds, prounds, cpu_seqid;

	ASSERT(cp->cache_next == NULL || IN_UPDATE());

	for (cpu_seqid = 0; cpu_seqid < umem_max_ncpus; cpu_seqid++) {
		ccp = &cp->cache_cpu[cpu_seqid];

		(void) mutex_lock(&ccp->cc_lock);
		mp = ccp->cc_loaded;
		pmp = ccp->cc_ploaded;
		rounds = ccp->cc_rounds;
		prounds = ccp->cc_prounds;
		ccp->cc_loaded = NULL;
		ccp->cc_ploaded = NULL;
		ccp->cc_rounds = -1;
		ccp->cc_prounds = -1;
		ccp->cc_magsize = 0;
		(void) mutex_unlock(&ccp->cc_lock);

		if (mp)
			umem_magazine_destroy(cp, mp, rounds);
		if (pmp)
			umem_magazine_destroy(cp, pmp, prounds);
	}

	/*
	 * Updating the working set statistics twice in a row has the
	 * effect of setting the working set size to zero, so everything
	 * is eligible for reaping.
	 */
	umem_depot_ws_update(cp);
	umem_depot_ws_update(cp);

	umem_depot_ws_reap(cp);
}

/*
 * Enable per-cpu magazines on a cache.
 */
static void
umem_cache_magazine_enable(umem_cache_t *cp)
{
	int cpu_seqid;

	if (unlikely(cp->cache_flags & UMF_NOMAGAZINE))
		return;

	for (cpu_seqid = 0; cpu_seqid < umem_max_ncpus; cpu_seqid++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[cpu_seqid];
		(void) mutex_lock(&ccp->cc_lock);
		ccp->cc_magsize = cp->cache_magtype->mt_magsize;
		(void) mutex_unlock(&ccp->cc_lock);
	}

}

/*
 * Recompute a cache's magazine size.  The trade-off is that larger magazines
 * provide a higher transfer rate with the depot, while smaller magazines
 * reduce memory consumption.  Magazine resizing is an expensive operation;
 * it should not be done frequently.
 *
 * Changes to the magazine size are serialized by only having one thread
 * doing updates. (the update thread)
 *
 * Note: at present this only grows the magazine size.  It might be useful
 * to allow shrinkage too.
 */
static void
umem_cache_magazine_resize(umem_cache_t *cp)
{
	umem_magtype_t *mtp = cp->cache_magtype;
	umem_magtype_t *last = &umem_magtype[
	    sizeof (umem_magtype) / sizeof (umem_magtype[0]) - 1];

	ASSERT(IN_UPDATE());

	if (mtp < last && cp->cache_chunksize < mtp->mt_maxbuf) {
		umem_cache_magazine_purge(cp);
		(void) mutex_lock(&cp->cache_full.ml_lock);
		cp->cache_magtype = ++mtp;
		cp->cache_depot_contention_prev =
		    cp->cache_depot_contention + INT_MAX;
		(void) mutex_unlock(&cp->cache_full.ml_lock);
		umem_cache_magazine_enable(cp);
	}
}

/*
 * Rescale a cache's hash table, so that the table size is roughly the
 * cache size.  We want the average lookup time to be extremely small.
 */
static void
umem_hash_rescale(umem_cache_t *cp)
{
	umem_bufctl_t **old_table, **new_table, *bcp;
	size_t old_size, new_size, h;

	ASSERT(IN_UPDATE());

	new_size = MAX(UMEM_HASH_INITIAL,
	    1 << (highbit(3 * cp->cache_buftotal + 4) - 2));
	old_size = cp->cache_hash_mask + 1;

	if ((old_size >> 1) <= new_size && new_size <= (old_size << 1))
		return;

	new_table = vmem_alloc(umem_hash_arena, new_size * sizeof (void *),
	    VM_NOSLEEP);
	if (unlikely(new_table == NULL))
		return;
	bzero(new_table, new_size * sizeof (void *));

	(void) mutex_lock(&cp->cache_lock);

	old_size = cp->cache_hash_mask + 1;
	old_table = cp->cache_hash_table;

	cp->cache_hash_mask = new_size - 1;
	cp->cache_hash_table = new_table;
	cp->cache_rescale++;

	for (h = 0; h < old_size; h++) {
		bcp = old_table[h];
		while (bcp != NULL) {
			void *addr = bcp->bc_addr;
			umem_bufctl_t *next_bcp = bcp->bc_next;
			umem_bufctl_t **hash_bucket = UMEM_HASH(cp, addr);
			bcp->bc_next = *hash_bucket;
			*hash_bucket = bcp;
			bcp = next_bcp;
		}
	}

	(void) mutex_unlock(&cp->cache_lock);

	vmem_free(umem_hash_arena, old_table, old_size * sizeof (void *));
}

/*
 * True if a discarded page of cp's buffer region would take allocator
 * metadata with it.
 *
 * MADV_DONTNEED (and MEM_RESET / MADV_FREE) make the next touch see a
 * zero-filled page.  Two kinds of metadata live INSIDE the buffer region
 * and are built exactly once, by umem_slab_create(); nothing rebuilds them
 * when umem_slab_alloc() reactivates an idle slab:
 *
 *  - Embedded bufctls (every cache without UMF_HASH).  The slab freelist
 *    links bc_next sit at buf + cache_bufctl in each chunk, i.e. spread
 *    over every page of the buffer region.  umem_slab_alloc() follows
 *    them, so a zeroed page truncates the freelist and breaks the
 *    slab_refcnt/slab_chunks invariant.  (Single-page slabs -- the default
 *    arena case, quantum == PAGESIZE -- never reach madvise anyway,
 *    because the range below the metadata page is empty.  Multi-page
 *    non-hash slabs, which a caller-supplied arena with quantum >
 *    PAGESIZE produces, do.)
 *
 *  - In-buffer buftags and the UMEM_FREE_PATTERN (UMF_BUFTAG, i.e.
 *    UMF_DEADBEEF or UMF_REDZONE).  umem_cache_alloc_debug() validates
 *    bt_bxstat == (bt_bufctl ^ UMEM_BUFTAG_FREE) and the free pattern
 *    BEFORE running the constructor, so a zeroed page makes a correct
 *    allocation report heap corruption -- and no constructor can repair
 *    it, because validation runs first.
 *
 * Discarding either is a correctness bug, not a space/time trade, so such
 * slabs keep their pages.  They still advance DIRTY -> CLEAN and are
 * destroyed outright after umem_reclaim_delay * 2, which returns the whole
 * span to the arena.
 *
 * ponytail: this gives up page reclamation for multi-page non-hash slabs
 * (only reachable through a caller-supplied arena with quantum > PAGESIZE)
 * and for debug builds.  Default-arena non-hash slabs are one page and
 * never had any page to discard, so nothing regresses there; hash caches
 * without buftags -- which is where the large multi-page slabs live -- keep
 * reclaiming.  Upgrade path if the retained case ever matters: rebuild the
 * in-buffer metadata on reactivation (walk the chunks re-linking bc_next,
 * re-stamping bt_bufctl/bt_bxstat/bt_redzone and UMEM_FREE_PATTERN) instead
 * of excluding the slab, and only then discard its pages.
 */
static int
umem_slab_keeps_metadata(umem_cache_t *cp)
{
	return (!(cp->cache_flags & UMF_HASH) ||
	    (cp->cache_flags & UMF_BUFTAG) != 0);
}

/*
 * Release physical pages backing an empty slab via madvise.
 * The virtual address range remains valid so the slab can be reused;
 * the kernel will supply zero-filled pages on next access.
 *
 * Advises only; the caller publishes the resulting SLAB_CLEAN state under
 * cp->cache_lock (see umem_cache_reclaim_pages()).
 */
static void
umem_slab_reclaim(umem_cache_t *cp, umem_slab_t *sp)
{
	/*
	 * Slabs whose in-buffer metadata the allocator reads again after
	 * reactivation are never discarded.  See umem_slab_keeps_metadata().
	 */
	if (umem_slab_keeps_metadata(cp))
		return;

	/*
	 * MADV_DONTNEED / MADV_FREE act on whole pages: the kernel discards
	 * every page the range touches (empirically it discards the start
	 * page even for a sub-page length).  The umem_slab_t metadata is
	 * embedded at the END of the slab for a non-hash cache, sharing its
	 * page with the tail of the buffer region.  The old code advised
	 * [slab_base, slab_base + slabsize - sizeof(umem_slab_t)); when the
	 * slab color was 0 that range started page-aligned and its (sub-page)
	 * length still caused the kernel to discard the whole page that holds
	 * the metadata, zero-filling slab_cache/slab_next/slab_prev on next
	 * touch.  That corrupted the cache freelist and tripped the
	 * sp->slab_cache == cp assertion (or SEGV'd the reap walk) under heavy
	 * churn.  (With a non-zero color the start was unaligned and madvise
	 * returned EINVAL, so no memory was ever reclaimed at all.)
	 *
	 * Reclaim only whole pages that lie strictly below the page holding
	 * the metadata: round the start UP and the end DOWN to page
	 * boundaries, and for a non-hash cache stop at the start of the
	 * metadata's page.  If nothing full-page remains, reclaim nothing --
	 * correctness over reclaiming a partial page that shares metadata.
	 */
	uintptr_t start = P2ROUNDUP((uintptr_t)sp->slab_base, PAGESIZE);
	uintptr_t limit = P2END((uintptr_t)sp->slab_base, cp->cache_slabsize);
	uintptr_t end = P2ALIGN(limit, PAGESIZE);

	if (end <= start)
		return;

	size_t reclaim_size = (size_t)(end - start);
	void *reclaim_base = (void *)start;

#if defined(_WIN32)
	(void) VirtualAlloc(reclaim_base, reclaim_size,
	    MEM_RESET, PAGE_READWRITE);
#elif defined(__FreeBSD__)
	(void) madvise(reclaim_base, reclaim_size, MADV_FREE);
#else
	(void) madvise(reclaim_base, reclaim_size, MADV_DONTNEED);
#endif
}

/*
 * Walk the slab freelist looking for DIRTY slabs whose idle time
 * exceeds umem_reclaim_delay.  For each one, advise the kernel that
 * the pages are no longer needed.  Also increment idle time for
 * DIRTY slabs that haven't yet reached the threshold, and destroy
 * CLEAN slabs that have sat idle for twice the reclaim delay.
 *
 * Must be called with cp->cache_lock held.  Drops and reacquires
 * the lock around madvise and slab destroy calls.
 *
 * Serialized per cache by umem_cache_lock: every caller reaches here
 * through umem_cache_applyall(umem_cache_update), which holds
 * umem_cache_lock across the whole walk.  That is also what keeps
 * umem_cache_destroy() (which takes umem_cache_lock to unlink the cache)
 * from overlapping a reclaim pass on the same cache.
 */
static void
umem_cache_reclaim_pages(umem_cache_t *cp)
{
	umem_slab_t *sp, *next;
	umem_slab_t *nullsp = &cp->cache_nullslab;
	uint32_t delay = umem_reclaim_delay;
	umem_slab_t *reclaim_list = NULL;	/* DIRTY: madvise, stay linked */
	umem_slab_t *destroy_list = NULL;	/* CLEAN: unlinked, to destroy */

	ASSERT(MUTEX_HELD(&cp->cache_lock));

	/*
	 * Single pass under the lock: decide each slab's fate and collect
	 * the slow work (madvise / destroy) into local lists.  Previously
	 * this loop dropped and reacquired cache_lock in the middle of the
	 * walk, around each madvise/destroy; a concurrent allocation could
	 * then relink or the just-freed slab could invalidate the cached
	 * `next`, so the next iteration dereferenced freed slab metadata and
	 * crashed (SEGV in `next = sp->slab_prev`) under heavy churn.  By
	 * doing all list surgery under the single held lock and deferring
	 * only the lock-free madvise/destroy to after the walk, the walk
	 * never spans a lock drop and `next` is always valid.
	 *
	 * CLEAN slabs are unlinked here, so they are private to this thread
	 * before we ever drop the lock -- safe to destroy.  DIRTY slabs
	 * being madvised stay linked (madvise only advises their pages);
	 * they are chained through slab_reclaim_next, a field only the
	 * update thread touches, so leaving them linked is safe.
	 */
	for (sp = nullsp->slab_prev; sp != nullsp; sp = next) {
		next = sp->slab_prev;

		if (sp->slab_refcnt != 0)
			continue;

		if (sp->slab_state == SLAB_DIRTY) {
			sp->slab_idle_time += umem_reap_interval;
			if (sp->slab_idle_time >= delay) {
				sp->slab_state = SLAB_RECLAIMING;
				sp->slab_reclaim_next = reclaim_list;
				reclaim_list = sp;
			}
		} else if (sp->slab_state == SLAB_CLEAN) {
			sp->slab_idle_time += umem_reap_interval;
			if (sp->slab_idle_time >= delay * 2) {
				sp->slab_next->slab_prev = sp->slab_prev;
				sp->slab_prev->slab_next = sp->slab_next;
				if (sp == cp->cache_freelist)
					cp->cache_freelist = sp->slab_next;
				cp->cache_slab_destroy++;
				cp->cache_buftotal -= sp->slab_chunks;
				sp->slab_reclaim_next = destroy_list;
				destroy_list = sp;
			}
		}
	}

	/*
	 * Drop the lock once and do the slow work.  reclaim_list slabs are
	 * still linked but marked SLAB_RECLAIMING (so a concurrent free
	 * leaves them alone); destroy_list slabs are fully unlinked and
	 * private.
	 */
	(void) mutex_unlock(&cp->cache_lock);

	for (sp = reclaim_list; sp != NULL; sp = sp->slab_reclaim_next)
		umem_slab_reclaim(cp, sp);	/* advise only, no state write */

	while (destroy_list != NULL) {
		sp = destroy_list;
		destroy_list = sp->slab_reclaim_next;
		umem_slab_destroy(cp, sp);
	}

	(void) mutex_lock(&cp->cache_lock);

	/*
	 * INVARIANT: slab_state is read and written only under
	 * cp->cache_lock.  umem_slab_alloc() and umem_slab_free() read it
	 * under that lock, so publishing SLAB_CLEAN from umem_slab_reclaim()
	 * without the lock (as this code used to do, both on the madvise path
	 * and on the nothing-to-advise path) was a data race: a mutex
	 * serializes a reader only against writers that also take it.
	 *
	 * The RECLAIMING -> CLEAN transition is therefore made here, after
	 * retaking the lock.  Nothing else can have touched these slabs while
	 * it was dropped: they have slab_refcnt == 0 so no free can target
	 * them, and umem_slab_alloc() skips SLAB_RECLAIMING.
	 */
	while (reclaim_list != NULL) {
		sp = reclaim_list;
		reclaim_list = sp->slab_reclaim_next;
		ASSERT(sp->slab_state == SLAB_RECLAIMING);
		ASSERT(sp->slab_refcnt == 0);
		sp->slab_state = SLAB_CLEAN;
	}
}

/*
 * Destroy every empty slab the cache still holds, returning their backing
 * spans to the arena (and, for a UMF_HASH cache, their external slab and
 * bufctl metadata to the internal caches).
 *
 * Needed because background reclamation retains empty slabs: since
 * umem_reclaim_enabled defaults on, umem_slab_free()'s last-object path
 * marks the slab SLAB_DIRTY and leaves it linked for the update thread
 * instead of destroying it.  umem_cache_destroy() used to only log a
 * nonzero cache_buftotal and then free the cache descriptor, so those
 * retained slabs' spans and metadata stayed allocated forever with
 * slab_cache pointing at freed memory.  create/alloc/free/destroy on a
 * UMC_NOMAGAZINE cache leaked one slab per cache, no delay needed.
 *
 * Must be called with no locks held, after the magazine layer has been
 * purged (so every cached buffer has already returned to its slab) and
 * after the cache is off umem_cache_lock's list (so no update pass can be
 * walking its slabs).  Slabs with outstanding allocations are left alone
 * and reported: their buffers are still in the caller's hands, and freeing
 * the span under a live buffer would be worse than leaking it.
 */
static void
umem_cache_drain_slabs(umem_cache_t *cp)
{
	umem_slab_t *nullsp = &cp->cache_nullslab;
	umem_slab_t *sp, *next;
	umem_slab_t *destroy_list = NULL;
	long outstanding = 0;

	ASSERT(cp->cache_next == NULL);		/* off the global cache list */

	(void) mutex_lock(&cp->cache_lock);

	/*
	 * The slab list threaded through cache_nullslab holds every slab,
	 * not just the ones with free buffers (cache_freelist is only where
	 * the free-buffer portion starts), so this walk sees them all.
	 * Collect the empty ones under the lock and destroy them after
	 * dropping it, the same pattern umem_cache_reclaim_pages() uses.
	 */
	for (sp = nullsp->slab_next; sp != nullsp; sp = next) {
		next = sp->slab_next;

		ASSERT(sp->slab_cache == cp);
		ASSERT(sp->slab_state != SLAB_RECLAIMING);

		if (sp->slab_refcnt != 0) {
			outstanding += sp->slab_refcnt;
			continue;
		}

		sp->slab_next->slab_prev = sp->slab_prev;
		sp->slab_prev->slab_next = sp->slab_next;
		if (sp == cp->cache_freelist)
			cp->cache_freelist = sp->slab_next;
		cp->cache_slab_destroy++;
		cp->cache_buftotal -= sp->slab_chunks;
		sp->slab_reclaim_next = destroy_list;
		destroy_list = sp;
	}

	(void) mutex_unlock(&cp->cache_lock);

	while (destroy_list != NULL) {
		sp = destroy_list;
		destroy_list = sp->slab_reclaim_next;
		umem_slab_destroy(cp, sp);
	}

	if (outstanding != 0)
		log_message("umem_cache_destroy: '%s' (%p) has %ld "
		    "outstanding buffer(s); their slabs are leaked\n",
		    cp->cache_name, (void *)cp, outstanding);
}

/*
 * Perform periodic maintenance on a cache: hash rescaling,
 * depot working-set update, and magazine resizing.
 */
void
umem_cache_update(umem_cache_t *cp)
{
	int update_flags = 0;

	ASSERT(MUTEX_HELD(&umem_cache_lock));

	/*
	 * If the cache has become much larger or smaller than its hash table,
	 * fire off a request to rescale the hash table.
	 */
	(void) mutex_lock(&cp->cache_lock);

	if (unlikely(cp->cache_flags & UMF_HASH) &&
	    (cp->cache_buftotal > (cp->cache_hash_mask << 1) ||
	    (cp->cache_buftotal < (cp->cache_hash_mask >> 1) &&
	    cp->cache_hash_mask > UMEM_HASH_INITIAL)))
		update_flags |= UMU_HASH_RESCALE;

	(void) mutex_unlock(&cp->cache_lock);

	/*
	 * Update the depot working set statistics.
	 */
	umem_depot_ws_update(cp);

	/*
	 * Reap the depot's working-set excess on the periodic pass.
	 *
	 * The ws_update above is only bookkeeping: after two idle passes
	 * ml_reaplimit holds the number of magazines this list did not touch
	 * for a whole interval.  Nothing acted on that number unless the
	 * application called umem_reap() or a backend allocation failed --
	 * the Solaris kmem model, where the kernel's lowmem callback is the
	 * pressure signal.  A user process has no such signal, so on Linux a
	 * process that freed a heap and never ran out of memory kept every
	 * freed object in a depot magazine forever: slab_refcnt never reached
	 * zero, umem_cache_reclaim_pages() below skipped every slab, and the
	 * reclaim feature -- reclaim=1 by default, reclaim_delay documented --
	 * could not fire on the most ordinary shape of "free a big heap".
	 * Measured: 2 GB of freed 4 KiB objects 100 % resident at t = 100 s,
	 * 16,911 full depot magazines, update thread confirmed running.
	 *
	 * So when reclaim is on, request UMU_REAP whenever some list is above
	 * its working set.  The request is cheap (a flag; umem_process_updates
	 * does the work off the applyall walk), it reaps only the excess the
	 * bookkeeping already identified, and it turns the depot into the
	 * feeder that reclaim_pages was always waiting on.
	 */
	if (umem_reclaim_enabled && umem_depot_ws_excess(cp))
		update_flags |= UMU_REAP;

	/*
	 * If there's a lot of contention in the depot,
	 * increase the magazine size.
	 */
	(void) mutex_lock(&cp->cache_full.ml_lock);

	if (cp->cache_chunksize < cp->cache_magtype->mt_maxbuf &&
	    (int)(cp->cache_depot_contention -
	    cp->cache_depot_contention_prev) > umem_depot_contention)
		update_flags |= UMU_MAGAZINE_RESIZE;

	cp->cache_depot_contention_prev = cp->cache_depot_contention;

	(void) mutex_unlock(&cp->cache_full.ml_lock);

	/*
	 * Magazine size auto-tuning based on reload frequency.
	 * Only active when umem_magazine_tuning is enabled.
	 */
	if (unlikely(umem_magazine_tuning) && cp->cache_magtype != NULL) {
		/*
		 * Compute alloc_ops by summing per-CPU cc_alloc counters
		 * instead of using an atomic counter on the hot path.
		 */
		uint64_t allocs = 0;
		uint32_t ci;
		for (ci = 0; ci <= cp->cache_cpu_mask; ci++) {
			umem_cpu_cache_t *tc =
			    (umem_cpu_cache_t *)((char *)cp +
			    umem_cpus[ci].cpu_cache_offset);
			allocs += tc->cc_alloc;
		}
		cp->cache_alloc_ops = allocs;

		uint64_t reloads = cp->cache_mag_reloads;
		uint64_t reload_delta = reloads - cp->cache_mag_reloads_prev;
		uint64_t alloc_delta = allocs - cp->cache_alloc_ops_prev;

		cp->cache_mag_reloads_prev = reloads;
		cp->cache_alloc_ops_prev = allocs;

		if (alloc_delta > 100) {
			uint64_t reload_pct = (reload_delta * 100) /
			    alloc_delta;
			if (reload_pct > 15 &&
			    cp->cache_chunksize <
			    cp->cache_magtype->mt_maxbuf) {
				update_flags |= UMU_MAGAZINE_RESIZE;
			}
		}
	}

	if (update_flags)
		umem_add_update(cp, update_flags);

	/*
	 * Reclaim pages from idle slabs.
	 */
	if (umem_reclaim_enabled) {
		(void) mutex_lock(&cp->cache_lock);
		umem_cache_reclaim_pages(cp);
		(void) mutex_unlock(&cp->cache_lock);
	}
}

/*
 * Runs all pending updates.
 *
 * The update lock must be held on entrance, and will be held on exit.
 */
void
umem_process_updates(void)
{
	ASSERT(MUTEX_HELD(&umem_update_lock));

	while (umem_null_cache.cache_unext != &umem_null_cache) {
		int notify = 0;
		umem_cache_t *cp = umem_null_cache.cache_unext;

		cp->cache_uprev->cache_unext = cp->cache_unext;
		cp->cache_unext->cache_uprev = cp->cache_uprev;
		cp->cache_uprev = cp->cache_unext = NULL;

		ASSERT(!(cp->cache_uflags & UMU_ACTIVE));

		while (cp->cache_uflags) {
			int uflags = (cp->cache_uflags |= UMU_ACTIVE);
			(void) mutex_unlock(&umem_update_lock);

			/*
			 * The order here is important.  Each step can speed up
			 * later steps.
			 */

			if (uflags & UMU_HASH_RESCALE)
				umem_hash_rescale(cp);

			if (uflags & UMU_MAGAZINE_RESIZE)
				umem_cache_magazine_resize(cp);

			if (uflags & UMU_REAP)
				umem_cache_reap(cp);

			(void) mutex_lock(&umem_update_lock);

			/*
			 * check if anyone has requested notification
			 */
			if (cp->cache_uflags & UMU_NOTIFY) {
				uflags |= UMU_NOTIFY;
				notify = 1;
			}
			cp->cache_uflags &= ~uflags;
		}
		if (notify)
			(void) cond_broadcast(&umem_update_cv);
	}
}

#ifndef UMEM_STANDALONE
static void
umem_st_update(void)
{
	ASSERT(MUTEX_HELD(&umem_update_lock));
	ASSERT(umem_update_thr == 0 && umem_st_update_thr == 0);

	umem_st_update_thr = thr_self();

	(void) mutex_unlock(&umem_update_lock);

	vmem_update(NULL);
	umem_cache_applyall(umem_cache_update);

	(void) mutex_lock(&umem_update_lock);

	umem_process_updates(); /* does all of the requested work */

	umem_reap_next = gethrtime() +
	    (hrtime_t)umem_reap_interval * NANOSEC;

	umem_reaping = UMEM_REAP_DONE;

	umem_st_update_thr = 0;
}
#endif

/*
 * Reclaim all unused memory from all caches.  Called from vmem when memory
 * gets tight.  Must be called with no locks held.
 *
 * This just requests a reap on all caches, and notifies the update thread.
 */
void
umem_reap(void)
{
#ifndef UMEM_STANDALONE
	extern int __nthreads(void);
#endif

	/*
	 * A reap requested from inside an update pass is both redundant and
	 * fatal.  Redundant: the pass is already doing the reap work.  Fatal:
	 * umem_cache_applyall() holds umem_cache_lock across the whole walk,
	 * and umem_updateall() below takes umem_cache_lock -- which is not
	 * recursive, so the update thread deadlocks against itself.
	 *
	 * The path is real, not theoretical, and was observed hanging:
	 *
	 *   umem_cache_applyall(umem_cache_update)   [holds umem_cache_lock]
	 *     umem_cache_reclaim_pages -> umem_slab_destroy -> vmem_free
	 *       -> _umem_cache_free -> _umem_cache_alloc (vmem seg refill)
	 *         -> umem_slab_create -> vmem_alloc -> vmem_xalloc
	 *           -> vmem_reap -> umem_reap -> umem_updateall  [blocks here]
	 *
	 * It stayed latent because the rate limiter above usually returns
	 * early, so the inner reap normally never reaches umem_updateall().
	 *
	 * Dropping the reap is consistent with what this function already
	 * does when umem_reaping != UMEM_REAP_DONE or the interval has not
	 * elapsed: reaps are advisory and self-throttling, so the contract
	 * has always been "may be skipped", never "always performed".
	 */
	if (IN_UPDATE())
		return;

	if (umem_ready != UMEM_READY || umem_reaping != UMEM_REAP_DONE ||
	    gethrtime() < umem_reap_next)
		return;

	(void) mutex_lock(&umem_update_lock);

	if (umem_reaping != UMEM_REAP_DONE || gethrtime() < umem_reap_next) {
		(void) mutex_unlock(&umem_update_lock);
		return;
	}
	umem_reaping = UMEM_REAP_ADDING;        /* lock out other reaps */

	(void) mutex_unlock(&umem_update_lock);

	umem_updateall(UMU_REAP);

	(void) mutex_lock(&umem_update_lock);

	umem_reaping = UMEM_REAP_ACTIVE;

	/* Standalone is single-threaded */
#ifndef UMEM_STANDALONE
	if (umem_update_thr == 0) {
		/*
		 * The update thread does not exist.  If the process is
		 * multi-threaded, create it.  If not, or the creation fails,
		 * do the update processing inline.
		 */
		ASSERT(umem_st_update_thr == 0);

		if (__nthreads() <= 1 || umem_create_update_thread() == 0)
			umem_st_update();
	}

	(void) cond_broadcast(&umem_update_cv); /* wake up the update thread */
#endif

	(void) mutex_unlock(&umem_update_lock);
}

umem_cache_t *
umem_cache_create(
	char *name,             /* descriptive name for this cache */
	size_t bufsize,         /* size of the objects it manages */
	size_t align,           /* required object alignment */
	umem_constructor_t *constructor, /* object constructor */
	umem_destructor_t *destructor, /* object destructor */
	umem_reclaim_t *reclaim, /* memory reclaim callback */
	void *private,          /* pass-thru arg for constr/destr/reclaim */
	vmem_t *vmp,            /* vmem source for slab allocation */
	int cflags)             /* cache creation flags */
{
	int cpu_seqid;
	size_t chunksize;
	umem_cache_t *cp, *cnext, *cprev;
	umem_magtype_t *mtp;
	size_t csize;
	size_t phase;

	/*
	 * The init thread is allowed to create internal and quantum caches.
	 *
	 * Other threads must wait until until initialization is complete.
	 */
	if (umem_init_thr == thr_self())
		ASSERT((cflags & (UMC_INTERNAL | UMC_QCACHE)) != 0);
	else {
		ASSERT(!(cflags & UMC_INTERNAL));
		if (umem_ready != UMEM_READY && umem_init() == 0) {
			errno = EAGAIN;
			return (NULL);
		}
	}

	csize = UMEM_CACHE_SIZE(umem_max_ncpus);
	phase = P2NPHASE(csize, UMEM_CPU_CACHE_SIZE);

	if (vmp == NULL)
		vmp = umem_default_arena;

	ASSERT(P2PHASE(phase, UMEM_ALIGN) == 0);

	/*
	 * Check that the arguments are reasonable
	 */
	if ((align & (align - 1)) != 0 || align > vmp->vm_quantum ||
	    ((cflags & UMC_NOHASH) && (cflags & UMC_NOTOUCH)) ||
	    name == NULL || bufsize == 0) {
		errno = EINVAL;
		return (NULL);
	}

	/*
	 * If align == 0, we set it to the minimum required alignment.
	 *
	 * If align < UMEM_ALIGN, we round it up to UMEM_ALIGN, unless
	 * UMC_NOTOUCH was passed.
	 */
	if (align == 0) {
		if (P2ROUNDUP(bufsize, UMEM_ALIGN) >= UMEM_SECOND_ALIGN)
			align = UMEM_SECOND_ALIGN;
		else
			align = UMEM_ALIGN;
	} else if (align < UMEM_ALIGN && (cflags & UMC_NOTOUCH) == 0)
		align = UMEM_ALIGN;


	/*
	 * Get a umem_cache structure.  We arrange that cp->cache_cpu[]
	 * is aligned on a UMEM_CPU_CACHE_SIZE boundary to prevent
	 * false sharing of per-CPU data.
	 */
	cp = vmem_xalloc(umem_cache_arena, csize, UMEM_CPU_CACHE_SIZE, phase,
	    0, NULL, NULL, VM_NOSLEEP);

	if (unlikely(cp == NULL)) {
		errno = EAGAIN;
		return (NULL);
	}

	bzero(cp, csize);

	(void) mutex_lock(&umem_flags_lock);
	if (unlikely(umem_flags & UMF_RANDOMIZE))
		umem_flags = (((umem_flags | ~UMF_RANDOM) + 1) & UMF_RANDOM) |
		    UMF_RANDOMIZE;
	cp->cache_flags = umem_flags | (cflags & UMF_DEBUG);
	(void) mutex_unlock(&umem_flags_lock);

	/*
	 * Make sure all the various flags are reasonable.
	 */
	if (unlikely(cp->cache_flags & UMF_LITE)) {
		if (bufsize >= umem_lite_minsize &&
		    align <= umem_lite_maxalign &&
		    P2PHASE(bufsize, umem_lite_maxalign) != 0) {
			cp->cache_flags |= UMF_BUFTAG;
			cp->cache_flags &= ~(UMF_AUDIT | UMF_FIREWALL);
		} else {
			cp->cache_flags &= ~UMF_DEBUG;
		}
	}

	if ((cflags & UMC_QCACHE) && unlikely(cp->cache_flags & UMF_AUDIT))
		cp->cache_flags |= UMF_NOMAGAZINE;

	if (cflags & UMC_NODEBUG)
		cp->cache_flags &= ~UMF_DEBUG;

	if (cflags & UMC_NOTOUCH)
		cp->cache_flags &= ~UMF_TOUCH;

	if (cflags & UMC_NOHASH)
		cp->cache_flags &= ~(UMF_AUDIT | UMF_FIREWALL);

	if (cflags & UMC_NOMAGAZINE)
		cp->cache_flags |= UMF_NOMAGAZINE;

	if (unlikely(cp->cache_flags & UMF_AUDIT) && !(cflags & UMC_NOTOUCH))
		cp->cache_flags |= UMF_REDZONE;

	if (unlikely(cp->cache_flags & UMF_BUFTAG) && bufsize >= umem_minfirewall &&
	    !unlikely(cp->cache_flags & UMF_LITE) && !(cflags & UMC_NOHASH))
		cp->cache_flags |= UMF_FIREWALL;

	if (unlikely(vmp != umem_default_arena || umem_firewall_arena == NULL))
		cp->cache_flags &= ~UMF_FIREWALL;

	if (unlikely(cp->cache_flags & UMF_FIREWALL)) {
		cp->cache_flags &= ~UMF_BUFTAG;
		cp->cache_flags |= UMF_NOMAGAZINE;
		ASSERT(vmp == umem_default_arena);
		vmp = umem_firewall_arena;
	}

	/*
	 * Set cache properties.
	 */
	(void) strncpy(cp->cache_name, name, sizeof (cp->cache_name) - 1);
	cp->cache_bufsize = bufsize;
	cp->cache_align = align;
	cp->cache_constructor = constructor;
	cp->cache_destructor = destructor;
	cp->cache_reclaim = reclaim;
	cp->cache_private = private;
	cp->cache_arena = vmp;
	cp->cache_cflags = cflags;
	cp->cache_cpu_mask = umem_cpu_mask;

	/*
	 * Determine the chunk size.
	 */
	chunksize = bufsize;

	if (align >= UMEM_ALIGN) {
		chunksize = P2ROUNDUP(chunksize, UMEM_ALIGN);
		cp->cache_bufctl = chunksize - UMEM_ALIGN;
	}

	if (unlikely(cp->cache_flags & UMF_BUFTAG)) {
		cp->cache_bufctl = chunksize;
		cp->cache_buftag = chunksize;
		chunksize += sizeof (umem_buftag_t);
	}

	if (unlikely(cp->cache_flags & UMF_DEADBEEF)) {
		cp->cache_verify = MIN(cp->cache_buftag, umem_maxverify);
		if (unlikely(cp->cache_flags & UMF_LITE))
			cp->cache_verify = MIN(cp->cache_verify, UMEM_ALIGN);
	}

	cp->cache_contents = MIN(cp->cache_bufctl, umem_content_maxsave);

	cp->cache_chunksize = chunksize = P2ROUNDUP(chunksize, align);

	if (chunksize < bufsize) {
		errno = ENOMEM;
		goto fail;
	}

	/*
	 * Now that we know the chunk size, determine the optimal slab size.
	 */
	if (vmp == umem_firewall_arena) {
		cp->cache_slabsize = P2ROUNDUP(chunksize, vmp->vm_quantum);
		cp->cache_mincolor = cp->cache_slabsize - chunksize;
		cp->cache_maxcolor = cp->cache_mincolor;
		cp->cache_flags |= UMF_HASH;
		ASSERT(!(cp->cache_flags & UMF_BUFTAG));
	} else if ((cflags & UMC_NOHASH) || (!(cflags & UMC_NOTOUCH) &&
	    !unlikely(cp->cache_flags & UMF_AUDIT) &&
	    chunksize < vmp->vm_quantum / UMEM_VOID_FRACTION)) {
		cp->cache_slabsize = vmp->vm_quantum;
		cp->cache_mincolor = 0;
		cp->cache_maxcolor =
		    (cp->cache_slabsize - sizeof (umem_slab_t)) % chunksize;

		if (chunksize + sizeof (umem_slab_t) > cp->cache_slabsize) {
			errno = EINVAL;
			goto fail;
		}
		ASSERT(!(cp->cache_flags & UMF_AUDIT));
	} else {
		size_t chunks, bestfit, waste, slabsize;
		size_t minwaste = LONG_MAX;

		for (chunks = 1; chunks <= UMEM_VOID_FRACTION; chunks++) {
			slabsize = P2ROUNDUP(chunksize * chunks,
			    vmp->vm_quantum);
			/*
			 * check for overflow
			 */
			if ((slabsize / chunks) < chunksize) {
				errno = ENOMEM;
				goto fail;
			}
			chunks = slabsize / chunksize;
			waste = (slabsize % chunksize) / chunks;
			if (waste < minwaste) {
				minwaste = waste;
				bestfit = slabsize;
			}
		}
		if (cflags & UMC_QCACHE) {
			bestfit = MAX(1 << highbit(3 * vmp->vm_qcache_max), 64);
			/*
			 * Second half of the heap-ceiling fix: see
			 * UMEM_MIN_QCACHE_SLAB in umem_impl.h.  Small (<= 512 B)
			 * objects' one-page slabs are served through umem_va's
			 * qcache, whose 128 KiB slabs each cost a VMA; 4 MiB slabs
			 * cost 57x fewer at zero measured RSS cost.
			 */
			if (bestfit < UMEM_MIN_QCACHE_SLAB)
				bestfit = UMEM_MIN_QCACHE_SLAB;
		}

		/*
		 * Span-density floor (see UMEM_MIN_SLAB_OBJECTS in umem_impl.h).
		 * The best-fit loop above minimises waste per object but is blind
		 * to how many objects share a span.  With a page-sized quantum it
		 * happily picks one 4 KiB object per 4 KiB slab, and every such slab
		 * then costs a kernel VMA -- the ~5 GB Linux heap ceiling.  Raise
		 * the slab to hold at least UMEM_MIN_SLAB_OBJECTS objects unless
		 * that would exceed UMEM_MIN_SLAB_CEILING; large objects stay
		 * one-per-slab as they always did.  QCACHE slabs are sized by the
		 * arena and are left alone.
		 */
		if (!(cflags & UMC_QCACHE) &&
		    bestfit / chunksize < UMEM_MIN_SLAB_OBJECTS) {
			size_t want = P2ROUNDUP(chunksize * UMEM_MIN_SLAB_OBJECTS,
			    vmp->vm_quantum);

			if (want / chunksize >= UMEM_MIN_SLAB_OBJECTS &&
			    want <= UMEM_MIN_SLAB_CEILING)
				bestfit = want;
		}
		cp->cache_slabsize = bestfit;
		cp->cache_mincolor = 0;
		cp->cache_maxcolor = bestfit % chunksize;
		cp->cache_flags |= UMF_HASH;
	}

	if (unlikely(cp->cache_flags & UMF_HASH)) {
		ASSERT(!(cflags & UMC_NOHASH));
		cp->cache_bufctl_cache = unlikely(cp->cache_flags & UMF_AUDIT) ?
		    umem_bufctl_audit_cache : umem_bufctl_cache;
	}

	if (cp->cache_maxcolor >= vmp->vm_quantum)
		cp->cache_maxcolor = vmp->vm_quantum - 1;

	cp->cache_color = cp->cache_mincolor;

	/*
	 * Initialize the rest of the slab layer.
	 */
	(void) mutex_init(&cp->cache_lock, USYNC_THREAD, NULL);

	cp->cache_freelist = &cp->cache_nullslab;
	cp->cache_nullslab.slab_cache = cp;
	cp->cache_nullslab.slab_refcnt = -1;
	cp->cache_nullslab.slab_next = &cp->cache_nullslab;
	cp->cache_nullslab.slab_prev = &cp->cache_nullslab;

	if (unlikely(cp->cache_flags & UMF_HASH)) {
		cp->cache_hash_table = vmem_alloc(umem_hash_arena,
		    UMEM_HASH_INITIAL * sizeof (void *), VM_NOSLEEP);
		if (cp->cache_hash_table == NULL) {
			errno = EAGAIN;
			goto fail_lock;
		}
		bzero(cp->cache_hash_table,
		    UMEM_HASH_INITIAL * sizeof (void *));
		cp->cache_hash_mask = UMEM_HASH_INITIAL - 1;
		cp->cache_hash_shift = highbit((ulong_t)chunksize) - 1;
	}

	/*
	 * Initialize the depot magazine lists.
	 */
	(void) mutex_init(&cp->cache_full.ml_lock, USYNC_THREAD, NULL);
	cp->cache_full.ml_list = NULL;
	cp->cache_full.ml_total = 0;
	cp->cache_full.ml_min = 0;
	cp->cache_full.ml_reaplimit = 0;
	cp->cache_full.ml_alloc = 0;

	(void) mutex_init(&cp->cache_empty.ml_lock, USYNC_THREAD, NULL);
	cp->cache_empty.ml_list = NULL;
	cp->cache_empty.ml_total = 0;
	cp->cache_empty.ml_min = 0;
	cp->cache_empty.ml_reaplimit = 0;
	cp->cache_empty.ml_alloc = 0;

	for (mtp = umem_magtype; chunksize <= mtp->mt_minbuf; mtp++)
		continue;

	cp->cache_magtype = mtp;

	/*
	 * Per-CPU arrays: the two depot lists and, with rseq, the rseq
	 * caches -- carved from ONE mapping (P6.4; see cache_percpu_map in
	 * umem_impl.h).  Each array is a whole number of cache lines
	 * (asserted at file scope), so laying them end to end keeps every
	 * element 64-byte aligned.  ncpus is a power of 2 (rounded in
	 * umem_init).
	 */
	{
		int ncpus = (int)umem_max_ncpus;
		size_t depot_bytes = (size_t)ncpus * sizeof (umem_maglist_t);
		size_t rseq_bytes = 0;
		size_t len;
		char *base;
#ifdef UMEM_RSEQ_AVAILABLE
		int rseq_ncpus = umem_rseq_enabled ? umem_rseq_get_ncpus() : 0;
		rseq_bytes = (size_t)rseq_ncpus * sizeof (umem_rseq_cache_t);
#endif
		len = 2 * depot_bytes + rseq_bytes;
		/*
		 * From umem_cache_arena, like the descriptor itself, NOT a
		 * private mmap.  The first version of this consolidation
		 * (e00fdf2) used one mmap() per cache and got the footprint,
		 * but destroy then munmap()ed a 4 KiB hole between two heap
		 * pages -- mmap(NULL) hands out addresses top-down, so the
		 * per-cache mapping landed between consecutive heap growths
		 * -- and 2,000 destroys left 940 orphaned single-page heap
		 * VMAs.  vmem memory is never unmapped; a freed block is
		 * reused.  This is also how the original Solaris code did it:
		 * umem_cache_create() there has no mmap() in it at all.
		 */
		base = vmem_xalloc(umem_cache_arena, len,
		    UMEM_CACHE_LINE_SIZE, 0, 0, NULL, NULL, VM_NOSLEEP);

		cp->cache_depot_full = NULL;
		cp->cache_depot_empty = NULL;
		cp->cache_depot_ncpus = 0;
#ifdef UMEM_RSEQ_AVAILABLE
		cp->cache_rseq = NULL;
#endif
		if (base == NULL) {
			cp->cache_percpu_map = NULL;
			cp->cache_percpu_len = 0;
		} else {
			bzero(base, len);
			int i;

			cp->cache_percpu_map = base;
			cp->cache_percpu_len = len;
			cp->cache_depot_ncpus = ncpus;
			cp->cache_depot_full = (umem_maglist_t *)base;
			cp->cache_depot_empty =
			    (umem_maglist_t *)(base + depot_bytes);
			for (i = 0; i < ncpus; i++) {
				(void) mutex_init(
				    &cp->cache_depot_full[i].ml_lock,
				    USYNC_THREAD, NULL);
				(void) mutex_init(
				    &cp->cache_depot_empty[i].ml_lock,
				    USYNC_THREAD, NULL);
			}
#ifdef UMEM_RSEQ_AVAILABLE
			if (rseq_bytes != 0) {
				int magsize = cp->cache_magtype->mt_magsize;
				cp->cache_rseq = (umem_rseq_cache_t *)
				    (base + 2 * depot_bytes);
				for (i = 0; i < rseq_ncpus; i++)
					cp->cache_rseq[i].magsize = magsize;
			}
#endif
		}
	}

	/*
	 * Initialize the CPU layer.
	 */
	for (cpu_seqid = 0; cpu_seqid < umem_max_ncpus; cpu_seqid++) {
		umem_cpu_cache_t *ccp = &cp->cache_cpu[cpu_seqid];
		(void) mutex_init(&ccp->cc_lock, USYNC_THREAD, NULL);
		ccp->cc_flags = cp->cache_flags;
		ccp->cc_rounds = -1;
		ccp->cc_prounds = -1;
	}

	/*
	 * Add the cache to the global list.  This makes it visible
	 * to umem_update(), so the cache must be ready for business.
	 */
	(void) mutex_lock(&umem_cache_lock);
	cp->cache_next = cnext = &umem_null_cache;
	cp->cache_prev = cprev = umem_null_cache.cache_prev;
	cnext->cache_prev = cp;
	cprev->cache_next = cp;
	(void) mutex_unlock(&umem_cache_lock);

	if (umem_ready == UMEM_READY)
		umem_cache_magazine_enable(cp);

	return (cp);

fail_lock:
	(void) mutex_destroy(&cp->cache_lock);
fail:
	vmem_xfree(umem_cache_arena, cp, csize);
	return (NULL);
}

void
umem_cache_destroy(umem_cache_t *cp)
{
	int cpu_seqid;

	/*
	 * Remove the cache from the global cache list so that no new updates
	 * will be scheduled on its behalf, wait for any pending tasks to
	 * complete, purge the cache, and then destroy it.
	 */
	(void) mutex_lock(&umem_cache_lock);
	cp->cache_prev->cache_next = cp->cache_next;
	cp->cache_next->cache_prev = cp->cache_prev;
	cp->cache_prev = cp->cache_next = NULL;
	(void) mutex_unlock(&umem_cache_lock);

	umem_remove_updates(cp);

#ifdef UMEM_RSEQ_AVAILABLE
	/*
	 * Return the rounds held in the rseq per-CPU loaded magazines to the
	 * slab layer BEFORE draining slabs, so those buffers stop pinning
	 * their slabs.  Destroy each magazine with its own round count:
	 * handing it to the depot as "full" (rounds > 0) or "empty" instead,
	 * as this code used to, mislabels a partially filled magazine, and
	 * after the purge below nothing reaps the depot anyway, so the
	 * magazine and its rounds were simply abandoned.
	 */
	if (cp->cache_rseq != NULL) {
		int ncpus = umem_rseq_get_ncpus();
		int i;
		for (i = 0; i < ncpus; i++) {
			umem_rseq_cache_t *rc = &cp->cache_rseq[i];
			umem_magazine_t *mag =
			    (umem_magazine_t *)rc->loaded_mag;
			int rounds = rc->rounds;

			rc->loaded_mag = NULL;
			rc->rounds = 0;
			if (mag != NULL)
				umem_magazine_destroy(cp, mag, rounds);
		}
	}
#endif

	umem_cache_magazine_purge(cp);

	/*
	 * Every cached buffer has now returned to the slab layer, so the
	 * retained empty slabs background reclamation left behind can be
	 * destroyed.  This must happen before the cache descriptor is freed:
	 * umem_slab_destroy() reads cache_arena, cache_slabsize, cache_flags
	 * and cache_bufctl_cache out of it.
	 */
	umem_cache_drain_slabs(cp);

	(void) mutex_lock(&cp->cache_lock);
	if (cp->cache_buftotal != 0)
		log_message("umem_cache_destroy: '%s' (%p) not empty\n",
		    cp->cache_name, (void *)cp);
	cp->cache_reclaim = NULL;
	/*
	 * The cache is now dead.  There should be no further activity.
	 * We enforce this by setting land mines in the constructor and
	 * destructor routines that induce a segmentation fault if invoked.
	 */
	cp->cache_constructor = (umem_constructor_t *)1;
	cp->cache_destructor = (umem_destructor_t *)2;
	(void) mutex_unlock(&cp->cache_lock);

	if (cp->cache_hash_table != NULL)
		vmem_free(umem_hash_arena, cp->cache_hash_table,
		    (cp->cache_hash_mask + 1) * sizeof (void *));

	for (cpu_seqid = 0; cpu_seqid < umem_max_ncpus; cpu_seqid++)
		(void) mutex_destroy(&cp->cache_cpu[cpu_seqid].cc_lock);

	(void) mutex_destroy(&cp->cache_full.ml_lock);
	(void) mutex_destroy(&cp->cache_empty.ml_lock);

	if (cp->cache_depot_ncpus > 0) {
		int i;
		for (i = 0; i < cp->cache_depot_ncpus; i++) {
			(void) mutex_destroy(
			    &cp->cache_depot_full[i].ml_lock);
			(void) mutex_destroy(
			    &cp->cache_depot_empty[i].ml_lock);
		}
	}
	/* Depot arrays and rseq caches together: one block, one free. */
	if (cp->cache_percpu_map != NULL) {
		vmem_xfree(umem_cache_arena, cp->cache_percpu_map,
		    cp->cache_percpu_len);
		cp->cache_percpu_map = NULL;
		cp->cache_depot_full = NULL;
		cp->cache_depot_empty = NULL;
#ifdef UMEM_RSEQ_AVAILABLE
		cp->cache_rseq = NULL;
#endif
	}

	(void) mutex_destroy(&cp->cache_lock);

	vmem_free(umem_cache_arena, cp, UMEM_CACHE_SIZE(umem_max_ncpus));
}

void
umem_alloc_sizes_clear(void)
{
	int i;

	umem_alloc_sizes[0] = UMEM_MAXBUF;
	for (i = 1; i < NUM_ALLOC_SIZES; i++)
		umem_alloc_sizes[i] = 0;
}

void
umem_alloc_sizes_add(size_t size_arg)
{
	int i, j;
	size_t size = size_arg;

	if (size == 0) {
		log_message("size_add: cannot add zero-sized cache\n",
		    size, UMEM_MAXBUF);
		return;
	}

	if (size > UMEM_MAXBUF) {
		log_message("size_add: %ld > %d, cannot add\n", size,
		    UMEM_MAXBUF);
		return;
	}

	if (umem_alloc_sizes[NUM_ALLOC_SIZES - 1] != 0) {
		log_message("size_add: no space in alloc_table for %d\n",
		    size);
		return;
	}

	if (P2PHASE(size, UMEM_ALIGN) != 0) {
		size = P2ROUNDUP(size, UMEM_ALIGN);
		log_message("size_add: rounding %d up to %d\n", size_arg,
		    size);
	}

	for (i = 0; i < NUM_ALLOC_SIZES; i++) {
		int cur = umem_alloc_sizes[i];
		if (cur == size) {
			log_message("size_add: %ld already in table\n",
			    size);
			return;
		}
		if (cur > size)
			break;
	}

	for (j = NUM_ALLOC_SIZES - 1; j > i; j--)
		umem_alloc_sizes[j] = umem_alloc_sizes[j-1];
	umem_alloc_sizes[i] = size;
}

void
umem_alloc_sizes_remove(size_t size)
{
	int i;

	if (size == UMEM_MAXBUF) {
		log_message("size_remove: cannot remove %ld\n", size);
		return;
	}

	for (i = 0; i < NUM_ALLOC_SIZES; i++) {
		int cur = umem_alloc_sizes[i];
		if (cur == size)
			break;
		else if (cur > size || cur == 0) {
			log_message("size_remove: %ld not found in table\n",
			    size);
			return;
		}
	}

	for (; i + 1 < NUM_ALLOC_SIZES; i++)
		umem_alloc_sizes[i] = umem_alloc_sizes[i+1];
	umem_alloc_sizes[i] = 0;
}

static int
umem_cache_init(void)
{
	int i;
	size_t size, max_size;
	umem_cache_t *cp;
	umem_magtype_t *mtp;
	char name[UMEM_CACHE_NAMELEN + 1];
	umem_cache_t *umem_alloc_caches[NUM_ALLOC_SIZES];

	for (i = 0; i < sizeof (umem_magtype) / sizeof (*mtp); i++) {
		mtp = &umem_magtype[i];
		(void) snprintf(name, sizeof (name), "umem_magazine_%d",
		    mtp->mt_magsize);
		mtp->mt_cache = umem_cache_create(name,
		    (mtp->mt_magsize + 1) * sizeof (void *),
		    mtp->mt_align, NULL, NULL, NULL, NULL,
		    umem_internal_arena, UMC_NOHASH | UMC_INTERNAL);
		if (mtp->mt_cache == NULL)
			return (0);
	}

	umem_slab_cache = umem_cache_create("umem_slab_cache",
	    sizeof (umem_slab_t), 0, NULL, NULL, NULL, NULL,
	    umem_internal_arena, UMC_NOHASH | UMC_INTERNAL);

	if (umem_slab_cache == NULL)
		return (0);

	umem_bufctl_cache = umem_cache_create("umem_bufctl_cache",
	    sizeof (umem_bufctl_t), 0, NULL, NULL, NULL, NULL,
	    umem_internal_arena, UMC_NOHASH | UMC_INTERNAL);

	if (umem_bufctl_cache == NULL)
		return (0);

	/*
	 * The size of the umem_bufctl_audit structure depends upon
	 * umem_stack_depth.   See umem_impl.h for details on the size
	 * restrictions.
	 */

	size = UMEM_BUFCTL_AUDIT_SIZE_DEPTH(umem_stack_depth);
	max_size = UMEM_BUFCTL_AUDIT_MAX_SIZE;

	if (size > max_size) {                  /* too large -- truncate */
		int max_frames = UMEM_MAX_STACK_DEPTH;

		ASSERT(UMEM_BUFCTL_AUDIT_SIZE_DEPTH(max_frames) <= max_size);

		umem_stack_depth = max_frames;
		size = UMEM_BUFCTL_AUDIT_SIZE_DEPTH(umem_stack_depth);
	}

	umem_bufctl_audit_cache = umem_cache_create("umem_bufctl_audit_cache",
	    size, 0, NULL, NULL, NULL, NULL, umem_internal_arena,
	    UMC_NOHASH | UMC_INTERNAL);

	if (umem_bufctl_audit_cache == NULL)
		return (0);

	if (vmem_backend & VMEM_BACKEND_MMAP)
		umem_va_arena = vmem_create("umem_va",
		    NULL, 0, pagesize,
		    vmem_alloc, vmem_free, heap_arena,
		    8 * pagesize, VM_NOSLEEP);
	else
		umem_va_arena = heap_arena;

	if (unlikely(umem_va_arena == NULL))
		return (0);

	umem_default_arena = vmem_create("umem_default",
	    NULL, 0, pagesize,
	    heap_alloc, heap_free, umem_va_arena,
	    0, VM_NOSLEEP);

	if (unlikely(umem_default_arena == NULL))
		return (0);

	/*
	 * make sure the umem_alloc table initializer is correct
	 */
	i = sizeof (umem_alloc_table) / sizeof (*umem_alloc_table);
	ASSERT(umem_alloc_table[i - 1] == &umem_null_cache);

	/*
	 * Create the default caches to back umem_alloc()
	 */
	for (i = 0; i < NUM_ALLOC_SIZES; i++) {
		size_t cache_size = umem_alloc_sizes[i];
		size_t align = 0;

		if (cache_size == 0)
			break;          /* 0 terminates the list */

		/*
		 * If they allocate a multiple of the coherency granularity,
		 * they get a coherency-granularity-aligned address.
		 */
		if (IS_P2ALIGNED(cache_size, 64))
			align = 64;
		if (IS_P2ALIGNED(cache_size, pagesize))
			align = pagesize;
		(void) snprintf(name, sizeof (name), "umem_alloc_%lu",
		    (long)cache_size);

		cp = umem_cache_create(name, cache_size, align,
		    NULL, NULL, NULL, NULL, NULL, UMC_INTERNAL);
		if (unlikely(cp == NULL))
			return (0);

		umem_alloc_caches[i] = cp;
	}

	/*
	 * Initialization cannot fail at this point.  Make the caches
	 * visible to umem_alloc() and friends.
	 */
	size = UMEM_ALIGN;
	for (i = 0; i < NUM_ALLOC_SIZES; i++) {
		size_t cache_size = umem_alloc_sizes[i];

		if (cache_size == 0)
			break;          /* 0 terminates the list */

		cp = umem_alloc_caches[i];

		while (size <= cache_size) {
			umem_alloc_table[(size - 1) >> UMEM_ALIGN_SHIFT] = cp;
			size += UMEM_ALIGN;
		}
	}
	ASSERT(size - UMEM_ALIGN == UMEM_MAXBUF);
	return (1);
}

/*
 * umem_startup() is called early on, and must be called explicitly if we're
 * the standalone version.
 */
#ifdef UMEM_STANDALONE
void
#else
/* #pragma init(umem_startup) -- handled via __attribute__((constructor)) */
static void
#endif
umem_startup(caddr_t start, size_t len, size_t pagesize, caddr_t minstack,
	caddr_t maxstack)
{
#ifdef UMEM_STANDALONE
	int idx;
	/* Standalone doesn't fork */
#else
	/* register the fork handler */
	(void) pthread_once(&umem_forkhandler_once, umem_forkhandler_init);
#endif

#ifdef __lint
	/* make lint happy */
	minstack = maxstack;
#endif
#ifndef UMEM_STANDALONE
	(void) minstack;
	(void) maxstack;
#endif

#ifdef UMEM_STANDALONE
	umem_ready = UMEM_READY_STARTUP;
	umem_init_env_ready = 0;

	umem_min_stack = minstack;
	umem_max_stack = maxstack;

	nofail_callback = NULL;
	umem_slab_cache = NULL;
	umem_bufctl_cache = NULL;
	umem_bufctl_audit_cache = NULL;
	heap_arena = NULL;
	heap_alloc = NULL;
	heap_free = NULL;
	umem_internal_arena = NULL;
	umem_cache_arena = NULL;
	umem_hash_arena = NULL;
	umem_log_arena = NULL;
	umem_oversize_arena = NULL;
	umem_va_arena = NULL;
	umem_default_arena = NULL;
	umem_firewall_va_arena = NULL;
	umem_firewall_arena = NULL;
	umem_memalign_arena = NULL;
	umem_transaction_log = NULL;
	umem_content_log = NULL;
	umem_failure_log = NULL;
	umem_slab_log = NULL;
	umem_cpu_mask = 0;

	umem_cpus = &umem_startup_cpu;
	umem_startup_cpu.cpu_cache_offset = UMEM_CACHE_SIZE(0);
	umem_startup_cpu.cpu_number = 0;

	bcopy(&umem_null_cache_template, &umem_null_cache,
	    sizeof (umem_cache_t));

	for (idx = 0; idx < (UMEM_MAXBUF >> UMEM_ALIGN_SHIFT); idx++)
		umem_alloc_table[idx] = &umem_null_cache;
#endif

	/*
	 * Perform initialization specific to the way we've been compiled
	 * (library or standalone)
	 */
	umem_type_init(start, len, pagesize);

	vmem_startup();
}

int
umem_init(void)
{
	size_t maxverify, minfirewall;
	size_t size;
	int idx;
	umem_cpu_t *new_cpus;

	vmem_t *memalign_arena, *oversize_arena;

	if (thr_self() != umem_init_thr) {
		/*
		 * The usual case -- non-recursive invocation of umem_init().
		 */
		(void) mutex_lock(&umem_init_lock);
		if (umem_ready != UMEM_READY_STARTUP) {
			/*
			 * someone else beat us to initializing umem.  Wait
			 * for them to complete, then return.
			 */
			while (umem_ready == UMEM_READY_INITING) {
				int cancel_state;

				(void) pthread_setcancelstate(
				    PTHREAD_CANCEL_DISABLE, &cancel_state);
				(void) cond_wait(&umem_init_cv,
				    &umem_init_lock);
				(void) pthread_setcancelstate(
				    cancel_state, NULL);
			}
			ASSERT(umem_ready == UMEM_READY ||
			    umem_ready == UMEM_READY_INIT_FAILED);
			(void) mutex_unlock(&umem_init_lock);
			return (umem_ready == UMEM_READY);
		}

		ASSERT(umem_ready == UMEM_READY_STARTUP);
		ASSERT(umem_init_env_ready == 0);

		umem_ready = UMEM_READY_INITING;
		umem_init_thr = thr_self();

		(void) mutex_unlock(&umem_init_lock);
		umem_setup_envvars(0);          /* can recurse -- see below */
		if (umem_init_env_ready) {
			/*
			 * initialization was completed already
			 */
			ASSERT(umem_ready == UMEM_READY ||
			    umem_ready == UMEM_READY_INIT_FAILED);
			ASSERT(umem_init_thr == 0);
			return (umem_ready == UMEM_READY);
		}
	} else if (!umem_init_env_ready) {
		/*
		 * The umem_setup_envvars() call (above) makes calls into
		 * the dynamic linker and directly into user-supplied code.
		 * Since we cannot know what that code will do, we could be
		 * recursively invoked (by, say, a malloc() call in the code
		 * itself, or in a (C++) _init section it causes to be fired).
		 *
		 * This code is where we end up if such recursion occurs.  We
		 * first clean up any partial results in the envvar code, then
		 * proceed to finish initialization processing in the recursive
		 * call.  The original call will notice this, and return
		 * immediately.
		 */
		umem_setup_envvars(1);          /* clean up any partial state */
	} else {
		umem_panic(
		    "recursive allocation while initializing umem\n");
	}
	umem_init_env_ready = 1;

	/*
	 * From this point until we finish, recursion into umem_init() will
	 * cause a umem_panic().
	 */
	maxverify = minfirewall = ULONG_MAX;

	/* LINTED constant condition */
	if (sizeof (umem_cpu_cache_t) != UMEM_CPU_CACHE_SIZE) {
		umem_panic("sizeof (umem_cpu_cache_t) = %d, should be %d\n",
		    sizeof (umem_cpu_cache_t), UMEM_CPU_CACHE_SIZE);
	}

	umem_max_ncpus = umem_get_max_ncpus();

	/*
	 * load tunables from environment
	 */
	umem_process_envvars();

	if (issetugid())
		umem_mtbf = 0;

#ifdef UMEM_NUMA_AVAILABLE
	/*
	 * Initialize NUMA support if available
	 */
	if (umem_numa_init() == 0) {
		/* NUMA successfully initialized */
		if (umem_numa_enabled) {
			(void) fprintf(stderr,
			    "umem: NUMA-aware allocation enabled "
			    "(%d nodes detected)\n",
			    umem_numa_topo ? umem_numa_topo->num_nodes : 0);
		}
	}
#endif

#ifdef UMEM_RSEQ_AVAILABLE
	/*
	 * Try to enable rseq-based per-CPU fast path.
	 * If the kernel supports rseq (Linux 4.18+), this gives us
	 * true lock-free per-CPU magazine access with zero
	 * synchronization overhead. Falls back gracefully if
	 * rseq is not available.
	 */
	(void) umem_rseq_init();
#endif

	/*
	 * set up vmem
	 */
	if (unlikely(!(umem_flags & UMF_AUDIT)))
		vmem_no_debug();

	heap_arena = vmem_heap_arena(&heap_alloc, &heap_free);

	pagesize = heap_arena->vm_quantum;

	umem_internal_arena = vmem_create("umem_internal", NULL, 0, pagesize,
	    heap_alloc, heap_free, heap_arena, 0, VM_NOSLEEP);

	umem_default_arena = umem_internal_arena;

	if (unlikely(umem_internal_arena == NULL))
		goto fail;

	umem_cache_arena = vmem_create("umem_cache", NULL, 0, UMEM_ALIGN,
	    vmem_alloc, vmem_free, umem_internal_arena, 0, VM_NOSLEEP);

	umem_hash_arena = vmem_create("umem_hash", NULL, 0, UMEM_ALIGN,
	    vmem_alloc, vmem_free, umem_internal_arena, 0, VM_NOSLEEP);

	umem_log_arena = vmem_create("umem_log", NULL, 0, UMEM_ALIGN,
	    heap_alloc, heap_free, heap_arena, 0, VM_NOSLEEP);

	umem_firewall_va_arena = vmem_create("umem_firewall_va",
	    NULL, 0, pagesize,
	    umem_firewall_va_alloc, umem_firewall_va_free, heap_arena,
	    0, VM_NOSLEEP);

	if (unlikely(umem_cache_arena == NULL || umem_hash_arena == NULL ||
	    umem_log_arena == NULL || umem_firewall_va_arena == NULL))
		goto fail;

	umem_firewall_arena = vmem_create("umem_firewall", NULL, 0, pagesize,
	    heap_alloc, heap_free, umem_firewall_va_arena, 0,
	    VM_NOSLEEP);

	if (unlikely(umem_firewall_arena == NULL))
		goto fail;

	oversize_arena = vmem_create("umem_oversize", NULL, 0, pagesize,
	    heap_alloc, heap_free, minfirewall < ULONG_MAX ?
	    umem_firewall_va_arena : heap_arena, 0, VM_NOSLEEP);

	memalign_arena = vmem_create("umem_memalign", NULL, 0, UMEM_ALIGN,
	    heap_alloc, heap_free, minfirewall < ULONG_MAX ?
	    umem_firewall_va_arena : heap_arena, 0, VM_NOSLEEP);

	if (unlikely(oversize_arena == NULL || memalign_arena == NULL))
		goto fail;

	if (umem_max_ncpus > CPUHINT_MAX())
		umem_max_ncpus = CPUHINT_MAX();

	while ((umem_max_ncpus & (umem_max_ncpus - 1)) != 0)
		umem_max_ncpus++;

	if (umem_max_ncpus == 0)
		umem_max_ncpus = 1;

	size = umem_max_ncpus * sizeof (umem_cpu_t);
	new_cpus = vmem_alloc(umem_internal_arena, size, VM_NOSLEEP);
	if (unlikely(new_cpus == NULL))
		goto fail;

	bzero(new_cpus, size);
	for (idx = 0; idx < umem_max_ncpus; idx++) {
		new_cpus[idx].cpu_number = idx;
		new_cpus[idx].cpu_cache_offset = UMEM_CACHE_SIZE(idx);
	}
	umem_cpus = new_cpus;
	umem_cpu_mask = (umem_max_ncpus - 1);

	/*
	 * Build the CPU-to-NUMA-node mapping table.
	 *
	 * When libnuma is available, query the actual topology.
	 * Otherwise all CPUs map to node 0 (single-node assumption).
	 * The depot stealing loop uses this to prefer same-node CPUs.
	 */
	{
		uint32_t ncpus_map = umem_max_ncpus;

		/*
		 * Size the CPU->node table to umem_max_ncpus so the depot
		 * stealing loop (which indexes [0, cache_depot_ncpus) ==
		 * [0, umem_max_ncpus)) never reads out of bounds.  Fall back
		 * to the fixed static table (single node) if the machine has
		 * <= UMEM_MAX_DEPOT_CPUS CPUs or if allocation fails.
		 */
		if (ncpus_map > UMEM_MAX_DEPOT_CPUS) {
			size_t nbytes = (size_t)ncpus_map * sizeof (int);
			void *tbl = mmap(NULL, nbytes,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANON, -1, 0);
			if (tbl != MAP_FAILED) {
				umem_cpu_node = (int *)tbl;
				umem_cpu_node_ncpus = ncpus_map;
			} else {
				/* fall back: cap mapping at the static size */
				ncpus_map = UMEM_MAX_DEPOT_CPUS;
			}
		}
		memset(umem_cpu_node, 0,
		    (size_t)umem_cpu_node_ncpus * sizeof (int));
#ifdef HAVE_LIBNUMA
		if (numa_available() >= 0) {
			umem_num_nodes = numa_max_node() + 1;
			for (uint32_t ci = 0; ci < ncpus_map; ci++) {
				int node = numa_node_of_cpu((int)ci);
				umem_cpu_node[ci] = (node >= 0) ? node : 0;
			}
		}
#endif
	}

	if (umem_maxverify == 0)
		umem_maxverify = maxverify;

	if (umem_minfirewall == 0)
		umem_minfirewall = minfirewall;

	/*
	 * Set up updating and reaping
	 */
	umem_reap_next = gethrtime() + NANOSEC;

#ifndef UMEM_STANDALONE
	(void) gettimeofday(&umem_update_next, NULL);
#endif

	/*
	 * Set up logging -- failure here is okay, since it will just disable
	 * the logs
	 */
	if (umem_logging) {
		umem_transaction_log = umem_log_init(umem_transaction_log_size);
		umem_content_log = umem_log_init(umem_content_log_size);
		umem_failure_log = umem_log_init(umem_failure_log_size);
		umem_slab_log = umem_log_init(umem_slab_log_size);
	}

	/*
	 * Set up caches -- if successful, initialization cannot fail, since
	 * allocations from other threads can now succeed.
	 */
	if (umem_cache_init() == 0) {
		log_message("unable to create initial caches\n");
		goto fail;
	}
	umem_oversize_arena = oversize_arena;
	umem_memalign_arena = memalign_arena;

	umem_cache_applyall(umem_cache_magazine_enable);

#ifndef UMEM_STANDALONE
	/*
	 * Initialize per-thread cache for fast small allocations.
	 * This provides zero-lock access for sizes 8-448 bytes.
	 */
	umem_ptc_init();
#endif

	umem_stacktrace_init();

	/*
	 * Initialize allocation profiling if configured via UMEM_OPTIONS
	 * (profile=record:/path or profile=use:/path) or the UMEM_PROFILE
	 * environment variable.
	 *
	 * P5.2: UMEM_PROFILE is read HERE, not through envvar.c's table, so
	 * envvar.c's secure-mode filter does not cover it.  Gate it directly:
	 * record: creates and truncates a caller-named file as this process's
	 * uid.  umem_profile_spec is already empty in secure mode (the
	 * "profile" option is marked secure-unsafe), but this second check is
	 * not redundant -- it is the one that covers the env var.
	 */
	if (!umem_secure_mode()) {
		const char *profile_env = getenv("UMEM_PROFILE");
		if (umem_profile_spec[0] != '\0')
			(void) umem_profile_init(umem_profile_spec);
		else if (profile_env != NULL && profile_env[0] != '\0')
			(void) umem_profile_init(profile_env);
	}

	/*
	 * Start the in-process introspection channel if UMEM_OPTIONS
	 * requested it (introspect=1). No-op when compiled out or disabled.
	 */
	umem_introspect_start();

	/*
	 * Start the update thread.
	 *
	 * Until now nothing did: the only creator was umem_reap(), which is
	 * called by the application or by a *failed* backend allocation.  So
	 * in a process that never ran out of memory and never called
	 * umem_reap() -- i.e. almost every process -- there was no update
	 * thread at all, and the periodic pass (umem_cache_update: hash
	 * rescale, magazine resize, depot working-set bookkeeping, slab page
	 * reclaim) never ran.  Every feature that documents itself as
	 * "background" or "periodic" was in fact dead until the first
	 * allocation failure.  The reclaim feature's own tests drove the pass
	 * by hand (repro_reclaim_reuse.c: update_pass()), which is how this
	 * stayed unseen.  Found by P6.8: a freed 128 MB heap, 0 bytes back in
	 * 20 s, gdb showing one thread in the process.
	 *
	 * Done here, after the caches exist and before READY, because
	 * pthread_create must not run under umem_update_lock held across a
	 * fork handler (umem_create_update_thread drops and retakes it) and
	 * because the thread's first act is umem_process_updates(), which
	 * needs the cache list.  Failure is non-fatal: the library works
	 * without the thread exactly as it always did, and umem_reap() will
	 * retry creation.  UMEM_STANDALONE is single-threaded by definition.
	 */
#ifndef UMEM_STANDALONE
	(void) mutex_lock(&umem_update_lock);
	if (umem_update_thr == 0)
		(void) umem_create_update_thread();
	(void) mutex_unlock(&umem_update_lock);
#endif

	/*
	 * initialization done, ready to go
	 */
	(void) mutex_lock(&umem_init_lock);
	umem_ready = UMEM_READY;
	umem_init_thr = 0;
	(void) cond_broadcast(&umem_init_cv);
	(void) mutex_unlock(&umem_init_lock);
	return (1);

fail:
	log_message("umem initialization failed\n");

	(void) mutex_lock(&umem_init_lock);
	umem_ready = UMEM_READY_INIT_FAILED;
	umem_init_thr = 0;
	(void) cond_broadcast(&umem_init_cv);
	(void) mutex_unlock(&umem_init_lock);
	return (0);
}

#ifndef UMEM_STANDALONE
void
__attribute__((constructor))
__umem_init (void)
{
	umem_startup(NULL, 0, 0, NULL, NULL);
}
#endif
